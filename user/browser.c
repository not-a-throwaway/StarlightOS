/*
 * BROWSER.ELF -- M7: browser chrome (address bar, back/forward, bookmarks,
 * cache and forms) on top of the M5 DOM/layout and M6 CSS-image engine.
 *
 * M5 parsed a page into a DOM and laid it out into boxes; M6 added the CSS
 * subset and bitmap images. M7 turns the demo into a browser you can drive:
 *
 *   - a real address bar (click it or Ctrl-L, type, Enter to go);
 *   - back/forward history with an explicit cursor (b/f, or the toolbar);
 *   - an in-memory HTTP cache (libslweb cache.c) so back/forward and repeat
 *     visits are instant; reload bypasses it;
 *   - bookmarks (Ctrl-D toggles one for the current URL, Ctrl-B + 1-9 opens),
 *     persisted to BOOKMARKS.TXT (one URL per line) in the current directory;
 *   - HTML forms: <input>/<textarea>/<select>/<button> are laid out as widgets,
 *     can be focused (Tab/click) and edited, and Enter (or a submit button)
 *     sends the form as GET or POST via libslweb form.c.
 *
 * Still a ring-3 program tying the earlier milestones together:
 *   - M1 syscalls (fbmap/present/getevents) for the 900x600 window;
 *   - M2 sockets via libslweb for the page and its images;
 *   - M5 DOM + layout, M6 CSS + images, M7 chrome + forms.
 *
 * M9.4: https:// is real. The kernel's TLS client (lifted into libslweb) does a
 * full handshake and validates the server certificate against a trust store that
 * starts empty, so the browser takes --cafile FILE (load PEM roots) and
 * --insecure / -k (skip validation, for tests). A verified page shows "[secure]";
 * an opted-out or unverifiable one shows "[unverified connection]".
 *
 * Keys:  Ctrl-L address bar   Ctrl-D bookmark   Ctrl-B bookmarks bar
 *        1-9 follow a link (or a bookmark when the bar is focused)
 *        tab next field/link  enter go/submit   b/f back/forward   r reload
 *        up/down, pgup/pgdn, home/end scroll   q/esc quit
 * Mouse: clicking the toolbar, a bookmark, a control or a link acts on it.
 *
 * stdout doubles as the test's assertion surface: BROWSER: layout/link/fetch,
 * css/image, cache, bookmark and submit lines are asserted by the QEMU test.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <slweb.h>
#include "slwindow.h"
#include "font8x8_basic.h"

/* ------------------------------------------------------------------ window */
#define W 900
#define H 600
#define MARGIN 8
#define NAV_H 28
#define BOOK_H 20
#define TOP_H (NAV_H + BOOK_H)
#define BOT_H 28
#define SB_W 8
#define CONTENT_W (W - 2 * MARGIN - SB_W)

#define NAV_BTN_W 22
#define NAV_BTN_GAP 4

#define HIST_MAX 64
#define MAX_IMAGES 24
#define MAX_FIELDS 64
#define BOOKMARK_MAX 16
#define BOOKMARK_FILE "BOOKMARKS.TXT"   /* one URL per line, in the shell's cwd */

/* ------------------------------------------------------------------ colours */
#define C_BG        0x10141A
#define C_FG        0xD8DEE9
#define C_BAR_BG    0x1B2230
#define C_BAR_FG    0x8FA1B3
#define C_URL_FG    0xFFFFFF
#define C_LINK      0x6FB3FF
#define C_LINK_HI   0xFFFFFF
#define C_SEL_BG    0x2A3A55
#define C_WARN      0xFFC857
#define C_OK        0x5FD37A
#define C_RULE      0x39465C
#define C_MARKER    0xA1B0C4
#define C_IMG_BG    0x1A2130
#define C_IMG_BR    0x39465C
#define C_FIELD_BG  0xF0F4F8
#define C_FIELD_FG  0x10141A
#define C_FIELD_BR  0x4A5A74
#define C_FIELD_FOC 0x6FB3FF
#define C_BTN_BG    0x2A3A55
#define C_BTN_FG    0xE6ECF5
#define C_BTN_BR    0x4A5A74
#define C_BM_BG     0x232C3D
#define C_BM_FG     0xC6D2E2

/* ------------------------------------------------------------------ state */
static uint32_t *fb;

static struct slweb_node *dom;
static struct slweb_css *css;
static struct slweb_box boxes[SLWEB_MAX_BOXES];
static int nboxes;
static struct slweb_link links[SLWEB_MAX_LINKS];
static size_t nlinks;
static int doc_h;

static struct slweb_image img_store[MAX_IMAGES];
static struct slweb_image_entry img_entries[MAX_IMAGES];
static size_t n_images;          /* entries used, and images in img_store[0..n) */

static char url[512];            /* current page URL (after redirects) */
static char hist[HIST_MAX][512];
static int nhist;                /* number of history entries */
static int hpos;                 /* index of the current entry, -1 = none */
static int selected;             /* 1-based link index, 0 = none */
static int scroll;
static int is_https;
static int tls_verified;         /* https result: 1 = chain validated */
static char tls_peer[128];       /* https peer CN ("" if none) */
static int opt_insecure;         /* --insecure: do not validate certs */
static char status_line[256];

static struct slweb_cache *cache;

/* address bar */
static char addr[512];
static int addr_len, addr_caret, url_focus, addr_sel_all;

/* form fields (live values for the laid-out controls) */
struct field {
    const struct slweb_node *node;
    int kind;                    /* 0 text, 1 checkbox/radio, 2 select       */
    char value[256];
    int len;                     /* strlen(value), for text fields           */
    int caret;                   /* insertion point for text fields          */
    int checked;                 /* checkbox/radio state                     */
    int sel;                     /* selected <option> index for a <select>   */
};
static struct field fields[MAX_FIELDS];
static int nfields;
static int ctrl_focus;           /* index into fields[], -1 = none */
static const struct slweb_node *activated_button;   /* submit button being sent */

/* bookmarks */
static char bookmarks[BOOKMARK_MAX][512];
static int nbookmarks;
static int bm_focus;             /* bookmarks bar has keyboard focus */

/* ------------------------------------------------------------------ helpers */
static int eq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

static const struct slweb_node *first_text_child(const struct slweb_node *n)
{
    for (const struct slweb_node *c = n->first_child; c; c = c->next_sibling)
        if (c->type == SLWEB_NODE_TEXT) return c;
    return NULL;
}

static const struct slweb_node *form_of(const struct slweb_node *n)
{
    for (const struct slweb_node *p = n ? n->parent : NULL; p; p = p->parent)
        if (p->type == SLWEB_NODE_ELEMENT && strcmp(p->tag, "form") == 0) return p;
    return NULL;
}

/* ------------------------------------------------------------------ pixels */
static void put_px(int x, int y, uint32_t c)
{
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    fb[y * W + x] = c;
}

