/*
 * Host test driver for libslweb (M3). Compiles libslweb/url.c + html.c + http.c
 * on any host with a scripted, in-memory replacement for the M2 socket syscalls
 * (sl_socket/sl_connect/sl_send/sl_recv/sl_poll/sl_resolve), so the HTTP parser,
 * redirects and chunked decoding can be tested without QEMU.
 *
 * Modes:
 *   slweb_host                     stdin HTML  -> stdout text      (html_test.py)
 *   slweb_host html                same, explicit
 *   slweb_host url   URL           print host=/port=/https=/path=
 *   slweb_host http  URL [HOPS]    stdin holds the scripted server reply; each
 *                                  "=== " separated block is used for one TCP
 *                                  connection (redirect hops consume more)
 *
 * Scripted socket semantics: a non-empty block reads out then reports EOF; an
 * empty block makes sl_poll() time out (SLWEB_ERR_TIMEOUT). Port 1 is refused
 * and host "bad.invalid" does not resolve, so error paths are reachable.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include "slweb.h"
#include "user/slnet.h"

/* ------------------------------------------------------------------ scripted sockets */
#define MAX_SCRIPT (1 << 20)
#define MAX_BLOCKS 16
#define MAX_REQ    (16 * 1024)

static unsigned char script[MAX_SCRIPT];
static size_t  script_len;
static unsigned char *blocks[MAX_BLOCKS];
static size_t  block_len[MAX_BLOCKS];
static int     nblocks, block_next;
static unsigned char *cur;
static size_t  cur_len, cur_pos;

static unsigned char req_buf[MAX_REQ];
static size_t  req_len;
static int     g_port;
static int     next_fd = 1000;

/* Split the script on the "@@@" marker between scripted connections. */
static void load_script(void)
{
    script_len = fread(script, 1, sizeof script, stdin);
    nblocks = 0; block_next = 0;
    const char *sep = "@@@";
    size_t seplen = strlen(sep);
    unsigned char *p = script, *end = script + script_len;
    unsigned char *start = p;
    while (nblocks < MAX_BLOCKS) {
        unsigned char *hit = NULL;
        for (unsigned char *q = p; q + seplen <= end; q++) {
            if (memcmp(q, sep, seplen) == 0) { hit = q; break; }
        }
        if (!hit) break;
        blocks[nblocks] = start; block_len[nblocks] = (size_t)(hit - start); nblocks++;
        p = hit + seplen; start = p;
    }
    blocks[nblocks] = start; block_len[nblocks] = (size_t)(end - start); nblocks++;
    if (nblocks == 0) { blocks[0] = script; block_len[0] = script_len; nblocks = 1; }
}

int sl_socket(int domain, int type, int protocol)
{
    (void)type; (void)protocol;
    if (domain != SL_AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (block_next < nblocks) { cur = blocks[block_next]; cur_len = block_len[block_next]; block_next++; }
    else { cur = script + script_len; cur_len = 0; }
    cur_pos = 0;
    return next_fd++;
}

int sl_connect(int fd, const struct sl_sockaddr_in *addr)
{
    (void)fd;
    g_port = (int)(uint16_t)((addr->sin_port >> 8) | (addr->sin_port << 8));
    if (g_port == 1) { errno = ECONNREFUSED; return -1; }
    return 0;
}

long sl_send(int fd, const void *buf, size_t len)
{
    (void)fd;
    if (req_len + len < sizeof req_buf) { memcpy(req_buf + req_len, buf, len); req_len += len; req_buf[req_len] = 0; }
    return (long)len;
}

long sl_recv(int fd, void *buf, size_t len)
{
    (void)fd;
    if (cur_pos >= cur_len) return 0;                 /* EOF */
    size_t n = cur_len - cur_pos;
    if (n > len) n = len;
    memcpy(buf, cur + cur_pos, n);
    cur_pos += n;
    return (long)n;
}

int sl_poll(struct sl_pollfd *fds, int nfds, int timeout_ms)
{
    (void)timeout_ms;
    if (nfds < 1) return 0;
    if (cur_len == 0) return 0;                        /* no reply: read times out */
    fds[0].revents = SL_POLLIN;
    return 1;
}

int sl_resolve(const char *host, uint8_t out_ip[4])
{
    if (strcmp(host, "bad.invalid") == 0) { errno = ETIMEDOUT; return -1; }
    out_ip[0] = 10; out_ip[1] = 0; out_ip[2] = 2; out_ip[3] = 2;
    return 0;
}

/* Deterministic bytes; the scripted sockets never complete a TLS handshake, so
 * this only has to be non-zero and stable (no /dev/urandom dependency). */
int sl_random(void *buf, size_t len)
{
    unsigned char *p = (unsigned char *)buf;
    for (size_t i = 0; i < len; i++) p[i] = (unsigned char)(0x5Au ^ (unsigned)(i * 31 + 7));
    return 0;
}

/* ------------------------------------------------------------------ helpers */
static void print_escaped(const char *tag, const void *p, size_t n)
{
    const unsigned char *s = p;
    printf("%s=", tag);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '\n') printf("\\n");
        else if (c == '\r') printf("\\r");
        else if (c == '\t') printf("\\t");
        else if (c == '\\') printf("\\\\");
        else putchar(c);
    }
    putchar('\n');
}

