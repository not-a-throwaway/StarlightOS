/*
 * libslweb/dom.c -- M5: a small HTML DOM parser.
 *
 * The M4 renderer (html.c) walked the byte stream and emitted text as it went.
 * The browser now needs structure, so this builds a tree of slweb_node instead:
 * elements carry a lowercased tag and their attributes, text nodes carry the
 * decoded text. The layout engine (layout.c) walks that tree.
 *
 * It is deliberately forgiving, like a browser: unclosed tags are closed at EOF
 * or by their implicit successors (<li> closes <li>, a block closes <p>, ...),
 * orphan close tags are ignored, and the contents of <script>/<style>/<title>/
 * <textarea>/<svg>/<noscript>/<template>/<iframe> are skipped as raw text so a
 * stray '<' or quote inside JavaScript cannot derail the parse.
 */
#include <stdlib.h>
#include <string.h>
#include "slweb.h"

#define DOM_STACK_MAX 128
#define DOM_NODE_MAX  200000

static int dom_nodes;

static char dom_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  dom_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static int dom_is(const char *tag, const char *name) { return strcmp(tag, name) == 0; }

/* ------------------------------------------------------------------ nodes */
static struct slweb_node *dom_new(int type)
{
    if (dom_nodes >= DOM_NODE_MAX) return NULL;
    struct slweb_node *n = (struct slweb_node *)calloc(1, sizeof *n);
    if (!n) return NULL;
    n->type = type;
    dom_nodes++;
    return n;
}

static void dom_append(struct slweb_node *parent, struct slweb_node *child)
{
    if (!parent || !child) return;
    child->parent = parent;
    child->next_sibling = NULL;
    if (parent->last_child) parent->last_child->next_sibling = child;
    else parent->first_child = child;
    parent->last_child = child;
}

static void dom_set_attr(struct slweb_node *n, const char *name, size_t nlen,
                         const char *val, size_t vlen)
{
    if (n->nattrs >= SLWEB_ATTR_MAX) return;
    if (nlen >= SLWEB_ATTR_NAME) nlen = SLWEB_ATTR_NAME - 1;
    if (vlen >= SLWEB_ATTR_VAL)  vlen = SLWEB_ATTR_VAL - 1;
    struct slweb_attr *a = &n->attrs[n->nattrs++];
    for (size_t i = 0; i < nlen; i++) a->name[i] = dom_lower(name[i]);
    a->name[nlen] = 0;
    memcpy(a->value, val, vlen);
    a->value[vlen] = 0;
}

const char *slweb_node_attr(const struct slweb_node *n, const char *name)
{
    if (!n || !name) return NULL;
    for (int i = 0; i < n->nattrs; i++) {
        const char *a = n->attrs[i].name, *b = name;
        while (*a && *a == dom_lower(*b)) { a++; b++; }
        if (!*a && !*b) return n->attrs[i].value;
    }
    return NULL;
}

void slweb_dom_free(struct slweb_node *node)
{
    while (node) {
        struct slweb_node *next = node->next_sibling;
        slweb_dom_free(node->first_child);
        free(node->text);
        free(node);
        dom_nodes--;
        node = next;
    }
}

