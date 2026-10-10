/*
 * libslweb/layout.c -- M5/M6: turn the DOM into a flat list of positioned boxes.
 *
 * M5 established the box model: block elements stack vertically, inline content
 * flows and wraps, and the result is an ordered array of struct slweb_box that
 * the browser draws and hit-tests. M6 layers a CSS subset and bitmap images on
 * top without changing the model:
 *
 *   - a stylesheet (css.c) supplies author declarations which are merged over
 *     the built-in UA defaults per element;
 *   - colour, background, font-weight/style, text-decoration, font-size (as a
 *     glyph scale), display:none, text-align, margin, padding, width and height
 *     are honoured;
 *   - a block with a background emits an SLWEB_BOX_RECT behind its content;
 *   - <img> becomes an SLWEB_BOX_IMAGE carrying the decoded slweb_image (or an
 *     alt-text placeholder when the bitmap could not be fetched).
 *
 * Metrics stay fixed so the host tests can assert exact geometry: an 8x8 glyph
 * in a 10 px line, doubled for <h1>/<h2>. Colour is data on the box; the
 * renderer (or its palette) decides how to paint it.
 */
#include <stdlib.h>
#include <string.h>
#include "slweb.h"

static int gw(int scale) { return scale >= 2 ? 16 : 8; }
static int lh(int scale) { return scale >= 2 ? 16 : 10; }

static int lay_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int lay_is(const char *t, const char *n) { return strcmp(t, n) == 0; }

struct style {
    int scale, bold, italic, link, underline;
    int color_set; uint32_t color;
    int bg_set; uint32_t bg;
    int align;
};

struct lctx {
    struct slweb_box *boxes;
    int max, n;
    int viewport;
    struct slweb_link *links;
    size_t max_links, nlinks;
    int pre;
    const struct slweb_css *css;
    const struct slweb_image_entry *images;
    size_t n_images;
};

struct pen {
    int x, y;          /* current pen in layout space                      */
    int left, right;   /* content edges for the current block              */
    int line_h;        /* tallest box on the current line                  */
    int active;        /* is anything on the current line?                 */
    int pending_space; /* collapsed whitespace waiting to be emitted       */
};

static void layout_content(struct lctx *C, const struct slweb_node *el,
                           struct pen *p, struct style st);

static void set_box_style(struct slweb_box *b, const struct style *st)
{
    if (!b) return;
    b->underline = (uint8_t)(st->underline ? 1 : 0);
    if (st->color_set) { b->fg_set = 1; b->fg = st->color; }
    if (st->bg_set) { b->bg_set = 1; b->bg = st->bg; }
}

static struct slweb_box *add_box(struct lctx *C, int kind, int x, int y, int w, int h,
                                 int scale, int bold, int italic, int link, int marker,
                                 const char *text, int tlen)
{
    if (C->n >= C->max) return NULL;
    struct slweb_box *b = &C->boxes[C->n++];
    memset(b, 0, sizeof *b);
    b->kind = (uint8_t)kind;
    b->scale = (uint8_t)(scale < 1 ? 1 : scale);
    b->bold = (uint8_t)(bold ? 1 : 0);
    b->italic = (uint8_t)(italic ? 1 : 0);
    b->link = link;
    b->marker = marker;
    b->x = x; b->y = y; b->w = w; b->h = h;
    b->text = text; b->text_len = tlen;
    return b;
}

/* ------------------------------------------------------------------ pen */
static void pen_newline(struct pen *p)
{
    if (p->active) p->y += p->line_h ? p->line_h : SLWEB_LINE_H;
    p->x = p->left;
    p->line_h = 0;
    p->active = 0;
    p->pending_space = 0;
}

static void pen_word(struct lctx *C, struct pen *p, const char *s, int len, struct style st)
{
    int w = len * gw(st.scale);
    int h = lh(st.scale);
    int sp = p->pending_space ? gw(st.scale) : 0;
    if (p->active && p->x + sp + w > p->right) pen_newline(p);
    if (!p->active) { p->x = p->left; p->active = 1; }
    else if (p->pending_space) p->x += gw(st.scale);
    struct slweb_box *b = add_box(C, SLWEB_BOX_TEXT, p->x, p->y, w, h,
                                  st.scale, st.bold, st.italic, st.link, 0, s, len);
    set_box_style(b, &st);
    p->x += w;
    if (h > p->line_h) p->line_h = h;
    p->pending_space = 0;
}

