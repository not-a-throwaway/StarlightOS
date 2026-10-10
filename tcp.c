/*
 * tcp.c -- a compact but real TCP.
 *
 *  - 3-way handshake (active and passive open), MSS + RFC 7323 window-scale
 *    options, so received and advertised windows can exceed 64 KiB
 *  - in-order receive with duplicate-ACK on gaps, window advertisement
 *  - per-socket send buffer, retransmission with RTT-estimated RTO
 *    (Karn's rule, exponential backoff) and a slow-start cwnd
 *  - graceful close (FIN handshake, TIME_WAIT), RST generation/handling
 */
#include "net.h"
#include "net_priv.h"
#include "kstring.h"
#include "ktime.h"
#include "ksched.h"
#include "poll.h"

#define TCP_SOCKS   16
#define RX_BUF      (128u * 1024u)     /* receive window; large enough for window scaling to matter */
#define TX_BUF      (64u * 1024u)
#define MSS         1460u
#define CWND_MAX    RX_BUF             /* congestion-window ceiling */
#define MAX_RETRIES 7
#define TIME_WAIT_MS 2000u

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

struct sock {
    uint8_t  used, app_closed, fin_pending, fin_sent, is_listener;
    int      state;
    int      err;
    uint8_t  rip[4];
    uint16_t lport, rport;
    uint32_t iss, snd_una, snd_nxt, snd_wnd, rcv_nxt, irs;
    uint32_t cwnd, rto, srtt;
    uint32_t rt_deadline;
    uint8_t  retries;
    uint32_t rtt_seq, rtt_start;         /* sequence being timed, 0 = none */
    uint32_t tw_deadline;
    uint16_t peer_mss;
    uint8_t  offer_ws;                   /* send the window-scale option in our SYN/SYN-ACK */
    uint8_t  ws_ok;                      /* peer offered window scaling */
    uint8_t  snd_wscale;                 /* peer's scale, applied to windows we receive */
    uint8_t  rcv_shift;                  /* our scale, applied to windows we advertise */
    int      parent;                     /* listener that spawned this socket, or -1 */
    uint8_t  accepted;
    uint32_t rx_head, rx_len, tx_head, tx_len;
    uint8_t  rx[RX_BUF];
    uint8_t  tx[TX_BUF];
};

static struct sock socks[TCP_SOCKS];
/* One wait queue per socket slot. Kept outside struct sock so sock_reset()
 * (which zeroes the struct) can never wipe a waiter list that kpoll() holds. */
static struct kwq sock_wq[TCP_SOCKS];
extern uint16_t net_alloc_port(void);

static inline int32_t sdiff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

