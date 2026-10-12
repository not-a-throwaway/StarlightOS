/*
 * cmd_net.c -- network shell commands.
 *   ip  ping  traceroute  nslookup  arp  netstat  ntpdate  fetch  nc  telnet  httpd
 */
#include "version.h"
#include "shell.h"
#include "net.h"
#include "vfs.h"
#include "terminal.h"
#include "kstring.h"
#include "kprintf.h"
#include "kheap.h"
#include "ktime.h"
#include "ksched.h"
#include "keyboard.h"
#include "rtl8139.h"
#include "tls.h"
#include "telnet.h"

static int ip_eq_(const uint8_t *a, const uint8_t *b) { return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3]; }
enum { C_LGRAY = 7, C_DGRAY = 8, C_LGREEN = 10, C_LCYAN = 11, C_LRED = 12, C_YELLOW = 14, C_WHITE = 15 };

static void fg(int c) { terminal_set_color((uint8_t)c, (uint8_t)(terminal_get_color() >> 4)); }
static void fg_reset(void) { fg(C_LGRAY); }
static void err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void err(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fg(C_LRED); kvprintf_err(fmt, ap); fg_reset();
    va_end(ap);
}

static const char *ipstr(const uint8_t ip[4]) { static char b[4][16]; static int i; i = (i + 1) & 3; net_fmt_ip(b[i], ip); return b[i]; }

/* Make sure we have an address, waiting for DHCP if needed. */
static int need_net(void)
{
    if (!net_ready()) { err("network: no RTL8139 adapter found\n"); return 0; }
    if (net_configured()) return 1;
    if (!net_link_up()) { err("network: link is down (no cable / virtual NIC disconnected)\n"); return 0; }
    kprintf("Waiting for DHCP lease... ");
    if (!net_wait_configured(12000)) { err("failed.\n"); err("hint: use 'ip set IP MASK GATEWAY DNS' for a static address, or 'ip dhcp' to retry.\n"); return 0; }
    kprintf("%s\n", ipstr(net_get_config()->ip));
    return 1;
}

static int resolve_or_complain(const char *host, uint8_t ip[4])
{
    if (dns_resolve(host, ip) == 0) return 0;
    if (kabort_requested()) return -1;
    err("cannot resolve '%s' (check 'nslookup %s' and 'ip')\n", host, host);
    return -1;
}

/* ---------------------------------------------------------------- ip */
static int cmd_ip(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "dhcp")) {
        if (!net_ready()) { err("no network adapter\n"); return 1; }
        net_dhcp_restart();
        kprintf("Requesting a new lease... ");
        if (net_wait_configured(12000)) kprintf("%s\n", ipstr(net_get_config()->ip)); else { err("no answer\n"); return 1; }
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "set")) {
        uint8_t ip[4], mask[4], gw[4], dns[4];
        if (!net_parse_ip(argv[2], ip) || !net_parse_ip(argv[3], mask) || !net_parse_ip(argv[4], gw)) { err("usage: ip set IP MASK GATEWAY [DNS]\n"); return 1; }
        if (argc >= 6) { if (!net_parse_ip(argv[5], dns)) { err("bad DNS address\n"); return 1; } }
        else memcpy(dns, gw, 4);
        net_set_static(ip, mask, gw, dns);
        kprintf("Static address configured.\n");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "dns")) {
        uint8_t d[4];
        if (!net_parse_ip(argv[2], d)) { err("usage: ip dns ADDRESS\n"); return 1; }
        net_set_dns(d); dns_cache_flush();
        return 0;
    }
    if (!net_ready()) { err("no network adapter (RTL8139 not found)\n"); return 1; }
    const struct net_config *c = net_get_config();
    char mac[18]; net_fmt_mac(mac, c->mac);
    fg(C_LCYAN); kprintf("eth0"); fg_reset();
    kprintf("  RTL8139  link %s\n", net_link_up() ? "up" : "DOWN");
    kprintf("  MAC      %s\n", mac);
    if (c->configured) {
        int bits = 0; for (int i = 0; i < 4; i++) for (int b = 7; b >= 0; b--) if (c->mask[i] & (1 << b)) bits++;
        kprintf("  inet     %s/%d  mask %s\n", ipstr(c->ip), bits, ipstr(c->mask));
        kprintf("  gateway  %s\n", ipstr(c->gateway));
        for (int i = 0; i < c->dns_count; i++) kprintf("  dns      %s\n", ipstr(c->dns[i]));
        if (c->has_ntp) kprintf("  ntp      %s\n", ipstr(c->ntp));
        if (c->domain[0]) kprintf("  domain   %s\n", c->domain);
        if (c->lease_secs) {
            uint32_t el = (ktime_ms() - c->lease_start_ms) / 1000u;
            kprintf("  lease    %us (%us used), server %s\n", c->lease_secs, el, ipstr(c->dhcp_server));
        }
    } else { fg(C_YELLOW); kprintf("  no IPv4 address yet (DHCP in progress; 'ip dhcp' to retry)\n"); fg_reset(); }
    const struct rtl8139_stats *s = rtl8139_get_stats();
    kprintf("  RX       %u frames, %u bytes, %u errors\n", s->rx_frames, s->rx_bytes, s->rx_errors);
    kprintf("  TX       %u frames, %u bytes, %u errors\n", s->tx_frames, s->tx_bytes, s->tx_errors);
    return 0;
}