static void fill(int x, int y, int w, int h, uint32_t c)
{
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            fb[(y + j) * W + (x + i)] = c;
}

static void rect_border(int x, int y, int w, int h, uint32_t c)
{
    for (int i = 0; i < w; i++) { put_px(x + i, y, c); put_px(x + i, y + h - 1, c); }
    for (int j = 0; j < h; j++) { put_px(x, y + j, c); put_px(x + w - 1, y + j, c); }
}

static void blend(int x, int y, uint32_t src)
{
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    unsigned a = (src >> 24) & 0xFF;
    if (a == 0) return;
    if (a == 255) { fb[y * W + x] = src & 0xFFFFFF; return; }
    uint32_t d = fb[y * W + x];
    unsigned r = (((src >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a) + 127) / 255;
    unsigned g = (((src >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a) + 127) / 255;
    unsigned b = ((src & 0xFF) * a + (d & 0xFF) * (255 - a) + 127) / 255;
    fb[y * W + x] = (r << 16) | (g << 8) | b;
}

static void draw_glyph(int x, int y, unsigned char ch, uint32_t color,
                       int scale, int bold, int italic)
{
    if (scale < 1) scale = 1;
    if (ch < 32 || ch >= 128) ch = '?';
    const unsigned char *g = (const unsigned char *)font8x8_basic[ch];
    for (int row = 0; row < 8; row++) {
        unsigned bits = g[row];
        if (!bits) continue;
        int sh = italic ? ((7 - row) * scale) / 5 : 0;
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80u >> col))) continue;
            int px = x + col * scale + sh;
            int py = y + row * scale;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    put_px(px + sx, py + sy, color);
            if (bold)
                for (int sy = 0; sy < scale; sy++)
                    put_px(px + scale, py + sy, color);
        }
    }
}

/* Draw `len` glyphs, stopping before x+maxw. */
static void draw_text_clipped(int x, int y, const char *s, int len, uint32_t color,
                              int scale, int bold, int italic, int maxw)
{
    int cx = x;
    for (int i = 0; i < len; i++) {
        if (cx + scale * 8 > x + maxw) break;
        draw_glyph(cx, y, (unsigned char)s[i], color, scale, bold, italic);
        cx += scale >= 2 ? scale * 8 : 8;
    }
}

static void draw_text(int x, int y, const char *s, uint32_t color)
{
    for (; *s; s++) { draw_glyph(x, y, (unsigned char)*s, color, 1, 0, 0); x += 8; }
}

static int text_w(const char *s) { return (int)strlen(s) * 8; }

/* ------------------------------------------------------- small text editor */
/* Apply one key to a NUL-terminated buffer. Returns 1 if the view changed. */
static int text_key(char *buf, int cap, int *lenp, int *caretp, unsigned key)
{
    int len = *lenp, c = *caretp;
    if (key == 8) {                                   /* backspace */
        if (c > 0) { memmove(&buf[c - 1], &buf[c], len - c + 1); len--; c--; }
    } else if (key == 0x87) {                         /* delete */
        if (c < len) { memmove(&buf[c], &buf[c + 1], len - c); len--; }
    } else if (key == 0x83) {                         /* left */
        if (c > 0) c--;
    } else if (key == 0x84) {                         /* right */
        if (c < len) c++;
    } else if (key == 0x85) {                         /* home */
        c = 0;
    } else if (key == 0x86) {                         /* end */
        c = len;
    } else if (key >= 32 && key < 127) {
        if (len + 1 < cap) {
            memmove(&buf[c + 1], &buf[c], len - c + 1);
            buf[c] = (char)key; len++; c++;
        } else {
            return 0;
        }
    } else {
        return 0;
    }
    *lenp = len;
    *caretp = c;
    return 1;
}

/* --------------------------------------------------------------- form model */
static int option_count(const struct slweb_node *sel)
{
    int n = 0;
    for (const struct slweb_node *o = sel->first_child; o; o = o->next_sibling)
        if (o->type == SLWEB_NODE_ELEMENT && strcmp(o->tag, "option") == 0) n++;
    return n;
}

/* want_value: submit option's value= attribute (else its text); otherwise text. */
static void option_str(const struct slweb_node *sel, int idx, int want_value,
                       char *out, size_t cap)
{
    int i = 0;
    for (const struct slweb_node *o = sel->first_child; o; o = o->next_sibling) {
        if (o->type != SLWEB_NODE_ELEMENT || strcmp(o->tag, "option") != 0) continue;
        if (i == idx) {
            if (want_value) {
                const char *v = slweb_node_attr(o, "value");
                if (v) { snprintf(out, cap, "%s", v); return; }
            }
            const struct slweb_node *t = first_text_child(o);
            snprintf(out, cap, "%.*s", t ? (int)t->text_len : 0, t ? t->text : "");
            return;
        }
        i++;
    }
    out[0] = 0;
}

static int selected_option_index(const struct slweb_node *sel)
{
    int i = 0, first = 0;
    for (const struct slweb_node *o = sel->first_child; o; o = o->next_sibling) {
        if (o->type != SLWEB_NODE_ELEMENT || strcmp(o->tag, "option") != 0) continue;
        if (slweb_node_attr(o, "selected")) return i;
        i++;
    }
    return first;
}

static const struct field *find_field(const struct slweb_node *n)
{
    for (int i = 0; i < nfields; i++)
        if (fields[i].node == n) return &fields[i];
    return NULL;
}

/* Build the live field list from the laid-out INPUT boxes. */
static void collect_fields(void)
{
    nfields = 0;
    ctrl_focus = -1;
    for (int i = 0; i < nboxes && nfields < MAX_FIELDS; i++) {
        struct slweb_box *b = &boxes[i];
        if (b->kind != SLWEB_BOX_INPUT || !b->node) continue;
        const struct slweb_node *n = b->node;
        struct field *f = &fields[nfields];
        memset(f, 0, sizeof *f);
        f->node = n;
        if (strcmp(n->tag, "select") == 0) {
            f->kind = 2;
            f->sel = selected_option_index(n);
        } else if (strcmp(n->tag, "input") == 0) {
            const char *type = slweb_node_attr(n, "type");
            if (type && (eq_ci(type, "checkbox") || eq_ci(type, "radio"))) {
                f->kind = 1;
                f->checked = slweb_node_attr(n, "checked") != NULL;
                const char *v = slweb_node_attr(n, "value");
                snprintf(f->value, sizeof f->value, "%s", v ? v : "on");
            } else {
                const char *v = slweb_node_attr(n, "value");
                f->kind = 0;
                snprintf(f->value, sizeof f->value, "%s", v ? v : "");
            }
        } else {                                   /* textarea */
            const struct slweb_node *t = first_text_child(n);
            f->kind = 0;
            snprintf(f->value, sizeof f->value, "%.*s", t ? (int)t->text_len : 0,
                     t ? t->text : "");
        }
        f->len = (int)strlen(f->value);
        f->caret = f->len;
        nfields++;
    }
}

