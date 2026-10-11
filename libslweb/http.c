/*
 * libslweb -- HTTP/1.1 client (lifted from the kernel's http.c).
 *
 * Redirects, chunked transfer, Content-Length, read-until-close, HEAD/POST.
 * Plain HTTP drives the M2 ring-3 syscalls (user/slnet.h):
 * sl_resolve/sl_socket/sl_connect/sl_send/sl_recv/sl_poll. HTTPS runs the
 * kernel's TLS client (tls.c, compiled in via tls_user.c) over the same socket;
 * the server's certificate chain is validated against a trust store that starts
 * empty (fail closed), so a caller must load roots with slweb_http_add_ca_pem()
 * / slweb_http_add_ca_file() or opt out per request with opts.insecure.
 *
 * The structure mirrors the kernel file so the two stay easy to diff; the
 * error codes are the same values, exposed as SLWEB_ERR_*.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "slweb.h"
#include "../user/slnet.h"
#include "../tls.h"

#define DEFAULT_MAX_BODY (2u * 1024u * 1024u)
#define HEAD_MAX 6144

/* milliseconds since boot (newlib's clock() is backed by the kernel's uptime) */
static uint32_t slweb_time_ms(void)
{
    return (uint32_t)clock();
}

static void sl_strlcpy(char *d, const char *s, size_t cap)
{
    size_t i = 0;
    if (!cap) return;
    for (; i + 1 < cap && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

static const char *sl_strcasestr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    if (!nl) return h;
    for (; *h; h++) if (strncasecmp(h, n, nl) == 0) return h;
    return NULL;
}

const char *slweb_http_error_text(int code)
{
    switch (code) {
    case SLWEB_OK:            return "ok";
    case SLWEB_ERR_RESOLVE:   return "could not resolve host";
    case SLWEB_ERR_CONNECT:   return "connection failed";
    case SLWEB_ERR_SEND:      return "send failed";
    case SLWEB_ERR_TIMEOUT:   return "no response (timed out)";
    case SLWEB_ERR_MALFORMED: return "malformed HTTP response";
    case SLWEB_ERR_REDIRECTS: return "too many redirects";
    case SLWEB_ERR_TLS:       return "TLS handshake failed";
    case SLWEB_ERR_NOMEM:     return "out of memory";
    case SLWEB_ERR_INTR:      return "interrupted";
    case SLWEB_ERR_NONET:     return "network not configured (no DHCP lease yet)";
    case SLWEB_ERR_BADURL:    return "bad URL";
    case SLWEB_ERR_HTTPS:     return "https is unavailable in this build";
    default:                  return "error";
    }
}

void slweb_http_response_free(struct slweb_response *r)
{
    if (r->body) free(r->body);
    r->body = NULL; r->body_len = 0;
}

/* A human string for the underlying TLS failure (res->tls_error, a TLS_E_* code). */
const char *slweb_http_tls_error_text(int tls_error)
{
    switch (tls_error) {
    case 0:                 return "ok";
    case TLS_E_CERT:        return "certificate did not verify";
    case TLS_E_ALERT:       return "server sent a fatal alert";
    case TLS_E_UNSUPPORTED: return "no supported TLS version or cipher";
    case TLS_E_MAC:         return "handshake finished MAC did not verify";
    case TLS_E_NOMEM:       return "out of memory during the handshake";
    case TLS_E_ABORT:       return "handshake interrupted";
    default:                return tls_error_text(tls_error);
    }
}

/* ---- TLS trust store (shared global; roots persist across requests) ---- */
int slweb_http_add_ca_pem(const char *pem, size_t len)
{
    return tls_ca_add_pem(pem, len);
}

int slweb_http_add_ca_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = 65536, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { fclose(f); return -1; }
    for (;;) {
        if (len == cap) {
            char *nb = (char *)realloc(buf, cap * 2);
            if (!nb) { free(buf); fclose(f); return -1; }
            buf = nb; cap *= 2;
        }
        size_t r = fread(buf + len, 1, cap - len, f);
        len += r;
        if (r == 0) break;
    }
    fclose(f);
    int added = tls_ca_add_pem(buf, len);
    free(buf);
    return added;
}