/* -------------------------------------------------------------- ping */
static int cmd_ping(int argc, char **argv)
{
    int count = 4, size = 32, ttl = 64; uint32_t wait = 1000;
    const char *host = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) count = (int)kstrtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) size = (int)kstrtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) ttl = (int)kstrtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-W") && i + 1 < argc) wait = kstrtoul(argv[++i], NULL, 10);
        else if (argv[i][0] != '-') host = argv[i];
    }
    if (!host) { err("usage: ping [-c COUNT] [-s SIZE] [-t TTL] [-W MS] HOST\n"); return 1; }
    if (!need_net()) return 1;
    uint8_t ip[4];
    if (resolve_or_complain(host, ip)) return 1;
    kprintf("PING %s (%s) %d data bytes\n", host, ipstr(ip), size);
    int sent = 0, recv = 0; uint32_t mn = 0xFFFFFFFFu, mx = 0; uint64_t sum = 0;
    for (int i = 0; i < count && !kabort_requested(); i++) {
        struct ping_result r;
        sent++;
        net_ping(ip, (uint16_t)(i + 1), (uint8_t)ttl, (uint16_t)size, wait, &r);
        if (r.status == PING_REPLY) {
            recv++; if (r.rtt_us < mn) mn = r.rtt_us; if (r.rtt_us > mx) mx = r.rtt_us; sum += r.rtt_us;
            kprintf("%u bytes from %s: seq=%d ttl=%u time=%u.%02u ms\n", r.bytes + 8u, ipstr(r.from), i + 1, r.ttl, r.rtt_us / 1000u, (r.rtt_us % 1000u) / 10u);
        } else if (r.status == PING_TTL_EXCEEDED) kprintf("From %s: seq=%d time to live exceeded\n", ipstr(r.from), i + 1);
        else if (r.status == PING_UNREACHABLE) kprintf("From %s: seq=%d destination unreachable\n", ipstr(r.from[0] ? r.from : ip), i + 1);
        else kprintf("Request timeout for seq=%d\n", i + 1);
        if (i + 1 < count) ksleep_ms(r.status == PING_REPLY ? 700 : 0);
    }
    kprintf("--- %s ping statistics ---\n%d sent, %d received, %d%% loss", host, sent, recv, sent ? (sent - recv) * 100 / sent : 0);
    if (recv) kprintf("   rtt min/avg/max = %u.%02u/%u.%02u/%u.%02u ms", mn / 1000u, mn % 1000u / 10u, (unsigned)(sum / (uint64_t)recv) / 1000u, (unsigned)(sum / (uint64_t)recv) % 1000u / 10u, mx / 1000u, mx % 1000u / 10u);
    kprintf("\n");
    return recv ? 0 : 1;
}

static int cmd_traceroute(int argc, char **argv)
{
    if (argc < 2) { err("usage: traceroute HOST\n"); return 1; }
    if (!need_net()) return 1;
    uint8_t ip[4];
    if (resolve_or_complain(argv[1], ip)) return 1;
    kprintf("traceroute to %s (%s), 20 hops max\n", argv[1], ipstr(ip));
    for (int ttl = 1; ttl <= 20 && !kabort_requested(); ttl++) {
        kprintf("%2d  ", ttl);
        uint8_t last[4] = { 0 }; int reached = 0, got = 0;
        for (int p = 0; p < 3; p++) {
            struct ping_result r;
            net_ping(ip, (uint16_t)(ttl * 4 + p), (uint8_t)ttl, 32, 1500, &r);
            if (r.status == PING_REPLY || r.status == PING_TTL_EXCEEDED || r.status == PING_UNREACHABLE) {
                if (!got || !ip_eq_(last, r.from)) { kprintf("%s  ", ipstr(r.from)); memcpy(last, r.from, 4); }
                kprintf("%u.%02u ms  ", r.rtt_us / 1000u, r.rtt_us % 1000u / 10u);
                got = 1; if (r.status == PING_REPLY) reached = 1;
            } else kprintf("*  ");
        }
        kprintf("\n");
        if (reached) break;
    }
    return 0;
}