/* One preformatted line: spaces are kept, no wrapping. */
static void pen_raw(struct lctx *C, struct pen *p, const char *s, int len, struct style st)
{
    int w = len * gw(st.scale);
    int h = lh(st.scale);
    if (!p->active) { p->x = p->left; p->active = 1; }
    struct slweb_box *b = add_box(C, SLWEB_BOX_TEXT, p->x, p->y, w, h,
                                  st.scale, st.bold, st.italic, st.link, 0, s, len);
    set_box_style(b, &st);
    p->x += w;
    if (h > p->line_h) p->line_h = h;
    p->pending_space = 0;
}

static void feed_text(struct lctx *C, struct pen *p, const char *s, int len, struct style st)
{
    if (C->pre) {
        int start = 0;
        for (int i = 0; i <= len; i++) {
            if (i == len || s[i] == '\n') {
                if (i > start) pen_raw(C, p, s + start, i - start, st);
                if (i < len) pen_newline(p);
                start = i + 1;
            }
        }
        return;
    }
    int i = 0;
    while (i < len) {
        if (lay_space(s[i])) { p->pending_space = 1; i++; continue; }
        int start = i;
        while (i < len && !lay_space(s[i])) i++;
        pen_word(C, p, s + start, i - start, st);
    }
}

/* ------------------------------------------------------------------ links */
static int collect_link(struct lctx *C, const struct slweb_node *el)
{
    const char *href = slweb_node_attr(el, "href");
    if (!href || !href[0] || href[0] == '#' || strncmp(href, "javascript", 10) == 0) return 0;
    if (!C->links || C->nlinks >= C->max_links) return 0;
    strncpy(C->links[C->nlinks].href, href, SLWEB_LINK_MAX - 1);
    C->links[C->nlinks].href[SLWEB_LINK_MAX - 1] = 0;
    C->nlinks++;
    return (int)C->nlinks;
}

/* ------------------------------------------------------------------ images */
static const struct slweb_image *find_image(struct lctx *C, const struct slweb_node *el)
{
    for (size_t i = 0; i < C->n_images; i++)
        if (C->images[i].node == el) return C->images[i].image;
    return NULL;
}

/* ------------------------------------------------------------------ tags */
static int is_hidden(const char *t)
{
    return lay_is(t, "head") || lay_is(t, "script") || lay_is(t, "style") ||
           lay_is(t, "title") || lay_is(t, "meta") || lay_is(t, "link") ||
           lay_is(t, "noscript") || lay_is(t, "template") || lay_is(t, "svg") ||
           lay_is(t, "base") || lay_is(t, "iframe") || lay_is(t, "option") ||
           lay_is(t, "optgroup") || lay_is(t, "datalist");
}

static int is_block(const char *t)
{
    static const char *b[] = {
        "address","article","aside","blockquote","body","center","dd","details","dialog","dir",
        "div","dl","dt","fieldset","figcaption","figure","footer","form","h1","h2","h3","h4","h5",
        "h6","header","hgroup","hr","html","li","main","menu","nav","ol","p","pre","section",
        "summary","table","tbody","td","tfoot","th","thead","tr","ul", NULL
    };
    for (int i = 0; b[i]; i++) if (lay_is(t, b[i])) return 1;
    return 0;
}

/* ------------------------------------------------------------------ blocks */
static void layout_rule(struct lctx *C, struct pen *p)
{
    pen_newline(p);
    p->y += 6;
    int w = p->right - p->left;
    if (w < 1) w = 1;
    add_box(C, SLWEB_BOX_RULE, p->left, p->y, w, 1, 1, 0, 0, 0, 0, NULL, 0);
    p->y += 7;
    p->x = p->left;
}

static void layout_image(struct lctx *C, const struct slweb_node *el, struct pen *p,
                         struct style st, int cw, int ch)
{
    const char *alt = slweb_node_attr(el, "alt");
    int alen = alt ? (int)strlen(alt) : 0;
    const struct slweb_image *img = find_image(C, el);