void slweb_http_reset_ca(void) { tls_ca_reset(); }

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
        uint8_t *nb = (uint8_t *)realloc(s->buf, nc);
        if (!nb) { s->oom = 1; return -1; }
        s->buf = nb; s->cap = nc;
    }
    memcpy(s->buf + s->len, d, n);
    s->len += n;
    s->buf[s->len] = 0;
    return 0;
}

enum { CH_SIZE, CH_EXT, CH_SIZE_LF, CH_DATA, CH_CR, CH_LF, CH_TRAILER, CH_DONE };

/* A connection to the server, plain TCP or wrapped in TLS. */
struct stream { int sock; struct tls_conn *tls; };

static int st_send(struct stream *st, const void *d, size_t n)
{
    if (st->tls) return tls_send(st->tls, d, n);
    long r = sl_send(st->sock, d, n);
    return r < 0 ? -1 : (int)r;
}

/* Wait for readability with the caller's timeout (the socket syscall itself
 * blocks up to 30 s), then read. 0 bytes means EOF. -1 means timeout/error.
 * With TLS the handshake/records are driven by tls.c (tcp_recv convention). */
static int st_recv(struct stream *st, void *b, size_t cap, uint32_t tmo)
{
    if (st->tls) return tls_recv(st->tls, b, cap, tmo);
    struct sl_pollfd p; p.fd = st->sock; p.events = SL_POLLIN; p.revents = 0;
    int pr = sl_poll(&p, 1, (int)tmo);
    if (pr == 0) { errno = EAGAIN; return -1; }
    if (pr < 0) return -1;
    long n = sl_recv(st->sock, b, cap);
    return n < 0 ? -1 : (int)n;
}

static void st_close(struct stream *st) { if (st->tls) tls_close(st->tls); close(st->sock); }

static uint16_t sw_htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