const char *tcp_state_name(int s)
{
    static const char *n[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT1",
                               "FIN_WAIT2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };
    return (s >= 0 && s <= TCP_TIME_WAIT) ? n[s] : "?";
}

static void sock_reset(struct sock *s)
{
    memset(s, 0, offsetof(struct sock, rx));
    s->parent = -1;
}

static int alloc_sock(void)
{
    for (int i = 0; i < TCP_SOCKS; i++)
        if (!socks[i].used) { sock_reset(&socks[i]); socks[i].used = 1; return i; }
    /* Reap an app-closed socket that is finished. */
    for (int i = 0; i < TCP_SOCKS; i++)
        if (socks[i].used && socks[i].app_closed && socks[i].state == TCP_CLOSED) {
            sock_reset(&socks[i]); socks[i].used = 1; return i;
        }
    return -1;
}

/* ---------------------------------------------------------------- output */

struct seg_ctx {
    uint8_t  rip[4];
    uint16_t lport, rport;
    uint32_t rx_len;
    uint8_t  shift;      /* right-shift for the advertised window (0 = unscaled) */
    uint8_t  ws;         /* window-scale value to advertise with the SYN options */
    uint8_t  ws_on;      /* include the window-scale option (only with SYN/SYN-ACK) */
};

/* Our RFC 7323 window scale: the smallest shift for which the whole receive
 * buffer fits in the 16-bit window field. Cached after the first call. */
static uint8_t local_wscale(void)
{
    static int s = -1;
    if (s < 0) {
        uint32_t b = RX_BUF; s = 0;
        while (s < 14 && (b >> s) > 65535u) s++;
    }
    return (uint8_t)s;
}

static void send_raw(const struct seg_ctx *s, uint32_t seq, uint32_t ack, uint8_t flags,
                     const uint8_t *data, uint16_t len, int with_opts)
{
    uint8_t seg[32 + MSS];
    uint8_t hl = with_opts ? (s->ws_on ? 28 : 24) : 20;
    memset(seg, 0, hl);
    wr16(seg, s->lport); wr16(seg + 2, s->rport);
    wr32(seg + 4, seq); wr32(seg + 8, ack);
    seg[12] = (uint8_t)((hl / 4) << 4);
    seg[13] = flags;
    uint32_t free_rx = RX_BUF - s->rx_len;
    if (with_opts) {
        /* The SYN/SYN-ACK window field is never scaled (RFC 7323). */
        wr16(seg + 14, (uint16_t)(free_rx > 65535u ? 65535u : free_rx));
        seg[20] = 2; seg[21] = 4; wr16(seg + 22, (uint16_t)MSS);       /* MSS       */
        if (s->ws_on) { seg[24] = 3; seg[25] = 3; seg[26] = s->ws; seg[27] = 1; } /* WS + NOP */
    } else {
        uint32_t w = free_rx >> s->shift;
        wr16(seg + 14, (uint16_t)(w > 65535u ? 65535u : w));
    }
    if (len) memcpy(seg + hl, data, len);
    const struct net_config *c = net_get_config();
    uint32_t sum = net_pseudo_sum(c->ip, s->rip, 6, (uint16_t)(hl + len));
    wr16(seg + 16, net_checksum(seg, (size_t)(hl + len), sum));
    net_stats_rw()->tcp_tx++;
    ip_send_ex(c->ip, s->rip, NULL, 6, 64, seg, (uint16_t)(hl + len));
}

static void send_segment(struct sock *s, uint32_t seq, uint32_t ack, uint8_t flags,
                         const uint8_t *data, uint16_t len, int with_opts)
{
    struct seg_ctx c;
    memcpy(c.rip, s->rip, 4); c.lport = s->lport; c.rport = s->rport;
    c.rx_len = s->rx_len; c.shift = s->rcv_shift;
    c.ws = local_wscale(); c.ws_on = s->offer_ws;
    send_raw(&c, seq, ack, flags, data, len, with_opts);
}

static void send_ack(struct sock *s) { send_segment(s, s->snd_nxt, s->rcv_nxt, F_ACK, NULL, 0, 0); }

static void send_rst_for(const uint8_t *src_ip, const uint8_t *seg, uint16_t len)
{
    /* Stateless reset in reply to a segment that matches no socket. */
    if (seg[13] & F_RST) return;
    struct seg_ctx tmp;
    memset(&tmp, 0, sizeof(tmp));
    memcpy(tmp.rip, src_ip, 4);
    tmp.lport = be16(seg + 2); tmp.rport = be16(seg);
    uint8_t th = (uint8_t)((seg[12] >> 4) * 4);
    uint32_t seg_len = (uint32_t)(len - th) + ((seg[13] & F_SYN) ? 1u : 0u) + ((seg[13] & F_FIN) ? 1u : 0u);
    if (seg[13] & F_ACK) send_raw(&tmp, be32(seg + 8), 0, F_RST, NULL, 0, 0);
    else send_raw(&tmp, 0, be32(seg + 4) + seg_len, F_RST | F_ACK, NULL, 0, 0);
    net_stats_rw()->tcp_resets++;
}

static void set_rto_timer(struct sock *s) { s->rt_deadline = ktime_ms() + s->rto; }

/* Push as much queued data (and a pending FIN) as window and cwnd allow. */
static void tcp_output(struct sock *s)
{
    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT &&
        s->state != TCP_FIN_WAIT1 && s->state != TCP_LAST_ACK && s->state != TCP_CLOSING) return;

    for (;;) {
        uint32_t inflight = s->snd_nxt - s->snd_una;
        uint32_t data_inflight = inflight - (s->fin_sent ? 1u : 0u);
        if (data_inflight > s->tx_len) data_inflight = s->tx_len;
        uint32_t unsent = s->tx_len - data_inflight;
        uint32_t wnd = s->snd_wnd < s->cwnd ? s->snd_wnd : s->cwnd;
        if (s->fin_sent) break;
        if (unsent == 0) break;
        if (wnd <= inflight) {
            if (s->snd_wnd == 0 && inflight == 0 && unsent) {             /* zero-window probe */
                uint8_t b = s->tx[(s->tx_head + data_inflight) % TX_BUF];
                send_segment(s, s->snd_nxt, s->rcv_nxt, F_ACK, &b, 1, 0);
                set_rto_timer(s);
            }
            break;
        }
        uint32_t n = unsent;
        uint32_t peer = s->peer_mss ? s->peer_mss : 536u;
        if (n > peer) n = peer;
        if (n > MSS) n = MSS;
        if (n > wnd - inflight) n = wnd - inflight;
        uint8_t chunk[MSS];
        for (uint32_t i = 0; i < n; i++) chunk[i] = s->tx[(s->tx_head + data_inflight + i) % TX_BUF];
        if (!s->rtt_seq) { s->rtt_seq = s->snd_nxt + n; s->rtt_start = ktime_ms(); }
        send_segment(s, s->snd_nxt, s->rcv_nxt, (uint8_t)(F_ACK | (n == unsent ? F_PSH : 0)), chunk, (uint16_t)n, 0);
        s->snd_nxt += n;
        if (s->snd_una + 0 == s->snd_nxt - n) set_rto_timer(s);
        if (s->rt_deadline == 0) set_rto_timer(s);
    }

    /* FIN goes out once every data byte has been transmitted. */
    uint32_t data_inflight = (s->snd_nxt - s->snd_una);
    if (s->fin_pending && !s->fin_sent && data_inflight >= s->tx_len) {
        send_segment(s, s->snd_nxt, s->rcv_nxt, F_FIN | F_ACK, NULL, 0, 0);
        s->snd_nxt++; s->fin_sent = 1; set_rto_timer(s);
    }
}

