/*
 * httpd.c -- single-threaded, event-driven HTTP server for StarlightOS.
 *
 *   httpd [-c] [PORT]       serve the current directory (default port 8080)
 *
 * One kpoll() call watches the listening socket and every client at once, so
 * one process can serve many slow connections and keep long-lived
 * Server-Sent-Events streams open without a thread per client.
 *
 * Chat room: when the current directory is named "httpd_test" (or -c is
 * given) the server also exposes
 *     GET  /api/stream?n=NAME   text/event-stream: history replay, then live
 *                               messages and presence updates (real time)
 *     POST /api/send            n=NAME&m=TEXT  (urlencoded body or query)
 * and the static files in that folder (INDEX.HTM) are the chat UI.
 */
#include "version.h"
#include "shell.h"
#include "net.h"
#include "poll.h"
#include "vfs.h"
#include "kstring.h"
#include "kprintf.h"
#include "kheap.h"
#include "ktime.h"
#include "ksched.h"

#define MAX_CONN      12
#define REQ_MAX       2048
#define SSE_BUF       800
#define CHAT_RING     64
#define NAME_MAX_LEN  20
#define TEXT_MAX_LEN  240
#define READ_TIMEOUT  10000u
#define SEND_TIMEOUT  20000u
#define PING_EVERY    15000u

enum { C_FREE = 0, C_READ, C_SEND, C_SSE };

struct conn {
    int       state;
    int       fd;
    uint32_t  last_act;
    char      req[REQ_MAX];
    int       req_len;
    /* pending output: a heap response (C_SEND) or sbuf (C_SSE) */
    const char *out;
    char     *out_heap;                 /* owned copy to free, or NULL */
    size_t    out_len, out_off;
    char      sbuf[SSE_BUF];
    /* SSE state */
    uint32_t  next_id, replay_until, pres_sent, last_ping;
    char      name[NAME_MAX_LEN + 1];
    uint8_t   peer_ip[4]; uint16_t peer_port;
    int       keepalive_close;
};

struct chat_msg { uint32_t id; char kind; char name[NAME_MAX_LEN + 1]; char text[TEXT_MAX_LEN + 1]; };

static struct conn conns[MAX_CONN];
static struct chat_msg ring[CHAT_RING];
static uint32_t next_msg_id = 1;
static uint32_t presence_ver = 1;
static int      chat_on;
static unsigned served;

/* ---------------------------------------------------------------- helpers */

static const char *mime_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (!strcasecmp(dot, ".HTM") || !strcasecmp(dot, ".HTML")) return "text/html; charset=utf-8";
    if (!strcasecmp(dot, ".TXT") || !strcasecmp(dot, ".MD") || !strcasecmp(dot, ".SH") || !strcasecmp(dot, ".MML")) return "text/plain";
    if (!strcasecmp(dot, ".CSS")) return "text/css";
    if (!strcasecmp(dot, ".JS")) return "text/javascript";
    if (!strcasecmp(dot, ".JSON")) return "application/json";
    if (!strcasecmp(dot, ".WAV")) return "audio/wav";
    if (!strcasecmp(dot, ".PNG")) return "image/png";
    if (!strcasecmp(dot, ".JPG")) return "image/jpeg";
    return "application/octet-stream";
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Finds key in an "a=b&c=d" string and url-decodes its value into out. */
static int form_get(const char *s, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    while (*s) {
        if (!strncmp(s, key, kl) && s[kl] == '=') {
            s += kl + 1;
            size_t o = 0;
            while (*s && *s != '&') {
                int ch = (unsigned char)*s++;
                if (ch == '+') ch = ' ';
                else if (ch == '%' && hexval(s[0]) >= 0 && hexval(s[1]) >= 0) {
                    ch = hexval(s[0]) * 16 + hexval(s[1]); s += 2;
                }
                if (o + 1 < cap) out[o++] = (char)ch;
            }
            out[o] = 0;
            return 1;
        }
        while (*s && *s != '&') s++;
        if (*s == '&') s++;
    }
    return 0;
}

