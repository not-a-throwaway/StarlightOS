/*
 * net.c -- Ethernet, ARP, IPv4, ICMP, UDP sockets and the DHCP client.
 */
#include "net.h"
#include "net_priv.h"
#include "rtl8139.h"
#include "kstring.h"
#include "kprintf.h"
#include "ktime.h"
#include "ksched.h"

#define ETH_IP  0x0800
#define ETH_ARP 0x0806
#define FRAME_MAX 1536

static uint8_t tx_frame[FRAME_MAX];
static uint8_t rx_frame[FRAME_MAX];
static struct net_config cfg;
static struct net_stats nstats;
static int nic_ok;
static int in_poll;
static uint16_t ip_ident;
static const uint8_t bcast_mac[6] = { 255, 255, 255, 255, 255, 255 };
static const uint8_t bcast_ip[4]  = { 255, 255, 255, 255 };
static const uint8_t zero_ip[4]   = { 0, 0, 0, 0 };

struct net_stats *net_stats_rw(void) { return &nstats; }
struct net_config *net_config_rw(void) { return &cfg; }
int net_in_poll(void) { return in_poll; }
int net_ready(void) { return nic_ok; }
int net_link_up(void) { return nic_ok && rtl8139_link_up(); }
int net_configured(void) { return cfg.configured; }
const struct net_config *net_get_config(void) { return &cfg; }
const struct net_stats *net_get_stats(void) { return &nstats; }

/* ------------------------------------------------------------ helpers */

int net_parse_ip(const char *s, uint8_t ip[4])
{
    unsigned part = 0, value = 0, digits = 0;
    for (;; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') {
            value = value * 10 + (unsigned)(c - '0');
            if (value > 255 || ++digits > 3) return 0;
        } else if ((c == '.' || c == 0) && digits && part < 4) {
            ip[part++] = (uint8_t)value; value = 0; digits = 0;
            if (!c) return part == 4;
        } else return 0;
    }
}

void net_fmt_ip(char *out, const uint8_t ip[4]) { ksnprintf(out, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]); }
void net_fmt_mac(char *out, const uint8_t m[6]) { ksnprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]); }

uint16_t net_checksum(const void *data, size_t len, uint32_t sum)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 1) { sum += be16(p); p += 2; len -= 2; }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint32_t net_pseudo_sum(const uint8_t src[4], const uint8_t dst[4], uint8_t proto, uint16_t len)
{
    return (uint32_t)be16(src) + be16(src + 2) + be16(dst) + be16(dst + 2) + proto + len;
}

int net_on_link(const uint8_t ip[4])
{
    for (int i = 0; i < 4; i++) if ((ip[i] & cfg.mask[i]) != (cfg.ip[i] & cfg.mask[i])) return 0;
    return 1;
}

/* ---------------------------------------------------------------- ARP */

#define ARP_ENTRIES 16
static struct { uint8_t ip[4], mac[6]; uint32_t stamp; uint8_t valid; } arp_cache[ARP_ENTRIES];

static void arp_learn(const uint8_t ip[4], const uint8_t mac[6])
{
    if (ip_eq(ip, zero_ip)) return;
    int slot = -1, oldest = 0;
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (arp_cache[i].valid && ip_eq(arp_cache[i].ip, ip)) { slot = i; break; }
        if (!arp_cache[i].valid) { if (slot < 0) slot = i; }
        else if (arp_cache[i].stamp < arp_cache[oldest].stamp) oldest = i;
    }
    if (slot < 0) slot = oldest;
    memcpy(arp_cache[slot].ip, ip, 4);
    memcpy(arp_cache[slot].mac, mac, 6);
    arp_cache[slot].stamp = ktime_ms();
    arp_cache[slot].valid = 1;
}

static int arp_lookup(const uint8_t ip[4], uint8_t mac[6])
{
    for (int i = 0; i < ARP_ENTRIES; i++)
        if (arp_cache[i].valid && ip_eq(arp_cache[i].ip, ip)) {
            if ((uint32_t)(ktime_ms() - arp_cache[i].stamp) > 300000u) { arp_cache[i].valid = 0; return 0; }
            memcpy(mac, arp_cache[i].mac, 6);
            return 1;
        }
    return 0;
}