/* ------------------------------------------------------------------ entities */
static int dom_entity(const char *s, size_t n, char *out)
{
    if (n == 0) return 0;
    if (s[0] == '#') {
        long v = 0;
        if (n > 1 && (s[1] == 'x' || s[1] == 'X')) {
            for (size_t i = 2; i < n; i++) {
                char c = s[i]; int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (s[i] >= 'a' && s[i] <= 'f') d = s[i] - 'a' + 10;
                else if (s[i] >= 'A' && s[i] <= 'F') d = s[i] - 'A' + 10;
                else return 0;
                if (v > 0x10FFFF) v = 0x10FFFF; else v = v * 16 + d;
            }
        } else {
            for (size_t i = 1; i < n; i++) {
                if (s[i] < '0' || s[i] > '9') return 0;
                v = v * 10 + (s[i] - '0');
                if (v > 0x10FFFF) v = 0x10FFFF;
            }
        }
        if (v == 0xA0 || v == 0x2009 || v == 0x202F) { *out = ' '; return 1; }
        if (v < 32 || v > 126) { *out = '?'; return 1; }
        *out = (char)v;
        return 1;
    }
    static const struct { const char *n; char c; } tab[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
        {"nbsp", ' '}, {"copy", 'c'}, {"reg", 'R'}, {"trade", 'T'}, {"deg", 'o'},
        {"mdash", '-'}, {"ndash", '-'}, {"hellip", '.'}, {"middot", '.'}, {"bull", '*'},
        {"rsquo", '\''}, {"lsquo", '\''}, {"rdquo", '"'}, {"ldquo", '"'},
        {"times", 'x'}, {"divide", '/'}, {"plusmn", '+'}, {"laquo", '<'}, {"raquo", '>'},
        {"euro", 'E'}, {"pound", '#'}, {"cent", 'c'}, {"sect", 'S'}, {"para", 'P'},
        {"sup2", '2'}, {"sup3", '3'}, {"frac12", ' '}, {"micro", 'u'}, {"deg", 'o'},
        {NULL, 0}
    };
    for (int i = 0; tab[i].n; i++)
        if (strlen(tab[i].n) == n && strncmp(tab[i].n, s, n) == 0) { *out = tab[i].c; return 1; }
    return 0;
}

static void dom_append_text(struct slweb_node *parent, const char *s, size_t n, int decode)
{
    char *buf = (char *)malloc(n + 1);
    if (!buf) return;
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        char c = s[i];
        if (decode && c == '&') {
            size_t j = i + 1;
            while (j < n && j - i <= 12 && s[j] != ';' && s[j] != '&' && s[j] != '<' && !dom_space(s[j])) j++;
            if (j < n && s[j] == ';') {
                char d;
                if (dom_entity(s + i + 1, j - i - 1, &d)) { buf[o++] = d; i = j + 1; continue; }
            }
        }
        buf[o++] = c;
        i++;
    }
    if (o == 0) { free(buf); return; }
    buf[o] = 0;
    struct slweb_node *t = dom_new(SLWEB_NODE_TEXT);
    if (!t) { free(buf); return; }
    t->text = buf;
    t->text_len = o;
    dom_append(parent, t);
}

/* ------------------------------------------------------------------ tag helpers */
static int dom_void(const char *t)
{
    static const char *v[] = { "area", "base", "br", "col", "embed", "hr", "img", "input",
                               "link", "meta", "param", "source", "track", "wbr", NULL };
    for (int i = 0; v[i]; i++) if (dom_is(t, v[i])) return 1;
    return 0;
}

static int dom_raw(const char *t)
{
    return dom_is(t, "script") || dom_is(t, "style") || dom_is(t, "noscript") ||
           dom_is(t, "svg") || dom_is(t, "template") || dom_is(t, "title") ||
           dom_is(t, "textarea") || dom_is(t, "iframe") || dom_is(t, "xmp") ||
           dom_is(t, "plaintext") || dom_is(t, "listing");
}

static int dom_blockish(const char *t)
{
    static const char *b[] = {
        "address","article","aside","blockquote","center","dd","details","dialog","dir","div",
        "dl","dt","fieldset","figcaption","figure","footer","form","h1","h2","h3","h4","h5","h6",
        "header","hgroup","hr","li","main","menu","nav","ol","p","pre","section","summary","table",
        "tbody","td","tfoot","th","thead","tr","ul", NULL
    };
    for (int i = 0; b[i]; i++) if (dom_is(t, b[i])) return 1;
    return 0;
}

