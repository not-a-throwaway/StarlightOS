/*
 * libslweb -- small HTML -> text renderer (lifted from the kernel's html.c).
 *
 * Handles block structure, lists, headings, links, entities, <pre>, and skips
 * <script>/<style>/<head>. Not a layout engine; just readable text. M3 moved it
 * to newlib (isspace/snprintf/strtoul/strncmp/strncasecmp) and renamed the entry
 * point slweb_html_to_text; the algorithm is unchanged so the kernel host test
 * corpus (tests/host/html_test.py) applies to it verbatim.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "slweb.h"

/* exact ASCII whitespace set the kernel used; isspace() would be locale/UB-prone on bytes >= 0x80 */
static int h_isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
static void h_strlcpy(char *d, const char *s, size_t cap)
{
    size_t i = 0;
    if (!cap) return;
    for (; i + 1 < cap && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

struct r {
    char *out; size_t cap, len;
    int width, col;
    int pending_space, pending_nl, at_line_start;
    int pre, list_depth, in_head;
    int ol[4], ol_idx;
    int links;
    /* Collected hrefs (document order). If the caller passed a links[] buffer
     * we write straight into it; otherwise we use the internal fallback so
     * slweb_html_to_text() can still number links and print the list. */
    struct slweb_link *links_out; size_t max_links, n_links;
    char link_buf[SLWEB_MAX_LINKS][SLWEB_LINK_MAX];
    int link_pending;                 /* 1-based index of the open <a>, 0 if none */
};

static const char *r_href(const struct r *r, size_t k)
{
    return r->links_out ? r->links_out[k].href : r->link_buf[k];
}

static void put_raw(struct r *r, char c) { if (r->len + 1 < r->cap) r->out[r->len++] = c; }

static void newline(struct r *r, int count)
{
    if (r->len == 0) return;
    if (count > r->pending_nl) r->pending_nl = count;
}

static void flush_nl(struct r *r)
{
    if (!r->pending_nl) return;
    int have = 0;                                  /* newlines already at end of output */
    for (size_t i = r->len; i > 0 && r->out[i - 1] == '\n' && have < 2; i--) have++;
    for (int i = have; i < r->pending_nl; i++) put_raw(r, '\n');
    r->pending_nl = 0; r->col = 0; r->at_line_start = 1; r->pending_space = 0;
}

static void put_word_char(struct r *r, char c)
{
    flush_nl(r);
    if (r->pending_space && !r->at_line_start) { put_raw(r, ' '); r->col++; }
    r->pending_space = 0; r->at_line_start = 0;
    put_raw(r, c); r->col++;
}

static void put_text_word(struct r *r, const char *w, size_t n)
{
    flush_nl(r);
    if (!r->pre && r->col + (int)n + (r->pending_space && !r->at_line_start ? 1 : 0) > r->width && r->col > 0) {
        put_raw(r, '\n'); r->col = 0; r->at_line_start = 1; r->pending_space = 0;
        for (int i = 0; i < r->list_depth * 2 && i < 8; i++) { put_raw(r, ' '); r->col++; }
    }
    for (size_t i = 0; i < n; i++) put_word_char(r, w[i]);
}

static int decode_entity(const char *s, size_t n, char *out)
{
    static const struct { const char *n; char c; } tab[] = {
        {"amp",'&'},{"lt",'<'},{"gt",'>'},{"quot",'"'},{"apos",'\''},{"nbsp",' '},{"copy",'c'},{"reg",'R'},
        {"hellip",'.'},{"mdash",'-'},{"ndash",'-'},{"lsquo",'\''},{"rsquo",'\''},{"ldquo",'"'},{"rdquo",'"'},
        {"bull",'*'},{"middot",'.'},{"raquo",'>'},{"laquo",'<'},{"trade",'T'},{"times",'x'},{"larr",'<'},{"rarr",'>'}
    };
    if (n >= 2 && s[0] == '#') {
        uint32_t v = (s[1] == 'x' || s[1] == 'X') ? (uint32_t)strtoul(s + 2, NULL, 16) : (uint32_t)strtoul(s + 1, NULL, 10);
        *out = (v >= 32 && v < 127) ? (char)v : (v == 8217 || v == 8216) ? '\'' : (v == 8220 || v == 8221) ? '"' :
               (v == 8211 || v == 8212) ? '-' : (v == 160) ? ' ' : '?';
        return 1;
    }
    for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (strlen(tab[i].n) == n && strncmp(tab[i].n, s, n) == 0) { *out = tab[i].c; return 1; }
    return 0;
}

static int tag_is(const char *name, size_t nl, const char *want) { return strlen(want) == nl && strncasecmp(name, want, nl) == 0; }

static int get_attr(const char *tag, size_t tl, const char *attr, char *out, size_t cap)
{
    size_t al = strlen(attr);
    for (size_t i = 0; i + al + 1 < tl; i++) {
        if ((i == 0 || h_isspace(tag[i - 1])) && strncasecmp(tag + i, attr, al) == 0 && tag[i + al] == '=') {
            size_t p = i + al + 1; char q = 0;
            if (tag[p] == '"' || tag[p] == '\'') q = tag[p++];
            size_t o = 0;
            while (p < tl && o + 1 < cap && (q ? tag[p] != q : !h_isspace(tag[p]) && tag[p] != '>')) out[o++] = tag[p++];
            out[o] = 0;
            return 1;
        }
    }
    return 0;
}

static void handle_tag(struct r *r, const char *t, size_t tl)
{
    int closing = 0;
    if (tl && t[0] == '/') { closing = 1; t++; tl--; }
    size_t nl = 0;
    while (nl < tl && !h_isspace(t[nl]) && t[nl] != '/') nl++;

    /* <head> is skipped until </head>, <body>, or the first tag that cannot live in a head (so a page that
     * never closes its head still renders). script/style/noscript/svg/template/title are raw-skipped by
     * the caller (skip_raw), never seen here. */
    if (tag_is(t, nl, "head")) { r->in_head = !closing; return; }
    if (r->in_head) {
        if (closing || tag_is(t, nl, "meta") || tag_is(t, nl, "link") || tag_is(t, nl, "base")) return;
        r->in_head = 0;
    }

    if (tag_is(t, nl, "br")) { r->pending_nl = r->pending_nl < 1 ? 1 : r->pending_nl; flush_nl(r); return; }
    if (tag_is(t, nl, "p") || tag_is(t, nl, "div") || tag_is(t, nl, "section") || tag_is(t, nl, "article") ||
        tag_is(t, nl, "header") || tag_is(t, nl, "footer") || tag_is(t, nl, "blockquote") || tag_is(t, nl, "table") ||
        tag_is(t, nl, "form") || tag_is(t, nl, "main") || tag_is(t, nl, "nav") || tag_is(t, nl, "figure")) {
        newline(r, tag_is(t, nl, "div") ? 1 : 2); return;
    }
    if (nl == 2 && (t[0] == 'h' || t[0] == 'H') && t[1] >= '1' && t[1] <= '6') {
        newline(r, 2);
        if (!closing) { flush_nl(r); if (t[1] <= '2') put_text_word(r, "# ", 2); else put_text_word(r, "## ", 3); r->pending_space = 0; }
        return;
    }
    if (tag_is(t, nl, "ul") || tag_is(t, nl, "ol")) {
        if (!closing) { if (r->list_depth < 4) { r->ol[r->list_depth] = tag_is(t, nl, "ol") ? 1 : 0; r->list_depth++; } }
        else if (r->list_depth) r->list_depth--;
        newline(r, 1); return;
    }
    if (tag_is(t, nl, "li")) {
        if (!closing) {
            newline(r, 1); flush_nl(r);
            for (int i = 1; i < r->list_depth; i++) { put_raw(r, ' '); put_raw(r, ' '); r->col += 2; }
            if (r->list_depth && r->ol[r->list_depth - 1]) {
                char b[8]; snprintf(b, sizeof(b), "%d. ", r->ol_idx++); put_text_word(r, b, strlen(b));
            } else put_text_word(r, "* ", 2);
            r->pending_space = 0;
        }
        return;
    }
    if (tag_is(t, nl, "tr")) { newline(r, 1); return; }
    if (tag_is(t, nl, "td") || tag_is(t, nl, "th")) { if (!closing) { r->pending_space = 1; } return; }
    if (tag_is(t, nl, "hr")) { newline(r, 1); flush_nl(r); for (int i = 0; i < r->width && i < 60; i++) put_raw(r, '-'); r->col = r->width; newline(r, 1); return; }
    if (tag_is(t, nl, "pre")) { r->pre = !closing; newline(r, 2); return; }
    if (tag_is(t, nl, "a")) {
        size_t cap = r->links_out ? r->max_links : SLWEB_MAX_LINKS;
        if (!closing) {
            char href[SLWEB_LINK_MAX];
            if (get_attr(t, tl, "href", href, sizeof(href)) && href[0] && href[0] != '#' && strncmp(href, "javascript", 10) != 0 && r->n_links < cap) {
                char *dst = r->links_out ? r->links_out[r->n_links].href : r->link_buf[r->n_links];
                h_strlcpy(dst, href, SLWEB_LINK_MAX);
                r->n_links++;
                r->link_pending = (int)r->n_links;          /* 1-based */
            } else r->link_pending = 0;
        } else if (r->link_pending) {
            char b[16]; snprintf(b, sizeof(b), "[%d]", r->link_pending);
            r->pending_space = 0; put_text_word(r, b, strlen(b));
            r->link_pending = 0;
        }
        return;
    }
    if (tag_is(t, nl, "img")) {
        char alt[60];
        if (get_attr(t, tl, "alt", alt, sizeof(alt)) && alt[0]) {
            r->pending_space = 1; put_text_word(r, "[", 1); put_text_word(r, alt, strlen(alt)); put_text_word(r, "]", 1);
        }
        return;
    }
}

/* Elements whose content is not markup (or not wanted): jump past the matching close tag, ignoring any
 * '<' or quotes inside -- JavaScript is full of both. Returns the index after the close tag, or (size_t)-1 if there is none. */
static int is_raw_element(const char *name, size_t nl)
{
    return tag_is(name, nl, "script") || tag_is(name, nl, "style") || tag_is(name, nl, "noscript") ||
           tag_is(name, nl, "svg") || tag_is(name, nl, "template") || tag_is(name, nl, "title") ||
           tag_is(name, nl, "textarea") || tag_is(name, nl, "iframe") || tag_is(name, nl, "object");
}

static size_t skip_raw(const char *html, size_t len, size_t i, const char *name, size_t nl)
{
    for (; i + 2 + nl < len; i++) {
        if (html[i] == '<' && html[i + 1] == '/' && strncasecmp(html + i + 2, name, nl) == 0) {
            char after = html[i + 2 + nl];
            if (after == '>' || after == ' ' || after == '\t' || after == '\n' || after == '\r' || after == '/') {
                size_t j = i + 2 + nl;
                while (j < len && html[j] != '>') j++;
                return j < len ? j + 1 : len;
            }
        }
    }
    return (size_t)-1;                                                  /* no close tag */
}

size_t slweb_html_to_text_ex(const char *html, size_t len, char *out, size_t cap, int width, int show_links,
                             struct slweb_link *links, size_t max_links, size_t *n_links)
{
    struct r r; memset(&r, 0, sizeof(r));
    r.out = out; r.cap = cap; r.width = width < 20 ? 20 : width; r.at_line_start = 1; r.links = show_links; r.ol_idx = 1;
    r.links_out = links; r.max_links = max_links;
    size_t i = 0;
    while (i < len && r.len + 1 < cap) {
        char c = html[i];
        if (c == '<') {
            if (i + 3 < len && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {      /* comment */
                size_t j = i + 4;
                while (j + 2 < len && !(html[j] == '-' && html[j + 1] == '-' && html[j + 2] == '>')) j++;
                i = j + 3; continue;
            }
            size_t j = i + 1;
            char q = 0, prev = 0;
            /* quotes only open inside a tag right after '=' (so a stray ' in text can't swallow the page) */
            while (j < len && (q || html[j] != '>')) {
                char ch = html[j];
                if (q) { if (ch == q) q = 0; }
                else if ((ch == '"' || ch == '\'') && prev == '=') q = ch;
                if (!q && !h_isspace(ch)) prev = ch;
                j++;
                if (j - i > 8192) break;                                   /* not a real tag: give up */
            }
            if (j - i > 8192 || j >= len) { i++; continue; }              /* lone '<' or unterminated tag: drop the '<' */
            const char *tn = html + i + 1; size_t tl = j - i - 1;
            int closing = (tl && tn[0] == '/');
            if (closing) { tn++; }
            size_t nl = 0;
            while (nl < tl - (closing ? 1u : 0u) && !h_isspace(tn[nl]) && tn[nl] != '/' && tn[nl] != '>') nl++;
            if (!closing && nl && is_raw_element(tn, nl) && !(tl && html[j - 1] == '/')) {
                size_t end = skip_raw(html, len, j + 1, tn, nl);
                if (end != (size_t)-1) { i = end; continue; }
                if (tag_is(tn, nl, "script") || tag_is(tn, nl, "style")) { i = len; continue; }   /* unterminated: browsers swallow the rest too */
                /* unterminated title/textarea/...: ignore the tag, keep rendering */
            }
            handle_tag(&r, html + i + 1, j - i - 1);
            i = j + 1;
            continue;
        }
        if (r.in_head) { i++; continue; }
        if (c == '&') {
            size_t j = i + 1;
            while (j < len && j - i < 10 && html[j] != ';' && !h_isspace(html[j]) && html[j] != '<') j++;
            char d;
            if (j < len && html[j] == ';' && decode_entity(html + i + 1, j - i - 1, &d)) { c = d; i = j + 1; goto emit; }
        }
        i++;
    emit:
        if (r.pre) {
            flush_nl(&r);
            if (c == '\n') { put_raw(&r, '\n'); r.col = 0; } else if (c != '\r') { put_raw(&r, c); r.col++; }
            r.at_line_start = c == '\n';
        } else if (h_isspace(c)) {
            r.pending_space = 1;
        } else {
            put_text_word(&r, &c, 1);
        }
    }
    r.pending_nl = 1; flush_nl(&r);
    if (r.links && r.n_links) {
        put_raw(&r, '\n');
        for (size_t k = 0; k < r.n_links; k++) {
            char b[SLWEB_LINK_MAX + 24]; snprintf(b, sizeof(b), "[%d] %s\n", (int)k + 1, r_href(&r, k));
            for (const char *p = b; *p; p++) put_raw(&r, *p);
        }
    }
    if (n_links) *n_links = r.n_links;
    out[r.len < cap ? r.len : cap - 1] = 0;
    return r.len;
}

size_t slweb_html_to_text(const char *html, size_t len, char *out, size_t cap, int width, int show_links)
{
    return slweb_html_to_text_ex(html, len, out, cap, width, show_links, NULL, 0, NULL);
}