int net_arp_table(struct arp_info *out, int max)
{
    int n = 0;
    for (int i = 0; i < ARP_ENTRIES && n < max; i++) {
        if (!arp_cache[i].valid) continue;
        memcpy(out[n].ip, arp_cache[i].ip, 4);
        memcpy(out[n].mac, arp_cache[i].mac, 6);
        out[n].age_ms = (uint32_t)(ktime_ms() - arp_cache[i].stamp);
        n++;
    }
    return n;
}

static void arp_send(uint16_t op, const uint8_t *dst_mac, const uint8_t *target_mac, const uint8_t target_ip[4])
{
    uint8_t f[42];
    memcpy(f, dst_mac, 6);
    memcpy(f + 6, cfg.mac, 6);
    wr16(f + 12, ETH_ARP);
    uint8_t *a = f + 14;
    wr16(a, 1); wr16(a + 2, ETH_IP); a[4] = 6; a[5] = 4; wr16(a + 6, op);
    memcpy(a + 8, cfg.mac, 6); memcpy(a + 14, cfg.ip, 4);
    memcpy(a + 18, target_mac, 6); memcpy(a + 24, target_ip, 4);
    rtl8139_send(f, 42);
    nstats.arp_tx++;
}

static void arp_input(const uint8_t *f, uint16_t n)
{
    if (n < 42) return;
    const uint8_t *a = f + 14;
    if (be16(a) != 1 || be16(a + 2) != ETH_IP || a[4] != 6 || a[5] != 4) return;
    nstats.arp_rx++;
    uint16_t op = be16(a + 6);
    const uint8_t *smac = a + 8, *sip = a + 14, *tip = a + 24;
    arp_learn(sip, smac);
    if (op == 1 && cfg.configured && ip_eq(tip, cfg.ip))
        arp_send(2, smac, smac, sip);
}

int net_arp_resolve(const uint8_t ip[4], uint8_t mac[6], uint32_t timeout_ms)
{
    if (arp_lookup(ip, mac)) return 1;
    if (!nic_ok || !cfg.configured) return 0;
    static const uint8_t zero_mac[6] = { 0 };
    uint32_t start = ktime_ms();
    uint32_t next_tx = 0;
    int sent = 0;
    while ((uint32_t)(ktime_ms() - start) < timeout_ms) {
        if (!sent || (uint32_t)(ktime_ms() - start) >= next_tx) {
            arp_send(1, bcast_mac, zero_mac, ip);
            sent++; next_tx = (uint32_t)(ktime_ms() - start) + 300;
        }
        net_poll();
        if (arp_lookup(ip, mac)) return 1;
        if (kabort_requested()) return 0;
        kyield();
    }
    return 0;
}

/* ----------------------------------------------------------------- IPv4 */

/* Resolve the L2 destination. Inside the RX path we must never block. */
static int resolve_mac(const uint8_t dst[4], uint8_t mac[6])
{
    if (ip_eq(dst, bcast_ip)) { memcpy(mac, bcast_mac, 6); return 1; }
    /* directed broadcast of our subnet */
    if (cfg.configured) {
        int is_bcast = 1;
        for (int i = 0; i < 4; i++) if ((dst[i] | cfg.mask[i]) != 255) is_bcast = 0;
        if (is_bcast && net_on_link(dst)) { memcpy(mac, bcast_mac, 6); return 1; }
    }
    const uint8_t *hop = dst;
    if (cfg.configured && !net_on_link(dst) && !ip_eq(cfg.gateway, zero_ip)) hop = cfg.gateway;
    if (arp_lookup(hop, mac)) return 1;
    if (in_poll) {                                    /* RX path: ask, but don't wait */
        static const uint8_t zero_mac[6] = { 0 };
        arp_send(1, bcast_mac, zero_mac, hop);
        return 0;
    }
    return net_arp_resolve(hop, mac, 1500);
}