/* Value callback for slweb_form_collect: live values where the browser has
 * them, DOM defaults otherwise (so hidden inputs and off-screen controls
 * still submit). */
static int field_value(const struct slweb_node *n, void *user, char *out, size_t cap)
{
    (void)user;
    if (n == activated_button) {
        const char *v = slweb_node_attr(n, "value");
        if (v) { snprintf(out, cap, "%s", v); return 0; }
        const struct slweb_node *t = first_text_child(n);
        snprintf(out, cap, "%.*s", t ? (int)t->text_len : 0, t ? t->text : "");
        return 0;
    }
    const struct field *f = find_field(n);
    if (f) {
        if (f->kind == 1) {
            if (!f->checked) return -1;
            snprintf(out, cap, "%s", f->value);
        } else if (f->kind == 2) {
            option_str(n, f->sel, 1, out, cap);
        } else {
            snprintf(out, cap, "%s", f->value);
        }
        return 0;
    }
    /* no live field: fall back to attributes (hidden inputs, etc.) */
    const char *type = slweb_node_attr(n, "type");
    const char *v = slweb_node_attr(n, "value");
    if (type && (eq_ci(type, "submit") || eq_ci(type, "reset") ||
                 eq_ci(type, "button") || eq_ci(type, "image")))
        return -1;
    if (type && (eq_ci(type, "checkbox") || eq_ci(type, "radio"))) {
        if (!slweb_node_attr(n, "checked")) return -1;
        snprintf(out, cap, "%s", v ? v : "on");
        return 0;
    }
    if (strcmp(n->tag, "textarea") == 0) {
        const struct slweb_node *t = first_text_child(n);
        snprintf(out, cap, "%.*s", t ? (int)t->text_len : 0, t ? t->text : "");
        return 0;
    }
    if (strcmp(n->tag, "select") == 0) {
        option_str(n, selected_option_index(n), 1, out, cap);
        return 0;
    }
    snprintf(out, cap, "%s", v ? v : "");
    return 0;
}

/* ------------------------------------------------------------------ layout */
static void log_text(void)
{
    static char buf[1 << 16];
    size_t o = 0;
    int last_y = -100000;
    int prev_end = 0;                /* x where the previous inline box ended */
    for (int i = 0; i < nboxes; i++) {
        struct slweb_box *b = &boxes[i];
        if (b->kind == SLWEB_BOX_RECT) continue;
        if (o + 64 >= sizeof buf) break;

        if (b->y != last_y) {
            if (o > 0) buf[o++] = '\n';
            last_y = b->y;
            prev_end = 0;
        }
        if (b->kind == SLWEB_BOX_RULE) {
            for (int k = 0; k < 4 && o + 1 < sizeof buf; k++) buf[o++] = '-';
            prev_end = 0;
            continue;
        }
        /* A gap between two inline boxes is a word space. */
        if (prev_end > 0 && b->x > prev_end && o + 1 < sizeof buf) buf[o++] = ' ';

        if (b->kind == SLWEB_BOX_MARKER) {
            if (b->marker > 0)
                o += (size_t)snprintf(buf + o, sizeof buf - o, "%d.", b->marker);
            else if (o + 1 < sizeof buf) buf[o++] = '*';
            prev_end = b->x + b->w;
            continue;
        }
        if (b->kind != SLWEB_BOX_TEXT && b->kind != SLWEB_BOX_IMAGE) continue;

        for (int k = 0; k < b->text_len && o + 1 < sizeof buf; k++) buf[o++] = b->text[k];
        prev_end = b->x + b->w;
    }
    buf[o] = 0;
    printf("BROWSER: text |%s|\n", buf);
}

static void layout_and_log(void)
{
    struct slweb_layout_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.css = css;
    opts.images = img_entries;
    opts.n_images = n_images;

    nboxes = slweb_layout_ex(dom, CONTENT_W, &opts, boxes, SLWEB_MAX_BOXES,
                             links, SLWEB_MAX_LINKS, &nlinks, NULL, &doc_h);
    printf("BROWSER: layout boxes=%d doc_h=%d links=%zu\n", nboxes, doc_h, nlinks);
    printf("BROWSER: css rules=%d images=%zu\n", slweb_css_rule_count(css), n_images);
    for (size_t i = 0; i < nlinks; i++) printf("BROWSER: link %zu href=%s\n", i + 1, links[i].href);

    uint32_t fg = 0, bg = 0;
    int have_fg = 0, have_bg = 0;
    for (int i = 0; i < nboxes; i++) {
        if (!have_fg && boxes[i].fg_set) { fg = boxes[i].fg; have_fg = 1; }
        if (!have_bg && boxes[i].bg_set) { bg = boxes[i].bg; have_bg = 1; }
    }
    if (have_fg || have_bg)
        printf("BROWSER: css applied fg=%s%06lx bg=%s%06lx\n",
               have_fg ? "#" : "-", have_fg ? (unsigned long)(fg & 0xFFFFFF) : 0,
               have_bg ? "#" : "-", have_bg ? (unsigned long)(bg & 0xFFFFFF) : 0);
    log_text();
}

/* ------------------------------------------------------------------ images */
static void fetch_images(const struct slweb_node *doc)
{
    n_images = 0;
    memset(img_store, 0, sizeof img_store);

    const struct slweb_node *stack[2048];
    int sp = 0;
    if (doc) stack[sp++] = doc;
    while (sp > 0) {
        const struct slweb_node *n = stack[--sp];
        for (; n; n = n->next_sibling) {
            if (n->type == SLWEB_NODE_ELEMENT && strcmp(n->tag, "img") == 0) {
                const char *src = slweb_node_attr(n, "src");
                if (src && src[0] && n_images < MAX_IMAGES) {
                    struct slweb_image *im = &img_store[n_images];
                    int rc = -1;
                    if (strncmp(src, "data:", 5) == 0) {
                        rc = slweb_image_data_uri(src, im);
                    } else {
                        char target[512];
                        if (slweb_url_resolve(url, src, target, sizeof target) == 0) {
                            struct slweb_http_opts o;
                            memset(&o, 0, sizeof o);
                            o.follow_redirects = 3;
                            o.quiet = 1;
                            o.timeout_ms = 8000;
                            o.max_body = 2 * 1024 * 1024;
                            struct slweb_response r;
                            int err = slweb_http_request(target, &o, &r);
                            if (err == 0 && r.body && r.body_len)
                                rc = slweb_image_decode(r.body, r.body_len, im);
                            slweb_http_response_free(&r);
                        }
                    }
                    size_t idx = n_images + 1;
                    if (rc == 0) {
                        img_entries[n_images].node = n;
                        img_entries[n_images].image = im;
                        n_images++;
                        printf("BROWSER: image %zu src=%s w=%d h=%d alpha=%d\n",
                               idx, src, im->w, im->h, im->has_alpha);
                    } else {
                        printf("BROWSER: image %zu src=%s error\n", idx, src);
                    }
                }
            }
            if (n->first_child && sp < 2048) stack[sp++] = n->first_child;
        }
    }
}