    int w, h;
    if (img) {
        int iw = img->w, ih = img->h;
        if (iw < 1) iw = 1;
        if (ih < 1) ih = 1;
        if (cw >= 0 && ch >= 0) { w = cw; h = ch; }
        else if (cw >= 0)       { w = cw; h = (int)((long)ih * cw / iw); }
        else if (ch >= 0)       { h = ch; w = (int)((long)iw * ch / ih); }
        else                    { w = iw; h = ih; }
    } else {
        w = alen ? alen * gw(st.scale) + 8 : 24;
        h = lh(st.scale) + 4;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    int sp = p->pending_space ? gw(st.scale) : 0;
    if (p->active && p->x + sp + w > p->right) pen_newline(p);
    if (!p->active) { p->x = p->left; p->active = 1; }
    else if (p->pending_space) p->x += gw(st.scale);
    struct slweb_box *b = add_box(C, SLWEB_BOX_IMAGE, p->x, p->y, w, h,
                                  st.scale, 0, 0, st.link, 0, alt, alen);
    if (b) {
        set_box_style(b, &st);
        b->image = img;
    }
    p->x += w;
    if (h > p->line_h) p->line_h = h;
    p->pending_space = 0;
}

/* ------------------------------------------------------------ M7 controls */
static int eq_ci_str(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

static int attr_int(const struct slweb_node *el, const char *name, int dflt, int lo, int hi)
{
    const char *v = slweb_node_attr(el, name);
    int n = (v && v[0]) ? atoi(v) : dflt;
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    return n;
}

/* Place an atomic inline box (like an image): wrap when it does not fit. */
static struct slweb_box *layout_atomic(struct lctx *C, struct pen *p, struct style st,
                                       int w, int h, int kind,
                                       const struct slweb_node *node,
                                       const char *text, int text_len)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int sp = p->pending_space ? gw(st.scale) : 0;
    if (p->active && p->x + sp + w > p->right) pen_newline(p);
    if (!p->active) { p->x = p->left; p->active = 1; }
    else if (p->pending_space) p->x += gw(st.scale);
    struct slweb_box *b = add_box(C, kind, p->x, p->y, w, h, st.scale, st.bold,
                                  st.italic, st.link, 0, text, text_len);
    if (b) { set_box_style(b, &st); b->node = node; }
    p->x += w;
    if (h > p->line_h) p->line_h = h;
    p->pending_space = 0;
    return b;
}

static const struct slweb_node *first_text_node(const struct slweb_node *n)
{
    for (const struct slweb_node *c = n->first_child; c; c = c->next_sibling)
        if (c->type == SLWEB_NODE_TEXT) return c;
    return NULL;
}

static int is_button_type(const char *t)
{
    return eq_ci_str(t, "submit") || eq_ci_str(t, "reset") ||
           eq_ci_str(t, "button") || eq_ci_str(t, "image");
}

/* <input>: hidden draws nothing; submit/reset/button/image -> BUTTON;
 * checkbox/radio -> small INPUT; everything else -> a text INPUT. */
static void layout_input(struct lctx *C, const struct slweb_node *el, struct pen *p,
                         struct style st, int width, int height)
{
    const char *type = slweb_node_attr(el, "type");
    if (type && eq_ci_str(type, "hidden")) return;