/* Should an open element be closed implicitly when `tag` starts? */
static int dom_autoclose(const char *open, const char *tag)
{
    if (dom_is(tag, "li") && dom_is(open, "li")) return 1;
    if (dom_is(tag, "p") && dom_is(open, "p")) return 1;
    if ((dom_is(tag, "dt") || dom_is(tag, "dd")) && (dom_is(open, "dt") || dom_is(open, "dd"))) return 1;
    if (dom_is(tag, "tr") && (dom_is(open, "tr") || dom_is(open, "td") || dom_is(open, "th"))) return 1;
    if ((dom_is(tag, "td") || dom_is(tag, "th")) && (dom_is(open, "td") || dom_is(open, "th"))) return 1;
    if (dom_is(tag, "option") && dom_is(open, "option")) return 1;
    if (dom_is(tag, "body") && dom_is(open, "head")) return 1;
    if (dom_blockish(tag) && dom_is(open, "p")) return 1;
    return 0;
}

/* ------------------------------------------------------------------ tokenizer */
/* Read a tag/attribute name starting at *j; lowercase it into out. */
static int dom_name(const char *s, size_t len, size_t *j, char *out, size_t cap)
{
    size_t i = *j, o = 0;
    while (i < len) {
        char c = s[i];
        if (dom_space(c) || c == '>' || c == '/' || c == '<') break;
        if (o + 1 < cap) out[o++] = dom_lower(c);
        i++;
    }
    out[o] = 0;
    *j = i;
    return (int)o;
}

static void dom_attrs(struct slweb_node *n, const char *s, size_t len, size_t *j)
{
    size_t i = *j;
    for (;;) {
        while (i < len && dom_space(s[i])) i++;
        if (i >= len) break;
        if (s[i] == '>') { i++; break; }
        if (s[i] == '/') { i++; continue; }
        if (!n) {                          /* out of nodes: just find the '>' */
            while (i < len && s[i] != '>') i++;
            if (i < len) i++;
            break;
        }
        size_t ns = i;
        while (i < len && !dom_space(s[i]) && s[i] != '=' && s[i] != '>' && s[i] != '/') i++;
        size_t nlen = i - ns;
        size_t k = i;
        while (k < len && dom_space(s[k])) k++;
        if (k < len && s[k] == '=') {
            k++;
            while (k < len && dom_space(s[k])) k++;
            if (k < len && (s[k] == '"' || s[k] == '\'')) {
                char q = s[k++];
                size_t vs = k;
                while (k < len && s[k] != q) k++;
                dom_set_attr(n, s + ns, nlen, s + vs, k - vs);
                if (k < len) k++;
            } else {
                size_t vs = k;
                while (k < len && !dom_space(s[k]) && s[k] != '>') k++;
                dom_set_attr(n, s + ns, nlen, s + vs, k - vs);
            }
            i = k;
        } else {
            dom_set_attr(n, s + ns, nlen, "", 0);
            i = k;
        }
    }
    *j = i;
}

/* Skip the content of a raw element up to its real close tag. `cstart`/`cend`
 * (optional) receive the raw content range, so <style> rules can be kept. */
static size_t dom_skip_raw(const char *s, size_t len, size_t i, const char *name,
                           size_t *cstart, size_t *cend)
{
    if (cstart) *cstart = i;
    size_t nl = strlen(name);
    while (i < len) {
        if (s[i] == '<' && i + 1 < len && s[i + 1] == '/') {
            size_t k = i + 2;
            int match = 1;
            for (size_t q = 0; q < nl; q++)
                if (k + q >= len || dom_lower(s[k + q]) != name[q]) { match = 0; break; }
            if (match && (k + nl >= len || dom_space(s[k + nl]) || s[k + nl] == '>')) {
                size_t j = k + nl;
                while (j < len && s[j] != '>') j++;
                if (cend) *cend = i;
                return (j < len) ? j + 1 : len;
            }
        }
        i++;
    }
    if (cend) *cend = len;
    return len;
}