static int sw_connect(const struct slweb_url *u, const uint8_t ip[4])
{
    int fd = sl_socket(SL_AF_INET, SL_SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sl_sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = SL_AF_INET;
    a.sin_port = sw_htons(u->port);
    memcpy(a.sin_addr, ip, 4);
    if (sl_connect(fd, &a) != 0) { int e = errno; close(fd); errno = e; return -1; }
    return fd;
}

static int single_request(const struct slweb_url *u, const struct slweb_http_opts *o, struct slweb_response *res)
{
    uint8_t ip[4];
    if (sl_resolve(u->host, ip) != 0) return errno == EINTR ? SLWEB_ERR_INTR : SLWEB_ERR_RESOLVE;

    int sock = sw_connect(u, ip);
    if (sock < 0) return errno == EINTR ? SLWEB_ERR_INTR : SLWEB_ERR_CONNECT;
    struct stream st = { sock, NULL };

    if (u->https) {
        int terr = 0;
        tls_set_trace(o->verbose);
        tls_set_verify(!o->insecure);
        st.tls = tls_connect(sock, u->host, o->timeout_ms ? o->timeout_ms : 8000, &terr);
        if (!st.tls) {
            res->tls_error = terr;
            res->tls_alert = tls_last_alert();
            close(sock);
            return terr == TLS_E_ABORT ? SLWEB_ERR_INTR : SLWEB_ERR_TLS;
        }
        res->tls_verified = tls_verify_result(st.tls) == 0;
        sl_strlcpy(res->tls_peer, tls_peer_cn(st.tls), sizeof(res->tls_peer));
        if (o->verbose)
            printf("* %s (%s%s%s)\n", tls_describe(st.tls),
                   res->tls_verified ? "certificate verified" : "certificate NOT verified",
                   res->tls_peer[0] ? ": " : "", res->tls_peer);
    }

    /* ---- request ---- */
    const char *method = o->method ? o->method : "GET";
    char *req = (char *)malloc(1024 + (o->extra_headers ? strlen(o->extra_headers) : 0));
    if (!req) { st_close(&st); return SLWEB_ERR_NOMEM; }
    char hosthdr[110];
    if (u->port == (u->https ? 443 : 80)) sl_strlcpy(hosthdr, u->host, sizeof(hosthdr));
    else snprintf(hosthdr, sizeof(hosthdr), "%s:%u", u->host, u->port);
    int rl = snprintf(req, 1024 + (o->extra_headers ? strlen(o->extra_headers) : 0),
        "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: StarlightOS/" SLWEB_VERSION " (x86_64; slweb)\r\nAccept: */*\r\n"
        "Accept-Encoding: identity\r\nConnection: close\r\n", method, u->path, hosthdr);
    if (o->body) rl += snprintf(req + rl, 64, "Content-Length: %u\r\n", (unsigned)o->body_len);
    if (o->extra_headers) rl += snprintf(req + rl, strlen(o->extra_headers) + 1, "%s", o->extra_headers);
    rl += snprintf(req + rl, 8, "\r\n");
    if (o->verbose) {
        printf("> ");
        for (int i = 0; i < rl; i++) { if (req[i] == '\r') continue; putchar(req[i]); if (req[i] == '\n' && i + 1 < rl - 1) printf("> "); }
    }
    int sr = st_send(&st, req, (size_t)rl);
    free(req);
    if (sr >= 0 && o->body && o->body_len) sr = st_send(&st, o->body, o->body_len);
    if (sr < 0) { int e = errno; st_close(&st); errno = e; return errno == EINTR ? SLWEB_ERR_INTR : SLWEB_ERR_SEND; }

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
    uint32_t t0 = slweb_time_ms();

    while (!done) {
        int n = st_recv(&st, chunk, sizeof(chunk), tmo);
        if (n < 0 && errno == EINTR) { error = SLWEB_ERR_INTR; break; }
        if (n == -1) { error = have_head ? 0 : SLWEB_ERR_TIMEOUT; if (have_head) res->truncated = 1; break; }
        if (n < 0) { error = have_head ? 0 : SLWEB_ERR_TIMEOUT; if (have_head && content_length >= 0 && (int)sink.len < content_length) res->truncated = 1; break; }
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
                if (hlen >= HEAD_MAX - 1) { error = SLWEB_ERR_MALFORMED; break; }
                continue;
            }
            head_end = (size_t)(sep - head) + 4;
            have_head = 1;
            /* parse status line */
            if (strncmp(head, "HTTP/1.", 7) != 0) { error = SLWEB_ERR_MALFORMED; break; }
            const char *sp = strchr(head, ' ');
            if (!sp) { error = SLWEB_ERR_MALFORMED; break; }
            res->status = (int)strtoul(sp + 1, NULL, 10);
            const char *e = strchr(sp + 1, ' ');
            const char *le = strchr(head, '\r');
            if (e && le && e < le) { size_t k = (size_t)(le - e - 1); if (k >= sizeof(res->status_text)) k = sizeof(res->status_text) - 1; memcpy(res->status_text, e + 1, k); res->status_text[k] = 0; }
            head[head_end - 2] = 0;                                     /* terminate header block (keep final line CRLF out) */
            const char *hdrs = strchr(head, '\n'); hdrs = hdrs ? hdrs + 1 : head;
            sl_strlcpy(res->headers, hdrs, sizeof(res->headers));
            char v[200];
            if (header_value(hdrs, "Content-Length", v, sizeof(v))) content_length = (int)strtoul(v, NULL, 10);
            if (header_value(hdrs, "Transfer-Encoding", v, sizeof(v)) && sl_strcasestr(v, "chunked")) chunked = 1;
            if (header_value(hdrs, "Content-Type", v, sizeof(v))) sl_strlcpy(res->content_type, v, sizeof(res->content_type));
            if (header_value(hdrs, "Location", v, sizeof(v))) sl_strlcpy(res->location, v, sizeof(res->location));
            res->content_length = chunked ? -1 : content_length;
            if (o->verbose) {
                printf("< ");
                for (const char *c = head; *c; c++) { if (*c == '\r') continue; putchar(*c); if (*c == '\n' && c[1]) printf("< "); }
                printf("\n");
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
                    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) { ch_size = ch_size * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10); ch_digits++; i++; }
                    else if (c == ';' || c == ' ') ch_state = CH_EXT;
                    else if (c == '\r') ch_state = CH_SIZE_LF, i++;
                    else { error = SLWEB_ERR_MALFORMED; ch_state = CH_DONE; }
                    break;
                case CH_EXT: if (c == '\r') ch_state = CH_SIZE_LF; i++; break;
                case CH_SIZE_LF:
                    if (c != '\n' || !ch_digits) { error = SLWEB_ERR_MALFORMED; ch_state = CH_DONE; break; }
                    i++; ch_remaining = ch_size;
                    ch_state = ch_size ? CH_DATA : CH_TRAILER;
                    ch_size = 0; ch_digits = 0;
                    break;
                case CH_DATA: {
                    size_t take = dn - i < ch_remaining ? dn - i : ch_remaining;
                    if (sink_add(&sink, data + i, take) < 0) { error = SLWEB_ERR_NOMEM; ch_state = CH_DONE; break; }
                    i += take; ch_remaining -= take;
                    if (!ch_remaining) ch_state = CH_CR;
                    break;
                }
                case CH_CR: if (c == '\r') { ch_state = CH_LF; i++; } else { error = SLWEB_ERR_MALFORMED; ch_state = CH_DONE; } break;
                case CH_LF: if (c == '\n') { ch_state = CH_SIZE; i++; } else { error = SLWEB_ERR_MALFORMED; ch_state = CH_DONE; } break;
                case CH_TRAILER:                                          /* skip trailers until blank line */
                    if (c == '\n') { ch_state = CH_DONE; }               /* simplification: first LF ends */
                    i++;
                    break;
                }
            }
            if (ch_state == CH_DONE) { done = 1; }
        } else {
            if (sink_add(&sink, data, dn) < 0) { error = SLWEB_ERR_NOMEM; break; }
            if (content_length >= 0 && (int)sink.len >= content_length) { done = 1; }
            if (sink.truncated) { done = 1; }
        }
        if (o->progress) o->progress(sink.len, res->content_length);
        if (sink.truncated) break;
    }

    st_close(&st);
    res->elapsed_ms = (uint32_t)(slweb_time_ms() - t0);
    res->body = sink.buf; res->body_len = sink.len;
    if (sink.truncated) res->truncated = 1;
    if (!have_head && !error) error = SLWEB_ERR_TIMEOUT;
    if (sink.oom && !error) error = SLWEB_ERR_NOMEM;
    return error;
}