/* ---------------------------------------------------------- nslookup */
static int cmd_nslookup(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--flush")) { dns_cache_flush(); kprintf("DNS cache cleared.\n"); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "--cache")) {
        static char names[16][48]; static uint8_t ips[16][4]; static uint32_t ttl[16];
        int n = dns_cache_list(names, ips, ttl, 16);
        if (!n) kprintf("(cache empty)\n");
        for (int i = 0; i < n; i++) kprintf("%-30s %-16s ttl %us\n", names[i], ipstr(ips[i]), ttl[i]);
        return 0;
    }
    if (argc < 2) { err("usage: nslookup NAME [SERVER[:PORT]] | --cache | --flush\n"); return 1; }
    if (!need_net()) return 1;
    struct dns_result r; int rc;
    uint8_t srv[4];
    if (argc >= 3) {
        char spec[40]; uint16_t port = 53;
        strlcpy(spec, argv[2], sizeof(spec));
        char *colon = strchr(spec, ':');
        if (colon) { *colon = 0; port = (uint16_t)kstrtoul(colon + 1, NULL, 10); }
        if (!net_parse_ip(spec, srv)) { err("SERVER must be an IPv4 address (optionally :PORT)\n"); return 1; }
        rc = dns_query_server(srv, port, argv[1], &r);
    } else rc = dns_query(argv[1], &r);
    if (r.rcode >= 0) kprintf("Server:  %s   (%u.%03u ms)\n", ipstr(r.server), r.rtt_us / 1000u, r.rtt_us % 1000u);
    if (rc == 0) {
        if (r.cname[0]) kprintf("%s is an alias for %s\n", argv[1], r.cname);
        for (int i = 0; i < r.addr_count; i++) kprintf("Address: %s\n", ipstr(r.addrs[i]));
        kprintf("TTL: %us\n", r.ttl);
        return 0;
    }
    if (r.rcode == 3) err("** '%s' not found (NXDOMAIN)\n", argv[1]);
    else if (r.rcode > 0) err("** server returned error code %d\n", r.rcode);
    else if (r.rcode == 0) err("** '%s' has no IPv4 address\n", argv[1]);
    else err("** no answer from DNS server (timeout)\n");
    return 1;
}

static int cmd_arp(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct arp_info t[16];
    int n = net_arp_table(t, 16);
    if (!n) kprintf("(ARP table empty)\n");
    for (int i = 0; i < n; i++) { char m[18]; net_fmt_mac(m, t[i].mac); kprintf("%-16s %s   %us ago\n", ipstr(t[i].ip), m, t[i].age_ms / 1000u); }
    return 0;
}

static int cmd_netstat(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "-s")) {
        const struct net_stats *s = net_get_stats();
        kprintf("ARP   rx %u  tx %u\nIPv4  rx %u  tx %u\nICMP  rx %u  tx %u\nUDP   rx %u  tx %u\nTCP   rx %u  tx %u  retransmits %u  resets %u  window-scaled %u\nDropped: %u bad checksum, %u other\n",
                s->arp_rx, s->arp_tx, s->ip_rx, s->ip_tx, s->icmp_rx, s->icmp_tx, s->udp_rx, s->udp_tx, s->tcp_rx, s->tcp_tx, s->tcp_retrans, s->tcp_resets, s->tcp_ws, s->drop_csum, s->drop_other);
        return 0;
    }
    struct tcp_info t[8];
    int n = tcp_list(t, 8);
    kprintf("%-6s %-22s %-13s %6s %6s %6s\n", "Proto", "Remote", "State", "RxQ", "TxQ", "WS");
    for (int i = 0; i < n; i++) {
        char rem[24];
        if (t[i].state == TCP_LISTEN) ksnprintf(rem, sizeof(rem), "*:%u (listen)", t[i].local_port);
        else ksnprintf(rem, sizeof(rem), "%s:%u", ipstr(t[i].remote_ip), t[i].remote_port);
        char ws[8];
        if (t[i].state == TCP_LISTEN || t[i].state == TCP_CLOSED) ksnprintf(ws, sizeof(ws), "-");
        else ksnprintf(ws, sizeof(ws), "%u/%u", (unsigned)t[i].snd_wscale, (unsigned)t[i].rcv_shift);
        kprintf("%-6s %-22s %-13s %6u %6u %6s\n", "tcp", rem, tcp_state_name(t[i].state), t[i].rx_queued, t[i].tx_queued, ws);
    }
    if (!n) kprintf("(no TCP sockets)\n");
    return 0;
}

