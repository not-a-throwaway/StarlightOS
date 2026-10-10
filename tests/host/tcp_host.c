/*
 * Host harness for tcp.c -- the RFC 7323 window-scale option and the windows
 * beyond 64 KiB that it unlocks.
 *
 * The whole layer below TCP is mocked: ip_send_ex() captures outgoing segments,
 * and the test feeds crafted segments back through tcp_input(). That makes the
 * negotiation deterministic and lets us check both roles and both peers:
 *
 *   - our SYN carries MSS + a window-scale option; our SYN-ACK echoes it only
 *     when the peer offered one;
 *   - a scaling peer -> we advertise a >64 KiB window and enlarge the window
 *     we accept from it (snd_wnd = peer_window << peer_scale);
 *   - a peer without the option -> both directions stay unscaled and the
 *     advertised window is capped at 65535;
 *   - the advertised receive window shrinks as unread data arrives.
 *
 *   tcp_host       run the tests; exit status 0 = all passed.
 *
 * Build:  gcc -I. -Itests/host -o /tmp/tcp_host tests/host/tcp_host.c tcp.c
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "../../net.h"
#include "../../net_priv.h"
#include "../../poll.h"

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

/* The peer we pretend to talk to. */
static const uint8_t PEER[4] = { 10, 0, 2, 2 };

/* ------------------------------------------------------- mocked bottom half */

static struct net_config g_cfg;
static struct net_stats  g_stats;
static uint32_t          g_now_ms;

const struct net_config *net_get_config(void) { return &g_cfg; }
struct net_stats        *net_stats_rw(void)   { return &g_stats; }
int  net_configured(void) { return 1; }
int  net_on_link(const uint8_t ip[4]) { (void)ip; return 1; }
int  net_arp_resolve(const uint8_t ip[4], uint8_t mac[6], uint32_t timeout_ms)
{ (void)ip; (void)timeout_ms; memset(mac, 0x02, 6); return 1; }
uint16_t net_alloc_port(void) { static uint16_t p = 40000; return p++; }
uint32_t ktime_ms(void) { return ++g_now_ms; }
uint64_t ktime_us(void) { return (uint64_t)(++g_now_ms) * 1000u; }
void kyield(void) {}
int  kabort_requested(void) { return 0; }
void kwq_wake_all(struct kwq *q) { (void)q; }

/* A correct Internet checksum, so tcp_input()'s checksum check passes and we
 * can verify the segments tcp.c emits. */