/* ----------------------------------------------------------------- input */

static void enter_closed(struct sock *s, int err)
{
    if (err && !s->err) s->err = err;
    s->state = TCP_CLOSED;
    s->rt_deadline = 0;
}

static struct sock *find_sock(const uint8_t *rip, uint16_t rport, uint16_t lport)
{
    for (int i = 0; i < TCP_SOCKS; i++) {
        struct sock *s = &socks[i];
        if (s->used && !s->is_listener && s->state != TCP_CLOSED && s->lport == lport &&
            s->rport == rport && ip_eq(s->rip, rip)) return s;
    }
    return NULL;
}

static struct sock *find_listener(uint16_t lport)
{
    for (int i = 0; i < TCP_SOCKS; i++)
        if (socks[i].used && socks[i].is_listener && socks[i].state == TCP_LISTEN && socks[i].lport == lport) return &socks[i];
    return NULL;
}

/* Parse the MSS (kind 2) and window-scale (kind 3) options from a SYN/SYN-ACK. */
static void parse_opts(const uint8_t *opt, unsigned optlen, uint16_t *mss, uint8_t *ws, int *has_ws)
{
    *mss = 536; *ws = 0; *has_ws = 0;
    for (unsigned i = 0; i + 1 < optlen; ) {
        uint8_t k = opt[i];
        if (k == 0) break;                 /* end of option list */
        if (k == 1) { i++; continue; }     /* NOP */
        uint8_t kl = opt[i + 1];
        if (kl < 2 || i + kl > optlen) break;
        if (k == 2 && kl == 4) *mss = be16(opt + i + 2);
        else if (k == 3 && kl == 3) { *ws = opt[i + 2]; if (*ws > 14) *ws = 14; *has_ws = 1; }
        i += kl;
    }
}