static void html_mode(void)
{
    static char in[1 << 20], out[1 << 21];
    size_t n = fread(in, 1, sizeof in - 1, stdin);
    size_t r = slweb_html_to_text(in, n, out, sizeof out, 78, 1);
    fwrite(out, 1, r, stdout);
}

static void url_mode(const char *url)
{
    struct slweb_url u;
    int rc = slweb_url_parse(url, &u);
    if (rc != 0) { printf("rc=-1\n"); return; }
    printf("rc=0\nhost=%s\nport=%u\nhttps=%d\npath=%s\n", u.host, (unsigned)u.port, u.https, u.path);
}

static void resolve_mode(const char *base, const char *loc)
{
    char out[512];
    int rc = slweb_url_resolve(base, loc, out, sizeof out);
    if (rc != 0) { printf("rc=-1\n"); return; }
    printf("rc=0\nurl=%s\n", out);
}

static void links_mode(void)
{
    static char in[1 << 20], out[1 << 21];
    size_t n = fread(in, 1, sizeof in - 1, stdin);
    struct slweb_link links[SLWEB_MAX_LINKS];
    size_t nl = 0;
    size_t r = slweb_html_to_text_ex(in, n, out, sizeof out, 78, 1, links, SLWEB_MAX_LINKS, &nl);
    printf("n_links=%zu\n", nl);
    for (size_t i = 0; i < nl; i++) printf("link%zu=%s\n", i, links[i].href);
    printf("text_len=%zu\n", r);
    print_escaped("text", out, r);
}

/* ------------------------------------------------------------------ M5: DOM + layout */
static void dom_dump(const struct slweb_node *n, int depth)
{
    for (; n; n = n->next_sibling) {
        for (int i = 0; i < depth; i++) putchar('.');
        if (n->type == SLWEB_NODE_TEXT) {
            printf("text=");
            for (size_t i = 0; i < n->text_len; i++) {
                unsigned char c = (unsigned char)n->text[i];
                if (c == '\n') printf("\\n");
                else if (c == '\t') printf("\\t");
                else if (c == '\\') printf("\\\\");
                else putchar(c);
            }
            putchar('\n');
        } else if (n->type == SLWEB_NODE_ELEMENT) {
            printf("<%s", n->tag);
            for (int i = 0; i < n->nattrs; i++) printf(" %s=%s", n->attrs[i].name, n->attrs[i].value);
            printf(">\n");
        } else {
            printf("#document\n");
        }
        dom_dump(n->first_child, depth + 1);
    }
}

static void dom_mode(void)
{
    static char in[1 << 20];
    size_t n = fread(in, 1, sizeof in - 1, stdin);
    struct slweb_node *doc = slweb_dom_parse(in, n);
    dom_dump(doc, 0);
    slweb_dom_free(doc);
}

