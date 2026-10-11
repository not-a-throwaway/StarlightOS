/*
 * http.c -- HTTP/1.1 client: redirects, chunked transfer, Content-Length,
 * read-until-close, HEAD/POST. https:// goes through the TLS 1.3 client (tls.c);
 * the server certificate is NOT validated (see tls.h).
 */
#include "version.h"
#include "net.h"
#include "kstring.h"
#include "kprintf.h"
#include "kheap.h"
#include "ktime.h"
#include "ksched.h"
#include "tls.h"

extern void terminal_putchar(char c);
#define DEFAULT_MAX_BODY (2u * 1024u * 1024u)
#define HEAD_MAX 6144

const char *http_error_text(int code)
{
    switch (code) {
    case 0:   return "ok";
    case -1:  return "could not resolve host";
    case -2:  return "connection failed";
    case -3:  return "send failed";
    case -4:  return "no response (timed out)";
    case -5:  return "malformed HTTP response";
    case -6:  return "too many redirects";
    case -7:  return "TLS handshake failed";
    case -8:  return "out of memory";
    case -9:  return "interrupted";
    case -10: return "network not configured (no DHCP lease yet)";
    case -11: return "bad URL";
    default:  return "error";
    }
}

void http_response_free(struct http_response *r)
{
    if (r->body) kfree(r->body);
    r->body = NULL; r->body_len = 0;
}

/* case-insensitive header lookup inside the raw header block */
static int header_value(const char *headers, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    const char *p = headers;
    while (*p) {
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;
        if ((size_t)(eol - p) > nl && strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t n = 0;
            while (v + n < eol && v[n] != '\r' && n + 1 < cap) { out[n] = v[n]; n++; }
            out[n] = 0;
            return 1;
        }
        p = *eol ? eol + 1 : eol;
    }
    return 0;
}

struct body_sink {
    uint8_t *buf; size_t len, cap, max;
    int truncated, oom;
};

static int sink_add(struct body_sink *s, const uint8_t *d, size_t n)
{
    if (s->len + n > s->max) { n = s->max > s->len ? s->max - s->len : 0; s->truncated = 1; }
    if (!n) return 0;
    if (s->len + n + 1 > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 8192;
        while (nc < s->len + n + 1) nc *= 2;
        uint8_t *nb = (uint8_t *)krealloc(s->buf, nc);
        if (!nb) { s->oom = 1; return -1; }
        s->buf = nb; s->cap = nc;
    }
    memcpy(s->buf + s->len, d, n);
    s->len += n;
    s->buf[s->len] = 0;
    return 0;
}

enum { CH_SIZE, CH_EXT, CH_SIZE_LF, CH_DATA, CH_CR, CH_LF, CH_TRAILER, CH_DONE };

/* A connection that is either plain TCP or TCP + TLS. */
struct stream { int sock; struct tls_conn *tls; };

static int st_send(struct stream *st, const void *d, size_t n) { return st->tls ? tls_send(st->tls, d, n) : tcp_send(st->sock, d, n); }
static int st_recv(struct stream *st, void *b, size_t cap, uint32_t tmo) { return st->tls ? tls_recv(st->tls, b, cap, tmo) : tcp_recv(st->sock, b, cap, tmo); }
static void st_close(struct stream *st) { if (st->tls) tls_close(st->tls); tcp_close(st->sock); }
static void st_abort(struct stream *st) { if (st->tls) tls_close(st->tls); tcp_abort(st->sock); }