int ip_send_ex(const uint8_t src[4], const uint8_t dst[4], const uint8_t *dst_mac,
               uint8_t proto, uint8_t ttl, const uint8_t *payload, uint16_t len)
{
    if (!nic_ok || len > 1480) return -1;
    uint8_t mac[6];
    if (dst_mac) memcpy(mac, dst_mac, 6);
    else if (!resolve_mac(dst, mac)) return -1;       /* resolve BEFORE touching tx_frame */

    memcpy(tx_frame, mac, 6);
    memcpy(tx_frame + 6, cfg.mac, 6);
    wr16(tx_frame + 12, ETH_IP);
    uint8_t *ip = tx_frame + 14;
    ip[0] = 0x45; ip[1] = 0;
    wr16(ip + 2, (uint16_t)(20 + len));
    wr16(ip + 4, ip_ident++);
    wr16(ip + 6, 0x4000);                             /* DF */
    ip[8] = ttl; ip[9] = proto; wr16(ip + 10, 0);
    memcpy(ip + 12, src, 4); memcpy(ip + 16, dst, 4);
    wr16(ip + 10, net_checksum(ip, 20, 0));
    memcpy(ip + 20, payload, len);
    nstats.ip_tx++;
    return rtl8139_send(tx_frame, (uint16_t)(34 + len));
}

int ip_send(const uint8_t dst[4], uint8_t proto, const uint8_t *payload, uint16_t len)
{
    return ip_send_ex(cfg.ip, dst, NULL, proto, 64, payload, len);
}

/* ----------------------------------------------------------------- ICMP */

static struct {
    volatile int active, done;
    uint16_t id, seq;
    uint64_t sent_us;
    struct ping_result r;
} ping_state;

static void icmp_input(const uint8_t *ip, uint8_t ihl, uint16_t total)
{
    const uint8_t *ic = ip + ihl;
    uint16_t len = (uint16_t)(total - ihl);
    if (len < 8) return;
    if (net_checksum(ic, len, 0) != 0) { nstats.drop_csum++; return; }
    nstats.icmp_rx++;
    uint8_t type = ic[0];

    if (type == 8 && cfg.configured) {                          /* echo request -> reply */
        static uint8_t reply[1480];
        if (len > sizeof(reply)) return;
        memcpy(reply, ic, len);
        reply[0] = 0; wr16(reply + 2, 0);
        wr16(reply + 2, net_checksum(reply, len, 0));
        nstats.icmp_tx++;
        ip_send_ex(cfg.ip, ip + 12, NULL, 1, 64, reply, len);
    } else if (type == 0 && ping_state.active && be16(ic + 4) == ping_state.id && be16(ic + 6) == ping_state.seq) {
        ping_state.r.status = PING_REPLY;
        memcpy(ping_state.r.from, ip + 12, 4);
        ping_state.r.rtt_us = (uint32_t)(ktime_us() - ping_state.sent_us);
        ping_state.r.ttl = ip[8];
        ping_state.r.bytes = (uint16_t)(len - 8);
        ping_state.done = 1;
    } else if ((type == 11 || type == 3) && ping_state.active && len >= 8 + 20 + 8) {
        const uint8_t *inner = ic + 8;                           /* quoted IP header + 8 bytes */
        uint8_t iihl = (uint8_t)((inner[0] & 15) * 4);
        if (inner[9] == 1 && len >= 8u + iihl + 8u) {
            const uint8_t *iic = inner + iihl;
            if (be16(iic + 4) == ping_state.id && be16(iic + 6) == ping_state.seq) {
                ping_state.r.status = type == 11 ? PING_TTL_EXCEEDED : PING_UNREACHABLE;
                memcpy(ping_state.r.from, ip + 12, 4);
                ping_state.r.rtt_us = (uint32_t)(ktime_us() - ping_state.sent_us);
                ping_state.r.ttl = ip[8];
                ping_state.done = 1;
            }
        }
    }
}