/* ---- M6: inline data: images so the test build needs no network ---- */
#define HOST_IMAGES 16
static struct { const struct slweb_node *node; struct slweb_image img; } host_images[HOST_IMAGES];
static int host_nimages;

static void collect_data_images(const struct slweb_node *n)
{
    for (; n; n = n->next_sibling) {
        if (n->type == SLWEB_NODE_ELEMENT && strcmp(n->tag, "img") == 0) {
            const char *src = slweb_node_attr(n, "src");
            if (src && strncmp(src, "data:", 5) == 0 && host_nimages < HOST_IMAGES &&
                slweb_image_data_uri(src, &host_images[host_nimages].img) == 0) {
                host_images[host_nimages].node = n;
                host_nimages++;
            }
        }
        if (n->first_child) collect_data_images(n->first_child);
    }
}

static void layout_mode(int width)
{
    static char in[1 << 20];
    static struct slweb_box boxes[SLWEB_MAX_BOXES];
    struct slweb_link links[SLWEB_MAX_LINKS];
    size_t n = fread(in, 1, sizeof in - 1, stdin);
    in[n] = 0;
    struct slweb_node *doc = slweb_dom_parse(in, n);

    struct slweb_css *css = slweb_css_parse(doc);
    host_nimages = 0;
    collect_data_images(doc);
    static struct slweb_image_entry entries[HOST_IMAGES];
    for (int i = 0; i < host_nimages; i++) {
        entries[i].node = host_images[i].node;
        entries[i].image = &host_images[i].img;
    }
    struct slweb_layout_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.css = css;
    opts.images = entries;
    opts.n_images = (size_t)host_nimages;

    size_t nl = 0;
    int dw = 0, dh = 0;
    int nb = slweb_layout_ex(doc, width, &opts, boxes, SLWEB_MAX_BOXES,
                             links, SLWEB_MAX_LINKS, &nl, &dw, &dh);
    printf("width=%d\nheight=%d\nnboxes=%d\nn_links=%zu\nrules=%d\nimages=%d\n",
           dw, dh, nb, nl, slweb_css_rule_count(css), host_nimages);
    for (size_t i = 0; i < nl; i++) printf("link%zu=%s\n", i, links[i].href);
    for (int i = 0; i < nb; i++) {
        struct slweb_box *b = &boxes[i];
        printf("box%d kind=%d x=%d y=%d w=%d h=%d scale=%d bold=%d italic=%d link=%d marker=%d",
               i, b->kind, b->x, b->y, b->w, b->h, b->scale, b->bold, b->italic, b->link, b->marker);
        if (b->fg_set) printf(" fg=%06x", (unsigned)b->fg & 0xFFFFFF);
        if (b->bg_set) printf(" bg=%06x", (unsigned)b->bg & 0xFFFFFF);
        if (b->underline) printf(" u=1");
        if (b->image) printf(" img=%dx%d", b->image->w, b->image->h);
        if (b->node) {
            printf(" el=%s", b->node->tag);
            const char *nm = slweb_node_attr(b->node, "name");
            if (nm) printf(" name=%s", nm);
            const char *ty = slweb_node_attr(b->node, "type");
            if (ty) printf(" type=%s", ty);
        }
        if (b->text_len) {
            printf(" text=");
            for (int k = 0; k < b->text_len; k++) {
                unsigned char c = (unsigned char)b->text[k];
                if (c == '\n') printf("\\n");
                else if (c == '\\') printf("\\\\");
                else putchar(c);
            }
        }
        putchar('\n');
    }
    slweb_css_free(css);
    slweb_dom_free(doc);
}