static int cmd_ntpdate(int argc, char **argv)
{
    int set = 0; const char *server = NULL;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-s")) set = 1; else server = argv[i]; }
    if (!need_net()) return 1;
    static const char *pool[] = { "pool.ntp.org", "time.google.com", "time.cloudflare.com" };
    struct ntp_result r; int rc = -1;
    if (!server && net_get_config()->has_ntp) { char b[16]; net_fmt_ip(b, net_get_config()->ntp); rc = ntp_query(b, &r); if (!rc) server = "DHCP-provided"; }
    if (server && rc) rc = ntp_query(server, &r);
    else if (!server) for (unsigned i = 0; i < 3 && rc; i++) { server = pool[i]; rc = ntp_query(server, &r); }
    if (rc) { err("ntpdate: no response from NTP server\n"); return 1; }
    int y, mo, d, h, mi, s;
    ktime_unix_to_fields(r.unix_secs, &y, &mo, &d, &h, &mi, &s, 0);
    kprintf("server %s (%s), stratum %u, delay %u ms\n", server, ipstr(r.server_ip), r.stratum, r.rtt_ms);
    kprintf("UTC time: %04d-%02d-%02d %02d:%02d:%02d   local clock offset %+d ms\n", y, mo, d, h, mi, s, r.offset_ms);
    if (set) { ktime_set_unix(r.unix_secs + (r.rtt_ms / 2000u)); kprintf("Clock set.\n"); }
    else kprintf("(use 'ntpdate -s' to set the clock)\n");
    return 0;
}

/* ------------------------------------------------------------- fetch */
static uint32_t progress_last;
static void on_progress(size_t got, int total)
{
    uint32_t now = ktime_ms();
    if (now - progress_last < 250) return;
    progress_last = now;
    if (total > 0) kprintf("\r  %u / %d bytes (%d%%)   ", (unsigned)got, total, (int)((uint64_t)got * 100 / (unsigned)total));
    else kprintf("\r  %u bytes   ", (unsigned)got);
}

static void filename_from_url(const char *url, char *out, size_t cap)
{
    struct url_parts u;
    out[0] = 0;
    if (url_parse(url, &u) == 0) {
        const char *base = strrchr(u.path, '/');
        base = base ? base + 1 : u.path;
        char tmp[200]; strlcpy(tmp, base, sizeof(tmp));
        char *q = strchr(tmp, '?'); if (q) *q = 0;
        if (tmp[0]) {
            /* squeeze into 8.3 */
            char name[9] = "", ext[4] = ""; int ni = 0, ei = 0;
            char *dot = strrchr(tmp, '.');
            for (char *p = tmp; *p && p != dot; p++) if (kisalnum(*p) && ni < 8) name[ni++] = (char)ktoupper(*p);
            if (dot) for (char *p = dot + 1; *p && ei < 3; p++) if (kisalnum(*p)) ext[ei++] = (char)ktoupper(*p);
            name[ni] = 0; ext[ei] = 0;
            if (ni) { ksnprintf(out, cap, ei ? "%s.%s" : "%s", name, ext); return; }
        }
        ksnprintf(out, cap, "%s", "INDEX.HTM");
        return;
    }
    strlcpy(out, "FETCH.OUT", cap);
}

static int looks_binary(const uint8_t *b, size_t n)
{
    size_t chk = n < 512 ? n : 512, bad = 0;
    for (size_t i = 0; i < chk; i++) if (b[i] == 0 || (b[i] < 9) || (b[i] > 13 && b[i] < 32 && b[i] != 27)) bad++;
    return bad * 10 > chk;
}