/* ------------------------------------------------------------------ render */
static void free_page(void)
{
    if (css) { slweb_css_free(css); css = NULL; }
    if (dom) { slweb_dom_free(dom); dom = NULL; }
    for (size_t i = 0; i < n_images; i++) slweb_image_free(&img_store[i]);
    n_images = 0;
    nfields = 0;
    ctrl_focus = -1;
}

static int is_html_type(const char *ct)
{
    return ct && (strstr(ct, "text/html") || strstr(ct, "application/xhtml") ||
                  strstr(ct, "text/xml") || strstr(ct, "application/xml"));
}

static void build_and_layout(const char *content_type, const char *body, size_t blen, int status)
{
    if (!dom) {
        if (body && is_html_type(content_type)) dom = slweb_dom_parse(body, blen);
        else if (body) dom = slweb_dom_from_text(body, blen);
    }
    if (!dom) dom = slweb_dom_from_text("(empty page)", 12);
    css = slweb_css_parse(dom);
    fetch_images(dom);
    layout_and_log();
    snprintf(status_line, sizeof status_line, "HTTP %d   %s   links %zu   images %zu",
             status, content_type && content_type[0] ? content_type : "?", nlinks, n_images);
}

static void draw(void);

/* --------------------------------------------------------------- navigation */
static void free_page(void);
static void collect_fields(void);

static void navigate(const char *target, const char *method, const char *body,
                     size_t blen, int use_cache, int push)
{
    printf("BROWSER: load url=%s\n", target);
    free_page();
    nboxes = nlinks = 0;
    nfields = 0;
    ctrl_focus = -1;
    doc_h = 0;
    scroll = 0;
    selected = 0;
    snprintf(url, sizeof url, "%s", target);

    struct slweb_url parsed;
    is_https = 0;
    tls_verified = 0;
    tls_peer[0] = 0;
    if (slweb_url_parse(url, &parsed) == 0) is_https = parsed.https;

    struct slweb_response r;
    memset(&r, 0, sizeof r);
    const char *ctype = NULL;
    const uint8_t *pbody = NULL;
    size_t plen = 0;
    int status = 0;
    int from_cache = 0;

    if (use_cache && !method && cache) {
        const struct slweb_cache_entry *ce = slweb_cache_get(cache, target);
        if (ce) {
            from_cache = 1;
            status = ce->status;
            ctype = ce->content_type;
            pbody = ce->body;
            plen = ce->body_len;
            printf("BROWSER: cache hit url=%s len=%zu\n", target, plen);
        }
    }

    if (!from_cache) {
        struct slweb_http_opts o;
        memset(&o, 0, sizeof o);
        o.follow_redirects = 5;
        o.quiet = 1;
        o.timeout_ms = 8000;
        o.max_body = 4 * 1024 * 1024;
        o.method = method;
        o.body = body;
        o.body_len = blen;
        o.insecure = opt_insecure;

        int err = slweb_http_request(url, &o, &r);
        if (err == 0 && r.final_url[0]) {
            snprintf(url, sizeof url, "%s", r.final_url);
            struct slweb_url fin;
            if (slweb_url_parse(url, &fin) == 0) is_https = fin.https;
        }

        if (err != 0) {
            if (is_https && r.tls_error)
                printf("BROWSER: tls error=%d (%s)\n", r.tls_error,
                       slweb_http_tls_error_text(r.tls_error));
            char detail[160];
            if (is_https && r.tls_error)
                snprintf(detail, sizeof detail, "%s", slweb_http_tls_error_text(r.tls_error));
            else
                snprintf(detail, sizeof detail, "%s (error %d)", slweb_http_error_text(err), err);
            char page[768];
            snprintf(page, sizeof page,
                     "<h2>Could not load page</h2><p>%s</p><p>%s</p>", url, detail);
            free_page();
            dom = slweb_dom_parse(page, strlen(page));
            if (!dom) dom = slweb_dom_from_text(page, strlen(page));
            css = slweb_css_parse(dom);
            snprintf(status_line, sizeof status_line, "error: %s", detail);
            printf("BROWSER: error err=%d (%s)\n", err, detail);
            layout_and_log();
            collect_fields();
            slweb_http_response_free(&r);
            if (!url_focus) { snprintf(addr, sizeof addr, "%s", url); addr_len = (int)strlen(addr); addr_caret = addr_len; }
            selected = nlinks ? 1 : 0;
            draw();
            return;
        }

        tls_verified = r.tls_verified;
        snprintf(tls_peer, sizeof tls_peer, "%s", r.tls_peer);
        if (is_https)
            printf("BROWSER: tls verified=%d peer=%s\n", tls_verified, tls_peer[0] ? tls_peer : "-");

        printf("BROWSER: fetch status=%d type=%s len=%zu\n",
               r.status, r.content_type, r.body_len);
        status = r.status;
        ctype = r.content_type;
        pbody = r.body;
        plen = r.body_len;
        if (cache && !method) {
            slweb_cache_put(cache, target, status, ctype, r.body, r.body_len);
            printf("BROWSER: cache store url=%s len=%zu\n", target, r.body_len);
        }
    }

    build_and_layout(ctype, (const char *)pbody, plen, status);
    printf("BROWSER: fetch links=%zu\n", nlinks);
    collect_fields();

    if (push) {
        nhist = hpos + 1;
        if (nhist < HIST_MAX) {
            snprintf(hist[nhist], sizeof hist[nhist], "%s", target);
            nhist++;
            hpos = nhist - 1;
        }
    }
    if (!url_focus) { snprintf(addr, sizeof addr, "%s", url); addr_len = (int)strlen(addr); addr_caret = addr_len; }

    selected = nlinks ? 1 : 0;
    if (!from_cache) slweb_http_response_free(&r);
    draw();
}

static void reload(void)
{
    if (!url[0]) return;
    printf("BROWSER: reload url=%s\n", url);
    navigate(url, NULL, NULL, 0, 0, 0);
}

static void go_back(void)
{
    if (hpos <= 0) { printf("BROWSER: back none\n"); return; }
    hpos--;
    navigate(hist[hpos], NULL, NULL, 0, 1, 0);
    printf("BROWSER: back -> %s\n", url);
}

static void go_forward(void)
{
    if (hpos < 0 || hpos + 1 >= nhist) { printf("BROWSER: forward none\n"); return; }
    hpos++;
    navigate(hist[hpos], NULL, NULL, 0, 1, 0);
    printf("BROWSER: forward -> %s\n", url);
}