/* slweb_host image  : decode a PNG / BS64 blob from stdin */
static void image_mode(void)
{
    static unsigned char in[4 << 20];
    size_t n = fread(in, 1, sizeof in, stdin);
    const char *fmt = slweb_image_format(in, n);
    printf("format=%s\n", fmt ? fmt : "-");
    struct slweb_image im;
    memset(&im, 0, sizeof im);
    int rc = slweb_image_decode(in, n, &im);
    printf("rc=%d\n", rc);
    if (rc == 0) {
        printf("w=%d\nh=%d\nhas_alpha=%d\n", im.w, im.h, im.has_alpha);
        unsigned long sum = 0;
        long total = (long)im.w * im.h;
        for (long i = 0; i < total; i++) {
            uint32_t p = im.pixels[i];
            sum = (sum + ((p >> 16) & 0xFF) + ((p >> 8) & 0xFF) + (p & 0xFF)) & 0xFFFFFFFFul;
        }
        printf("sum=%lu\n", sum);
        printf("p0=%08lx\n", (unsigned long)im.pixels[0]);
        if (total > 1) printf("plast=%08lx\n", (unsigned long)im.pixels[total - 1]);
        slweb_image_free(&im);
    }
}

/* slweb_host css    : compile stdin HTML, print the cascade for each element */
static void css_dump(const struct slweb_css *css, const struct slweb_node *n)
{
    for (; n; n = n->next_sibling) {
        if (n->type == SLWEB_NODE_ELEMENT) {
            struct slweb_style s;
            slweb_style_compute(css, n, &s);
            int any = s.color_set || s.bg_set || s.bold_set || s.italic_set ||
                      s.underline_set || s.scale_set || s.display_set || s.align_set ||
                      s.width_set || s.height_set;
            for (int i = 0; i < 4; i++) any = any || s.margin_set[i] || s.pad_set[i];
            if (any) {
                printf("el=%s", n->tag);
                const char *id = slweb_node_attr(n, "id");
                if (id) printf("#%s", id);
                const char *cl = slweb_node_attr(n, "class");
                if (cl) printf(".%s", cl);
                if (s.color_set)     printf(" color=%06x", (unsigned)s.color & 0xFFFFFF);
                if (s.bg_set)        printf(" bg=%06x",    (unsigned)s.bg & 0xFFFFFF);
                if (s.bold_set)      printf(" bold=%d",    s.bold);
                if (s.italic_set)    printf(" italic=%d",  s.italic);
                if (s.underline_set) printf(" underline=%d", s.underline);
                if (s.scale_set)     printf(" scale=%d",   s.scale);
                if (s.display_set)   printf(" display=%d", s.display_none ? 0 : 1);
                if (s.align_set)     printf(" align=%d",   s.align);
                if (s.margin_set[0] || s.margin_set[1] || s.margin_set[2] || s.margin_set[3])
                    printf(" margin=%d,%d,%d,%d", s.margin[0], s.margin[1], s.margin[2], s.margin[3]);
                if (s.pad_set[0] || s.pad_set[1] || s.pad_set[2] || s.pad_set[3])
                    printf(" pad=%d,%d,%d,%d", s.pad[0], s.pad[1], s.pad[2], s.pad[3]);
                if (s.width_set)  printf(" width=%d", s.width);
                if (s.height_set) printf(" height=%d", s.height);
                putchar('\n');
            }
        }
        if (n->first_child) css_dump(css, n->first_child);
    }
}

static void css_mode(void)
{
    static char in[1 << 20];
    size_t n = fread(in, 1, sizeof in - 1, stdin);
    in[n] = 0;
    struct slweb_node *doc = slweb_dom_parse(in, n);
    struct slweb_css *css = slweb_css_parse(doc);
    printf("rules=%d\n", slweb_css_rule_count(css));
    css_dump(css, doc);
    slweb_css_free(css);
    slweb_dom_free(doc);
}

/* slweb_host cache : a tiny in-memory HTTP cache; a line protocol on stdin
 *   put URL STATUS CTYPE BODY      (BODY is the rest of the line)
 *   get URL
 *   clear
 * Exercises the LRU/byte-budget eviction and the hit/miss counters. */
static char *cache_tok(char **p)
{
    while (**p == ' ' || **p == '\t') (*p)++;
    char *s = *p;
    while (**p && **p != ' ' && **p != '\t') (*p)++;
    if (**p) *(*p)++ = 0;
    return s;
}