/* Collapse to printable text; names additionally lose anything odd. */
static void clean_text(char *s, int is_name)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        unsigned char ch = (unsigned char)*r;
        if (ch < 32 || ch == 127) ch = ' ';
        if (is_name && !(kisalnum(ch) || ch == ' ' || ch == '_' || ch == '-' || ch == '.')) continue;
        if (ch == ' ' && (w == s || w[-1] == ' ')) continue;       /* trim + squeeze */
        *w++ = (char)ch;
    }
    while (w > s && w[-1] == ' ') w--;
    *w = 0;
}

/* Appends a JSON-escaped copy of src to dst; returns the new length. */
static size_t json_cat(char *dst, size_t len, size_t cap, const char *src)
{
    for (; *src && len + 7 < cap; src++) {
        unsigned char ch = (unsigned char)*src;
        if (ch == '"' || ch == '\\') { dst[len++] = '\\'; dst[len++] = (char)ch; }
        else if (ch < 32) dst[len++] = ' ';
        else dst[len++] = (char)ch;
    }
    dst[len] = 0;
    return len;
}

/* ------------------------------------------------------------ connection */

static void out_free(struct conn *c)
{
    if (c->out_heap) kfree(c->out_heap);
    c->out_heap = 0; c->out = 0; c->out_len = c->out_off = 0;
}

static void chat_post(char kind, const char *name, const char *text);
static void chat_system(const char *who, const char *what);

static int sse_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CONN; i++) if (conns[i].state == C_SSE) n++;
    return n;
}

static void conn_close(struct conn *c)
{
    if (c->state == C_FREE) return;
    int was_sse = (c->state == C_SSE);
    char who[NAME_MAX_LEN + 1]; strlcpy(who, c->name, sizeof(who));
    kfd_close(c->fd);                 /* non-blocking: queues the FIN */
    out_free(c);
    c->state = C_FREE; c->fd = -1;
    if (was_sse) {
        presence_ver++;
        if (chat_on) chat_system(who[0] ? who : "someone", "left");
    }
}

/* Queue a complete heap-allocated HTTP response and switch to sending. */
static void respond(struct conn *c, int code, const char *text, const char *ctype,
                    const void *body, size_t len, int head_only, const char *extra)
{
    char *buf = (char *)kmalloc(len + 320);
    if (!buf) { conn_close(c); return; }
    int hl = ksnprintf(buf, 320, "HTTP/1.1 %d %s\r\nServer: StarlightOS/" STARLIGHTOS_VERSION
                       "\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\n%sConnection: close\r\n\r\n",
                       code, text, ctype, (unsigned)len, extra ? extra : "");
    if (hl < 0 || hl >= 320) hl = 319;
    size_t total = (size_t)hl;
    if (!head_only && len) { memcpy(buf + hl, body, len); total += len; }
    out_free(c);
    c->out_heap = buf; c->out = buf; c->out_len = total; c->out_off = 0;
    c->state = C_SEND;
    c->last_act = ktime_ms();
}

/* Non-blocking flush. 1 = drained, 0 = would block, -1 = peer gone. */
static int flush_out(struct conn *c)
{
    while (c->out_off < c->out_len) {
        int n = kfd_write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
        if (n == KE_AGAIN) return 0;
        if (n <= 0) return -1;
        c->out_off += (size_t)n;
        c->last_act = ktime_ms();
    }
    return 1;
}

/* ------------------------------------------------------------------ chat */

static uint32_t oldest_id(void) { return next_msg_id > CHAT_RING ? next_msg_id - CHAT_RING : 1; }

static void chat_post(char kind, const char *name, const char *text)
{
    struct chat_msg *m = &ring[next_msg_id % CHAT_RING];
    m->id = next_msg_id++;
    m->kind = kind;
    strlcpy(m->name, name, sizeof(m->name));
    strlcpy(m->text, text, sizeof(m->text));
}