static void follow(int idx)
{
    if (idx < 1 || (size_t)idx > nlinks) { printf("BROWSER: no link %d\n", idx); return; }
    char target[512];
    if (slweb_url_resolve(url, links[idx - 1].href, target, sizeof target) != 0) {
        printf("BROWSER: bad link %s\n", links[idx - 1].href);
        return;
    }
    printf("BROWSER: follow %d -> %s\n", idx, target);
    navigate(target, NULL, NULL, 0, 0, 1);
    selected = 1;
    draw();
}

/* ------------------------------------------------------------------ submit */
static void submit_form(const struct slweb_node *control, const struct slweb_node *button)
{
    const struct slweb_node *form = form_of(control);
    const struct slweb_node *scope = form ? form : dom;

    activated_button = button;
    struct slweb_form_pair pairs[64];
    size_t np = slweb_form_collect(scope, field_value, NULL, pairs, 64);
    activated_button = NULL;

    char query[2048];
    if (slweb_form_query(pairs, np, query, sizeof query) < 0) {
        printf("BROWSER: submit error query too long\n");
        return;
    }
    char target[512], body[2048];
    int is_post = 0;
    if (slweb_form_submit(url, form, query, target, sizeof target, &is_post,
                          body, sizeof body) != 0) {
        printf("BROWSER: submit error bad action\n");
        return;
    }
    printf("BROWSER: submit method=%s fields=%zu url=%s\n",
           is_post ? "post" : "get", np, target);
    ctrl_focus = -1;
    if (is_post) navigate(target, "POST", body, strlen(body), 0, 1);
    else navigate(target, NULL, NULL, 0, 0, 1);
}

static void submit_from_field(int idx)
{
    if (idx < 0 || idx >= nfields) return;
    submit_form(fields[idx].node, NULL);
}

/* ------------------------------------------------------------------ chrome */
static int addr_field_x(void) { return MARGIN + 3 * (NAV_BTN_W + NAV_BTN_GAP); }
static int addr_field_w(void) { return W - MARGIN - addr_field_x(); }

static void draw_nav_button(int x, const char *label)
{
    fill(x, 3, NAV_BTN_W, NAV_H - 6, C_BTN_BG);
    rect_border(x, 3, NAV_BTN_W, NAV_H - 6, C_BTN_BR);
    int tx = x + (NAV_BTN_W - text_w(label)) / 2;
    if (tx < x + 1) tx = x + 1;
    draw_text(tx, 10, label, C_BTN_FG);
}

static int bookmark_at(int mx, int my)
{
    if (my < NAV_H || my >= TOP_H) return -1;
    int cx = MARGIN;
    for (int k = 0; k < nbookmarks; k++) {
        const char *u = bookmarks[k];
        const char *label = strrchr(u, '/');
        label = (label && label[1]) ? label + 1 : u;
        int bw = text_w(label) + 12;
        if (bw > 140) bw = 140;
        if (mx >= cx && mx < cx + bw) return k;
        cx += bw + 4;
    }
    return -1;
}

static void draw_toolbar(void)
{
    fill(0, 0, W, NAV_H, C_BAR_BG);
    draw_nav_button(MARGIN, "<");
    draw_nav_button(MARGIN + NAV_BTN_W + NAV_BTN_GAP, ">");
    draw_nav_button(MARGIN + 2 * (NAV_BTN_W + NAV_BTN_GAP), "R");

    int ax = addr_field_x(), aw = addr_field_w();
    fill(ax, 4, aw, NAV_H - 8, url_focus ? 0xFFFFFF : C_FIELD_BG);
    rect_border(ax, 4, aw, NAV_H - 8, url_focus ? C_FIELD_FOC : C_FIELD_BR);

    const char *shown = url_focus ? addr : url;
    int slen = url_focus ? addr_len : (int)strlen(url);
    int maxw = aw - 6;
    int vis = slen;
    if (vis * 8 > maxw) vis = maxw / 8;
    draw_text_clipped(ax + 3, 10, shown, vis, C_FIELD_FG, 1, 0, 0, maxw);

    if (url_focus) {
        int cx = ax + 3 + addr_caret * 8;
        if (cx > ax + aw - 4) cx = ax + aw - 4;
        for (int i = 0; i < 12; i++) put_px(cx, 8 + i, C_FIELD_FG);
    }
}

static void draw_bookmarks(void)
{
    fill(0, NAV_H, W, BOOK_H, C_BM_BG);
    fill(0, NAV_H, W, 1, C_RULE);

    int cx = MARGIN;
    for (int k = 0; k < nbookmarks; k++) {
        const char *u = bookmarks[k];
        const char *label = strrchr(u, '/');
        label = (label && label[1]) ? label + 1 : u;
        int bw = text_w(label) + 12;
        if (bw > 140) bw = 140;
        int hot = bm_focus && k == 0;
        fill(cx, NAV_H + 2, bw, BOOK_H - 5, hot ? C_SEL_BG : C_BAR_BG);
        rect_border(cx, NAV_H + 2, bw, BOOK_H - 5, C_BTN_BR);
        draw_text_clipped(cx + 6, NAV_H + 7, label, (int)strlen(label), C_BM_FG, 1, 0, 0,
                          bw - 10);
        cx += bw + 4;
    }
    if (nbookmarks == 0)
        draw_text(MARGIN, NAV_H + 7, "Ctrl-D bookmarks this page", C_BAR_FG);
    if (bm_focus)
        draw_text(MARGIN, NAV_H + 7, "[bookmarks] 1-9 open", C_WARN);
}

/* ------------------------------------------------------------- page drawing */
static void draw_image(const struct slweb_box *b, int x, int y)
{
    if (!b->image) {
        fill(x, y, b->w, b->h, C_IMG_BG);
        rect_border(x, y, b->w, b->h, C_IMG_BR);
        return;
    }
    const struct slweb_image *im = b->image;
    for (int dy = 0; dy < b->h; dy++) {
        int sy = (int)((long)dy * im->h / b->h);
        if (sy >= im->h) sy = im->h - 1;
        const uint32_t *row = im->pixels + (size_t)sy * im->w;
        for (int dx = 0; dx < b->w; dx++) {
            int sx = (int)((long)dx * im->w / b->w);
            if (sx >= im->w) sx = im->w - 1;
            blend(x + dx, y + dy, row[sx]);
        }
    }
}