int net_ping(const uint8_t dst[4], uint16_t seq, uint8_t ttl, uint16_t payload,
             uint32_t timeout_ms, struct ping_result *res)
{
    memset(res, 0, sizeof(*res));
    res->status = PING_ERROR;
    if (!cfg.configured) return -1;
    if (payload > 1400) payload = 1400;
    uint8_t pkt[8 + 1400];
    pkt[0] = 8; pkt[1] = 0; wr16(pkt + 2, 0);
    wr16(pkt + 4, 0x4D59); wr16(pkt + 6, seq);
    for (uint16_t i = 0; i < payload; i++) pkt[8 + i] = (uint8_t)('a' + i % 23);
    wr16(pkt + 2, net_checksum(pkt, (size_t)(8 + payload), 0));

    /* Resolve first so ARP latency isn't counted in the RTT. */
    uint8_t mac[6];
    if (!resolve_mac(dst, mac)) { res->status = PING_UNREACHABLE; return -1; }

    memset(&ping_state, 0, sizeof(ping_state));
    ping_state.id = 0x4D59; ping_state.seq = seq;
    ping_state.sent_us = ktime_us();
    ping_state.active = 1;
    nstats.icmp_tx++;
    if (ip_send_ex(cfg.ip, dst, mac, 1, ttl, pkt, (uint16_t)(8 + payload)) != 0) { ping_state.active = 0; return -1; }

    uint32_t start = ktime_ms();
    while (!ping_state.done && (uint32_t)(ktime_ms() - start) < timeout_ms && !kabort_requested()) {
        net_poll();
        if (ping_state.done) break;
        kyield();
    }
    ping_state.active = 0;
    if (ping_state.done) { *res = ping_state.r; return res->status == PING_REPLY ? 0 : -1; }
    res->status = PING_TIMEOUT;
    return -1;
}

/* ------------------------------------------------------------------ UDP */

#define UDP_SOCKS 6
#define UDP_Q 4
#define UDP_MAX 600
struct udp_pkt { uint16_t len, port; uint8_t ip[4]; uint8_t data[UDP_MAX]; };
static struct udp_sock_s { uint8_t used; uint16_t port; uint8_t head, count; struct udp_pkt q[UDP_Q]; } udp_socks[UDP_SOCKS];
static uint16_t next_ephemeral = 49200;

uint16_t net_alloc_port(void)
{
    if (next_ephemeral < 49152 || next_ephemeral > 65000) next_ephemeral = 49152;
    return next_ephemeral++;
}

int udp_open(uint16_t port)
{
    for (int i = 0; i < UDP_SOCKS; i++) {
        if (udp_socks[i].used) continue;
        memset(&udp_socks[i], 0, sizeof(udp_socks[i]));
        udp_socks[i].used = 1;
        udp_socks[i].port = port ? port : net_alloc_port();
        return i;
    }
    return -1;
}

void udp_close(int h) { if (h >= 0 && h < UDP_SOCKS) udp_socks[h].used = 0; }

int udp_sendto(int h, const uint8_t ip[4], uint16_t port, const void *data, uint16_t len)
{
    if (h < 0 || h >= UDP_SOCKS || !udp_socks[h].used || len > 1400) return -1;
    uint8_t d[8 + 1400];
    uint16_t sport = udp_socks[h].port;
    wr16(d, sport); wr16(d + 2, port); wr16(d + 4, (uint16_t)(8 + len)); wr16(d + 6, 0);
    memcpy(d + 8, data, len);
    const uint8_t *src = cfg.configured ? cfg.ip : zero_ip;
    /* UDP checksum over pseudo header (optional in IPv4, but nice to have). */
    uint32_t sum = net_pseudo_sum(src, ip, 17, (uint16_t)(8 + len));
    uint16_t ck = net_checksum(d, (size_t)(8 + len), sum);
    wr16(d + 6, ck ? ck : 0xFFFF);
    nstats.udp_tx++;
    const uint8_t *mac = ip_eq(ip, bcast_ip) ? bcast_mac : NULL;
    return ip_send_ex(src, ip, mac, 17, 64, d, (uint16_t)(8 + len));
}

static void udp_input(const uint8_t *ip, uint8_t ihl, uint16_t total)
{
    const uint8_t *u = ip + ihl;
    uint16_t len = (uint16_t)(total - ihl);
    if (len < 8) return;
    uint16_t ulen = be16(u + 4);
    if (ulen < 8 || ulen > len) return;
    if (be16(u + 6) != 0) {
        uint32_t sum = net_pseudo_sum(ip + 12, ip + 16, 17, ulen);
        if (net_checksum(u, ulen, sum) != 0) { nstats.drop_csum++; return; }
    }
    nstats.udp_rx++;
    uint16_t dport = be16(u + 2);
    for (int i = 0; i < UDP_SOCKS; i++) {
        struct udp_sock_s *s = &udp_socks[i];
        if (!s->used || s->port != dport) continue;
        if (s->count == UDP_Q) return;                           /* queue full: drop */
        struct udp_pkt *p = &s->q[(s->head + s->count) % UDP_Q];
        uint16_t n = (uint16_t)(ulen - 8);
        if (n > UDP_MAX) n = UDP_MAX;
        p->len = n; p->port = be16(u);
        memcpy(p->ip, ip + 12, 4);
        memcpy(p->data, u + 8, n);
        s->count++;
        return;
    }
    /* No listener: a real stack would answer ICMP port-unreachable. */
    nstats.drop_other++;
}