static int cmd_fetch(int argc, char **argv)
{
    struct http_opts o; memset(&o, 0, sizeof(o));
    o.follow_redirects = 5; o.timeout_ms = 8000;
    const char *url = NULL, *outfile = NULL, *data = NULL, *cafile = NULL;
    int out_auto = 0, include = 0, head = 0, force_text = 0, force_raw = 0;
    static char hdrs[512]; hdrs[0] = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) outfile = argv[++i];
        else if (!strcmp(a, "-O")) out_auto = 1;
        else if (!strcmp(a, "-i")) include = 1;
        else if (!strcmp(a, "-I")) { head = 1; include = 1; o.method = "HEAD"; }
        else if (!strcmp(a, "-v")) o.verbose = 1;
        else if (!strcmp(a, "-q") || !strcmp(a, "-s")) o.quiet = 1;
        else if (!strcmp(a, "-t") || !strcmp(a, "--text")) force_text = 1;
        else if (!strcmp(a, "-r") || !strcmp(a, "--raw")) force_raw = 1;
        else if (!strcmp(a, "-L")) o.follow_redirects = 5;
        else if (!strcmp(a, "-k") || !strcmp(a, "--insecure")) o.insecure = 1;
        else if (!strcmp(a, "--cafile") && i + 1 < argc) cafile = argv[++i];
        else if (!strcmp(a, "--no-redirect")) o.follow_redirects = 0;
        else if (!strcmp(a, "-X") && i + 1 < argc) o.method = argv[++i];
        else if (!strcmp(a, "-d") && i + 1 < argc) { data = argv[++i]; if (!o.method) o.method = "POST"; }
        else if (!strcmp(a, "-H") && i + 1 < argc) { strlcat(hdrs, argv[++i], sizeof(hdrs)); strlcat(hdrs, "\r\n", sizeof(hdrs)); }
        else if (!strcmp(a, "-m") && i + 1 < argc) o.timeout_ms = kstrtoul(argv[++i], NULL, 10) * 1000u;
        else if (!strcmp(a, "--max") && i + 1 < argc) o.max_body = kstrtoul(argv[++i], NULL, 10);
        else if (a[0] == '-' && a[1]) { err("fetch: unknown option %s\n", a); return 2; }
        else url = a;
    }
    if (!url) {
        kprintf("usage: fetch [options] URL\n"
                "  -o FILE  save to FILE        -O  save using the URL's file name\n"
                "  -i       show response headers   -I  HEAD request only\n"
                "  -t       render HTML as text     -r  print raw body\n"
                "  -v       verbose (request+headers)   -q  quiet (no status lines)\n"
                "  -X METH  method   -d DATA  POST data   -H 'K: V'  extra header\n"
                "  -m SECS  timeout  --max BYTES  size cap   --no-redirect\n"
                "  --cafile FILE  trust the PEM certificates in FILE (https)\n"
                "  -k       do not verify the server certificate (https)\n"
                "http:// and https:// (TLS 1.2/1.3). https uses the system roots in\n"
                "/etc/ssl/certs/ca-certificates.crt unless --cafile or -k is given; redirects are followed.\n");
        return 1;
    }
    if (!need_net()) return 1;
    if (data) { o.body = data; o.body_len = strlen(data); if (!strstr(hdrs, "Content-Type")) strlcat(hdrs, "Content-Type: application/x-www-form-urlencoded\r\n", sizeof(hdrs)); }
    if (hdrs[0]) o.extra_headers = hdrs;
    if (outfile || out_auto) o.progress = on_progress;

    static int default_ca_loaded;

    if (cafile) {
        static char cabuf[65536];
        uint32_t cn = 0;
        int vr = vfs_read_all(cafile, cabuf, sizeof(cabuf) - 1, &cn);
        if (vr != 0) { err("fetch: cannot read %s (%s)\n", cafile, vfs_strerror(vr)); return 1; }
        tls_ca_reset();
        default_ca_loaded = 0;              /* --cafile replaces the trust store */
        int k = tls_ca_add_pem(cabuf, cn);
        if (k <= 0) { err("fetch: no usable CA certificates in %s\n", cafile); return 1; }
        if (o.verbose) kprintf("* loaded %d CA certificate(s) from %s\n", k, cafile);
    } else if (!o.insecure && !strncasecmp(url, "https://", 8)) {
        /* No --cafile: load the built-in root bundle once so https works out of
         * the box (the same PEM the browser uses by default). */
        if (!default_ca_loaded) {
            int32_t sz = vfs_file_size("/etc/ssl/certs/ca-certificates.crt");
            char *cabuf = sz > 0 ? (char *)kmalloc((size_t)sz + 1) : NULL;
            if (cabuf) {
                uint32_t cn = 0;
                if (vfs_read_all("/etc/ssl/certs/ca-certificates.crt", cabuf, (uint32_t)sz, &cn) == 0) {
                    tls_ca_reset();
                    int k = tls_ca_add_pem(cabuf, cn);
                    if (k > 0) {
                        default_ca_loaded = 1;
                        if (o.verbose) kprintf("* loaded %d CA certificate(s) from the system bundle\n", k);
                    }
                }
                kfree(cabuf);
            }
        }
    }

    static char urlbuf[256];
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)) ksnprintf(urlbuf, sizeof(urlbuf), "http://%s", url); else strlcpy(urlbuf, url, sizeof(urlbuf));

    struct http_response r;
    int rc = http_request(urlbuf, &o, &r);
    if (outfile || out_auto) kprintf("\r%*s\r", 40, "");
    if (rc != 0 && !(r.status && r.body)) {
        err("fetch: %s\n", http_error_text(rc));
        if (rc == -7) {
            err("  %s", tls_error_text(r.tls_error));
            if (r.tls_alert >= 0) err(" (%s, alert %d)", tls_alert_name(r.tls_alert), r.tls_alert);
            err("\n");
            if (r.tls_error == TLS_E_UNSUPPORTED || r.tls_alert == 70 || r.tls_alert == 40 || r.tls_alert == 71)
                err("  StarlightOS speaks TLS 1.3 and 1.2 (ECDHE: x25519 or P-256); this server may want HelloRetryRequest, a client certificate, or something else unsupported.\n");
            if (r.tls_error == TLS_E_CERT)
                err("  add the issuing CA with --cafile FILE, or use -k to accept the certificate unchecked.\n");
            if (r.redirects) err("  (redirected %d time(s) to %s)\n", r.redirects, r.final_url);
        }
        http_response_free(&r);
        return 1;
    }
    if (!o.quiet) {
        uint32_t rate = r.elapsed_ms ? (uint32_t)((uint64_t)r.body_len * 1000u / r.elapsed_ms / 1024u) : 0;
        const char *tlsnote = strncasecmp(r.final_url, "https://", 8) ? "" : (r.tls_verified ? "  [TLS verified]" : "  [TLS unverified]");
        fg(r.status >= 400 ? C_LRED : r.status >= 300 ? C_YELLOW : C_LGREEN);
        kprintf("%d %s", r.status, r.status_text); fg(C_DGRAY);
        kprintf("  %s  %u bytes in %u ms (%u KB/s)%s%s%s\n", r.content_type[0] ? r.content_type : "unknown type", (unsigned)r.body_len, r.elapsed_ms, rate,
                r.redirects ? "  [redirected]" : "", r.truncated ? "  [TRUNCATED]" : "", tlsnote);
        fg_reset();
    }
    if (include) kprintf("%s\n", r.headers);
    if (head) { http_response_free(&r); return 0; }

    if (out_auto && !outfile) { static char autoname[16]; filename_from_url(r.final_url, autoname, sizeof(autoname)); outfile = autoname; }
    if (outfile) {
        int w = vfs_write(outfile, (const char *)r.body, (uint32_t)r.body_len);
        if (w != 0) { err("fetch: cannot write %s (%s)\n", outfile, vfs_strerror(w)); http_response_free(&r); return 1; }
        kprintf("Saved %u bytes to %s\n", (unsigned)r.body_len, outfile);
        http_response_free(&r);
        return 0;
    }
    if (!r.body || !r.body_len) { http_response_free(&r); return rc ? 1 : 0; }

    int is_html = strcasestr(r.content_type, "html") != NULL || (r.body_len > 14 && strcasestr((const char *)r.body, "<html") != NULL);
    int cols, rows; terminal_get_size(&cols, &rows);
    if (!terminal_capturing() && !force_raw && (force_text || is_html)) {
        size_t cap = r.body_len + 4096;
        char *txt = (char *)kmalloc(cap);
        if (txt) {
            size_t tn = html_to_text((const char *)r.body, r.body_len, txt, cap, cols - 2, 1);
            int has_text = 0;
            for (size_t k = 0; k < tn; k++) if (txt[k] > ' ') { has_text = 1; break; }
            if (has_text) terminal_writestring(txt);
            else { fg(C_YELLOW); kprintf("(no readable text: the page is probably built by JavaScript. 'fetch -r URL' shows the raw HTML)\n"); fg_reset(); }
            kfree(txt);
        }
    } else if (looks_binary(r.body, r.body_len) && !terminal_capturing()) {
        fg(C_YELLOW); kprintf("(binary data, %u bytes: use -o FILE to save it)\n", (unsigned)r.body_len); fg_reset();
    } else {
        size_t limit = terminal_capturing() ? r.body_len : (r.body_len < 16384 ? r.body_len : 16384);
        for (size_t i = 0; i < limit; i++) { char c = (char)r.body[i]; if (c != '\r') terminal_putchar(c); }
        if (limit < r.body_len) { fg(C_YELLOW); kprintf("\n... (%u more bytes; use -o FILE to save everything, or pipe to 'more')\n", (unsigned)(r.body_len - limit)); fg_reset(); }
        else if (r.body[r.body_len - 1] != '\n') terminal_putchar('\n');
    }
    int status = r.status;
    http_response_free(&r);
    return status >= 400 ? 1 : 0;
}