static void draw_control(const struct slweb_box *b, int x, int y)
{
    const struct slweb_node *n = b->node;
    const struct field *f = n ? find_field(n) : NULL;
    int focused = (ctrl_focus >= 0 && fields[ctrl_focus].node == n);

    if (b->kind == SLWEB_BOX_BUTTON) {
        fill(x, y, b->w, b->h, C_BTN_BG);
        rect_border(x, y, b->w, b->h, C_BTN_BR);
        int tw = b->text_len * 8;
        int tx = x + (b->w - tw) / 2;
        if (tx < x + 2) tx = x + 2;
        int ty = y + (b->h - 8) / 2;
        if (ty < y + 1) ty = y + 1;
        draw_text_clipped(tx, ty, b->text, b->text_len, C_BTN_FG, 1, 0, 0, x + b->w - 2 - tx);
        return;
    }

    const char *type = n ? slweb_node_attr(n, "type") : NULL;
    int is_check = type && (eq_ci(type, "checkbox") || eq_ci(type, "radio"));

    fill(x, y, b->w, b->h, C_FIELD_BG);
    rect_border(x, y, b->w, b->h, focused ? C_FIELD_FOC : C_FIELD_BR);

    if (is_check) {
        if (f && f->checked) {
            int lim = (b->w < b->h ? b->w : b->h) - 4;
            for (int i = 1; i < lim; i++) {
                put_px(x + 2 + i, y + 2 + i, C_FIELD_FG);
                put_px(x + b->w - 3 - i, y + 2 + i, C_FIELD_FG);
            }
        }
        return;
    }

    char opt[256];
    const char *disp = "";
    int disp_len = 0;
    if (f) {
        if (f->kind == 2) {
            option_str(n, f->sel, 0, opt, sizeof opt);
            disp = opt;
            disp_len = (int)strlen(opt);
        } else {
            disp = f->value;
            disp_len = f->len;
        }
    }
    int ty = y + (b->h - 8) / 2;
    if (ty < y + 1) ty = y + 1;
    int maxw = b->w - 4;
    int vis = disp_len;
    if (vis * 8 > maxw) vis = maxw / 8;
    draw_text_clipped(x + 2, ty, disp, vis, C_FIELD_FG, 1, 0, 0, maxw);

    if (f && f->kind == 2) {
        draw_glyph(x + b->w - 10, ty, 'v', C_FIELD_BR, 1, 0, 0);
    }
    if (focused && f && f->kind == 0) {
        int cx = x + 2 + f->caret * 8;
        if (cx > x + 2 + maxw - 1) cx = x + 2 + maxw - 1;
        for (int i = 0; i < 8; i++) put_px(cx, ty + i, C_FIELD_FG);
    }
}

static void draw_box(const struct slweb_box *b)
{
    int x = MARGIN + b->x;
    int y = TOP_H + b->y - scroll;
    if (y + b->h < TOP_H || y > H - BOT_H) return;
    if (b->kind == SLWEB_BOX_RECT) return;

    if (b->kind == SLWEB_BOX_RULE) {
        fill(x, y, b->w, b->h < 1 ? 1 : b->h, C_RULE);
        return;
    }
    if (b->kind == SLWEB_BOX_INPUT || b->kind == SLWEB_BOX_BUTTON) {
        draw_control(b, x, y);
        return;
    }

    int sel = (b->link && b->link == selected);
    if (sel) fill(x - 2, y - 1, b->w + 4, b->h + 2, C_SEL_BG);

    uint32_t fg = b->fg_set ? (b->fg & 0xFFFFFF) : C_FG;
    if (b->link) fg = sel ? C_LINK_HI : C_LINK;

    if (b->kind == SLWEB_BOX_TEXT) {
        int cx = x;
        for (int i = 0; i < b->text_len; i++) {
            draw_glyph(cx, y, (unsigned char)b->text[i], fg, b->scale, b->bold, b->italic);
            cx += b->scale >= 2 ? 16 : 8;
        }
        if (b->link || b->underline)
            fill(x, y + (b->scale >= 2 ? 16 : 10) - 2, b->text_len * (b->scale >= 2 ? 16 : 8), 1, fg);
    } else if (b->kind == SLWEB_BOX_IMAGE) {
        draw_image(b, x, y);
    } else if (b->kind == SLWEB_BOX_MARKER) {
        int cx = x;
        if (b->marker > 0) {
            char mk[16];
            int ml = snprintf(mk, sizeof mk, "%d.", b->marker);
            for (int i = 0; i < ml; i++) { draw_glyph(cx, y, (unsigned char)mk[i], C_MARKER, 1, 0, 0); cx += 8; }
        } else {
            draw_glyph(cx, y, '*', C_MARKER, 1, 0, 0);
        }
    }
}

static void draw(void)
{
    fill(0, 0, W, H, C_BG);
    for (int i = 0; i < nboxes; i++) draw_box(&boxes[i]);

    draw_toolbar();
    draw_bookmarks();

    fill(0, H - BOT_H, W, 1, C_RULE);
    draw_text(MARGIN, H - BOT_H + 4, status_line, C_BAR_FG);
    draw_text(MARGIN, H - BOT_H + 15,
              "1-9 follow  tab next  enter go  b/f back/fwd  r reload  ctrl-l url  ctrl-d mark  q quit",
              C_BAR_FG);
    if (is_https) {
        const char *badge = tls_verified ? "[secure]" : "[unverified connection]";
        draw_text(W - MARGIN - text_w(badge), H - BOT_H + 4, badge,
                  tls_verified ? C_OK : C_WARN);
    }

    if (doc_h > H - TOP_H - BOT_H) {
        int track_y = TOP_H, track_h = H - TOP_H - BOT_H;
        fill(W - SB_W, track_y, SB_W, track_h, 0x161C26);
        int view = track_h;
        int thumb_h = (int)((long)view * view / doc_h);
        if (thumb_h < 20) thumb_h = 20;
        int maxscroll = doc_h - view;
        int ty = track_y + (maxscroll > 0 ? (view - thumb_h) * scroll / maxscroll : 0);
        fill(W - SB_W, ty, SB_W, thumb_h, 0x4A5A74);
    }

    sl_present();
}

/* ------------------------------------------------------------------ focus */
static int box_for_node(const struct slweb_node *node)
{
    for (int i = 0; i < nboxes; i++)
        if (boxes[i].node == node &&
            (boxes[i].kind == SLWEB_BOX_INPUT || boxes[i].kind == SLWEB_BOX_BUTTON))
            return i;
    return -1;
}

static void ensure_field_visible(int idx)
{
    if (idx < 0 || idx >= nfields) return;
    int bi = box_for_node(fields[idx].node);
    if (bi < 0) return;
    int top = boxes[bi].y, bot = boxes[bi].y + boxes[bi].h;
    int view = H - TOP_H - BOT_H;
    if (top < scroll) scroll = top;
    if (bot > scroll + view) scroll = bot - view;
    if (scroll < 0) scroll = 0;
    if (doc_h > view && scroll > doc_h - view) scroll = doc_h - view;
}

static void ensure_link_visible(void)
{
    if (selected < 1 || (size_t)selected > nlinks) return;
    int top = 0, bot = 0;
    for (int i = 0; i < nboxes; i++)
        if (boxes[i].link == selected) {
            top = boxes[i].y;
            bot = boxes[i].y + boxes[i].h;
            break;
        }
    int view = H - TOP_H - BOT_H;
    if (top < scroll) scroll = top;
    if (bot > scroll + view) scroll = bot - view;
    if (scroll < 0) scroll = 0;
    if (doc_h > view && scroll > doc_h - view) scroll = doc_h - view;
}