    if (type && is_button_type(type)) {
        const char *val = slweb_node_attr(el, "value");
        const char *label = val ? val : (eq_ci_str(type, "reset") ? "Reset"
                                        : eq_ci_str(type, "button") ? "Button" : "Submit");
        int w = width >= 0 ? width : (int)strlen(label) * gw(st.scale) + 12;
        int h = height >= 0 ? height : lh(st.scale) + 6;
        layout_atomic(C, p, st, w, h, SLWEB_BOX_BUTTON, el, label, (int)strlen(label));
        return;
    }
    if (type && (eq_ci_str(type, "checkbox") || eq_ci_str(type, "radio"))) {
        int w = width >= 0 ? width : SLWEB_GLYPH_W + 4;
        int h = height >= 0 ? height : SLWEB_GLYPH_W + 4;
        layout_atomic(C, p, st, w, h, SLWEB_BOX_INPUT, el, NULL, 0);
        return;
    }
    int size = attr_int(el, "size", 20, 1, 200);
    int w = width >= 0 ? width : size * gw(st.scale) + 4;
    int h = height >= 0 ? height : lh(st.scale) + 4;
    layout_atomic(C, p, st, w, h, SLWEB_BOX_INPUT, el, NULL, 0);
}

/* <textarea>: cols x rows, initial text comes from the child text node. */
static void layout_textarea(struct lctx *C, const struct slweb_node *el, struct pen *p,
                            struct style st, int width, int height)
{
    int cols = attr_int(el, "cols", 20, 1, 400);
    int rows = attr_int(el, "rows", 3, 1, 100);
    int w = width >= 0 ? width : cols * gw(st.scale) + 4;
    int h = height >= 0 ? height : rows * lh(st.scale) + 4;
    layout_atomic(C, p, st, w, h, SLWEB_BOX_INPUT, el, NULL, 0);
}

/* <button>: label is its text content, else value=, else "Button". */
static void layout_button(struct lctx *C, const struct slweb_node *el, struct pen *p,
                          struct style st, int width, int height)
{
    const struct slweb_node *t = first_text_node(el);
    const char *val = slweb_node_attr(el, "value");
    const char *label = t ? t->text : (val ? val : "Button");
    int len = t ? (int)t->text_len : (int)strlen(label);
    int w = width >= 0 ? width : len * gw(st.scale) + 12;
    int h = height >= 0 ? height : lh(st.scale) + 6;
    layout_atomic(C, p, st, w, h, SLWEB_BOX_BUTTON, el, label, len);
}

/* <select>: one box sized to the widest option, plus a dropdown arrow. */
static void layout_select(struct lctx *C, const struct slweb_node *el, struct pen *p,
                          struct style st, int width, int height)
{
    int maxlen = 0;
    for (const struct slweb_node *o = el->first_child; o; o = o->next_sibling) {
        if (o->type != SLWEB_NODE_ELEMENT || !lay_is(o->tag, "option")) continue;
        const struct slweb_node *t = first_text_node(o);
        int l = t ? (int)t->text_len : 0;
        if (l > maxlen) maxlen = l;
    }
    if (maxlen < 1) maxlen = 1;
    int w = width >= 0 ? width : maxlen * gw(st.scale) + 20;
    int h = height >= 0 ? height : lh(st.scale) + 4;
    layout_atomic(C, p, st, w, h, SLWEB_BOX_INPUT, el, NULL, 0);
}

static void layout_list_item(struct lctx *C, const struct slweb_node *li,
                             struct pen *p, struct style st, int ordered, int num)
{
    pen_newline(p);
    p->y += 3;
    add_box(C, SLWEB_BOX_MARKER, p->left, p->y, (ordered ? 3 : 1) * SLWEB_GLYPH_W,
            SLWEB_LINE_H, 1, st.bold, 0, 0, ordered ? num : 0, NULL, 0);
    int sl = p->left, sr = p->right;
    p->left += 20;
    if (p->left > p->right - 8) p->left = p->right - 8;
    layout_content(C, li, p, st);
    pen_newline(p);
    p->left = sl;
    p->right = sr;
    p->x = p->left;
    p->y += 3;
}

static void layout_list(struct lctx *C, const struct slweb_node *el,
                        struct pen *p, struct style st)
{
    int ordered = lay_is(el->tag, "ol");
    int idx = 1;
    if (ordered) {
        const char *sv = slweb_node_attr(el, "start");
        if (sv && sv[0]) idx = atoi(sv);
    }
    pen_newline(p);
    p->y += 6;
    for (const struct slweb_node *c = el->first_child; c; c = c->next_sibling) {
        if (c->type == SLWEB_NODE_ELEMENT && lay_is(c->tag, "li")) {
            layout_list_item(C, c, p, st, ordered, idx);
            if (ordered) idx++;
        } else if (c->type == SLWEB_NODE_TEXT) {
            feed_text(C, p, c->text, (int)c->text_len, st);
        } else if (c->type == SLWEB_NODE_ELEMENT) {
            layout_content(C, c, p, st);
        }
    }
    pen_newline(p);
    p->y += 6;
    p->x = p->left;
}

/* Inline box kinds that flow on a line and can be shifted by text-align. */
static int is_inline_kind(int k)
{
    return k == SLWEB_BOX_TEXT || k == SLWEB_BOX_IMAGE ||
           k == SLWEB_BOX_INPUT || k == SLWEB_BOX_BUTTON;
}

/* Shift inline runs on each line to honour text-align. */
static void apply_align(struct lctx *C, int from, int to, int left, int right, int align)
{
    if (align == SLWEB_ALIGN_LEFT || from >= to) return;
    int avail = right - left;
    if (avail <= 0) return;
    for (int i = from; i < to; i++) {
        struct slweb_box *b = &C->boxes[i];
        if (!is_inline_kind(b->kind)) continue;
        int y = b->y;
        int seen = 0;
        for (int j = from; j < i; j++) {
            struct slweb_box *o = &C->boxes[j];
            if (is_inline_kind(o->kind) && o->y == y) { seen = 1; break; }
        }
        if (seen) continue;
        int minx = b->x, maxx = b->x + b->w;
        for (int j = i + 1; j < to; j++) {
            struct slweb_box *o = &C->boxes[j];
            if (!is_inline_kind(o->kind) || o->y != y) continue;
            if (o->x < minx) minx = o->x;
            if (o->x + o->w > maxx) maxx = o->x + o->w;
        }
        int linew = maxx - minx;
        int target = (align == SLWEB_ALIGN_CENTER) ? left + (avail - linew) / 2
                                                   : left + (avail - linew);
        int shift = target - minx;
        if (shift == 0) continue;
        for (int j = i; j < to; j++) {
            struct slweb_box *o = &C->boxes[j];
            if (is_inline_kind(o->kind) && o->y == y)
                o->x += shift;
        }
    }
}

static void layout_block(struct lctx *C, const struct slweb_node *el, struct pen *p,
                         struct style st, const int m[4], const int pad[4],
                         int width, int height)
{
    const char *t = el->tag;
    if (lay_is(t, "hr")) { layout_rule(C, p); return; }
    if (lay_is(t, "ul") || lay_is(t, "ol")) { layout_list(C, el, p, st); return; }

    pen_newline(p);
    int savedL = p->left, savedR = p->right;

    int bgx = p->left + m[3];
    int bgw = (p->right - m[1]) - bgx;
    p->y += m[0];
    int y0 = p->y;

    int slot = -1;
    if (st.bg_set && bgw > 0) {
        struct slweb_box *rb = add_box(C, SLWEB_BOX_RECT, bgx, y0, bgw, 0, 1, 0, 0, 0, 0, NULL, 0);
        if (rb) { rb->bg_set = 1; rb->bg = st.bg; slot = C->n - 1; }
    }

    p->y += pad[0];
    p->left += m[3] + pad[3];
    p->right -= m[1] + pad[1];
    if (width >= 0 && p->left + width < p->right) p->right = p->left + width;
    if (p->left > p->right - 8) p->left = p->right - 8;

    int mark = C->n;
    if (lay_is(t, "pre")) {
        C->pre++;
        layout_content(C, el, p, st);
        C->pre--;
    } else {
        layout_content(C, el, p, st);
    }
    pen_newline(p);
    apply_align(C, mark, C->n, p->left, p->right, st.align);

    p->left = savedL;
    p->right = savedR;
    p->x = p->left;
    p->y += pad[2];
    if (height >= 0 && y0 + height > p->y) p->y = y0 + height;
    if (slot >= 0) C->boxes[slot].h = p->y - y0;
    p->y += m[2];
}

/* ------------------------------------------------------------------ dispatch */
static void layout_element(struct lctx *C, const struct slweb_node *el,
                           struct pen *p, struct style st)
{
    const char *t = el->tag;
    if (is_hidden(t)) return;