static void tcp_input_impl(const uint8_t *src_ip, const uint8_t *dst_ip, const uint8_t *seg, uint16_t len)
{
    if (len < 20) return;
    uint32_t sum = net_pseudo_sum(src_ip, dst_ip, 6, len);
    if (net_checksum(seg, len, sum) != 0) { net_stats_rw()->drop_csum++; return; }
    uint8_t th = (uint8_t)((seg[12] >> 4) * 4);
    if (th < 20 || th > len) return;

    uint16_t sport = be16(seg), dport = be16(seg + 2);
    uint32_t seq = be32(seg + 4), ack = be32(seg + 8);
    uint8_t flags = seg[13];
    uint16_t wnd = be16(seg + 14);
    const uint8_t *payload = seg + th;
    uint32_t plen = (uint32_t)(len - th);

    struct sock *s = find_sock(src_ip, sport, dport);

    if (!s) {
        struct sock *l;
        if ((flags & F_SYN) && !(flags & (F_ACK | F_RST)) && (l = find_listener(dport)) != NULL) {
            int idx = alloc_sock();
            if (idx < 0) return;                                         /* no room: drop SYN */
            struct sock *n = &socks[idx];
            memcpy(n->rip, src_ip, 4);
            n->lport = dport; n->rport = sport; n->parent = (int)(l - socks);
            n->irs = seq; n->rcv_nxt = seq + 1;
            n->iss = (uint32_t)ktime_us() * 2654435761u; n->snd_una = n->iss; n->snd_nxt = n->iss + 1;
            n->snd_wnd = wnd; n->cwnd = 2 * MSS; n->rto = 400; n->srtt = 0;
            n->peer_mss = 536;
            uint8_t pws = 0; int has_ws = 0; uint16_t pmss = 536;
            parse_opts(seg + 20, (unsigned)(th - 20), &pmss, &pws, &has_ws);
            n->peer_mss = pmss;
            n->ws_ok = (uint8_t)has_ws;
            n->snd_wscale = (uint8_t)(has_ws ? pws : 0);
            n->rcv_shift = (uint8_t)(has_ws ? local_wscale() : 0);
            n->offer_ws = (uint8_t)has_ws;     /* only echo WS if the peer offered it */
            if (has_ws) net_stats_rw()->tcp_ws++;
            n->state = TCP_SYN_RCVD;
            send_segment(n, n->iss, n->rcv_nxt, F_SYN | F_ACK, NULL, 0, 1);
            n->retries = 0; set_rto_timer(n);
            return;
        }
        send_rst_for(src_ip, seg, len);
        return;
    }

    if (flags & F_RST) {
        if (s->state == TCP_SYN_SENT) { if ((flags & F_ACK) && ack == s->snd_nxt) enter_closed(s, TCP_ERR_REFUSED); }
        else if (sdiff(seq, s->rcv_nxt) >= 0 && sdiff(seq, s->rcv_nxt + RX_BUF) < 0) enter_closed(s, TCP_ERR_RESET);
        net_stats_rw()->tcp_resets++;
        return;
    }

    if (s->state == TCP_SYN_SENT) {
        if ((flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == s->snd_nxt) {
            s->irs = seq; s->rcv_nxt = seq + 1;
            s->snd_una = ack; s->snd_wnd = wnd; s->rt_deadline = 0; s->retries = 0;
            s->peer_mss = 536;
            uint8_t pws = 0; int has_ws = 0; uint16_t pmss = 536;
            parse_opts(seg + 20, (unsigned)(th - 20), &pmss, &pws, &has_ws);
            s->peer_mss = pmss;
            s->ws_ok = (uint8_t)has_ws;
            s->snd_wscale = (uint8_t)(has_ws ? pws : 0);
            s->rcv_shift = (uint8_t)(has_ws ? local_wscale() : 0);
            if (has_ws) net_stats_rw()->tcp_ws++;
            if (s->rtt_seq && sdiff(ack, s->rtt_seq) >= 0) {
                uint32_t r = ktime_ms() - s->rtt_start; if (!r) r = 1;
                s->srtt = r; s->rto = 3 * r < 200 ? 200 : 3 * r > 3000 ? 3000 : 3 * r;
                s->rtt_seq = 0;
            }
            s->state = TCP_ESTABLISHED;
            send_ack(s);
        } else if ((flags & F_ACK) && ack != s->snd_nxt) {
            send_rst_for(src_ip, seg, len);
        }
        return;
    }

    /* ---- ACK processing ---- */
    if (flags & F_ACK) {
        if (s->state == TCP_SYN_RCVD) {
            if (ack == s->snd_nxt) {
                s->snd_una = ack; s->state = TCP_ESTABLISHED; s->rt_deadline = 0; s->retries = 0;
            } else { send_rst_for(src_ip, seg, len); return; }
        }
        if (sdiff(ack, s->snd_una) > 0 && sdiff(ack, s->snd_nxt) <= 0) {
            uint32_t acked = ack - s->snd_una;
            uint32_t data_acked = acked;
            int fin_acked = 0;
            if (s->fin_sent && ack == s->snd_nxt) { fin_acked = 1; data_acked--; }
            if (data_acked > s->tx_len) data_acked = s->tx_len;
            s->tx_head = (s->tx_head + data_acked) % TX_BUF;
            s->tx_len -= data_acked;
            s->snd_una = ack;
            s->retries = 0;
            if (s->rtt_seq && sdiff(ack, s->rtt_seq) >= 0) {                 /* RTT sample (Karn: only if not retransmitted) */
                uint32_t r = ktime_ms() - s->rtt_start; if (!r) r = 1;
                s->srtt = s->srtt ? (s->srtt * 7 + r) / 8 : r;
                uint32_t rto = s->srtt * 3; if (rto < 200) rto = 200; if (rto > 3000) rto = 3000;
                s->rto = rto; s->rtt_seq = 0;
            }
            if (s->cwnd < CWND_MAX) s->cwnd += (s->cwnd < 8 * MSS) ? data_acked : MSS;
            if (s->snd_una == s->snd_nxt) s->rt_deadline = 0; else set_rto_timer(s);
            if (fin_acked) {
                if (s->state == TCP_FIN_WAIT1) s->state = TCP_FIN_WAIT2;
                else if (s->state == TCP_CLOSING) { s->state = TCP_TIME_WAIT; s->tw_deadline = ktime_ms() + TIME_WAIT_MS; }
                else if (s->state == TCP_LAST_ACK) { enter_closed(s, 0); return; }
            }
        }
        s->snd_wnd = (uint32_t)wnd << s->snd_wscale;     /* windows after the handshake are scaled */
    }

    /* ---- data ---- */
    int send_ack_now = 0;
    uint32_t dseq = seq;
    const uint8_t *d = payload;
    uint32_t dlen = plen;
    if ((s->state == TCP_ESTABLISHED || s->state == TCP_FIN_WAIT1 || s->state == TCP_FIN_WAIT2) && dlen) {
        if (sdiff(dseq, s->rcv_nxt) < 0) {                                  /* overlap with already-received bytes */
            uint32_t skip = s->rcv_nxt - dseq;
            if (skip >= dlen) { dlen = 0; send_ack_now = 1; } else { d += skip; dlen -= skip; dseq = s->rcv_nxt; }
        }
        if (dlen) {
            if (dseq != s->rcv_nxt) { send_ack_now = 1; dlen = 0; }          /* gap: dup-ACK, drop */
            else {
                uint32_t room = RX_BUF - s->rx_len;
                uint32_t take = dlen < room ? dlen : room;
                for (uint32_t i = 0; i < take; i++) s->rx[(s->rx_head + s->rx_len + i) % RX_BUF] = d[i];
                s->rx_len += take; s->rcv_nxt += take;
                send_ack_now = 1;
                if (take < dlen) dlen = take, flags &= (uint8_t)~F_FIN;      /* couldn't take it all: ignore FIN */
            }
        }
    }

    /* ---- FIN ---- */
    if ((flags & F_FIN) && sdiff(seq + plen, s->rcv_nxt) == 0 &&
        (s->state == TCP_ESTABLISHED || s->state == TCP_FIN_WAIT1 || s->state == TCP_FIN_WAIT2 || s->state == TCP_SYN_RCVD)) {
        s->rcv_nxt++;
        send_ack_now = 1;
        if (s->state == TCP_ESTABLISHED || s->state == TCP_SYN_RCVD) s->state = TCP_CLOSE_WAIT;
        else if (s->state == TCP_FIN_WAIT1) s->state = (s->snd_una == s->snd_nxt) ? TCP_TIME_WAIT : TCP_CLOSING;
        else { s->state = TCP_TIME_WAIT; }
        if (s->state == TCP_TIME_WAIT) s->tw_deadline = ktime_ms() + TIME_WAIT_MS;
    } else if (s->state == TCP_TIME_WAIT && (flags & F_FIN)) { send_ack_now = 1; }

    if (send_ack_now) send_ack(s);
    tcp_output(s);
}

/* ---------------------------------------------------------------- timers */

static void tcp_timers_impl(void)
{
    uint32_t now = ktime_ms();
    for (int i = 0; i < TCP_SOCKS; i++) {
        struct sock *s = &socks[i];
        if (!s->used) continue;
        if (s->state == TCP_TIME_WAIT && (int32_t)(now - s->tw_deadline) >= 0) enter_closed(s, 0);
        if (s->state == TCP_FIN_WAIT2 && s->app_closed && s->tw_deadline == 0) s->tw_deadline = now + 8000;
        if (s->state == TCP_FIN_WAIT2 && s->app_closed && (int32_t)(now - s->tw_deadline) >= 0) enter_closed(s, 0);
        if (s->state == TCP_CLOSED && s->app_closed && s->used) { s->used = 0; continue; }
        if (!s->rt_deadline || (int32_t)(now - s->rt_deadline) < 0) continue;

        /* retransmission timeout */
        if (++s->retries > MAX_RETRIES) { enter_closed(s, TCP_ERR_TIMEOUT); continue; }
        net_stats_rw()->tcp_retrans++;
        s->rto = s->rto * 2 > 4000 ? 4000 : s->rto * 2;
        s->cwnd = MSS; s->rtt_seq = 0;
        if (s->state == TCP_SYN_SENT) send_segment(s, s->iss, 0, F_SYN, NULL, 0, 1);
        else if (s->state == TCP_SYN_RCVD) send_segment(s, s->iss, s->rcv_nxt, F_SYN | F_ACK, NULL, 0, 1);
        else {
            uint32_t data_inflight = (s->snd_nxt - s->snd_una) - (s->fin_sent ? 1u : 0u);
            if (data_inflight > s->tx_len) data_inflight = s->tx_len;
            if (data_inflight) {
                uint32_t n = data_inflight < MSS ? data_inflight : MSS;
                if (s->peer_mss && n > s->peer_mss) n = s->peer_mss;
                uint8_t chunk[MSS];
                for (uint32_t k = 0; k < n; k++) chunk[k] = s->tx[(s->tx_head + k) % TX_BUF];
                send_segment(s, s->snd_una, s->rcv_nxt, F_ACK, chunk, (uint16_t)n, 0);
            } else if (s->fin_sent) {
                send_segment(s, s->snd_nxt - 1, s->rcv_nxt, F_FIN | F_ACK, NULL, 0, 0);
            } else if (s->snd_wnd == 0) {
                tcp_output(s);
            }
        }
        set_rto_timer(s);
    }
}

/* ----------------------------------------------------- poll integration */

struct kwq *tcp_waitq(int h) { return (h >= 0 && h < TCP_SOCKS) ? &sock_wq[h] : 0; }

static void wake_slot(int i) { if (i >= 0 && i < TCP_SOCKS) kwq_wake_all(&sock_wq[i]); }

/* Readiness of a socket as POLL* bits (the kfd layer and kpoll() use this). */
unsigned tcp_poll_mask(int h)
{
    if (h < 0 || h >= TCP_SOCKS || !socks[h].used) return POLLNVAL;
    struct sock *s = &socks[h];
    unsigned m = 0;
    if (s->is_listener) {
        for (int i = 0; i < TCP_SOCKS; i++)
            if (socks[i].used && socks[i].parent == h && !socks[i].accepted &&
                (socks[i].state == TCP_ESTABLISHED || socks[i].state == TCP_CLOSE_WAIT)) { m |= POLLIN; break; }
        return m;
    }
    if (s->rx_len) m |= POLLIN;
    switch (s->state) {
    case TCP_ESTABLISHED:
        if (TX_BUF - s->tx_len > 0) m |= POLLOUT;
        break;
    case TCP_CLOSE_WAIT:                       /* peer sent FIN: reads see EOF, we may still write */
        m |= POLLIN;
        if (TX_BUF - s->tx_len > 0) m |= POLLOUT;
        break;
    case TCP_LAST_ACK: case TCP_CLOSING: case TCP_TIME_WAIT:
    case TCP_FIN_WAIT1: case TCP_FIN_WAIT2:
        if (s->state != TCP_FIN_WAIT1 && s->state != TCP_FIN_WAIT2) m |= POLLIN;
        break;
    case TCP_CLOSED:
        m |= POLLIN | POLLHUP;
        break;
    default: break;                            /* SYN_SENT / SYN_RCVD: nothing yet */
    }
    if (s->err) m |= POLLERR;
    return m;
}

int tcp_tx_space(int h)
{
    if (h < 0 || h >= TCP_SOCKS || !socks[h].used) return 0;
    return (int)(TX_BUF - socks[h].tx_len);
}

/* Called after every received segment and timer pass. Wakes the queue of
 * every socket whose state could have changed; a listener is woken when one
 * of its children completes the handshake. Spurious wake-ups are harmless:
 * kpoll() just re-evaluates. */
static void wake_all_waiters(void)
{
    for (int i = 0; i < TCP_SOCKS; i++) {
        if (!socks[i].used || !sock_wq[i].head) continue;
        kwq_wake_all(&sock_wq[i]);
    }
    for (int i = 0; i < TCP_SOCKS; i++)
        if (socks[i].used && socks[i].parent >= 0) wake_slot(socks[i].parent);
}

void tcp_input(const uint8_t *src_ip, const uint8_t *dst_ip, const uint8_t *seg, uint16_t len)
{
    tcp_input_impl(src_ip, dst_ip, seg, len);
    wake_all_waiters();
}

void tcp_timers(void)
{
    tcp_timers_impl();
    wake_all_waiters();
}

/* ------------------------------------------------------------------- API */

int tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms)
{
    if (!net_configured()) return TCP_ERR_NOROUTE;
    int idx = alloc_sock();
    if (idx < 0) return TCP_ERR_NOSOCK;
    struct sock *s = &socks[idx];
    memcpy(s->rip, ip, 4);
    s->lport = net_alloc_port(); s->rport = port;
    s->iss = (uint32_t)ktime_us() * 2654435761u;
    s->snd_una = s->iss; s->snd_nxt = s->iss + 1;
    s->snd_wnd = 1460; s->cwnd = 2 * MSS; s->rto = 500; s->peer_mss = 536;
    s->offer_ws = 1;                       /* offer RFC 7323 scaling in our SYN */
    s->state = TCP_SYN_SENT;

    /* Resolve L2 first so a slow ARP doesn't eat SYN retransmits. */
    const struct net_config *c = net_get_config();
    uint8_t mac[6];
    const uint8_t *hop = net_on_link(ip) ? ip : c->gateway;
    if (!net_arp_resolve(hop, mac, 2000)) { s->used = 0; return TCP_ERR_NOROUTE; }

    s->rtt_seq = s->snd_nxt; s->rtt_start = ktime_ms();
    send_segment(s, s->iss, 0, F_SYN, NULL, 0, 1);
    set_rto_timer(s);

    uint32_t start = ktime_ms();
    while (s->state == TCP_SYN_SENT) {
        if ((uint32_t)(ktime_ms() - start) >= timeout_ms) { s->used = 0; s->state = TCP_CLOSED; return TCP_ERR_TIMEOUT; }
        if (kabort_requested()) { s->used = 0; s->state = TCP_CLOSED; return TCP_ERR_ABORT; }
        net_poll();
        if (s->state == TCP_SYN_SENT) kyield();
    }
    if (s->state != TCP_ESTABLISHED) { int e = s->err ? s->err : TCP_ERR_REFUSED; s->used = 0; return e; }
    return idx;
}