uint32_t net_pseudo_sum(const uint8_t src[4], const uint8_t dst[4], uint8_t proto, uint16_t len)
{
    uint32_t sum = 0;
    sum += (uint32_t)((src[0] << 8) | src[1]);
    sum += (uint32_t)((src[2] << 8) | src[3]);
    sum += (uint32_t)((dst[0] << 8) | dst[1]);
    sum += (uint32_t)((dst[2] << 8) | dst[3]);
    sum += proto; sum += len;
    return sum;
}
uint16_t net_checksum(const void *data, size_t len, uint32_t initial)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = initial;
    while (len >= 2) { sum += (uint32_t)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint32_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ------------------------------------------------- outgoing-segment capture */

#define MAX_CAPS 128
#define CAP_SIZE 4096
struct cap { uint8_t seg[CAP_SIZE]; uint16_t len; };
static struct cap caps[MAX_CAPS];
static int ncap;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static void reset_caps(void) { ncap = 0; }

int ip_send_ex(const uint8_t src[4], const uint8_t dst[4], const uint8_t *mac,
               uint8_t proto, uint8_t ttl, const uint8_t *payload, uint16_t len)
{
    (void)src; (void)dst; (void)mac; (void)proto; (void)ttl;
    if (ncap < MAX_CAPS && len <= CAP_SIZE) { memcpy(caps[ncap].seg, payload, len); caps[ncap].len = len; ncap++; }
    return 0;
}

/* First captured segment whose flags contain `want` and none of `notwant`. */
static int find_cap(int want, int notwant)
{
    for (int i = 0; i < ncap; i++) {
        int f = caps[i].seg[13];
        if ((f & want) == want && (f & notwant) == 0) return i;
    }
    return -1;
}

/* Check the TCP checksum of a captured segment (src = us, dst = peer). */
static int cap_csum_ok(int i)
{
    return caps[i].len >= 20 &&
           net_checksum(caps[i].seg, caps[i].len,
                        net_pseudo_sum(g_cfg.ip, PEER, 6, caps[i].len)) == 0;
}

/* Parse an option (MSS kind 2 / WS kind 3) from a captured segment. */
static int find_opt(const struct cap *c, uint8_t kind, unsigned *val)
{
    uint8_t th = (uint8_t)((c->seg[12] >> 4) * 4);
    if (th < 20 || th > c->len) return 0;
    for (unsigned i = 20; i + 1 < th; ) {
        uint8_t k = c->seg[i];
        if (k == 0) break;
        if (k == 1) { i++; continue; }
        uint8_t kl = c->seg[i + 1];
        if (kl < 2 || i + kl > th) break;
        if (k == kind && ((kind == 2 && kl == 4) || (kind == 3 && kl == 3))) {
            *val = (kind == 2) ? be16(c->seg + i + 2) : c->seg[i + 2];
            return 1;
        }
        i += kl;
    }
    return 0;
}

/* ------------------------------------------------------- segment injection */

static uint16_t build_seg(uint8_t *seg, uint16_t sport, uint16_t dport,
                          uint32_t seq, uint32_t ack, uint8_t flags, uint16_t wnd,
                          const uint8_t *opts, uint8_t optlen, const uint8_t *data, uint16_t dlen)
{
    uint16_t hl = (uint16_t)(20 + optlen);
    memset(seg, 0, (size_t)hl + dlen);
    wr16(seg, sport); wr16(seg + 2, dport);
    wr32(seg + 4, seq); wr32(seg + 8, ack);
    seg[12] = (uint8_t)((hl / 4) << 4);
    seg[13] = flags;
    wr16(seg + 14, wnd);
    if (optlen) memcpy(seg + 20, opts, optlen);
    if (dlen) memcpy(seg + hl, data, dlen);
    uint32_t sum = net_pseudo_sum(PEER, g_cfg.ip, 6, (uint16_t)(hl + dlen));
    wr16(seg + 16, net_checksum(seg, (size_t)hl + dlen, sum));
    return (uint16_t)(hl + dlen);
}

static uint8_t init_opts(uint8_t *opts, int with_ws, uint8_t ws)
{
    opts[0] = 2; opts[1] = 4; wr16(opts + 2, 1460);        /* MSS */
    if (!with_ws) return 4;
    opts[4] = 3; opts[5] = 3; opts[6] = ws; opts[7] = 1;   /* window scale + NOP */
    return 8;
}

/* tcp_connect() blocks until the handshake completes, so the SYN-ACK has to be
 * delivered from inside the net_poll() it calls. Arm this before connecting. */
static int      g_inject;          /* 0 none, 1 = with WS, 2 = without WS */
static uint8_t  g_inj_ws;
static uint16_t g_inj_wnd;
static uint32_t g_inj_isn;

void net_poll(void)
{
    if (!g_inject) return;
    int with_ws = (g_inject == 1); g_inject = 0;
    if (ncap == 0) return;                 /* no SYN captured: nothing to answer */
    const struct cap *syn = &caps[0];
    uint16_t our_port = rd16(syn->seg);    /* the port the client chose */
    uint32_t our_isn = rd32(syn->seg + 4);
    uint8_t opts[8];
    uint8_t optlen = init_opts(opts, with_ws, g_inj_ws);
    uint8_t seg[64];
    uint16_t len = build_seg(seg, 80, our_port, g_inj_isn, our_isn + 1,
                             F_SYN | F_ACK, g_inj_wnd, opts, optlen, NULL, 0);
    uint32_t sum = net_pseudo_sum(PEER, g_cfg.ip, 6, len);
    wr16(seg + 16, 0);
    wr16(seg + 16, net_checksum(seg, len, sum));
    tcp_input(PEER, g_cfg.ip, seg, len);
}

/* ---------------------------------------------------------------- framework */

static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
    printf("  FAIL (line %d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int sock_info(int h, struct tcp_info *ti)
{
    struct tcp_info t[16];
    int n = tcp_list(t, 16);
    for (int i = 0; i < n; i++) if (t[i].sock == h) { *ti = t[i]; return 1; }
    return 0;
}

/* Active open: we are the client. `with_ws` selects the peer's SYN-ACK. */
static void test_active(int with_ws)
{
    printf("active open, peer %s window scaling\n", with_ws ? "with" : "without");
    reset_caps();
    memset(&g_stats, 0, sizeof g_stats);

    g_inject = with_ws ? 1 : 2;
    g_inj_ws = 7; g_inj_wnd = 64240; g_inj_isn = 0x2000;

    int s = tcp_connect(PEER, 80, 1000);
    CHECK(s >= 0, "tcp_connect failed (%d)", s);
    if (s < 0) return;

    /* Our SYN: MSS + a window-scale option, and an unscaled window field. */
    int si = find_cap(F_SYN, F_ACK);
    CHECK(si >= 0, "client sent a SYN");
    unsigned syn_ws = 0, mss = 0;
    if (si >= 0) {
        CHECK(rd16(caps[si].seg + 14) == 65535, "SYN window is unscaled (got %u)", rd16(caps[si].seg + 14));
        CHECK(find_opt(&caps[si], 2, &mss) && mss == 1460, "SYN carries MSS=1460 (got %u)", mss);
        CHECK(find_opt(&caps[si], 3, &syn_ws), "SYN carries a window-scale option");
        CHECK(syn_ws >= 1 && syn_ws <= 14, "offered scale in range (got %u)", syn_ws);
        CHECK(cap_csum_ok(si), "SYN checksum is valid");
    }
    uint32_t our_isn = (si >= 0) ? rd32(caps[si].seg + 4) : 0;

    struct tcp_info ti;
    CHECK(sock_info(s, &ti) && ti.state == TCP_ESTABLISHED, "socket is ESTABLISHED");
    CHECK(ti.ws_ok == (unsigned)(with_ws ? 1 : 0), "ws_ok (got %u)", ti.ws_ok);
    CHECK(ti.snd_wscale == (unsigned)(with_ws ? 7 : 0), "negotiated peer scale (got %u)", ti.snd_wscale);
    CHECK(ti.rcv_shift == (unsigned)(with_ws ? syn_ws : 0), "negotiated local scale (got %u)", ti.rcv_shift);
    CHECK(ti.snd_wnd == 64240, "handshake window is unscaled (got %u)", ti.snd_wnd);

    /* The ACK we sent at handshake completion advertises our receive window. */
    int ai = find_cap(F_ACK, F_SYN);
    CHECK(ai >= 0, "client sent a final ACK");
    if (ai >= 0) {
        unsigned win = rd16(caps[ai].seg + 14);
        if (with_ws)
            CHECK((unsigned)(win << syn_ws) >= 65536u, "advertised window exceeds 64 KiB (%u)", win << syn_ws);
        else
            CHECK(win == 65535, "unscaled window is capped at 65535 (got %u)", win);
        CHECK(cap_csum_ok(ai), "ACK checksum is valid");
    }

    /* A later peer ACK: its window field is interpreted with the peer's scale. */
    {
        uint8_t seg[20];
        uint16_t len = build_seg(seg, 80, (uint16_t)ti.local_port, g_inj_isn + 1, our_isn + 1,
                                 F_ACK, 1000, NULL, 0, NULL, 0);
        tcp_input(PEER, g_cfg.ip, seg, len);
    }
    CHECK(sock_info(s, &ti), "socket still listed");
    CHECK(ti.snd_wnd == (with_ws ? (1000u << 7) : 1000u),
          "peer window scaled by snd_wscale (got %u)", ti.snd_wnd);

    CHECK(g_stats.tcp_ws == (unsigned)(with_ws ? 1 : 0),
          "stats: window-scaled count is %u", g_stats.tcp_ws);

    tcp_abort(s);
}

/* Passive open: we are the server. `with_ws` selects the peer's SYN. */
static void test_passive(int with_ws)
{
    printf("passive open, peer %s window scaling\n", with_ws ? "with" : "without");
    reset_caps();
    memset(&g_stats, 0, sizeof g_stats);

    int l = tcp_listen(1234);
    CHECK(l >= 0, "tcp_listen failed (%d)", l);
    if (l < 0) return;

    const uint32_t peer_isn = 0x4000;

    /* Incoming SYN. */
    uint8_t opts[8];
    uint8_t optlen = init_opts(opts, with_ws, 7);
    uint8_t seg[64];
    uint16_t len = build_seg(seg, 40000, 1234, peer_isn, 0, F_SYN, 64240, opts, optlen, NULL, 0);
    tcp_input(PEER, g_cfg.ip, seg, len);

    int si = find_cap(F_SYN, 0);
    CHECK(si >= 0, "server sent a SYN-ACK");
    unsigned ls = 0;
    if (si >= 0) {
        unsigned mss = 0;
        CHECK((caps[si].seg[13] & F_ACK) != 0, "SYN-ACK has ACK set");
        CHECK(rd32(caps[si].seg + 8) == peer_isn + 1, "SYN-ACK acknowledges the SYN");
        CHECK(find_opt(&caps[si], 2, &mss) && mss == 1460, "SYN-ACK carries MSS=1460");
        int has_ws = find_opt(&caps[si], 3, &ls);
        CHECK(has_ws == (with_ws ? 1 : 0), "SYN-ACK %s a window-scale option",
              with_ws ? "carries" : "omits");
        CHECK(rd16(caps[si].seg + 14) == 65535, "SYN-ACK window is unscaled");
        CHECK(cap_csum_ok(si), "SYN-ACK checksum is valid");
    }
    uint32_t our_isn_v = (si >= 0) ? rd32(caps[si].seg + 4) : 0;

    /* The final ACK completes the handshake; its window is scaled. */
    uint8_t ackseg[24];
    uint16_t alen = build_seg(ackseg, 40000, 1234, peer_isn + 1, our_isn_v + 1, F_ACK, 1000, NULL, 0, NULL, 0);
    tcp_input(PEER, g_cfg.ip, ackseg, alen);

    int s = tcp_accept(l, 100);
    CHECK(s >= 0, "tcp_accept returned a socket (%d)", s);

    struct tcp_info ti;
    CHECK(sock_info(s, &ti) && ti.state == TCP_ESTABLISHED, "child is ESTABLISHED");
    CHECK(ti.snd_wscale == (unsigned)(with_ws ? 7 : 0), "child peer scale (got %u)", ti.snd_wscale);
    CHECK(ti.rcv_shift == (unsigned)(with_ws ? ls : 0), "child local scale (got %u)", ti.rcv_shift);
    CHECK(ti.snd_wnd == (with_ws ? (1000u << 7) : 1000u), "child snd_wnd (got %u)", ti.snd_wnd);

    /* Data arriving shrinks our advertised receive window. */
    static uint8_t payload[10000];
    memset(payload, 0x41, sizeof payload);

    reset_caps();
    uint8_t dseg[20 + sizeof payload];
    uint16_t dl = build_seg(dseg, 40000, 1234, peer_isn + 1, our_isn_v + 1, F_ACK | F_PSH,
                            1000, NULL, 0, payload, (uint16_t)sizeof payload);
    tcp_input(PEER, g_cfg.ip, dseg, dl);
    int w1i = find_cap(F_ACK, 0);
    unsigned w1 = (w1i >= 0) ? rd16(caps[w1i].seg + 14) : 0;

    reset_caps();
    uint8_t dseg2[20 + sizeof payload];
    uint16_t dl2 = build_seg(dseg2, 40000, 1234, peer_isn + 1 + 10000, our_isn_v + 1, F_ACK,
                             1000, NULL, 0, payload, (uint16_t)sizeof payload);
    tcp_input(PEER, g_cfg.ip, dseg2, dl2);
    int w2i = find_cap(F_ACK, 0);
    unsigned w2 = (w2i >= 0) ? rd16(caps[w2i].seg + 14) : 0;

    if (with_ws) {
        unsigned scaled_w1 = (unsigned)(w1 << ls);
        long shrink = (long)((long)w1 - (long)w2) << ls;
        CHECK(scaled_w1 >= 65536u, "receive window exceeds 64 KiB with scaling (got %u)", scaled_w1);
        CHECK(shrink >= 10000L - (1L << ls) && shrink <= 10000L + (1L << ls),
              "window shrank by ~10000 bytes when scaled (got %ld)", shrink);
    } else {
        CHECK(w1 == 65535, "unscaled window is capped (got %u)", w1);
        CHECK(w2 == 65535, "unscaled window stays capped (got %u)", w2);
    }

    if (s >= 0) tcp_abort(s);
    tcp_close(l);
}

int main(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.ip[0] = 10; g_cfg.ip[1] = 0; g_cfg.ip[2] = 2; g_cfg.ip[3] = 15;
    g_cfg.configured = 1;

    printf("tcp_host: window scaling (RFC 7323)\n");
    test_active(1);
    test_active(0);
    test_passive(1);
    test_passive(0);

    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails != 0;
}