int udp_recvfrom(int h, void *buf, uint16_t cap, uint8_t from_ip[4], uint16_t *from_port, uint32_t timeout_ms)
{
    if (h < 0 || h >= UDP_SOCKS || !udp_socks[h].used) return -1;
    struct udp_sock_s *s = &udp_socks[h];
    uint32_t start = ktime_ms();
    for (;;) {
        if (s->count) {
            struct udp_pkt *p = &s->q[s->head];
            uint16_t n = p->len < cap ? p->len : cap;
            memcpy(buf, p->data, n);
            if (from_ip) memcpy(from_ip, p->ip, 4);
            if (from_port) *from_port = p->port;
            s->head = (uint8_t)((s->head + 1) % UDP_Q);
            s->count--;
            return n;
        }
        if ((uint32_t)(ktime_ms() - start) >= timeout_ms || kabort_requested()) return -1;
        net_poll();
        if (!s->count) kyield();
    }
}

/* ---------------------------------------------------------------- DHCP */

enum { DHCP_IDLE, DHCP_DISCOVER, DHCP_REQUEST, DHCP_BOUND, DHCP_FAILED };
static int dhcp_state = DHCP_IDLE;
static int dhcp_sock = -1;
static uint32_t dhcp_xid, dhcp_deadline, dhcp_tries;
static uint8_t offer_ip[4], offer_server[4];

static unsigned dhcp_build(uint8_t *m, uint8_t msg_type, int with_request)
{
    memset(m, 0, 320);
    m[0] = 1; m[1] = 1; m[2] = 6;
    wr32(m + 4, dhcp_xid);
    wr16(m + 10, 0x8000);                                        /* ask for broadcast replies */
    memcpy(m + 28, cfg.mac, 6);
    m[236] = 99; m[237] = 130; m[238] = 83; m[239] = 99;
    unsigned o = 240;
    m[o++] = 53; m[o++] = 1; m[o++] = msg_type;
    m[o++] = 61; m[o++] = 7; m[o++] = 1; memcpy(m + o, cfg.mac, 6); o += 6;
    unsigned hl = (unsigned)strlen(cfg.hostname);
    if (hl) { m[o++] = 12; m[o++] = (uint8_t)hl; memcpy(m + o, cfg.hostname, hl); o += hl; }
    if (with_request) {
        m[o++] = 50; m[o++] = 4; memcpy(m + o, offer_ip, 4); o += 4;
        m[o++] = 54; m[o++] = 4; memcpy(m + o, offer_server, 4); o += 4;
    }
    m[o++] = 55; m[o++] = 8;
    m[o++] = 1; m[o++] = 3; m[o++] = 6; m[o++] = 15; m[o++] = 42; m[o++] = 51; m[o++] = 54; m[o++] = 12;
    m[o++] = 255;
    return o < 300 ? 300 : o;
}

static const uint8_t *dhcp_opt(const uint8_t *m, unsigned n, uint8_t want, unsigned *len)
{
    unsigned i = 240;
    while (i < n) {
        uint8_t tag = m[i++];
        if (tag == 255) break;
        if (tag == 0) continue;
        if (i >= n) break;
        uint8_t l = m[i++];
        if (i + l > n) break;
        if (tag == want) { *len = l; return m + i; }
        i += l;
    }
    return NULL;
}

static void dhcp_send_discover(void)
{
    uint8_t m[320];
    unsigned n = dhcp_build(m, 1, 0);
    udp_sendto(dhcp_sock, bcast_ip, 67, m, (uint16_t)n);
}

static void dhcp_send_request(void)
{
    uint8_t m[320];
    unsigned n = dhcp_build(m, 3, 1);
    udp_sendto(dhcp_sock, bcast_ip, 67, m, (uint16_t)n);
}

static void dhcp_begin(void)
{
    if (dhcp_sock < 0) dhcp_sock = udp_open(68);
    if (dhcp_sock < 0) return;
    dhcp_xid = (uint32_t)ktime_us() ^ 0x4D594F53u;
    dhcp_state = DHCP_DISCOVER;
    dhcp_tries = 0;
    dhcp_deadline = ktime_ms();                                  /* send immediately */
}