    struct slweb_style cs;
    slweb_style_compute(C->css, el, &cs);
    if (cs.display_set && cs.display_none) return;

    struct style ns = st;
    ns.bg_set = 0; ns.bg = 0;                 /* background does not inherit   */

    /* inline UA defaults */
    if (lay_is(t, "b") || lay_is(t, "strong")) ns.bold = 1;
    else if (lay_is(t, "i") || lay_is(t, "em") || lay_is(t, "cite") ||
             lay_is(t, "var") || lay_is(t, "dfn")) ns.italic = 1;
    else if (lay_is(t, "big")) { if (ns.scale < 2) ns.scale = 2; }

    /* block UA defaults */
    int m[4] = {0,0,0,0}, pad[4] = {0,0,0,0};
    int width = -1, height = -1;
    int block = is_block(t);
    if (block) {
        if (lay_is(t, "p")) { m[0] = 6; m[2] = 6; }
        else if (lay_is(t, "blockquote")) { m[3] = 20; m[0] = 6; m[2] = 6; ns.italic = 1; }
        else if (lay_is(t, "dd")) { m[3] = 20; m[0] = 2; m[2] = 2; }
        else if (lay_is(t, "dt")) { m[0] = 3; m[2] = 1; ns.bold = 1; }
        else if (lay_is(t, "figure")) { m[3] = 12; m[1] = 12; m[0] = 6; m[2] = 6; }
        else if (lay_is(t, "figcaption")) { m[0] = 2; m[2] = 6; ns.italic = 1; }
        else if (lay_is(t, "table")) { m[0] = 6; m[2] = 6; }
        else if (lay_is(t, "tr")) { m[0] = 2; m[2] = 2; }
        else if (lay_is(t, "address")) { m[0] = 6; m[2] = 6; ns.italic = 1; }
        else if (lay_is(t, "center")) { ns.align = SLWEB_ALIGN_CENTER; }
        else if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2]) {
            ns.scale = (t[1] <= '2') ? 2 : 1;
            ns.bold = 1;
            m[0] = (ns.scale == 2) ? 10 : 8;
            m[2] = 6;
        } else if (lay_is(t, "pre")) { m[0] = 6; m[2] = 6; }
    }

    /* author declarations override UA defaults */
    if (cs.color_set) { ns.color_set = 1; ns.color = cs.color; }
    if (cs.bold_set) ns.bold = cs.bold;
    if (cs.italic_set) ns.italic = cs.italic;
    if (cs.underline_set) ns.underline = cs.underline;
    if (cs.scale_set && cs.scale >= 1) ns.scale = cs.scale;
    if (cs.align_set) ns.align = cs.align;
    if (cs.bg_set) { ns.bg_set = 1; ns.bg = cs.bg; }
    for (int i = 0; i < 4; i++) if (cs.margin_set[i]) m[i] = cs.margin[i];
    for (int i = 0; i < 4; i++) if (cs.pad_set[i]) pad[i] = cs.pad[i];
    if (cs.width_set) width = cs.width;
    if (cs.height_set) height = cs.height;

    if (ns.scale < 1) ns.scale = 1;

    if (lay_is(t, "br")) { pen_newline(p); return; }
    if (lay_is(t, "wbr")) return;
    if (lay_is(t, "img")) { layout_image(C, el, p, ns, width, height); return; }
    if (lay_is(t, "input"))   { layout_input(C, el, p, ns, width, height); return; }
    if (lay_is(t, "textarea")){ layout_textarea(C, el, p, ns, width, height); return; }
    if (lay_is(t, "button"))  { layout_button(C, el, p, ns, width, height); return; }
    if (lay_is(t, "select"))  { layout_select(C, el, p, ns, width, height); return; }

    if (lay_is(t, "a")) {
        ns.link = collect_link(C, el);
        layout_content(C, el, p, ns);
        return;
    }

    if (block) { layout_block(C, el, p, ns, m, pad, width, height); return; }
    layout_content(C, el, p, ns);
}