int tcp_listen(uint16_t port)
{
    if (find_listener(port)) return TCP_ERR_NOSOCK;
    int idx = alloc_sock();
    if (idx < 0) return TCP_ERR_NOSOCK;
    socks[idx].is_listener = 1; socks[idx].lport = port; socks[idx].state = TCP_LISTEN;
    return idx;
}

int tcp_accept(int l, uint32_t timeout_ms)
{
    if (l < 0 || l >= TCP_SOCKS || !socks[l].is_listener) return -1;
    uint32_t start = ktime_ms();
    for (;;) {
        for (int i = 0; i < TCP_SOCKS; i++) {
            struct sock *s = &socks[i];
            if (s->used && s->parent == l && !s->accepted &&
                (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT)) { s->accepted = 1; return i; }
        }
        if ((uint32_t)(ktime_ms() - start) >= timeout_ms || kabort_requested()) return -1;
        net_poll();
        kyield();
    }
}

int tcp_get_state(int h) { return (h >= 0 && h < TCP_SOCKS && socks[h].used) ? socks[h].state : TCP_CLOSED; }

int tcp_peer(int h, uint8_t ip[4], uint16_t *port)
{
    if (h < 0 || h >= TCP_SOCKS || !socks[h].used) return -1;
    if (ip) memcpy(ip, socks[h].rip, 4);
    if (port) *port = socks[h].rport;
    return 0;
}