static int single_request(const struct url_parts *u, const struct http_opts *o, struct http_response *res,
                          char *host_ip_text)
{
    uint8_t ip[4];
    if (!net_configured()) return -10;
    if (dns_resolve(u->host, ip) != 0) return kabort_requested() ? -9 : -1;
    net_fmt_ip(host_ip_text, ip);

    int sock = tcp_connect(ip, u->port, 6000);
    if (sock < 0) return sock == TCP_ERR_ABORT ? -9 : -2;
    struct stream st = { sock, NULL };
    if (u->https) {
        int terr = 0;
        tls_set_trace(o->verbose);
        tls_set_verify(!o->insecure);
        st.tls = tls_connect(sock, u->host, o->timeout_ms ? o->timeout_ms : 8000, &terr);
        if (!st.tls) {
            res->tls_error = terr; res->tls_alert = tls_last_alert();
            tcp_abort(sock);
            return terr == TLS_E_ABORT ? -9 : -7;
        }
        res->tls_verified = tls_verify_result(st.tls) == 0;
        ksnprintf(res->tls_peer, sizeof(res->tls_peer), "%s", tls_peer_cn(st.tls));
        if (o->verbose) kprintf("* %s (%s%s%s)\n", tls_describe(st.tls),
            res->tls_verified ? "certificate verified" : "certificate NOT verified",
            res->tls_peer[0] ? ": " : "", res->tls_peer);
    }

    /* ---- request ---- */
    const char *method = o->method ? o->method : "GET";
    char *req = (char *)kmalloc(1024 + (o->extra_headers ? strlen(o->extra_headers) : 0));
    if (!req) { st_abort(&st); return -8; }
    char hosthdr[110];
    if (u->port == (u->https ? 443 : 80)) strlcpy(hosthdr, u->host, sizeof(hosthdr)); else ksnprintf(hosthdr, sizeof(hosthdr), "%s:%u", u->host, u->port);
    int rl = ksnprintf(req, 1024 + (o->extra_headers ? strlen(o->extra_headers) : 0),
        "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: StarlightOS/" STARLIGHTOS_VERSION " (x86_64; fetch)\r\nAccept: */*\r\n"
        "Accept-Encoding: identity\r\nConnection: close\r\n", method, u->path, hosthdr);
    if (o->body) rl += ksnprintf(req + rl, 64, "Content-Length: %u\r\n", (unsigned)o->body_len);
    if (o->extra_headers) rl += ksnprintf(req + rl, strlen(o->extra_headers) + 1, "%s", o->extra_headers);
    rl += ksnprintf(req + rl, 8, "\r\n");
    if (o->verbose) {
        kprintf("> ");
        for (int i = 0; i < rl; i++) { if (req[i] == '\r') continue; terminal_putchar(req[i]); if (req[i] == '\n' && i + 1 < rl - 1) kprintf("> "); }
    }
    int sr = st_send(&st, req, (size_t)rl);
    kfree(req);
    if (sr >= 0 && o->body && o->body_len) sr = st_send(&st, o->body, o->body_len);
    if (sr < 0) { st_abort(&st); return sr == TCP_ERR_ABORT ? -9 : -3; }

    /* ---- headers ---- */
    static char head[HEAD_MAX];
    size_t hlen = 0;
    int have_head = 0;
    size_t head_end = 0;
    uint32_t tmo = o->timeout_ms ? o->timeout_ms : 8000;
    uint8_t chunk[1460];
    struct body_sink sink = { 0, 0, 0, o->max_body ? o->max_body : DEFAULT_MAX_BODY, 0, 0 };

    int content_length = -1, chunked = 0;
    int ch_state = CH_SIZE; size_t ch_remaining = 0; unsigned ch_size = 0; int ch_digits = 0;
    int no_body = (strcmp(method, "HEAD") == 0);
    int done = 0, error = 0;
    uint32_t t0 = ktime_ms();

    while (!done) {
        int n = st_recv(&st, chunk, sizeof(chunk), tmo);
        if (n == TCP_ERR_ABORT) { error = -9; break; }
        if (n == -1) { error = have_head ? 0 : -4; if (have_head) res->truncated = 1; break; }
        if (n < 0) { error = have_head ? 0 : -4; if (have_head && content_length >= 0 && (int)sink.len < content_length) res->truncated = 1; break; }
        if (n == 0) break;                                              /* EOF */

        const uint8_t *data = chunk; size_t dn = (size_t)n;
        if (!have_head) {
            size_t take = dn < HEAD_MAX - 1 - hlen ? dn : HEAD_MAX - 1 - hlen;
            memcpy(head + hlen, data, take);
            size_t scan_from = hlen > 3 ? hlen - 3 : 0;
            hlen += take; head[hlen] = 0;
            char *sep = NULL;
            for (size_t i = scan_from; i + 3 < hlen; i++)
                if (head[i] == '\r' && head[i+1] == '\n' && head[i+2] == '\r' && head[i+3] == '\n') { sep = head + i; break; }
            if (!sep) {
                if (hlen >= HEAD_MAX - 1) { error = -5; break; }
                continue;
            }
            head_end = (size_t)(sep - head) + 4;
            have_head = 1;
            /* parse status line */
            if (strncmp(head, "HTTP/1.", 7) != 0) { error = -5; break; }
            const char *sp = strchr(head, ' ');
            if (!sp) { error = -5; break; }
            res->status = (int)kstrtoul(sp + 1, NULL, 10);
            const char *e = strchr(sp + 1, ' ');
            const char *le = strchr(head, '\r');
            if (e && le && e < le) { size_t k = (size_t)(le - e - 1); if (k >= sizeof(res->status_text)) k = sizeof(res->status_text) - 1; memcpy(res->status_text, e + 1, k); res->status_text[k] = 0; }
            head[head_end - 2] = 0;                                     /* terminate header block (keep final line CRLF out) */
            const char *hdrs = strchr(head, '\n'); hdrs = hdrs ? hdrs + 1 : head;
            strlcpy(res->headers, hdrs, sizeof(res->headers));
            char v[200];
            if (header_value(hdrs, "Content-Length", v, sizeof(v))) content_length = (int)kstrtoul(v, NULL, 10);
            if (header_value(hdrs, "Transfer-Encoding", v, sizeof(v)) && strcasestr(v, "chunked")) chunked = 1;
            if (header_value(hdrs, "Content-Type", v, sizeof(v))) strlcpy(res->content_type, v, sizeof(res->content_type));
            if (header_value(hdrs, "Location", v, sizeof(v))) strlcpy(res->location, v, sizeof(res->location));
            res->content_length = chunked ? -1 : content_length;
            if (o->verbose) {
                kprintf("< ");
                for (const char *c = head; *c; c++) { if (*c == '\r') continue; terminal_putchar(*c); if (*c == '\n' && c[1]) kprintf("< "); }
                kprintf("\n");
            }
            if (no_body || res->status == 204 || res->status == 304 || (res->status >= 100 && res->status < 200)) { done = 1; break; }
            data = (const uint8_t *)head + head_end;                    /* leftover body bytes inside head buffer? */
            dn = hlen - head_end;
            /* bytes after the header terminator that came in the same read */
            if (dn == 0) continue;
        }

        /* ---- body bytes in [data, data+dn) ---- */
        if (chunked) {
            size_t i = 0;
            while (i < dn && ch_state != CH_DONE) {
                uint8_t c = data[i];
                switch (ch_state) {
                case CH_SIZE:
                    if (kisxdigit(c)) { ch_size = ch_size * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10); ch_digits++; i++; }
                    else if (c == ';' || c == ' ') ch_state = CH_EXT;
                    else if (c == '\r') ch_state = CH_SIZE_LF, i++;
                    else { error = -5; ch_state = CH_DONE; }
                    break;
                case CH_EXT: if (c == '\r') ch_state = CH_SIZE_LF; i++; break;
                case CH_SIZE_LF:
                    if (c != '\n' || !ch_digits) { error = -5; ch_state = CH_DONE; break; }
                    i++; ch_remaining = ch_size;
                    ch_state = ch_size ? CH_DATA : CH_TRAILER;
                    ch_size = 0; ch_digits = 0;
                    break;
                case CH_DATA: {
                    size_t take = dn - i < ch_remaining ? dn - i : ch_remaining;
                    if (sink_add(&sink, data + i, take) < 0) { error = -8; ch_state = CH_DONE; break; }
                    i += take; ch_remaining -= take;
                    if (!ch_remaining) ch_state = CH_CR;
                    break;
                }
                case CH_CR: if (c == '\r') { ch_state = CH_LF; i++; } else { error = -5; ch_state = CH_DONE; } break;
                case CH_LF: if (c == '\n') { ch_state = CH_SIZE; i++; } else { error = -5; ch_state = CH_DONE; } break;
                case CH_TRAILER:                                          /* skip trailers until blank line */
                    if (c == '\n') { ch_state = CH_DONE; }               /* simplification: first LF ends */
                    i++;
                    break;
                }
            }
            if (ch_state == CH_DONE) { done = 1; }
        } else {
            if (sink_add(&sink, data, dn) < 0) { error = -8; break; }
            if (content_length >= 0 && (int)sink.len >= content_length) { done = 1; }
            if (sink.truncated) { done = 1; }
        }
        if (o->progress) o->progress(sink.len, res->content_length);
        if (sink.truncated) break;
    }

    st_close(&st);
    res->elapsed_ms = (uint32_t)(ktime_ms() - t0);
    res->body = sink.buf; res->body_len = sink.len;
    if (sink.truncated) res->truncated = 1;
    if (!have_head && !error) error = -4;
    if (sink.oom && !error) error = -8;
    return error;
}

