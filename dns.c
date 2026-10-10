/*
 * dns.c -- stub resolver (A records), CNAME following, TTL cache, NTP client.
 */
#include "net.h"
#include "net_priv.h"
#include "kstring.h"
#include "ktime.h"
#include "ksched.h"

#define CACHE_N 16
static struct { char name[48]; uint8_t ip[4]; uint32_t expires_ms; uint8_t valid; } cache[CACHE_N];
static uint16_t dns_id = 0x1000;

void dns_cache_flush(void) { memset(cache, 0, sizeof(cache)); }

int dns_cache_list(char names[][48], uint8_t ips[][4], uint32_t *ttl_left, int max)
{
    int n = 0;
    uint32_t now = ktime_ms();
    for (int i = 0; i < CACHE_N && n < max; i++) {
        if (!cache[i].valid || (int32_t)(now - cache[i].expires_ms) >= 0) continue;
        strlcpy(names[n], cache[i].name, 48);
        memcpy(ips[n], cache[i].ip, 4);
        ttl_left[n] = (cache[i].expires_ms - now) / 1000u;
        n++;
    }
    return n;
}

static void cache_put(const char *name, const uint8_t ip[4], uint32_t ttl)
{
    if (ttl < 10) ttl = 10;
    if (ttl > 3600) ttl = 3600;
    int slot = 0;
    for (int i = 0; i < CACHE_N; i++) {
        if (cache[i].valid && strcasecmp(cache[i].name, name) == 0) { slot = i; goto fill; }
        if (!cache[i].valid) { slot = i; goto fill; }
        if (cache[i].expires_ms < cache[slot].expires_ms) slot = i;
    }
fill:
    strlcpy(cache[slot].name, name, sizeof(cache[slot].name));
    memcpy(cache[slot].ip, ip, 4);
    cache[slot].expires_ms = ktime_ms() + ttl * 1000u;
    cache[slot].valid = 1;
}

static int cache_get(const char *name, uint8_t ip[4])
{
    uint32_t now = ktime_ms();
    for (int i = 0; i < CACHE_N; i++)
        if (cache[i].valid && strcasecmp(cache[i].name, name) == 0) {
            if ((int32_t)(now - cache[i].expires_ms) >= 0) { cache[i].valid = 0; return 0; }
            memcpy(ip, cache[i].ip, 4);
            return 1;
        }
    return 0;
}

/* Reads a (possibly compressed) name starting at *pos into out; advances *pos past it in the original stream. */
static int read_name(const uint8_t *m, unsigned n, unsigned *pos, char *out, unsigned cap)
{
    unsigned p = *pos, o = 0, jumps = 0;
    int jumped = 0;
    for (;;) {
        if (p >= n) return -1;
        uint8_t l = m[p];
        if ((l & 0xC0) == 0xC0) {
            if (p + 1 >= n || ++jumps > 12) return -1;
            if (!jumped) *pos = p + 2;
            jumped = 1;
            p = (unsigned)(((l & 0x3F) << 8) | m[p + 1]);
            continue;
        }
        p++;
        if (l == 0) break;
        if (p + l > n) return -1;
        if (o && o + 1 < cap) out[o++] = '.';
        for (unsigned i = 0; i < l; i++) if (o + 1 < cap) out[o++] = (char)m[p + i];
        p += l;
    }
    if (!jumped) *pos = p;
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return 0;
}

static int encode_name(uint8_t *q, unsigned cap, const char *host)
{
    unsigned o = 0, i = 0;
    while (host[i]) {
        unsigned start = i;
        while (host[i] && host[i] != '.') i++;
        unsigned len = i - start;
        if (!len || len > 63 || o + len + 2 > cap) return -1;
        q[o++] = (uint8_t)len;
        memcpy(q + o, host + start, len); o += len;
        if (host[i] == '.') i++;
    }
    if (o + 1 > cap) return -1;
    q[o++] = 0;
    return (int)o;
}