int tcp_available(int h) { return (h >= 0 && h < TCP_SOCKS && socks[h].used) ? (int)socks[h].rx_len : 0; }

int tcp_send(int h, const void *data, size_t len)
{
    if (h < 0 || h >= TCP_SOCKS || !socks[h].used) return TCP_ERR_NOSOCK;
    struct sock *s = &socks[h];
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    uint32_t last_progress = ktime_ms();
    while (sent < len) {
        if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT) return s->err ? s->err : TCP_ERR_RESET;
        uint32_t room = TX_BUF - s->tx_len;
        if (room) {
            uint32_t n = (uint32_t)(len - sent) < room ? (uint32_t)(len - sent) : room;
            for (uint32_t i = 0; i < n; i++) s->tx[(s->tx_head + s->tx_len + i) % TX_BUF] = p[sent + i];
            s->tx_len += n; sent += n;
            tcp_output(s);
            last_progress = ktime_ms();
        } else {
            net_poll();
            tcp_output(s);
            if ((uint32_t)(ktime_ms() - last_progress) > 20000u) return TCP_ERR_TIMEOUT;
            if (kabort_requested()) return TCP_ERR_ABORT;
            kyield();
        }
    }
    return (int)sent;
}

int tcp_recv(int h, void *buf, size_t cap, uint32_t timeout_ms)
{
    if (h < 0 || h >= TCP_SOCKS || !socks[h].used) return TCP_ERR_NOSOCK;
    struct sock *s = &socks[h];
    uint32_t start = ktime_ms();
    for (;;) {
        if (s->rx_len) {
            uint32_t n = s->rx_len < cap ? s->rx_len : (uint32_t)cap;
            uint8_t *o = (uint8_t *)buf;
            for (uint32_t i = 0; i < n; i++) o[i] = s->rx[(s->rx_head + i) % RX_BUF];
            s->rx_head = (s->rx_head + n) % RX_BUF; s->rx_len -= n;
            if (RX_BUF - s->rx_len - n < 2 * MSS && RX_BUF - s->rx_len >= 2 * MSS) send_ack(s);   /* window update */
            return (int)n;
        }
        if (s->state == TCP_CLOSED) return s->err ? s->err : 0;
        if (s->state == TCP_CLOSE_WAIT || s->state == TCP_LAST_ACK || s->state == TCP_CLOSING || s->state == TCP_TIME_WAIT) return 0;
        if ((uint32_t)(ktime_ms() - start) >= timeout_ms) return -1;
        if (kabort_requested()) return TCP_ERR_ABORT;
        net_poll();
        if (!s->rx_len && s->state != TCP_CLOSED) kyield();
    }
}

