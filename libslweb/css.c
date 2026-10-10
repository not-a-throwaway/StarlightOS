/*
 * libslweb/css.c -- M6: a small CSS parser and cascade.
 *
 * The browser supports a practical subset of CSS 2.1 for static pages:
 *
 *   selectors   type, .class, #id, * and descendant/child combinators, with
 *               comma-separated selector lists;
 *   properties  color, background(-color), font-weight, font-style,
 *               text-decoration, font-size, display, text-align, the margin
 *               and padding shorthands (and their -top/-right/-bottom/-left
 *               longhands), width and height;
 *   values      named colours, #rgb/#rrggbb, rgb(r,g,b), px lengths and the
 *               usual keywords (bold, italic, underline, none, center, ...).
 *
 * <style> rules and inline style="" attributes are both understood. The parser
 * is forgiving: unknown selectors, properties and at-rules are skipped.
 *
 * slweb_style_compute fills only the properties the author actually declared,
 * each with a "_set" flag, so the layout engine can merge them over its
 * built-in defaults and inherit colour/scale/align down the tree.
 */
#include <stdlib.h>
#include <string.h>
#include "slweb.h"

#define CSS_SEL_MAX 256

enum {
    P_COLOR, P_BG, P_BOLD, P_ITALIC, P_UNDERLINE, P_SCALE, P_DISPLAY, P_ALIGN,
    P_MT, P_MR, P_MB, P_ML, P_PT, P_PR, P_PB, P_PL, P_W, P_H, P_COUNT
};

struct css_decl { uint8_t prop; uint32_t color; int ival; };

#define MAX_RULE_DECL 64
#define MAX_DECL       16

struct slweb_css {
    struct css_decl *decls;      /* flattened declaration pool              */
    int ndecls, declcap;
    struct {
        char sel[256];
        int spec;
        int decl_off, decl_n;
    } *rules;
    int nrules, rulecap;
};

/* ------------------------------------------------------------------ colours */
struct named_color { const char *name; uint32_t rgb; };
static const struct named_color NAMED[] = {
    {"black",0x000000},{"white",0xffffff},{"red",0xff0000},{"green",0x008000},
    {"lime",0x00ff00},{"blue",0x0000ff},{"yellow",0xffff00},{"aqua",0x00ffff},
    {"cyan",0x00ffff},{"magenta",0xff00ff},{"fuchsia",0xff00ff},{"gray",0x808080},
    {"grey",0x808080},{"silver",0xc0c0c0},{"maroon",0x800000},{"navy",0x000080},
    {"olive",0x808000},{"purple",0x800080},{"teal",0x008080},{"orange",0xffa500},
    {"brown",0xa52a2a},{"pink",0xffc0cb},{"gold",0xffd700},{"beige",0xf5f5dc},
    {"ivory",0xfffff0},{"khaki",0xf0e68c},{"salmon",0xfa8072},{"tomato",0xff6347},
    {"turquoise",0x40e0d0},{"violet",0xee82ee},{"indigo",0x4b0082},{"coral",0xff7f50},
    {"crimson",0xdc143c},{"darkblue",0x00008b},{"darkgreen",0x006400},
    {"darkred",0x8b0000},{"lightblue",0xadd8e6},{"lightgray",0xd3d3d3},
    {"lightgrey",0xd3d3d3},{"lightgreen",0x90ee90},{"skyblue",0x87ceeb},
    {"steelblue",0x4682b4},{"seagreen",0x2e8b57},{"darkgray",0xa9a9a9},
    {"darkgrey",0xa9a9a9},{"dimgray",0x696969},{"dimgrey",0x696969},
    {NULL,0}
};