static void cache_mode(void)
{
    struct slweb_cache *c = slweb_cache_new(3, 40);   /* tiny: force eviction */
    char line[2048];
    while (fgets(line, sizeof line, stdin)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (strncmp(line, "put ", 4) == 0) {
            char *p = line + 4;
            char *url = cache_tok(&p), *st = cache_tok(&p), *ct = cache_tok(&p);
            while (*p == ' ' || *p == '\t') p++;
            const char *body = p;
            int rc = slweb_cache_put(c, url, atoi(st), ct, (const uint8_t *)body, strlen(body));
            printf("put rc=%d count=%d bytes=%zu\n", rc, slweb_cache_count(c), slweb_cache_bytes(c));
        } else if (strncmp(line, "get ", 4) == 0) {
            const char *url = line + 4;
            const struct slweb_cache_entry *e = slweb_cache_get(c, url);
            if (e)
                printf("get url=%s hit status=%d type=%s len=%zu body=%s\n",
                       url, e->status, e->content_type, e->body_len,
                       e->body ? (const char *)e->body : "");
            else
                printf("get url=%s miss\n", url);
        } else if (strcmp(line, "clear") == 0) {
            slweb_cache_clear(c);
            printf("clear count=%d\n", slweb_cache_count(c));
        }
    }
    printf("hits=%d misses=%d count=%d bytes=%zu\n",
           slweb_cache_hits(c), slweb_cache_misses(c),
           slweb_cache_count(c), slweb_cache_bytes(c));
    slweb_cache_free(c);
}

/* slweb_host form PAGEURL : parse HTML from stdin, collect the controls of the
 * first <form> (or the whole document) using their value=/checked attributes,
 * then encode the query and resolve the submission. */
static int host_eq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

static const struct slweb_node *find_form(const struct slweb_node *n)
{
    for (; n; n = n->next_sibling) {
        if (n->type == SLWEB_NODE_ELEMENT && strcmp(n->tag, "form") == 0) return n;
        if (n->first_child) {
            const struct slweb_node *f = find_form(n->first_child);
            if (f) return f;
        }
    }
    return NULL;
}

/* The "selected" <option> of a <select>, or the first one. */
static const struct slweb_node *pick_option(const struct slweb_node *sel)
{
    const struct slweb_node *first = NULL;
    for (const struct slweb_node *o = sel->first_child; o; o = o->next_sibling) {
        if (o->type != SLWEB_NODE_ELEMENT || strcmp(o->tag, "option") != 0) continue;
        if (!first) first = o;
        if (slweb_node_attr(o, "selected")) return o;
    }
    return first;
}

/* The terminal value of a control from its attributes/children only (no live
 * edit state), matching what a browser sends for an untouched form. */
static int host_value(const struct slweb_node *n, void *user, char *out, size_t cap)
{
    (void)user;
    const char *type = slweb_node_attr(n, "type");
    const char *v = slweb_node_attr(n, "value");
    if (type && (host_eq_ci(type, "submit") || host_eq_ci(type, "reset") ||
                 host_eq_ci(type, "button") || host_eq_ci(type, "image"))) return -1;
    if (type && (host_eq_ci(type, "checkbox") || host_eq_ci(type, "radio"))) {
        if (!slweb_node_attr(n, "checked")) return -1;
        snprintf(out, cap, "%s", v ? v : "on");
        return 0;
    }
    if (strcmp(n->tag, "textarea") == 0) {
        const struct slweb_node *t = n->first_child;
        while (t && t->type != SLWEB_NODE_TEXT) t = t->next_sibling;
        snprintf(out, cap, "%s", t ? t->text : "");
        return 0;
    }
    if (strcmp(n->tag, "select") == 0) {
        const struct slweb_node *o = pick_option(n);
        const char *ov = o ? slweb_node_attr(o, "value") : NULL;
        if (!ov && o) {
            const struct slweb_node *t = o->first_child;
            while (t && t->type != SLWEB_NODE_TEXT) t = t->next_sibling;
            ov = t ? t->text : "";
        }
        snprintf(out, cap, "%s", ov ? ov : "");
        return 0;
    }
    snprintf(out, cap, "%s", v ? v : "");
    return 0;
}