static void focus_url(void)
{
    snprintf(addr, sizeof addr, "%s", url);
    addr_len = (int)strlen(addr);
    addr_caret = addr_len;
    addr_sel_all = 1;                    /* typing replaces the current URL */
    url_focus = 1;
    ctrl_focus = -1;
    bm_focus = 0;
    printf("BROWSER: addr focus\n");
    draw();
}

static void tab_next(void)
{
    if (nfields > 0) {
        ctrl_focus = (ctrl_focus + 1) % nfields;
        fields[ctrl_focus].caret = fields[ctrl_focus].len;
        ensure_field_visible(ctrl_focus);
        printf("BROWSER: field focus %d\n", ctrl_focus + 1);
    } else if (nlinks) {
        selected = (int)(selected % (int)nlinks) + 1;
        ensure_link_visible();
    }
    draw();
}

/* Bookmarks are persisted to BOOKMARKS.TXT (one URL per line) in the shell's
 * cwd, so they survive across browser runs. Errors are non-fatal: a read-only
 * cwd just means bookmarks stay in memory. */
static void bookmarks_load(void)
{
    nbookmarks = 0;
    FILE *f = fopen(BOOKMARK_FILE, "r");
    if (!f) return;
    char line[512];
    while (nbookmarks < BOOKMARK_MAX && fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n == 0) continue;
        snprintf(bookmarks[nbookmarks], 512, "%s", line);
        nbookmarks++;
    }
    fclose(f);
    printf("BROWSER: bookmarks loaded count=%d\n", nbookmarks);
}

static void bookmarks_save(void)
{
    FILE *f = fopen(BOOKMARK_FILE, "w");
    if (!f) { printf("BROWSER: bookmarks save failed errno=%d\n", errno); return; }
    for (int i = 0; i < nbookmarks; i++) fprintf(f, "%s\n", bookmarks[i]);
    if (fclose(f) != 0)
        printf("BROWSER: bookmarks save failed errno=%d\n", errno);
    else
        printf("BROWSER: bookmarks saved count=%d\n", nbookmarks);
}

static void toggle_bookmark(void)
{
    if (!url[0]) return;
    for (int i = 0; i < nbookmarks; i++)
        if (strcmp(bookmarks[i], url) == 0) {
            for (int j = i; j + 1 < nbookmarks; j++) snprintf(bookmarks[j], 512, "%s", bookmarks[j + 1]);
            nbookmarks--;
            printf("BROWSER: bookmark remove url=%s count=%d\n", url, nbookmarks);
            bookmarks_save();
            draw();
            return;
        }
    if (nbookmarks < BOOKMARK_MAX) {
        snprintf(bookmarks[nbookmarks], 512, "%s", url);
        nbookmarks++;
        printf("BROWSER: bookmark add url=%s count=%d\n", url, nbookmarks);
        bookmarks_save();
    } else {
        printf("BROWSER: bookmark full\n");
    }
    draw();
}

static void open_bookmark(int i)
{
    if (i < 0 || i >= nbookmarks) { printf("BROWSER: no bookmark %d\n", i + 1); return; }
    printf("BROWSER: bookmark open %d -> %s\n", i + 1, bookmarks[i]);
    bm_focus = 0;
    navigate(bookmarks[i], NULL, NULL, 0, 1, 1);
}

static void change_select(int idx, int delta)
{
    if (idx < 0 || idx >= nfields) return;
    const struct slweb_node *n = fields[idx].node;
    int cnt = option_count(n);
    if (cnt <= 0) return;
    fields[idx].sel = (fields[idx].sel + delta + cnt) % cnt;
}

/* ------------------------------------------------------------------ input */
static int click_link(int mx, int my)
{
    if (my < TOP_H || my > H - BOT_H) return 0;
    int lx = mx - MARGIN;
    int ly = my - TOP_H + scroll;
    for (int i = 0; i < nboxes; i++) {
        struct slweb_box *b = &boxes[i];
        if (b->kind == SLWEB_BOX_RECT || !b->link) continue;
        if (b->x <= lx && b->x + b->w >= lx && b->y <= ly && b->y + b->h > ly)
            return b->link;
    }
    return 0;
}

static void click_control(const struct slweb_box *b, int lx, int ly)
{
    const struct slweb_node *n = b->node;
    if (!n) return;

    if (b->kind == SLWEB_BOX_BUTTON) {
        const char *type = slweb_node_attr(n, "type");
        if (type && eq_ci(type, "reset")) {
            collect_fields();
            printf("BROWSER: form reset\n");
            draw();
            return;
        }
        if (type && eq_ci(type, "button")) return;   /* no action */
        submit_form(n, n);
        return;
    }

    /* INPUT */
    const char *type = slweb_node_attr(n, "type");
    int is_check = type && (eq_ci(type, "checkbox") || eq_ci(type, "radio"));
    for (int i = 0; i < nfields; i++) {
        if (fields[i].node != n) continue;
        ctrl_focus = i;
        if (is_check) {
            fields[i].checked = !fields[i].checked;
        } else if (fields[i].kind == 0) {
            int c = (lx - 2) / 8;
            if (c < 0) c = 0;
            if (c > fields[i].len) c = fields[i].len;
            fields[i].caret = c;
        }
        break;
    }
    (void)ly;
    draw();
}

static void handle_click(int mx, int my)
{
    if (my < NAV_H) {
        int bx = MARGIN;
        if (mx >= bx && mx < bx + NAV_BTN_W) { go_back(); return; }
        bx += NAV_BTN_W + NAV_BTN_GAP;
        if (mx >= bx && mx < bx + NAV_BTN_W) { go_forward(); return; }
        bx += NAV_BTN_W + NAV_BTN_GAP;
        if (mx >= bx && mx < bx + NAV_BTN_W) { reload(); return; }
        focus_url();
        return;
    }
    if (my < TOP_H) {
        int i = bookmark_at(mx, my);
        if (i >= 0) open_bookmark(i);
        return;
    }

    int lx = mx - MARGIN;
    int ly = my - TOP_H + scroll;
    for (int i = 0; i < nboxes; i++) {
        struct slweb_box *b = &boxes[i];
        if ((b->kind != SLWEB_BOX_INPUT && b->kind != SLWEB_BOX_BUTTON) || !b->node) continue;
        if (b->x <= lx && b->x + b->w >= lx && b->y <= ly && b->y + b->h > ly) {
            click_control(b, lx - b->x, ly - b->y);
            return;
        }
    }
    int idx = click_link(mx, my);
    if (idx) follow(idx);
}

static int done;