static void chat_system(const char *who, const char *what)
{
    char t[TEXT_MAX_LEN + 1];
    ksnprintf(t, sizeof(t), "%s %s", who, what);
    chat_post('s', "", t);
    kprintf("[chat] * %s\n", t);
}

/* Builds the next SSE record for c (if any) into sbuf. Returns 1 if queued. */
static int sse_next(struct conn *c)
{
    size_t n;
    if (c->pres_sent != presence_ver) {
        n = (size_t)ksnprintf(c->sbuf, SSE_BUF, "event: presence\ndata: %d\n\n", sse_count());
        c->pres_sent = presence_ver;
    } else if (c->next_id < next_msg_id) {
        if (c->next_id < oldest_id()) c->next_id = oldest_id();     /* slow client fell off the ring */
        struct chat_msg *m = &ring[c->next_id % CHAT_RING];
        n = (size_t)ksnprintf(c->sbuf, SSE_BUF, "id: %u\ndata: {\"k\":\"%c\",\"h\":%d,\"n\":\"", (unsigned)m->id, m->kind,
                              m->id < c->replay_until ? 1 : 0);
        n = json_cat(c->sbuf, n, SSE_BUF, m->name);
        n += (size_t)ksnprintf(c->sbuf + n, SSE_BUF - n, "\",\"t\":\"");
        n = json_cat(c->sbuf, n, SSE_BUF, m->text);
        n += (size_t)ksnprintf(c->sbuf + n, SSE_BUF - n, "\"}\n\n");
        c->next_id++;
    } else if ((uint32_t)(ktime_ms() - c->last_ping) >= PING_EVERY) {
        n = (size_t)ksnprintf(c->sbuf, SSE_BUF, ": ping\n\n");
        c->last_ping = ktime_ms();
    } else return 0;
    c->out = c->sbuf; c->out_len = n; c->out_off = 0;
    return 1;
}

/* Push everything this stream is owed until the socket would block. */
static void sse_pump(struct conn *c)
{
    for (;;) {
        if (c->out_off >= c->out_len && !sse_next(c)) return;
        int r = flush_out(c);
        if (r < 0) { conn_close(c); return; }
        if (r == 0) return;
    }
}

static void sse_pump_all(void)
{
    for (int i = 0; i < MAX_CONN; i++) if (conns[i].state == C_SSE) sse_pump(&conns[i]);
}

static void chat_start_stream(struct conn *c, const char *query, const char *hdrs)
{
    char nm[NAME_MAX_LEN + 8] = "";
    form_get(query, "n", nm, sizeof(nm));
    clean_text(nm, 1);
    if (!nm[0]) strlcpy(nm, "guest", sizeof(nm));
    nm[NAME_MAX_LEN] = 0;
    strlcpy(c->name, nm, sizeof(c->name));

    static const char head[] =
        "HTTP/1.1 200 OK\r\nServer: StarlightOS/" STARLIGHTOS_VERSION "\r\nContent-Type: text/event-stream\r\n"
        "Cache-Control: no-store\r\nConnection: keep-alive\r\n\r\nretry: 2000\n\n";
    strlcpy(c->sbuf, head, SSE_BUF);
    c->out = c->sbuf; c->out_len = strlen(head); c->out_off = 0;

    /* Last-Event-ID (sent by EventSource on auto-reconnect) resumes where it left off. */
    uint32_t last = 0;
    const char *h = strcasestr(hdrs, "\nLast-Event-ID:");
    if (h) { h += 15; while (*h == ' ') h++; last = (uint32_t)kstrtoul(h, 0, 10); }
    uint32_t start = last ? last + 1 : (next_msg_id > 30 ? next_msg_id - 30 : 1);
    if (start < oldest_id()) start = oldest_id();
    c->next_id = start;
    c->replay_until = next_msg_id;
    c->pres_sent = 0;
    c->last_ping = ktime_ms();
    c->state = C_SSE;
    presence_ver++;
    chat_system(c->name, "joined");
}