/* Start a graceful close without waiting: queue the FIN behind any unsent data
 * and return. tcp_input()/tcp_timers() finish the teardown and reap the slot.
 * Returns the socket, or NULL if there is nothing left to wait for. */
static struct sock *tcp_close_begin(int h)
{
    if (h < 0 || h >= TCP_SOCKS || !socks[h].used) return NULL;
    struct sock *s = &socks[h];
    s->app_closed = 1;
    s->rx_len = 0;
    if (s->is_listener) { s->state = TCP_CLOSED; s->used = 0; kwq_wake_all(&sock_wq[h]); return NULL; }
    switch (s->state) {
    case TCP_ESTABLISHED: s->state = TCP_FIN_WAIT1; s->fin_pending = 1; break;
    case TCP_CLOSE_WAIT:  s->state = TCP_LAST_ACK;  s->fin_pending = 1; break;
    case TCP_SYN_RCVD:    s->state = TCP_FIN_WAIT1; s->fin_pending = 1; break;
    case TCP_SYN_SENT:    s->state = TCP_CLOSED; break;
    default: break;
    }
    tcp_output(s);
    return s;
}

void tcp_close_nowait(int h) { (void)tcp_close_begin(h); }

void tcp_close(int h)
{
    struct sock *s = tcp_close_begin(h);
    if (!s) return;
    /* Flush: give the data/FIN a chance to leave before returning. */
    uint32_t start = ktime_ms();
    while (s->state != TCP_CLOSED && s->state != TCP_TIME_WAIT && s->state != TCP_FIN_WAIT2 &&
           (uint32_t)(ktime_ms() - start) < 1500u && !kabort_requested()) {
        net_poll();
        if (s->tx_len == 0 && s->snd_una == s->snd_nxt) break;
        kyield();
    }
    /* Remaining teardown (FIN_WAIT2/TIME_WAIT/LAST_ACK) is reaped by tcp_timers(). */
}