static void layout_content(struct lctx *C, const struct slweb_node *el,
                           struct pen *p, struct style st)
{
    for (const struct slweb_node *c = el->first_child; c; c = c->next_sibling) {
        if (c->type == SLWEB_NODE_TEXT) {
            feed_text(C, p, c->text, (int)c->text_len, st);
        } else if (c->type == SLWEB_NODE_ELEMENT) {
            layout_element(C, c, p, st);
        }
    }
}

int slweb_layout_ex(const struct slweb_node *root, int viewport_w,
                    const struct slweb_layout_opts *opts,
                    struct slweb_box *boxes, int max_boxes,
                    struct slweb_link *links, size_t max_links, size_t *n_links,
                    int *doc_w, int *doc_h)
{
    struct lctx C;
    memset(&C, 0, sizeof C);
    C.boxes = boxes;
    C.max = max_boxes;
    C.viewport = viewport_w < 40 ? 40 : viewport_w;
    C.links = links;
    C.max_links = max_links;
    if (opts) {
        C.css = opts->css;
        C.images = opts->images;
        C.n_images = opts->n_images;
    }

    struct pen p;
    memset(&p, 0, sizeof p);
    p.left = 0;
    p.right = C.viewport;
    p.x = 0;
    p.y = 0;

    struct style st;
    memset(&st, 0, sizeof st);
    st.scale = 1;
    st.align = SLWEB_ALIGN_LEFT;

    if (root) layout_content(&C, root, &p, st);
    if (p.active) p.y += p.line_h ? p.line_h : SLWEB_LINE_H;

    if (doc_w) *doc_w = C.viewport;
    if (doc_h) *doc_h = p.y;
    if (n_links) *n_links = C.nlinks;
    return C.n;
}

int slweb_layout(const struct slweb_node *root, int viewport_w,
                 struct slweb_box *boxes, int max_boxes,
                 struct slweb_link *links, size_t max_links, size_t *n_links,
                 int *doc_w, int *doc_h)
{
    return slweb_layout_ex(root, viewport_w, NULL, boxes, max_boxes,
                           links, max_links, n_links, doc_w, doc_h);
}