static int ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == 0 && *b == 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int slweb_color_parse(const char *s, uint32_t *out)
{
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#') {
        s++;
        int digits = 0;
        while (hexval(s[digits]) >= 0) digits++;
        if (digits == 3) {
            int r = hexval(s[0]), g = hexval(s[1]), b = hexval(s[2]);
            *out = (uint32_t)((r * 17) << 16 | (g * 17) << 8 | b * 17);
            return 0;
        }
        if (digits >= 6) {
            int r = hexval(s[0]) * 16 + hexval(s[1]);
            int g = hexval(s[2]) * 16 + hexval(s[3]);
            int b = hexval(s[4]) * 16 + hexval(s[5]);
            *out = (uint32_t)(r << 16 | g << 8 | b);
            return 0;
        }
        return -1;
    }
    if ((s[0]=='r'||s[0]=='R') && (s[1]=='g'||s[1]=='G') && (s[2]=='b'||s[2]=='B') && s[3]=='(') {
        int v[3] = {0,0,0}, k = 0;
        const char *p = s + 4;
        while (k < 3) {
            while (*p == ' ') p++;
            if (*p < '0' || *p > '9') return -1;
            int n = 0;
            while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
            if (n > 255) n = 255;
            v[k++] = n;
            while (*p == ' ') p++;
            if (k < 3) { if (*p != ',') return -1; p++; }
        }
        *out = (uint32_t)(v[0] << 16 | v[1] << 8 | v[2]);
        return 0;
    }
    char name[32];
    size_t i = 0;
    while (s[i] && s[i] != ' ' && s[i] != '\t' && s[i] != ';' && i < 31) { name[i] = s[i]; i++; }
    name[i] = 0;
    for (int k = 0; NAMED[k].name; k++)
        if (ci_eq(name, NAMED[k].name)) { *out = NAMED[k].rgb; return 0; }
    return -1;
}

/* --------------------------------------------------------------- value parse */
static void trim(const char **s, size_t *n)
{
    while (*n && ((*s)[0] == ' ' || (*s)[0] == '\t' || (*s)[0] == '\n' || (*s)[0] == '\r')) { (*s)++; (*n)--; }
    while (*n && ((*s)[*n-1] == ' ' || (*s)[*n-1] == '\t' || (*s)[*n-1] == '\n' || (*s)[*n-1] == '\r')) (*n)--;
}