static void chat_send(struct conn *c, const char *params)
{
    char nm[NAME_MAX_LEN + 8] = "", tx[TEXT_MAX_LEN + 8] = "";
    form_get(params, "n", nm, sizeof(nm));
    form_get(params, "m", tx, sizeof(tx));
    clean_text(nm, 1); clean_text(tx, 0);
    nm[NAME_MAX_LEN] = 0; tx[TEXT_MAX_LEN] = 0;
    if (!tx[0]) { respond(c, 400, "Bad Request", "text/plain", "empty message\n", 14, 0, 0); return; }
    if (!nm[0]) strlcpy(nm, "guest", sizeof(nm));
    chat_post('m', nm, tx);
    kprintf("[chat] %s: %s\n", nm, tx);
    respond(c, 204, "No Content", "text/plain", "", 0, 0, 0);
    sse_pump_all();                       /* real time: fan out right now, not on the next tick */
}

/* ---------------------------------------------------------- static files */

static void serve_listing(struct conn *c, const char *dir, int head_only)
{
    static char names[64][VFS_NAME_MAX + 1];
    static uint8_t isdir[64];
    char saved[VFS_PATH_MAX]; strlcpy(saved, vfs_get_cwd(), sizeof(saved));
    if (dir[0]) vfs_cd(dir);
    int n = vfs_get_entries(names, isdir, 64);
    char *page = (char *)kmalloc(4096);
    if (!page) { respond(c, 500, "Error", "text/plain", "oom\n", 4, 0, 0); vfs_cd(saved); return; }
    int o = ksnprintf(page, 4096, "<html><head><title>Index of /%s</title></head><body><h1>Index of /%s</h1><ul>", dir, dir);
    for (int i = 0; i < n && o < 3600; i++)
        o += ksnprintf(page + o, 4096 - (size_t)o, "<li><a href=\"/%s%s%s%s\">%s%s</a>", dir, dir[0] ? "/" : "", names[i],
                       isdir[i] ? "/" : "", names[i], isdir[i] ? "/" : "");
    o += ksnprintf(page + o, 4096 - (size_t)o, "</ul><p><i>StarlightOS httpd</i></body></html>");
    vfs_cd(saved);
    respond(c, 200, "OK", "text/html; charset=utf-8", page, (size_t)o, head_only, 0);
    kfree(page);
}

static int dispatch(struct conn *c)
{
    char method[8] = "", path[100] = "", query[300] = "";
    const char *p = c->req; int mi = 0, pi = 0, qi = 0;
    while (*p && *p != ' ' && mi < 7) method[mi++] = *p++;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && *p != '?' && *p != '\r' && *p != '\n' && pi < 99) path[pi++] = *p++;
    if (*p == '?') { p++; while (*p && *p != ' ' && *p != '\r' && *p != '\n' && qi < 299) query[qi++] = *p++; }
    const char *hdrs = strchr(c->req, '\n'); if (!hdrs) hdrs = c->req;
    const char *body = strstr(c->req, "\r\n\r\n"); body = body ? body + 4 : "";

    int head_only = !strcmp(method, "HEAD");
    int code = 200;
    int is_get = !strcmp(method, "GET") || head_only, is_post = !strcmp(method, "POST");

    if (chat_on && !strcmp(path, "/api/stream") && is_get) {
        chat_start_stream(c, query, hdrs);
        kprintf("%u.%u.%u.%u:%u  SSE %s\n", c->peer_ip[0], c->peer_ip[1], c->peer_ip[2], c->peer_ip[3], c->peer_port, c->name);
        sse_pump_all();
        return 0;
    }
    if (chat_on && !strcmp(path, "/api/send") && (is_get || is_post)) {
        chat_send(c, is_post ? body : query);
        return 0;
    }
    if (!is_get) { code = 405; respond(c, 405, "Method Not Allowed", "text/plain", "405\n", 4, 0, 0); goto log; }

    {
        char name[VFS_PATH_MAX]; size_t o = 0;
        const char *s = path[0] == '/' ? path + 1 : path;
        while (*s && o + 1 < sizeof(name)) {                               /* %XX decode */
            if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) { name[o++] = (char)(hexval(s[1]) * 16 + hexval(s[2])); s += 3; }
            else name[o++] = *s++;
        }
        name[o] = 0;
        while (o && name[o - 1] == '/') name[--o] = 0;
        if (strstr(name, "..")) { code = 403; respond(c, 403, "Forbidden", "text/plain", "403\n", 4, 0, 0); goto log; }
        if (!name[0] && vfs_exists("INDEX.HTM")) strlcpy(name, "INDEX.HTM", sizeof(name));
        if (!name[0] || vfs_is_dir(name)) { serve_listing(c, name, head_only); goto log; }
        size_t len; char *data = shell_read_file(name, &len);
        if (!data) { code = 404; respond(c, 404, "Not Found", "text/html", "<h1>404 Not Found</h1>", 22, 0, 0); goto log; }
        respond(c, 200, "OK", mime_for(name), data, len, head_only, 0);
        kfree(data);
    }