/* ---------------------------------------------------------------- nc */
static int cmd_nc(int argc, char **argv)
{
    int listen_mode = 0; const char *host = NULL; uint32_t port = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) listen_mode = 1;
        else if (!host && !listen_mode) host = argv[i];
        else if (kparse_uint(argv[i], &port) != 0) { err("bad port '%s'\n", argv[i]); return 1; }
    }
    if (listen_mode && host && !port) { kparse_uint(host, &port); host = NULL; }
    if (!port || port > 65535 || (!listen_mode && !host)) { err("usage: nc HOST PORT   |   nc -l PORT\n"); return 1; }
    if (!need_net()) return 1;
    int s;
    if (listen_mode) {
        int l = tcp_listen((uint16_t)port);
        if (l < 0) { err("nc: cannot listen on %u\n", (unsigned)port); return 1; }
        kprintf("Listening on port %u (Ctrl-C to stop)...\n", (unsigned)port);
        s = tcp_accept(l, 0xFFFFFFF0u);
        tcp_close(l);
        if (s < 0) { kprintf("\n"); return 1; }
        uint8_t pip[4]; uint16_t pp; tcp_peer(s, pip, &pp);
        kprintf("Connection from %s:%u\n", ipstr(pip), pp);
    } else {
        uint8_t ip[4];
        if (resolve_or_complain(host, ip)) return 1;
        s = tcp_connect(ip, (uint16_t)port, 6000);
        if (s < 0) { err("nc: connection to %s:%u failed\n", host, (unsigned)port); return 1; }
        kprintf("Connected to %s:%u. Type lines; Ctrl-C to quit.\n", ipstr(ip), (unsigned)port);
    }
    char line[200]; int ll = 0;
    while (!kabort_requested()) {
        uint8_t buf[256];
        int n = tcp_recv(s, buf, sizeof(buf), 0);
        if (n > 0) { for (int i = 0; i < n; i++) if (buf[i] != '\r') terminal_putchar((char)buf[i]); }
        else if (n == 0 || n < -1) { kprintf("\n[connection closed]\n"); break; }
        int c;
        while ((c = keyboard_getchar_nonblocking()) >= 0) {
            if (c == KEY_CTRL_C) goto done;
            if (c == '\n') { line[ll++] = '\n'; tcp_send(s, line, (size_t)ll); terminal_putchar('\n'); ll = 0; }
            else if (c == '\b') { if (ll) { ll--; terminal_putchar('\b'); } }
            else if (c >= 32 && c < 127 && ll < (int)sizeof(line) - 2) { line[ll++] = (char)c; terminal_putchar((char)c); }
        }
        kyield();
    }