static int kw(const char *v, size_t n, const char *k)
{
    trim(&v, &n);
    size_t kl = strlen(k);
    if (kl != n) return 0;
    for (size_t q = 0; q < n; q++) {
        char a = v[q], b = k[q];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* Parse a px length (bare number or Npx); -1 if not a plain px length. */
static int parse_len(const char *v, size_t n)
{
    trim(&v, &n);
    size_t i = 0;
    int sign = 1;
    if (i < n && (v[i] == '-' || v[i] == '+')) { if (v[i] == '-') sign = -1; i++; }
    if (i >= n || v[i] < '0' || v[i] > '9') return -1;
    int val = 0;
    while (i < n && v[i] >= '0' && v[i] <= '9') { val = val * 10 + (v[i] - '0'); i++; }
    if (i < n) {
        if ((v[i] == 'p' || v[i] == 'P') && i + 1 < n && (v[i+1] == 'x' || v[i+1] == 'X')) i += 2;
        else return -1;
    }
    if (i != n) return -1;
    return sign * val;
}

struct decls { struct css_decl d[MAX_DECL]; int n; };
static void add_decl(struct decls *dl, int prop, uint32_t color, int ival)
{
    if (dl->n >= MAX_DECL) return;
    dl->d[dl->n].prop = (uint8_t)prop;
    dl->d[dl->n].color = color;
    dl->d[dl->n].ival = ival;
    dl->n++;
}

/* slweb_color_parse reads a NUL-terminated string, but declaration values are
 * length-delimited slices of the stylesheet (e.g. "red" followed by "}"), so
 * copy into a scratch buffer first. */
static int parse_color_value(const char *v, size_t n, uint32_t *out)
{
    char buf[64];
    if (n >= sizeof buf) return -1;
    memcpy(buf, v, n);
    buf[n] = 0;
    return slweb_color_parse(buf, out);
}

static void shorthand_box(struct decls *dl, int base, const char *v, size_t n)
{
    int vals[4], count = 0;
    size_t i = 0;
    while (i < n && count < 4) {
        while (i < n && (v[i] == ' ' || v[i] == '\t')) i++;
        if (i >= n) break;
        size_t start = i;
        while (i < n && v[i] != ' ' && v[i] != '\t') i++;
        int len = parse_len(v + start, i - start);
        if (len < 0) return;
        vals[count++] = len;
    }
    if (count == 0) return;
    int t = vals[0];
    int r = count > 1 ? vals[1] : t;
    int b = count > 2 ? vals[2] : t;
    int l = count > 3 ? vals[3] : r;
    add_decl(dl, base + 0, 0, t);
    add_decl(dl, base + 1, 0, r);
    add_decl(dl, base + 2, 0, b);
    add_decl(dl, base + 3, 0, l);
}

static void parse_one_decl(const char *prop, size_t plen, const char *val, size_t vlen,
                           struct decls *dl)
{
    trim(&prop, &plen);
    trim(&val, &vlen);
    for (size_t i = 0; i + 1 <= vlen; i++)
        if (val[i] == '!') { vlen = i; break; }
    trim(&val, &vlen);
    if (plen == 0) return;

    char p[32];
    size_t i = 0;
    for (; i < plen && i < 31; i++) {
        char c = prop[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        p[i] = c;
    }
    p[i] = 0;

    if (ci_eq(p, "color") || ci_eq(p, "background-color")) {
        uint32_t c;
        if (parse_color_value(val, vlen, &c) == 0) add_decl(dl, ci_eq(p, "color") ? P_COLOR : P_BG, c, 0);
        return;
    }
    if (ci_eq(p, "background")) {
        uint32_t c;
        if (parse_color_value(val, vlen, &c) == 0) add_decl(dl, P_BG, c, 0);
        return;
    }
    if (ci_eq(p, "font-weight")) {
        int b;
        long num = 0; int isnum = (vlen > 0);
        for (size_t q = 0; q < vlen; q++) { if (val[q] < '0' || val[q] > '9') { isnum = 0; break; } num = num*10 + (val[q]-'0'); }
        if (kw(val, vlen, "bold") || kw(val, vlen, "bolder") || (isnum && num >= 600)) b = 1;
        else if (kw(val, vlen, "normal") || kw(val, vlen, "lighter") || isnum) b = 0;
        else return;
        add_decl(dl, P_BOLD, 0, b);
        return;
    }
    if (ci_eq(p, "font-style")) {
        if (kw(val, vlen, "italic") || kw(val, vlen, "oblique")) add_decl(dl, P_ITALIC, 0, 1);
        else if (kw(val, vlen, "normal")) add_decl(dl, P_ITALIC, 0, 0);
        return;
    }
    if (ci_eq(p, "text-decoration")) {
        if (kw(val, vlen, "underline")) add_decl(dl, P_UNDERLINE, 0, 1);
        else if (kw(val, vlen, "none")) add_decl(dl, P_UNDERLINE, 0, 0);
        return;
    }
    if (ci_eq(p, "font-size")) {
        int scale;
        if (kw(val, vlen, "large") || kw(val, vlen, "larger") ||
            kw(val, vlen, "x-large") || kw(val, vlen, "xx-large")) scale = 2;
        else if (kw(val, vlen, "medium") || kw(val, vlen, "small") ||
                 kw(val, vlen, "smaller") || kw(val, vlen, "x-small") ||
                 kw(val, vlen, "xx-small")) scale = 1;
        else {
            int px = parse_len(val, vlen);
            if (px < 0) return;
            scale = px >= 22 ? 2 : 1;
        }
        add_decl(dl, P_SCALE, 0, scale);
        return;
    }
    if (ci_eq(p, "display")) {
        add_decl(dl, P_DISPLAY, 0, kw(val, vlen, "none") ? 1 : 0);
        return;
    }
    if (ci_eq(p, "text-align")) {
        if (kw(val, vlen, "center")) add_decl(dl, P_ALIGN, 0, SLWEB_ALIGN_CENTER);
        else if (kw(val, vlen, "right")) add_decl(dl, P_ALIGN, 0, SLWEB_ALIGN_RIGHT);
        else if (kw(val, vlen, "left")) add_decl(dl, P_ALIGN, 0, SLWEB_ALIGN_LEFT);
        return;
    }
    if (ci_eq(p, "margin")) { shorthand_box(dl, P_MT, val, vlen); return; }
    if (ci_eq(p, "padding")) { shorthand_box(dl, P_PT, val, vlen); return; }
    {
        static const struct { const char *n; int prop; } sides[] = {
            {"margin-top",P_MT},{"margin-right",P_MR},{"margin-bottom",P_MB},{"margin-left",P_ML},
            {"padding-top",P_PT},{"padding-right",P_PR},{"padding-bottom",P_PB},{"padding-left",P_PL},
            {NULL,0}
        };
        for (int k = 0; sides[k].n; k++)
            if (ci_eq(p, sides[k].n)) {
                int px = parse_len(val, vlen);
                if (px >= 0) add_decl(dl, sides[k].prop, 0, px);
                return;
            }
    }
    if (ci_eq(p, "width") || ci_eq(p, "height")) {
        int px = parse_len(val, vlen);
        if (px >= 0) add_decl(dl, ci_eq(p, "width") ? P_W : P_H, 0, px);
        return;
    }
}

static void parse_decls(const char *s, size_t len, struct decls *dl)
{
    size_t i = 0;
    while (i < len) {
        while (i < len && (s[i] == ';' || s[i] == ' ' || s[i] == '\t' ||
                           s[i] == '\n' || s[i] == '\r')) i++;
        if (i >= len) break;
        size_t start = i;
        while (i < len && s[i] != ';') i++;
        size_t end = i;
        size_t colon = start;
        while (colon < end && s[colon] != ':') colon++;
        if (colon < end)
            parse_one_decl(s + start, colon - start, s + colon + 1, end - colon - 1, dl);
    }
}

/* ---------------------------------------------------------------- selectors */
static int class_has(const char *attr, const char *cls, size_t len)
{
    if (!attr) return 0;
    size_t i = 0;
    while (attr[i]) {
        while (attr[i] == ' ' || attr[i] == '\t' || attr[i] == '\n') i++;
        size_t start = i;
        while (attr[i] && attr[i] != ' ' && attr[i] != '\t' && attr[i] != '\n') i++;
        if (i - start == len && strncmp(attr + start, cls, len) == 0) return 1;
    }
    return 0;
}

static int match_compound(const char *s, size_t n, const struct slweb_node *node)
{
    if (node->type != SLWEB_NODE_ELEMENT) return 0;
    size_t i = 0;
    if (i < n && s[i] == '*') i++;
    else {
        size_t start = i;
        while (i < n && s[i] != '.' && s[i] != '#') i++;
        if (i > start) {
            size_t tl = strlen(node->tag);
            if (i - start != tl || strncmp(s + start, node->tag, tl) != 0) return 0;
        }
    }
    while (i < n) {
        if (s[i] == '.') {
            i++;
            size_t start = i;
            while (i < n && s[i] != '.' && s[i] != '#') i++;
            if (!class_has(slweb_node_attr(node, "class"), s + start, i - start)) return 0;
        } else if (s[i] == '#') {
            i++;
            size_t start = i;
            while (i < n && s[i] != '.' && s[i] != '#') i++;
            const char *id = slweb_node_attr(node, "id");
            if (!id || strlen(id) != i - start || strncmp(id, s + start, i - start) != 0) return 0;
        } else i++;
    }
    return 1;
}

struct csel { size_t off, len; };
static int split_selector(const char *sel, size_t len, struct csel *out, int *combs, int max)
{
    int n = 0, pending = 0;
    size_t i = 0;
    while (i < len && (sel[i] == ' ' || sel[i] == '\t' || sel[i] == '\n')) i++;
    while (i < len) {
        if (n >= max) return -1;
        size_t start = i;
        while (i < len && sel[i] != ' ' && sel[i] != '\t' && sel[i] != '\n' && sel[i] != '>') i++;
        if (i == start) return -1;
        out[n].off = start; out[n].len = i - start;
        combs[n] = pending;
        n++;
        pending = 0;
        int saw_space = 0;
        while (i < len && (sel[i] == ' ' || sel[i] == '\t' || sel[i] == '\n')) { i++; saw_space = 1; }
        if (i < len && sel[i] == '>') {
            pending = 2; i++;
            while (i < len && (sel[i] == ' ' || sel[i] == '\t' || sel[i] == '\n')) i++;
        } else if (saw_space && i < len) {
            pending = 1;
        }
    }
    return n;
}

static int match_selector(const char *sel, size_t len, const struct slweb_node *node)
{
    struct csel cs[16];
    int combs[16];
    int n = split_selector(sel, len, cs, combs, 16);
    if (n <= 0) return 0;
    if (!match_compound(sel + cs[n-1].off, cs[n-1].len, node)) return 0;
    const struct slweb_node *cur = node;
    for (int k = n - 2; k >= 0; k--) {
        int comb = combs[k+1];
        if (comb == 2) {
            cur = cur->parent;
            if (!cur || cur->type != SLWEB_NODE_ELEMENT ||
                !match_compound(sel + cs[k].off, cs[k].len, cur)) return 0;
        } else {
            const struct slweb_node *a = cur->parent;
            while (a && !(a->type == SLWEB_NODE_ELEMENT &&
                          match_compound(sel + cs[k].off, cs[k].len, a))) a = a->parent;
            if (!a) return 0;
            cur = a;
        }
    }
    return 1;
}

static int specificity(const char *sel, size_t len)
{
    int ids = 0, classes = 0, types = 0;
    size_t i = 0;
    while (i < len) {
        char c = sel[i];
        if (c == '#') { ids++; i++; while (i < len && sel[i]!=' ' && sel[i]!='>' && sel[i]!='.' && sel[i]!='#') i++; }
        else if (c == '.') { classes++; i++; while (i < len && sel[i] != ' ' && sel[i] != '>' && sel[i] != '.' && sel[i] != '#') i++; }
        else if (c == '*' || c == ' ' || c == '\t' || c == '\n' || c == '>') i++;
        else { types++; while (i < len && sel[i] != ' ' && sel[i] != '>' && sel[i] != '.' && sel[i] != '#') i++; }
    }
    return ids * 10000 + classes * 100 + types;
}

/* ------------------------------------------------------------------- parser */
static void css_add_rule(struct slweb_css *css, const char *sel, size_t slen, struct decls *dl)
{
    if (slen == 0 || dl->n == 0) return;
    if (css->nrules >= css->rulecap) {
        int nc = css->rulecap ? css->rulecap * 2 : 64;
        void *nr = realloc(css->rules, (size_t)nc * sizeof css->rules[0]);
        if (!nr) return;
        css->rules = nr; css->rulecap = nc;
    }
    if (css->ndecls + dl->n > css->declcap) {
        int nc = css->declcap ? css->declcap * 2 : 256;
        while (nc < css->ndecls + dl->n) nc *= 2;
        void *nd = realloc(css->decls, (size_t)nc * sizeof css->decls[0]);
        if (!nd) return;
        css->decls = nd; css->declcap = nc;
    }
    int roff = css->ndecls;
    memcpy(css->decls + roff, dl->d, (size_t)dl->n * sizeof dl->d[0]);
    css->ndecls += dl->n;

    size_t sl = slen > 255 ? 255 : slen;
    int r = css->nrules++;
    memcpy(css->rules[r].sel, sel, sl);
    css->rules[r].sel[sl] = 0;
    css->rules[r].spec = specificity(sel, sl);
    css->rules[r].decl_off = roff;
    css->rules[r].decl_n = dl->n;
}

static void css_parse_text(struct slweb_css *css, const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        while (i < len && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r'||s[i]==';')) i++;
        if (i + 1 < len && s[i] == '/' && s[i+1] == '*') {
            i += 2;
            while (i + 1 < len && !(s[i]=='*' && s[i+1]=='/')) i++;
            i = (i + 1 < len) ? i + 2 : len;
            continue;
        }
        if (i >= len) break;
        if (s[i] == '@') {
            size_t j = i;
            while (j < len && s[j] != '{' && s[j] != ';') j++;
            if (j < len && s[j] == '{') {
                int depth = 1; j++;
                while (j < len && depth) { if (s[j]=='{') depth++; else if (s[j]=='}') depth--; j++; }
                i = j;
            } else i = (j < len) ? j + 1 : len;
            continue;
        }
        size_t b = i;
        while (b < len && s[b] != '{') b++;
        if (b >= len) break;
        size_t e = b + 1;
        while (e < len && s[e] != '}') e++;
        struct decls dl; dl.n = 0;
        parse_decls(s + b + 1, (e > b + 1) ? e - b - 1 : 0, &dl);
        size_t ss = i;
        for (size_t k = i; k <= b; k++) {
            if (k == b || s[k] == ',') {
                const char *sel = s + ss;
                size_t sl = k - ss;
                trim(&sel, &sl);
                css_add_rule(css, sel, sl, &dl);
                ss = k + 1;
            }
        }
        i = (e < len) ? e + 1 : len;
    }
}

static void css_walk(struct slweb_css *css, const struct slweb_node *n)
{
    for (; n; n = n->next_sibling) {
        if (n->type == SLWEB_NODE_ELEMENT && strcmp(n->tag, "style") == 0) {
            for (const struct slweb_node *t = n->first_child; t; t = t->next_sibling)
                if (t->type == SLWEB_NODE_TEXT) css_parse_text(css, t->text, t->text_len);
        }
        if (n->first_child) css_walk(css, n->first_child);
    }
}

struct slweb_css *slweb_css_parse(const struct slweb_node *root)
{
    struct slweb_css *css = (struct slweb_css *)calloc(1, sizeof *css);
    if (!css) return NULL;
    if (root) css_walk(css, root);
    return css;
}

void slweb_css_free(struct slweb_css *css)
{
    if (!css) return;
    free(css->rules);
    free(css->decls);
    free(css);
}

int slweb_css_rule_count(const struct slweb_css *css)
{
    return css ? css->nrules : 0;
}

/* ----------------------------------------------------------------- cascade */
static void apply_decl(struct slweb_style *out, const struct css_decl *d)
{
    switch (d->prop) {
    case P_COLOR:     out->color_set = 1;     out->color = d->color; break;
    case P_BG:        out->bg_set = 1;        out->bg = d->color; break;
    case P_BOLD:      out->bold_set = 1;      out->bold = d->ival; break;
    case P_ITALIC:    out->italic_set = 1;    out->italic = d->ival; break;
    case P_UNDERLINE: out->underline_set = 1; out->underline = d->ival; break;
    case P_SCALE:     out->scale_set = 1;     out->scale = d->ival; break;
    case P_DISPLAY:   out->display_set = 1;   out->display_none = d->ival; break;
    case P_ALIGN:     out->align_set = 1;     out->align = d->ival; break;
    case P_MT: case P_MR: case P_MB: case P_ML:
        out->margin_set[d->prop - P_MT] = 1; out->margin[d->prop - P_MT] = d->ival; break;
    case P_PT: case P_PR: case P_PB: case P_PL:
        out->pad_set[d->prop - P_PT] = 1; out->pad[d->prop - P_PT] = d->ival; break;
    case P_W: out->width_set = 1; out->width = d->ival; break;
    case P_H: out->height_set = 1; out->height = d->ival; break;
    default: break;
    }
}

void slweb_style_compute(const struct slweb_css *css, const struct slweb_node *node,
                         struct slweb_style *out)
{
    memset(out, 0, sizeof *out);
    if (!node || node->type != SLWEB_NODE_ELEMENT) return;

    int spec[P_COUNT];
    for (int i = 0; i < P_COUNT; i++) spec[i] = -1;

    if (css) {
        for (int r = 0; r < css->nrules; r++) {
            if (!match_selector(css->rules[r].sel, strlen(css->rules[r].sel), node)) continue;
            int rs = css->rules[r].spec;
            for (int k = 0; k < css->rules[r].decl_n; k++) {
                struct css_decl *d = &css->decls[css->rules[r].decl_off + k];
                if (d->prop >= P_COUNT) continue;
                if (rs >= spec[d->prop]) { apply_decl(out, d); spec[d->prop] = rs; }
            }
        }
    }
    /* inline style="" wins outright */
    const char *st = slweb_node_attr(node, "style");
    if (st && st[0]) {
        struct decls dl; dl.n = 0;
        parse_decls(st, strlen(st), &dl);
        for (int k = 0; k < dl.n; k++) apply_decl(out, &dl.d[k]);
    }
}

/* silence an unused-function warning if split_selector is only used internally */