log:
    kprintf("%u.%u.%u.%u:%u  %s %s -> %d\n", c->peer_ip[0], c->peer_ip[1], c->peer_ip[2], c->peer_ip[3], c->peer_port, method, path, code);
    served++;
    return 0;
}

/* Is the request complete (headers + Content-Length bytes of body)? -1 = too big. */
static int request_complete(struct conn *c)
{
    char *e = strstr(c->req, "\r\n\r\n");
    if (!e) return 0;
    size_t hdr_len = (size_t)(e - c->req) + 4;
    const char *cl = strcasestr(c->req, "\ncontent-length:");
    if (cl && cl < e) {
        cl += 16; while (*cl == ' ') cl++;
        uint32_t want = (uint32_t)kstrtoul(cl, 0, 10);
        if (hdr_len + want >= REQ_MAX) return -1;
        if ((size_t)c->req_len < hdr_len + want) return 0;
    }
    return 1;
}

static void conn_service(struct conn *c, short rev)
{
    if (rev & POLLNVAL) { conn_close(c); return; }

    switch (c->state) {
    case C_READ:
        if (rev & (POLLIN | POLLHUP | POLLERR)) {
            int n = kfd_read(c->fd, c->req + c->req_len, (size_t)(REQ_MAX - 1 - c->req_len));
            if (n == KE_AGAIN) return;
            if (n <= 0) { conn_close(c); return; }
            c->req_len += n; c->req[c->req_len] = 0; c->last_act = ktime_ms();
            int rc = request_complete(c);
            if (rc < 0) { respond(c, 413, "Too Large", "text/plain", "413\n", 4, 0, 0); return; }
            if (rc == 1) dispatch(c);
            else if (c->req_len >= REQ_MAX - 1) respond(c, 413, "Too Large", "text/plain", "413\n", 4, 0, 0);
        }
        break;
    case C_SEND:
        if (rev & (POLLERR | POLLHUP)) { conn_close(c); return; }
        if (rev & POLLOUT) {
            int r = flush_out(c);
            if (r != 0) conn_close(c);            /* drained (Connection: close) or peer gone */
        }
        break;
    case C_SSE:
        if (rev & (POLLIN | POLLHUP | POLLERR)) {         /* clients never send after the request: any read = EOF/err */
            char junk[64];
            int n = kfd_read(c->fd, junk, sizeof(junk));
            if (n != KE_AGAIN && n <= 0) { conn_close(c); return; }
        }
        if (c->state == C_SSE) sse_pump(c);
        break;
    default: break;
    }
}

/* ----------------------------------------------------------------- main */