int slweb_http_request(const char *url, const struct slweb_http_opts *opts, struct slweb_response *res)
{
    struct slweb_http_opts def; memset(&def, 0, sizeof(def));
    if (!opts) opts = &def;
    memset(res, 0, sizeof(*res));
    res->content_length = -1; res->tls_alert = -1;
    char cur[256];
    sl_strlcpy(cur, url, sizeof(cur));
    int hops = 0;
    for (;;) {
        struct slweb_url u;
        if (slweb_url_parse(cur, &u) != 0) { res->error = SLWEB_ERR_BADURL; return SLWEB_ERR_BADURL; }
        sl_strlcpy(res->final_url, cur, sizeof(res->final_url));
        struct slweb_response attempt; memset(&attempt, 0, sizeof(attempt));
        attempt.content_length = -1; attempt.tls_alert = -1;
        int err = single_request(&u, opts, &attempt);
        int redirects = res->redirects;
        *res = attempt;
        res->redirects = redirects;
        sl_strlcpy(res->final_url, cur, sizeof(res->final_url));
        res->error = err;
        if (err && !(err == SLWEB_ERR_TIMEOUT && res->status)) return err;
        int st = res->status;
        if (opts->follow_redirects && (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && res->location[0]) {
            if (++hops > opts->follow_redirects) { res->error = SLWEB_ERR_REDIRECTS; return SLWEB_ERR_REDIRECTS; }
            char next[256];
            if (slweb_url_resolve(cur, res->location, next, sizeof(next)) != 0) { res->error = SLWEB_ERR_BADURL; return SLWEB_ERR_BADURL; }
            if (!opts->quiet) printf("  -> %d redirect to %s\n", st, next);
            slweb_http_response_free(res);
            sl_strlcpy(cur, next, sizeof(cur));
            res->redirects = hops;
            continue;
        }
        return err;
    }
}