static int query_server(const uint8_t server[4], uint16_t port, const char *name, struct dns_result *res, uint32_t timeout_ms)
{
    uint8_t q[300];
    uint16_t id = dns_id++;
    memset(q, 0, 12);
    wr16(q, id); wr16(q + 2, 0x0100); wr16(q + 4, 1);
    int nl = encode_name(q + 12, sizeof(q) - 16, name);
    if (nl < 0) return -2;
    unsigned qn = 12 + (unsigned)nl;
    wr16(q + qn, 1); wr16(q + qn + 2, 1); qn += 4;

    int h = udp_open(0);
    if (h < 0) return -2;
    uint64_t t0 = ktime_us();
    if (udp_sendto(h, server, port, q, (uint16_t)qn) != 0) { udp_close(h); return -2; }

    uint8_t r[560];
    uint32_t start = ktime_ms();
    int rc = -1;
    for (;;) {
        uint32_t el = (uint32_t)(ktime_ms() - start);
        if (el >= timeout_ms) break;
        uint8_t from[4]; uint16_t fp;
        int n = udp_recvfrom(h, r, sizeof(r), from, &fp, timeout_ms - el);
        if (n < 12) break;
        if (be16(r) != id || !(r[2] & 0x80)) continue;
        res->rtt_us = (uint32_t)(ktime_us() - t0);
        res->rcode = r[3] & 15;
        memcpy(res->server, server, 4);
        unsigned at = 12, qd = be16(r + 4), an = be16(r + 6);
        char tmp[64];
        for (unsigned z = 0; z < qd; z++) { if (read_name(r, (unsigned)n, &at, tmp, sizeof(tmp)) < 0) { rc = -2; goto done; } at += 4; }
        for (unsigned z = 0; z < an && at < (unsigned)n; z++) {
            if (read_name(r, (unsigned)n, &at, tmp, sizeof(tmp)) < 0) break;
            if (at + 10 > (unsigned)n) break;
            uint16_t type = be16(r + at), rdlen = be16(r + at + 8);
            uint32_t ttl = be32(r + at + 4);
            at += 10;
            if (at + rdlen > (unsigned)n) break;
            if (type == 1 && rdlen == 4 && res->addr_count < DNS_MAX_ADDRS) {
                memcpy(res->addrs[res->addr_count++], r + at, 4);
                if (!res->ttl || ttl < res->ttl) res->ttl = ttl;
            } else if (type == 5) {
                unsigned p = at;
                read_name(r, (unsigned)n, &p, res->cname, sizeof(res->cname));
            }
            at += rdlen;
        }
        rc = res->addr_count > 0 ? 0 : (res->rcode ? -3 : -4);
        break;
    }
done:
    udp_close(h);
    return rc;
}

int dns_query(const char *name, struct dns_result *res)
{
    memset(res, 0, sizeof(*res));
    res->rcode = -1;
    const struct net_config *c = net_get_config();
    if (!c->configured || c->dns_count == 0) return -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        for (int s = 0; s < c->dns_count; s++) {
            int r = query_server(c->dns[s], 53, name, res, attempt == 0 ? 1200u : 2500u);
            if (r == 0) return 0;
            if (r == -3 && res->rcode == 3) return -3;           /* NXDOMAIN is authoritative */
            if (kabort_requested()) return -1;
        }
    }
    return -1;
}

int dns_query_server(const uint8_t server[4], uint16_t port, const char *name, struct dns_result *res)
{
    memset(res, 0, sizeof(*res));
    res->rcode = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        int r = query_server(server, port, name, res, 1500);
        if (r == 0 || r == -3) return r;
    }
    return -1;
}

int dns_resolve(const char *host, uint8_t ip[4])
{
    if (net_parse_ip(host, ip)) return 0;
    if (strcasecmp(host, "localhost") == 0) { ip[0] = 127; ip[1] = 0; ip[2] = 0; ip[3] = 1; return 0; }
    if (cache_get(host, ip)) return 0;
    struct dns_result r;
    if (dns_query(host, &r) != 0) return -1;
    memcpy(ip, r.addrs[0], 4);
    cache_put(host, ip, r.ttl);
    return 0;
}

/* ------------------------------------------------------------------ NTP */

int ntp_query(const char *server_spec, struct ntp_result *res)
{
    uint8_t ip[4];
    char host[64]; uint16_t port = 123;
    strlcpy(host, server_spec, sizeof(host));
    char *colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = (uint16_t)kstrtoul(colon + 1, NULL, 10); }
    if (dns_resolve(host, ip) != 0) return -1;
    memcpy(res->server_ip, ip, 4);
    int h = udp_open(0);
    if (h < 0) return -2;
    uint8_t p[48];
    memset(p, 0, sizeof(p));
    p[0] = 0x23;                                              /* LI=0, VN=4, mode=3 (client) */
    uint32_t t_local = ktime_unix();
    wr32(p + 40, t_local + 2208988800u);                       /* transmit timestamp: lets us match the reply */
    uint64_t t0 = ktime_us();
    int result = -3;
    for (int attempt = 0; attempt < 3 && result != 0; attempt++) {
        if (udp_sendto(h, ip, port, p, sizeof(p)) != 0) { result = -2; break; }
        uint8_t r[96]; uint8_t fr[4]; uint16_t fp;
        int n = udp_recvfrom(h, r, sizeof(r), fr, &fp, 1500);
        if (n < 48 || (r[0] & 7) != 4 || r[1] == 0) { if (kabort_requested()) break; continue; }   /* mode 4 server, stratum!=0 (not KoD) */
        uint32_t tx = be32(r + 40);
        if (tx < 2208988800u) continue;
        uint32_t rtt_ms = (uint32_t)((ktime_us() - t0) / 1000u);
        res->unix_secs = tx - 2208988800u;
        res->rtt_ms = rtt_ms;
        res->stratum = r[1];
        /* server time at the moment of reply ~= tx + rtt/2 */
        int32_t offset = (int32_t)((int64_t)res->unix_secs * 1000 + rtt_ms / 2 - (int64_t)ktime_unix() * 1000);
        res->offset_ms = offset;
        result = 0;
    }
    udp_close(h);
    return result;
}