int httpd_main(int argc, char **argv)
{
    uint32_t port = 8080;
    int force_chat = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c")) force_chat = 1;
        else if (kparse_uint(argv[i], &port) != 0) { kprintf("usage: httpd [-c] [PORT]\n"); return 1; }
    }
    if (!net_configured()) { kprintf("httpd: network is not configured yet (try `ip dhcp`)\n"); return 1; }

    const char *cwd = vfs_get_cwd();
    const char *base = strrchr(cwd, '/'); base = base ? base + 1 : cwd;
    chat_on = force_chat || !strcmp(base, "httpd_test");

    int ls = tcp_listen((uint16_t)port);
    if (ls < 0) { kprintf("httpd: cannot listen on port %u\n", (unsigned)port); return 1; }
    int lfd = kfd_open_listener(ls);
    if (lfd < 0) { tcp_close(ls); kprintf("httpd: out of descriptors\n"); return 1; }

    char ip[16]; net_fmt_ip(ip, net_get_config()->ip);
    kprintf("Serving %s on http://%s:%u/  (Ctrl-C to stop)\n", cwd, ip, (unsigned)port);
    if (chat_on) kprintf("Chat room enabled: open the URL in a browser (several tabs = several users).\n");

    for (int i = 0; i < MAX_CONN; i++) { conns[i].state = C_FREE; conns[i].fd = -1; conns[i].out_heap = 0; }
    next_msg_id = 1; presence_ver = 1; served = 0;
    if (chat_on) chat_post('s', "", "Welcome to StarlightOS chat. Messages are relayed live by the kernel's poll() loop.");

    struct pollfd pfd[1 + MAX_CONN];
    struct conn *owner[1 + MAX_CONN];

    while (!kabort_requested()) {
        unsigned n = 0;
        pfd[n].fd = lfd; pfd[n].events = POLLIN; owner[n] = 0; n++;
        for (int i = 0; i < MAX_CONN; i++) {
            struct conn *c = &conns[i];
            if (c->state == C_FREE) continue;
            short ev = 0;
            if (c->state == C_READ) ev = POLLIN;
            else if (c->state == C_SEND) ev = POLLOUT;
            else ev = (short)(POLLIN | (c->out_off < c->out_len ? POLLOUT : 0));
            pfd[n].fd = c->fd; pfd[n].events = ev; owner[n] = c; n++;
        }

        int r = kpoll(pfd, n, 1000);            /* sleeps in hlt; woken by tcp_input() */
        if (r == KE_INTR) break;

        if (r > 0 && pfd[0].revents & POLLIN) {
            for (;;) {
                int cfd = kfd_accept(lfd);
                if (cfd < 0) break;
                struct conn *c = 0;
                for (int i = 0; i < MAX_CONN; i++) if (conns[i].state == C_FREE) { c = &conns[i]; break; }
                if (!c) { kfd_close(cfd); continue; }                 /* table full: shed load */
                memset(c, 0, sizeof(*c));
                c->state = C_READ; c->fd = cfd; c->last_act = ktime_ms();
                tcp_peer(kfd_tcp_handle(cfd), c->peer_ip, &c->peer_port);
            }
        }
        for (unsigned i = 1; i < n; i++)
            if (pfd[i].revents && owner[i]->state != C_FREE && owner[i]->fd == pfd[i].fd)
                conn_service(owner[i], pfd[i].revents);

        /* housekeeping: idle timeouts and SSE heartbeats */
        uint32_t now = ktime_ms();
        for (int i = 0; i < MAX_CONN; i++) {
            struct conn *c = &conns[i];
            if (c->state == C_READ && (uint32_t)(now - c->last_act) > READ_TIMEOUT) conn_close(c);
            else if (c->state == C_SEND && (uint32_t)(now - c->last_act) > SEND_TIMEOUT) conn_close(c);
            else if (c->state == C_SSE) sse_pump(c);
        }
    }

    for (int i = 0; i < MAX_CONN; i++) if (conns[i].state != C_FREE) { chat_on = 0; conn_close(&conns[i]); }
    kfd_close(lfd);
    kprintf("httpd stopped after %u request(s).\n", served);
    return 0;
}