static void form_mode(const char *page_url)
{
    static char in[1 << 20];
    size_t n = fread(in, 1, sizeof in - 1, stdin);
    in[n] = 0;
    struct slweb_node *doc = slweb_dom_parse(in, n);
    const struct slweb_node *form = find_form(doc);
    const struct slweb_node *scope = form ? form : doc;

    struct slweb_form_pair pairs[32];
    size_t np = slweb_form_collect(scope, host_value, NULL, pairs, 32);
    printf("n=%zu\n", np);
    for (size_t i = 0; i < np; i++)
        printf("pair%zu name=%s value=%s\n", i, pairs[i].name, pairs[i].value);

    char query[1024];
    int qr = slweb_form_query(pairs, np, query, sizeof query);
    printf("query rc=%d val=%s\n", qr, qr >= 0 ? query : "-");

    char url[512], body[512];
    int is_post = 0;
    int rc = slweb_form_submit(page_url, form, qr >= 0 ? query : "",
                               url, sizeof url, &is_post, body, sizeof body);
    printf("submit rc=%d method=%s\n", rc, is_post ? "post" : "get");
    printf("url=%s\n", rc == 0 ? url : "-");
    printf("body=%s\n", rc == 0 ? body : "-");
    slweb_dom_free(doc);
}

static void http_mode(const char *url, int hops, const char *method, const char *body)
{
    struct slweb_http_opts o;
    memset(&o, 0, sizeof o);
    o.follow_redirects = hops;
    o.quiet = 1;
    o.timeout_ms = 1000;
    o.method = (method && method[0]) ? method : NULL;
    o.body = body;
    o.body_len = body ? strlen(body) : 0;

    struct slweb_response r;
    int err = slweb_http_request(url, &o, &r);

    printf("error=%d\n", err);
    printf("error_text=%s\n", slweb_http_error_text(err));
    printf("status=%d\n", r.status);
    printf("status_text=%s\n", r.status_text);
    printf("content_type=%s\n", r.content_type);
    printf("location=%s\n", r.location);
    printf("final_url=%s\n", r.final_url);
    printf("content_length=%d\n", r.content_length);
    printf("redirects=%d\n", r.redirects);
    printf("truncated=%d\n", r.truncated);
    printf("tls_verified=%d\n", r.tls_verified);
    printf("tls_peer=%s\n", r.tls_peer);
    printf("tls_error=%d\n", r.tls_error);
    printf("tls_alert=%d\n", r.tls_alert);
    printf("body_len=%zu\n", r.body_len);
    if (r.body) print_escaped("body", r.body, r.body_len);
    else printf("body=\n");
    print_escaped("request", req_buf, req_len);
    slweb_http_response_free(&r);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "url") == 0) { url_mode(argv[2]); return 0; }
    if (argc >= 4 && strcmp(argv[1], "resolve") == 0) { resolve_mode(argv[2], argv[3]); return 0; }
    if (argc >= 2 && strcmp(argv[1], "links") == 0) { links_mode(); return 0; }
    if (argc >= 2 && strcmp(argv[1], "dom") == 0) { dom_mode(); return 0; }
    if (argc >= 3 && strcmp(argv[1], "layout") == 0) { layout_mode(atoi(argv[2])); return 0; }
    if (argc >= 2 && strcmp(argv[1], "image") == 0) { image_mode(); return 0; }
    if (argc >= 2 && strcmp(argv[1], "css") == 0) { css_mode(); return 0; }
    if (argc >= 2 && strcmp(argv[1], "cache") == 0) { cache_mode(); return 0; }
    if (argc >= 3 && strcmp(argv[1], "form") == 0) { form_mode(argv[2]); return 0; }
    if (argc >= 3 && strcmp(argv[1], "http") == 0) {
        int hops = (argc >= 4) ? atoi(argv[3]) : 5;
        const char *method = (argc >= 5) ? argv[4] : NULL;
        const char *body = (argc >= 6) ? argv[5] : NULL;
        load_script();
        http_mode(argv[2], hops, method, body);
        return 0;
    }
    html_mode();
    return 0;
}