void dhcp_poll(void)
{
    if (!nic_ok || dhcp_sock < 0) return;
    uint8_t buf[600];
    uint8_t from[4]; uint16_t fport;
    int n;
    while ((n = udp_recvfrom(dhcp_sock, buf, sizeof(buf), from, &fport, 0)) > 0) {
        if (n < 241 || buf[0] != 2 || be32(buf + 4) != dhcp_xid || memcmp(buf + 28, cfg.mac, 6) != 0) continue;
        unsigned l;
        const uint8_t *t = dhcp_opt(buf, (unsigned)n, 53, &l);
        if (!t || l != 1) continue;
        if (*t == 2 && dhcp_state == DHCP_DISCOVER) {            /* OFFER */
            const uint8_t *sid = dhcp_opt(buf, (unsigned)n, 54, &l);
            if (sid && l == 4) memcpy(offer_server, sid, 4); else memcpy(offer_server, from, 4);
            memcpy(offer_ip, buf + 16, 4);
            dhcp_state = DHCP_REQUEST; dhcp_tries = 0; dhcp_deadline = ktime_ms();
        } else if (*t == 5 && dhcp_state == DHCP_REQUEST) {      /* ACK */
            memcpy(cfg.ip, buf + 16, 4);
            memcpy(cfg.dhcp_server, offer_server, 4);
            const uint8_t *o;
            if ((o = dhcp_opt(buf, (unsigned)n, 1, &l)) && l == 4) memcpy(cfg.mask, o, 4); else { memset(cfg.mask, 255, 3); cfg.mask[3] = 0; }
            if ((o = dhcp_opt(buf, (unsigned)n, 3, &l)) && l >= 4) memcpy(cfg.gateway, o, 4); else memcpy(cfg.gateway, offer_server, 4);
            cfg.dns_count = 0;
            if ((o = dhcp_opt(buf, (unsigned)n, 6, &l)) && l >= 4) {
                for (unsigned i = 0; i + 4 <= l && cfg.dns_count < 2; i += 4) memcpy(cfg.dns[cfg.dns_count++], o + i, 4);
            }
            if (!cfg.dns_count) { memcpy(cfg.dns[0], cfg.gateway, 4); cfg.dns_count = 1; }
            cfg.has_ntp = 0;
            if ((o = dhcp_opt(buf, (unsigned)n, 42, &l)) && l >= 4) { memcpy(cfg.ntp, o, 4); cfg.has_ntp = 1; }
            cfg.domain[0] = 0;
            if ((o = dhcp_opt(buf, (unsigned)n, 15, &l)) && l) { unsigned c = l < sizeof(cfg.domain) - 1 ? l : (unsigned)sizeof(cfg.domain) - 1; memcpy(cfg.domain, o, c); cfg.domain[c] = 0; }
            cfg.lease_secs = 0;
            if ((o = dhcp_opt(buf, (unsigned)n, 51, &l)) && l == 4) cfg.lease_secs = be32(o);
            cfg.lease_start_ms = ktime_ms();
            cfg.configured = 1;
            dhcp_state = DHCP_BOUND;
        } else if (*t == 6 && dhcp_state == DHCP_REQUEST) {      /* NAK */
            dhcp_begin();
        }
    }

    uint32_t now = ktime_ms();
    if (dhcp_state == DHCP_DISCOVER || dhcp_state == DHCP_REQUEST) {
        if ((int32_t)(now - dhcp_deadline) >= 0) {
            if (dhcp_tries >= 6) { dhcp_state = DHCP_FAILED; return; }
            if (dhcp_state == DHCP_DISCOVER) dhcp_send_discover(); else dhcp_send_request();
            dhcp_tries++;
            dhcp_deadline = now + 1000u * (dhcp_tries < 4 ? dhcp_tries : 4);
        }
    } else if (dhcp_state == DHCP_BOUND && cfg.lease_secs && cfg.lease_secs != 0xFFFFFFFFu &&
               (uint32_t)(now - cfg.lease_start_ms) / 1000u > cfg.lease_secs / 2u) {
        dhcp_begin();                                             /* renew by re-requesting */
    }
}