void tcp_abort(int h)
{
    if (h < 0 || h >= TCP_SOCKS || !socks[h].used) return;
    struct sock *s = &socks[h];
    if (!s->is_listener && s->state != TCP_CLOSED && s->state != TCP_SYN_SENT)
        send_segment(s, s->snd_nxt, s->rcv_nxt, F_RST | F_ACK, NULL, 0, 0);
    s->state = TCP_CLOSED; s->used = 0;
}

int tcp_list(struct tcp_info *out, int max)
{
    int n = 0;
    for (int i = 0; i < TCP_SOCKS && n < max; i++) {
        if (!socks[i].used || (socks[i].state == TCP_CLOSED && !socks[i].is_listener)) continue;
        out[n].sock = i; out[n].state = socks[i].state;
        memcpy(out[n].remote_ip, socks[i].rip, 4);
        out[n].local_port = socks[i].lport; out[n].remote_port = socks[i].rport;
        out[n].rx_queued = socks[i].rx_len; out[n].tx_queued = socks[i].tx_len;
        out[n].snd_wnd = socks[i].snd_wnd; out[n].snd_wscale = socks[i].snd_wscale;
        out[n].rcv_shift = socks[i].rcv_shift; out[n].ws_ok = socks[i].ws_ok;
        n++;
    }
    return n;
}