done:
    tcp_close(s);
    return 0;
}


/* ----------------------------------------------------------- telnet */
/* The protocol engine lives in telnet.c; this is the shell side: connect, then pump bytes both ways.
 * Line mode (local echo + editing, line sent with CR LF) until the server says WILL ECHO, then character
 * mode. Ctrl-C closes the connection. */
struct tn_ctx { int s; };
static void tn_io_send(void *c, const uint8_t *d, size_t n) { tcp_send(((struct tn_ctx *)c)->s, d, n); }
static void tn_io_put(void *c, char ch) { (void)c; terminal_putchar(ch); }
static void tn_io_size(void *c, int *cols, int *rows) { (void)c; terminal_get_size(cols, rows); }
static int  tn_io_column(void *c) { (void)c; int row, col; terminal_get_cursor(&row, &col); return col; }

static int cmd_telnet(int argc, char **argv)
{
    const char *host = NULL; uint32_t port = 23;
    for (int i = 1; i < argc; i++) {
        if (!host) host = argv[i];
        else if (kparse_uint(argv[i], &port) != 0 || !port || port > 65535) { err("telnet: bad port '%s'\n", argv[i]); return 1; }
    }
    if (!host) { err("usage: telnet HOST [PORT]     (default port 23; Ctrl-C closes the connection)\n"); return 1; }
    if (!need_net()) return 1;
    uint8_t ip[4];
    if (resolve_or_complain(host, ip)) return 1;
    kprintf("Trying %s...\n", ipstr(ip));
    int s = tcp_connect(ip, (uint16_t)port, 8000);
    if (s < 0) { err("telnet: unable to connect to %s:%u (%s)\n", host, (unsigned)port,
                     s == TCP_ERR_REFUSED ? "connection refused" : s == TCP_ERR_TIMEOUT ? "timed out" : "failed"); return 1; }
    kprintf("Connected to %s. Ctrl-C closes the connection.\n", host);

    static struct tn_ctx ctx;
    static struct telnet t;
    static const struct telnet_io io = { tn_io_send, tn_io_put, tn_io_size, tn_io_column, &ctx };
    ctx.s = s;
    telnet_init(&t, &io);

    char line[240]; int ll = 0;
    int closed = 0;
    while (!kabort_requested()) {
        uint8_t buf[512];
        int n = tcp_recv(s, buf, sizeof(buf), 0);
        if (n > 0) telnet_input(&t, buf, (size_t)n);
        else if (n == 0 || n < -1) { closed = 1; break; }
        int c;
        while ((c = keyboard_getchar_nonblocking()) >= 0) {
            if (c == KEY_CTRL_C) goto done;
            if (t.remote_echo) {                                                            /* character mode: send every key */
                uint8_t o[8]; size_t on = telnet_encode_key(c, o);
                if (on) tcp_send(s, o, on);
            } else {                                                                        /* line mode: local echo + editing */
                if (c == '\n') { line[ll++] = '\r'; line[ll++] = '\n'; tcp_send(s, line, (size_t)ll); terminal_putchar('\n'); ll = 0; }
                else if (c == '\b') { if (ll) { ll--; terminal_putchar('\b'); } }
                else if (c >= 32 && c < 127 && ll < (int)sizeof(line) - 3) { line[ll++] = (char)c; terminal_putchar((char)c); }
            }
        }
        kyield();
    }
done:
    kprintf(closed ? "\nConnection closed by foreign host.\n" : "\nConnection closed.\n");
    tcp_close(s);
    return 0;
}