int net_dhcp_restart(void)
{
    if (!nic_ok) return -1;
    cfg.configured = 0;
    memset(cfg.ip, 0, 4);
    dhcp_begin();
    return 0;
}

int net_wait_configured(uint32_t timeout_ms)
{
    uint32_t start = ktime_ms();
    while (!cfg.configured) {
        if ((uint32_t)(ktime_ms() - start) >= timeout_ms || kabort_requested() || dhcp_state == DHCP_FAILED) break;
        net_poll();
        if (!cfg.configured) kyield();
    }
    return cfg.configured;
}

void net_set_static(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4], const uint8_t dns[4])
{
    memcpy(cfg.ip, ip, 4); memcpy(cfg.mask, mask, 4); memcpy(cfg.gateway, gw, 4);
    if (dns) { memcpy(cfg.dns[0], dns, 4); cfg.dns_count = 1; }
    cfg.lease_secs = 0; cfg.configured = 1; dhcp_state = DHCP_IDLE;
}

void net_set_dns(const uint8_t dns[4]) { memcpy(cfg.dns[0], dns, 4); if (cfg.dns_count < 1) cfg.dns_count = 1; }

void net_set_hostname(const char *name) { strlcpy(cfg.hostname, name, sizeof(cfg.hostname)); }

/* ------------------------------------------------------------ RX / poll */

static void ip_input(const uint8_t *f, uint16_t n)
{
    if (n < 34) return;
    const uint8_t *ip = f + 14;
    if ((ip[0] >> 4) != 4) return;
    uint8_t ihl = (uint8_t)((ip[0] & 15) * 4);
    uint16_t total = be16(ip + 2);
    if (ihl < 20 || total < ihl || total > n - 14) return;
    if (net_checksum(ip, ihl, 0) != 0) { nstats.drop_csum++; return; }
    if (be16(ip + 6) & 0x3FFF) { nstats.drop_other++; return; }  /* fragments unsupported */
    nstats.ip_rx++;

    const uint8_t *dst = ip + 16;
    uint8_t proto = ip[9];
    int for_us = cfg.configured && ip_eq(dst, cfg.ip);
    int broadcast = ip_eq(dst, bcast_ip);
    if (cfg.configured && !for_us && !broadcast) {
        int dbc = 1;
        for (int i = 0; i < 4; i++) if ((dst[i] | cfg.mask[i]) != 255) dbc = 0;
        broadcast = dbc && net_on_link(dst);
    }
    /* Before we own an address we still need DHCP replies (UDP :68). */
    int dhcp_reply = proto == 17 && total >= ihl + 8u && be16(ip + ihl + 2) == 68;
    if (!for_us && !broadcast && !dhcp_reply) return;

    if (net_on_link(ip + 12) && cfg.configured && !ip_eq(ip + 12, zero_ip)) arp_learn(ip + 12, f + 6);

    switch (proto) {
    case 1:  icmp_input(ip, ihl, total); break;
    case 17: udp_input(ip, ihl, total); break;
    case 6:
        if (for_us) { nstats.tcp_rx++; tcp_input(ip + 12, ip + 16, ip + ihl, (uint16_t)(total - ihl)); }
        break;
    default: break;
    }
}

void net_poll(void)
{
    if (!nic_ok || in_poll) return;
    in_poll = 1;
    for (int i = 0; i < 48; i++) {
        uint16_t n = 0;
        if (!rtl8139_receive(rx_frame, sizeof(rx_frame), &n)) { if (n == 0) break; continue; }
        if (n < 14) continue;
        uint16_t type = be16(rx_frame + 12);
        if (type == ETH_ARP) arp_input(rx_frame, n);
        else if (type == ETH_IP) ip_input(rx_frame, n);
    }
    dhcp_poll();
    tcp_timers();
    in_poll = 0;
}

static void net_background(void) { net_poll(); }

int net_initialize(void)
{
    memset(&cfg, 0, sizeof(cfg));
    memset(&nstats, 0, sizeof(nstats));
    strlcpy(cfg.hostname, "starlight", sizeof(cfg.hostname));
    nic_ok = (rtl8139_initialize() == 0);
    if (!nic_ok) return -1;
    memcpy(cfg.mac, rtl8139_mac_address(), 6);
    ksched_register(net_background);
    dhcp_begin();                                                 /* non-blocking */
    return 0;
}