static void resolve_location(const struct url_parts *base, const char *loc, char *out, size_t cap)
{
    if (strncasecmp(loc, "http://", 7) == 0 || strncasecmp(loc, "https://", 8) == 0) { strlcpy(out, loc, cap); return; }
    char hostport[120];
    if (base->port == (base->https ? 443 : 80)) strlcpy(hostport, base->host, sizeof(hostport));
    else ksnprintf(hostport, sizeof(hostport), "%s:%u", base->host, base->port);
    const char *scheme = base->https ? "https" : "http";
    if (loc[0] == '/' && loc[1] == '/') { ksnprintf(out, cap, "%s:%s", scheme, loc); return; }
    if (loc[0] == '/') { ksnprintf(out, cap, "%s://%s%s", scheme, hostport, loc); return; }
    /* relative to current directory */
    char dir[200];
    strlcpy(dir, base->path, sizeof(dir));
    char *q = strchr(dir, '?'); if (q) *q = 0;
    char *slash = strrchr(dir, '/'); if (slash) slash[1] = 0; else strlcpy(dir, "/", sizeof(dir));
    ksnprintf(out, cap, "%s://%s%s%s", scheme, hostport, dir, loc);
}

int http_request(const char *url, const struct http_opts *opts, struct http_response *res)
{
    struct http_opts def; memset(&def, 0, sizeof(def));
    if (!opts) opts = &def;
    memset(res, 0, sizeof(*res));
    res->content_length = -1; res->tls_alert = -1;
    char cur[256];
    strlcpy(cur, url, sizeof(cur));
    int hops = 0;
    for (;;) {
        struct url_parts u;
        if (url_parse(cur, &u) != 0) { res->error = -11; return -11; }
        strlcpy(res->final_url, cur, sizeof(res->final_url));
        char ipt[16] = "";
        struct http_response attempt; memset(&attempt, 0, sizeof(attempt));
        attempt.content_length = -1; attempt.tls_alert = -1;
        int err = single_request(&u, opts, &attempt, ipt);
        int redirects = res->redirects;
        *res = attempt;
        res->redirects = redirects;
        strlcpy(res->final_url, cur, sizeof(res->final_url));
        res->error = err;
        if (err && !(err == -4 && res->status)) return err;
        int st = res->status;
        if (opts->follow_redirects && (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && res->location[0]) {
            if (++hops > opts->follow_redirects) { res->error = -6; return -6; }
            char next[256];
            resolve_location(&u, res->location, next, sizeof(next));
            if (!opts->quiet) kprintf("  -> %d redirect to %s\n", st, next);
            http_response_free(res);
            strlcpy(cur, next, sizeof(cur));
            res->redirects = hops;
            continue;
        }
        return err;
    }
}