/* ------------------------------------------------------------- httpd */
/* The server itself lives in httpd.c: a poll()-driven event loop. */
extern int httpd_main(int argc, char **argv);
static int cmd_httpd(int argc, char **argv) { return httpd_main(argc, argv); }

const struct command net_commands[] = {
    { "ip",         "Network", "ip [dhcp | set IP MASK GW [DNS] | dns ADDR]", "show or configure the network interface", cmd_ip },
    { "ping",       "Network", "ping [-c N] [-s SIZE] [-t TTL] [-W MS] HOST", "ICMP echo with statistics", cmd_ping },
    { "traceroute", "Network", "traceroute HOST", "show the route to a host", cmd_traceroute },
    { "nslookup",   "Network", "nslookup NAME [SERVER[:PORT]] | --cache | --flush", "DNS lookup (A records, CNAME aware)", cmd_nslookup },
    { "arp",        "Network", "arp", "show the ARP table", cmd_arp },
    { "netstat",    "Network", "netstat [-s]", "TCP sockets and protocol counters", cmd_netstat },
    { "ntpdate",    "Network", "ntpdate [-s] [SERVER]", "query network time (-s sets the clock)", cmd_ntpdate },
    { "fetch",      "Network", "fetch [-o FILE|-O] [-i|-I|-v] [-t|-r] [-L|--no-redirect] [-X METHOD] [-d DATA] [-H HDR] [-m SEC] [--max BYTES] [-q] URL", "HTTP/HTTPS client (curl/wget aliases): redirects, chunked, save, render HTML", cmd_fetch },
    { "nc",         "Network", "nc HOST PORT | nc -l PORT", "raw TCP client / listener", cmd_nc },
    { "telnet",     "Network", "telnet HOST [PORT]", "telnet client (option negotiation; line or character mode)", cmd_telnet },
    { "httpd",      "Network", "httpd [-c] [PORT]", "serve the current directory over HTTP (event-driven; chat room in httpd_test)", cmd_httpd },
};
const int net_command_count = (int)(sizeof(net_commands) / sizeof(net_commands[0]));