/* ------------------------------------------------------------------ parse */
struct slweb_node *slweb_dom_parse(const char *html, size_t len)
{
    dom_nodes = 0;
    struct slweb_node *doc = dom_new(SLWEB_NODE_DOCUMENT);
    if (!doc) return NULL;

    struct slweb_node *stack[DOM_STACK_MAX];
    int sp = 0;
    stack[sp++] = doc;

    size_t i = 0;
    while (i < len) {
        char c = html[i];

        if (c != '<') {
            size_t j = i;
            while (j < len && html[j] != '<') j++;
            dom_append_text(stack[sp - 1], html + i, j - i, 1);
            i = j;
            continue;
        }

        /* <!-- comment --> */
        if (i + 3 < len && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
            size_t j = i + 4;
            while (j + 2 < len && !(html[j] == '-' && html[j + 1] == '-' && html[j + 2] == '>')) j++;
            i = (j + 2 < len) ? j + 3 : len;
            continue;
        }
        /* <!doctype ...> or <? ... ?> */
        if (i + 1 < len && (html[i + 1] == '!' || html[i + 1] == '?')) {
            size_t j = i + 2;
            while (j < len && html[j] != '>') j++;
            i = (j < len) ? j + 1 : len;
            continue;
        }

        size_t j = i + 1;
        int closing = 0;
        if (j < len && html[j] == '/') { closing = 1; j++; }
        char name[SLWEB_TAG_MAX];
        int nl = dom_name(html, len, &j, name, sizeof name);
        if (nl == 0) {                       /* a stray '<' */
            i++;
            continue;
        }

        if (closing) {
            while (j < len && html[j] != '>') j++;
            if (j < len) j++;
            /* pop to the matching element (ignore orphan close tags) */
            for (int k = sp - 1; k >= 1; k--)
                if (dom_is(stack[k]->tag, name)) { sp = k; break; }
            i = j;
            continue;
        }

        struct slweb_node *el = dom_new(SLWEB_NODE_ELEMENT);
        if (el) memcpy(el->tag, name, strlen(name) + 1);
        dom_attrs(el, html, len, &j);        /* j is now past '>' */
        size_t q = j;
        if (q > 0) q--;                      /* the '>' */
        while (q > 0 && dom_space(html[q - 1])) q--;
        int selfclose = (q > 0 && html[q - 1] == '/');

        /* implicit closes before attaching the new element */
        while (el && sp > 1 && dom_autoclose(stack[sp - 1]->tag, name)) sp--;
        if (el) dom_append(stack[sp - 1], el);
        i = j;

        if (!selfclose && dom_raw(name)) {   /* skip contents wholesale */
            size_t cs = i, ce = i;
            i = dom_skip_raw(html, len, i, name, &cs, &ce);
            /* keep <style> rules (CSS parser) and <textarea> initial value */
            if (el && (dom_is(name, "style") || dom_is(name, "textarea")) && ce > cs)
                dom_append_text(el, html + cs, ce - cs, 0);
            continue;
        }
        if (el && !selfclose && !dom_void(name)) {
            if (sp < DOM_STACK_MAX) stack[sp++] = el;
        }
    }
    return doc;
}

struct slweb_node *slweb_dom_from_text(const char *text, size_t len)
{
    dom_nodes = 0;
    struct slweb_node *doc = dom_new(SLWEB_NODE_DOCUMENT);
    struct slweb_node *pre = dom_new(SLWEB_NODE_ELEMENT);
    if (!doc || !pre) { slweb_dom_free(doc); return NULL; }
    memcpy(pre->tag, "pre", 4);
    dom_append(doc, pre);
    /* copy verbatim (no entity decoding) */
    char *buf = (char *)malloc(len + 1);
    if (buf) {
        memcpy(buf, text, len);
        buf[len] = 0;
        struct slweb_node *t = dom_new(SLWEB_NODE_TEXT);
        if (t) { t->text = buf; t->text_len = len; dom_append(pre, t); }
        else free(buf);
    }
    return doc;
}