static void handle_key(unsigned c)
{
    if (url_focus) {
        if (c == '\n' || c == '\r') {
            if (addr_len) {
                char target[512];
                snprintf(target, sizeof target, "%s", addr);
                url_focus = 0;
                addr_sel_all = 0;
                printf("BROWSER: go %s\n", target);
                navigate(target, NULL, NULL, 0, 0, 1);
            }
            return;
        }
        if (c == 27) { url_focus = 0; addr_sel_all = 0; printf("BROWSER: addr blur\n"); draw(); return; }
        if (c == '\t') { url_focus = 0; addr_sel_all = 0; tab_next(); return; }
        /* A freshly focused bar selects the whole URL: the first edit replaces it. */
        if (addr_sel_all && (c == 8 || c == 0x87 || (c >= 32 && c < 127))) {
            addr[0] = 0; addr_len = 0; addr_caret = 0; addr_sel_all = 0;
        } else if (addr_sel_all && c >= 0x83 && c <= 0x86) {
            addr_sel_all = 0;                 /* arrows/home/end just move the caret */
        }
        if (text_key(addr, sizeof addr, &addr_len, &addr_caret, c)) draw();
        return;
    }

    if (ctrl_focus >= 0) {
        struct field *f = &fields[ctrl_focus];
        if (c == 27) { ctrl_focus = -1; printf("BROWSER: field blur\n"); draw(); return; }
        if (c == '\t') { tab_next(); return; }
        if (c == '\n' || c == '\r') { submit_from_field(ctrl_focus); return; }
        if (f->kind == 1) {
            if (c == ' ') { f->checked = !f->checked; draw(); }
            return;
        }
        if (f->kind == 2) {
            if (c == 0x81) { change_select(ctrl_focus, -1); draw(); }
            else if (c == 0x82) { change_select(ctrl_focus, 1); draw(); }
            return;
        }
        if (text_key(f->value, sizeof f->value, &f->len, &f->caret, c)) draw();
        return;
    }

    if (c == 'q' || c == 27) { printf("BROWSER: quit key\n"); done = 1; }
    else if (c == 'b') go_back();
    else if (c == 'f') go_forward();
    else if (c == 'r') reload();
    else if (c == 12) focus_url();                 /* Ctrl-L */
    else if (c == 4) toggle_bookmark();            /* Ctrl-D */
    else if (c == 2) {                             /* Ctrl-B */
        bm_focus = !bm_focus;
        printf("BROWSER: bookmarks %s\n", bm_focus ? "focus" : "blur");
        draw();
    } else if (c >= '1' && c <= '9') {
        if (bm_focus) open_bookmark((int)(c - '1'));
        else follow((int)(c - '0'));
    } else if (c == '\t') {
        tab_next();
    } else if (c == '\n' || c == '\r') {
        if (selected) follow(selected);
    } else if (c == 0x82 || c == 'n') {
        int view = H - TOP_H - BOT_H;
        if (scroll + view < doc_h) { scroll += view / 2; draw(); }
    } else if (c == 0x81 || c == 'p') {
        int view = H - TOP_H - BOT_H;
        scroll -= view / 2; if (scroll < 0) scroll = 0; draw();
    } else if (c == 0x89) {
        int view = H - TOP_H - BOT_H;
        if (scroll + view < doc_h) { scroll += view; draw(); }
    } else if (c == 0x88) {
        int view = H - TOP_H - BOT_H;
        scroll -= view; if (scroll < 0) scroll = 0; draw();
    } else if (c == 0x85) { scroll = 0; draw(); }
    else if (c == 0x86) { scroll = doc_h; draw(); }
}

/* ------------------------------------------------------------------ main */
static void usage(void)
{
    static const char page[] =
        "<h2>BROWSER (M9)</h2>"
        "<p>Usage: BROWSER.ELF [--insecure] [--cafile FILE] &lt;url&gt;</p>"
        "<p>The page is parsed into a DOM, styled with its CSS subset, laid out "
        "into boxes and drawn with a built-in 8x8 font; PNG and base64 "
        "images are decoded and painted.</p>"
        "<p>M7 chrome: the address bar (Ctrl-L), back/forward history (b/f), an "
        "HTTP cache, bookmarks (Ctrl-D, then Ctrl-B + 1-9) and HTML forms "
        "(Tab to move, type to edit, Enter to submit). M8 adds TCP window "
        "scaling (RFC 7323) with a 128 KiB receive window for faster large "
        "transfers.</p>"
        "<p>M9.4 adds real https://: the server certificate is validated against "
        "roots loaded with --cafile (the store starts empty and fails closed), "
        "and --insecure / -k skips validation. A validated page shows [secure]; "
        "otherwise [unverified connection].</p>";
    free_page();
    nboxes = nlinks = 0;
    doc_h = 0;
    dom = slweb_dom_parse(page, strlen(page));
    css = slweb_css_parse(dom);
    snprintf(status_line, sizeof status_line, "no url given");
    layout_and_log();
    collect_fields();
    selected = 0;
    draw();
}

int main(int argc, char **argv)
{
    const char *start_url = NULL;

    printf("BROWSER: start\n");

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--insecure") || !strcmp(argv[i], "-k")) {
            opt_insecure = 1;
        } else if (!strcmp(argv[i], "--cafile") && i + 1 < argc) {
            int roots = slweb_http_add_ca_file(argv[++i]);
            printf("BROWSER: cafile %s roots=%d\n", argv[i], roots);
        } else if (argv[i][0]) {
            start_url = argv[i];
        }
    }
    /* M9.5 will ship a bundle here; a harmless no-op until then (the store
     * still starts empty and https fails closed). */
    if (!opt_insecure) slweb_http_add_ca_file("/etc/ssl/certs/ca-certificates.crt");
    if (opt_insecure) printf("BROWSER: insecure certificate validation off\n");

    fb = (uint32_t *)sl_fbmap(W, H);
    if (!fb) { printf("BROWSER: fbmap FAIL errno=%d\n", errno); return 1; }
    printf("BROWSER: fbmap ok %dx%d\n", W, H);

    cache = slweb_cache_new(64, 8 * 1024 * 1024);
    nhist = 0;
    hpos = -1;
    bookmarks_load();

    if (start_url && start_url[0]) navigate(start_url, NULL, NULL, 0, 0, 1);
    else usage();

    while (!done) {
        struct sl_event evs[16];
        int n = sl_getevents(evs, 16, 50);
        if (n < 0) {
            if (errno == EINTR) continue;
            printf("BROWSER: getevents errno=%d\n", errno);
            break;
        }
        for (int k = 0; k < n && !done; k++) {
            struct sl_event *e = &evs[k];
            if (e->type == SL_EV_QUIT) { printf("BROWSER: quit event\n"); done = 1; break; }
            if (e->type == SL_EV_DOWN) {
                if (e->data & SL_BTN_LEFT) handle_click(e->x, e->y);
                continue;
            }
            if (e->type != SL_EV_KEY) continue;
            handle_key(e->data);
        }
    }

    free_page();
    slweb_cache_free(cache);
    printf("BROWSER: done\n");
    return 0;
}
