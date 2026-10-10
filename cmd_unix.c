/*
 * cmd_unix.c -- classic Unix utilities and POSIX shell commands:
 *   printf tee tr cut xargs expr basename dirname env yes ln du stat
 *   id whoami test [ umask export chmod chown
 *
 * Select filters (tr, cut, tee, xargs, yes) work in pipelines by reading
 * and writing through the shell's capture/echo machinery.
 */
#include "shell.h"
#include "vfs.h"
#include "terminal.h"
#include "kstring.h"
#include "kprintf.h"
#include "kheap.h"
#include "ksched.h"

#define UNIX_LINE 256

static void errmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void errmsg(const char *fmt, ...)
{
    uint8_t old = terminal_get_color();
    terminal_set_color(12, (uint8_t)(old >> 4));
    va_list ap; va_start(ap, fmt); kvprintf_err(fmt, ap); va_end(ap);
    terminal_set_color((uint8_t)(old & 15), (uint8_t)(old >> 4));
}

/* Turns a uid/gid into a name: the two known accounts, else the number. */
static const char *uid_name(uint16_t id, char *buf, size_t n)
{
    if (id == 0) return "root";
    if (id == 1000) return "guest";
    ksnprintf(buf, n, "%u", id);
    return buf;
}

/* "rwxr-xr-x"-style permission string for a mode word (no type char). */
static void perm_str(uint16_t mode, char out[10])
{
    out[0] = (mode & VFS_IRUSR) ? 'r' : '-';
    out[1] = (mode & VFS_IWUSR) ? 'w' : '-';
    out[2] = (mode & VFS_IXUSR) ? 'x' : '-';
    out[3] = (mode & VFS_IRGRP) ? 'r' : '-';
    out[4] = (mode & VFS_IWGRP) ? 'w' : '-';
    out[5] = (mode & VFS_IXGRP) ? 'x' : '-';
    out[6] = (mode & VFS_IROTH) ? 'r' : '-';
    out[7] = (mode & VFS_IWOTH) ? 'w' : '-';
    out[8] = (mode & VFS_IXOTH) ? 'x' : '-';
    out[9] = 0;
}

static const char *type_name(const vfs_node_t *n)
{
    switch (n->type) {
    case VFS_NODE_DIR:  return "directory";
    case VFS_NODE_LNK:  return "symbolic link";
    case VFS_NODE_CHR:  return "character device";
    default:            return "regular file";
    }
}

/* ================================================================= printf */

/* Expands the common backslash escapes (also used for %b arguments). */
static void unescape(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    const char *p = s;
    while (*p && o + 1 < cap) {
        if (*p != '\\') { out[o++] = *p++; continue; }
        p++;
        switch (*p) {
        case 'n': out[o++] = '\n'; p++; break;
        case 't': out[o++] = '\t'; p++; break;
        case 'r': out[o++] = '\r'; p++; break;
        case 'a': out[o++] = 7;    p++; break;
        case 'b': out[o++] = 8;    p++; break;
        case 'f': out[o++] = 12;   p++; break;
        case 'v': out[o++] = 11;   p++; break;
        case '\\': out[o++] = '\\'; p++; break;
        case '0': {
            int v = 0, n = 0;
            p++;
            while (n < 3 && *p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); p++; n++; }
            out[o++] = (char)v;
            break;
        }
        case 'x': {
            int v = 0, n = 0;
            p++;
            while (n < 2 && kisxdigit(*p)) {
                v = v * 16 + (kisdigit(*p) ? (*p - '0') : (ktolower(*p) - 'a' + 10));
                p++; n++;
            }
            out[o++] = (char)v;
            break;
        }
        default:
            out[o++] = '\\';
            if (*p) { out[o++] = *p; p++; }
            break;
        }
    }
    out[o] = 0;
}

static int cmd_printf(int argc, char **argv)
{
    if (argc < 2) { errmsg("usage: printf FORMAT [ARG...]\n"); return 1; }
    const char *fmt = argv[1];
    int ai = 2;
    for (const char *p = fmt; *p && !kabort_requested(); ) {
        if (*p == '%') {
            p++;
            if (*p == '%') { terminal_putchar('%'); p++; continue; }
            char fm[24]; int f = 0; fm[f++] = '%';
            while (*p == '-' || *p == '0' || *p == '+' || *p == ' ' || *p == '#') fm[f++] = *p++;
            while (kisdigit(*p)) fm[f++] = *p++;
            if (*p == '.') { fm[f++] = *p++; while (kisdigit(*p)) fm[f++] = *p++; }
            while (*p == 'l' || *p == 'h') p++;
            if (!*p) break;
            char conv = *p++;
            char buf[512];
            if (conv == 'b') {
                char tmp[512];
                unescape(ai < argc ? argv[ai++] : "", tmp, sizeof(tmp));
                terminal_writestring(tmp);
                continue;
            }
            fm[f++] = conv; fm[f] = 0;
            switch (conv) {
            case 's':
                ksnprintf(buf, sizeof(buf), fm, ai < argc ? argv[ai++] : "");
                terminal_writestring(buf);
                break;
            case 'c':
                if (ai < argc && argv[ai][0]) terminal_putchar(argv[ai][0]);
                ai++;
                break;
            case 'd': case 'i': {
                int32_t v = 0;
                if (ai < argc) kparse_int(argv[ai++], &v);
                ksnprintf(buf, sizeof(buf), fm, (int)v);
                terminal_writestring(buf);
                break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                uint32_t v = 0;
                if (ai < argc) v = kstrtoul(argv[ai++], NULL, 0);
                ksnprintf(buf, sizeof(buf), fm, (unsigned)v);
                terminal_writestring(buf);
                break;
            }
            default:
                terminal_putchar('%');
                terminal_putchar(conv);
                break;
            }
            continue;
        }
        if (*p == '\\') {
            char small[8]; size_t n = 0;
            small[n++] = '\\';
            if (p[1] >= '0' && p[1] <= '7') {
                small[n++] = p[1];
                if (p[2] >= '0' && p[2] <= '7') { small[n++] = p[2]; if (p[3] >= '0' && p[3] <= '7') small[n++] = p[3]; }
            } else if (p[1] == 'x' && kisxdigit(p[2])) {
                small[n++] = p[1];
                if (kisxdigit(p[2])) small[n++] = p[2];
                if (kisxdigit(p[3])) small[n++] = p[3];
            } else if (p[1]) small[n++] = p[1];
            small[n] = 0;
            char esc[16];
            unescape(small, esc, sizeof(esc));
            terminal_writestring(esc);
            p += n;
            continue;
        }
        terminal_putchar(*p++);
    }
    return 0;
}

/* ==================================================================== tee */

static int cmd_tee(int argc, char **argv)
{
    int append = 0;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "-a") == 0) append = 1;
    if (!shell_stdin) { errmsg("tee: no input (pipe something in)\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) continue;
        size_t old = 0;
        char *oldtxt = append ? shell_read_file(argv[i], &old) : NULL;
        char *buf = (char *)kmalloc(old + shell_stdin_len + 1);
        if (!buf) { kfree(oldtxt); errmsg("tee: out of memory\n"); return 1; }
        size_t n = 0;
        if (oldtxt) { memcpy(buf, oldtxt, old); n = old; }
        memcpy(buf + n, shell_stdin, shell_stdin_len);
        n += shell_stdin_len;
        int r = vfs_write(argv[i], buf, (uint32_t)n);
        kfree(buf); kfree(oldtxt);
        if (r) { errmsg("tee: %s: %s\n", argv[i], vfs_strerror(r)); return 1; }
    }
    /* copy the input to standard output */
    for (size_t i = 0; i < shell_stdin_len; i++) terminal_putchar(shell_stdin[i]);
    return 0;
}

/* ====================================================================== tr */

struct charset {
    unsigned char set[256];
    unsigned char ordered[256];
    int n;
};

static void chset_push(struct charset *cs, unsigned char c)
{
    if (!cs->set[c]) { cs->set[c] = 1; cs->ordered[cs->n++] = c; }
}

static void chset_range(struct charset *cs, unsigned char a, unsigned char b)
{
    int lo = a < b ? a : b, hi = a < b ? b : a;
    for (int c = lo; c <= hi; c++) chset_push(cs, (unsigned char)c);
}

static void chset_parse(struct charset *cs, const char *s)
{
    const char *p = s;
    while (*p) {
        unsigned char c1;
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': c1 = '\n'; p++; break;
            case 't': c1 = '\t'; p++; break;
            case 'r': c1 = '\r'; p++; break;
            case '\\': c1 = '\\'; p++; break;
            default:  c1 = (unsigned char)*p++; break;
            }
        } else c1 = (unsigned char)*p++;
        if (*p == '-' && p[1]) {
            unsigned char c2;
            p++;
            if (*p == '\\' && p[1]) { p++; c2 = (unsigned char)*p++; }
            else c2 = (unsigned char)*p++;
            chset_range(cs, c1, c2);
        } else chset_push(cs, c1);
    }
}

static int cmd_tr(int argc, char **argv)
{
    int del = 0, sqz = 0, keep_extra = 0;
    const char *set1 = NULL, *set2 = NULL;
    struct charset s1 = { {0}, {0}, 0 }, s2 = { {0}, {0}, 0 };
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] && !set1) {
            for (const char *f = argv[i] + 1; *f; f++) {
                if (*f == 'd') del = 1;
                else if (*f == 's') sqz = 1;
                else if (*f == 't') keep_extra = 1;
            }
        } else if (!set1) set1 = argv[i];
        else if (!set2) set2 = argv[i];
    }
    if (!set1) { errmsg("usage: tr [-ds] SET1 [SET2]\n"); return 1; }
    if (!shell_stdin) { errmsg("tr: no input\n"); return 1; }
    chset_parse(&s1, set1);
    if (set2) chset_parse(&s2, set2);

    unsigned char outc, last_out = 0;
    int have_last = 0;
    for (size_t i = 0; i < shell_stdin_len; i++) {
        unsigned char c = (unsigned char)shell_stdin[i];
        int in1 = s1.set[c] ? 1 : 0;
        if (!in1) {
            outc = c;
            if (sqz && have_last && outc == last_out) continue;
            goto emit;
        }
        if (del) continue;
        if (set2) {
            int idx;
            for (idx = 0; idx < s1.n && s1.ordered[idx] != c; idx++);
            if (idx < s2.n) {
                outc = s2.ordered[idx];
                if (sqz && have_last && outc == last_out) continue;
            } else {
                if (keep_extra) {
                    outc = c;
                    if (sqz && have_last && outc == last_out) continue;
                } else continue;
            }
        } else {
            outc = c;
            if (sqz && have_last && outc == last_out) continue;
        }
emit:
        terminal_putchar((char)outc);
        last_out = outc;
        have_last = 1;
    }
    return 0;
}

/* ===================================================================== cut */

struct sel { int lo, hi; };                     /* 1-based, hi may be 0x7fffffff */

static int parse_selection(const char *list, struct sel items[], int max)
{
    int n = 0;
    const char *p = list;
    while (*p && n < max) {
        int lo = 0, hi = 0, open = 0;
        while (kisdigit(*p)) lo = lo * 10 + (*p++ - '0');
        if (*p == '-') {
            p++;
            if (kisdigit(*p)) { while (kisdigit(*p)) hi = hi * 10 + (*p++ - '0'); }
            else open = 1;
            if (lo == 0) lo = 1;                /* "-M" => 1..M */
        } else hi = lo;
        items[n].lo = lo;
        items[n].hi = open ? 0x7fffffff : (hi ? hi : lo);
        n++;
        if (*p == ',') p++;
        else if (*p && !kisdigit(*p)) break;
    }
    return n;
}

static void cut_chars_line(const char *line, size_t len, const struct sel items[], int nsel)
{
    for (size_t i = 1; i <= len; i++) {
        int want = 0;
        for (int s = 0; s < nsel && !want; s++)
            want = (int)i >= items[s].lo && (int)i <= items[s].hi;
        if (want) terminal_putchar(line[i - 1]);
    }
    terminal_putchar('\n');
}

static void cut_fields_line(const char *line, size_t len, char delim,
                            const struct sel items[], int nsel)
{
    char buf[UNIX_LINE];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, line, n);
    buf[n] = 0;
    char *fld[64];
    int nf = 0;
    fld[nf++] = buf;
    for (char *q = buf; *q && nf < 64; q++)
        if (*q == delim) { *q = 0; fld[nf++] = q + 1; }
    int first = 1;
    for (int s = 0; s < nsel; s++) {
        int lo = items[s].lo, hi = items[s].hi;
        if (hi > nf) hi = nf;
        for (int k = lo; k <= hi; k++) {
            if (k < 1 || k > nf) continue;
            if (!first) terminal_putchar(delim);
            terminal_writestring(fld[k - 1]);
            first = 0;
        }
    }
    terminal_putchar('\n');
}

static int cmd_cut(int argc, char **argv)
{
    int chars = 0, fields = 0;
    char delim = '\t';
    const char *list = NULL;
    int file_idx = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c")) { chars = 1; if (i + 1 < argc && argv[i + 1][0] != '-') list = argv[++i]; }
        else if (!strcmp(argv[i], "-f")) { fields = 1; if (i + 1 < argc && argv[i + 1][0] != '-') list = argv[++i]; }
        else if (!strcmp(argv[i], "-d")) { if (i + 1 < argc && argv[i + 1][0] != '-') delim = argv[++i][0]; }
        else if (argv[i][0] == '-' && argv[i][1]) { /* unknown option, ignore */ }
        else if (!list) list = argv[i];
        else if (file_idx < 0) file_idx = i;
    }
    if (!list || (chars == fields)) { errmsg("usage: cut -c LIST [FILE] | cut -d D -f LIST [FILE]\n"); return 1; }

    struct sel items[16];
    int nsel = parse_selection(list, items, 16);
    if (nsel <= 0) { errmsg("cut: bad list '%s'\n", list); return 1; }

    size_t len;
    const char *text = NULL;
    if (file_idx >= 0) {
        text = shell_read_file(argv[file_idx], &len);
        if (!text) { errmsg("cut: cannot read '%s'\n", argv[file_idx]); return 1; }
    } else if (shell_stdin) {
        text = shell_stdin;
        len = shell_stdin_len;
    } else {
        errmsg("cut: no input (give a file or pipe something in)\n");
        return 1;
    }

    size_t i = 0;
    while (i < len) {
        size_t s = i;
        while (i < len && text[i] != '\n') i++;
        size_t e = i;
        if (e > s && text[e - 1] == '\r') e--;
        if (chars) cut_chars_line(text + s, e - s, items, nsel);
        else cut_fields_line(text + s, e - s, delim, items, nsel);
        if (i < len) i++;
    }
    if (file_idx >= 0) kfree((void *)text);
    return 0;
}

/* ==================================================================== xargs */

static void xa_quote(char *b, size_t cap, size_t *o, const char *s)
{
    if (*o + 3 >= cap) return;
    b[(*o)++] = '\'';
    for (const char *p = s; *p && *o + 4 < cap; p++) {
        if (*p == '\'') {
            b[(*o)++] = '\'';
            b[(*o)++] = '\\';
            b[(*o)++] = '\'';
            b[(*o)++] = '\'';
        } else b[(*o)++] = *p;
    }
    if (*o + 1 < cap) b[(*o)++] = '\'';
}

/* (Re)builds `line` out of the command + fixed prefix arguments. */
static void xa_rebuild(char *line, size_t cap, size_t *o,
                       const char *cmd, const char *const *prefix, int np)
{
    *o = 0;
    strlcpy(line, cmd, cap);
    *o = strlen(line);
    for (int i = 0; i < np; i++) {
        size_t l = strlen(prefix[i]);
        if (*o + l + 2 < cap) { line[(*o)++] = ' '; memcpy(line + *o, prefix[i], l); *o += l; }
    }
}

static int cmd_xargs(int argc, char **argv)
{
    const char *cmd = "echo";
    const char *prefix[16];
    int np = 0;
    int have_cmd = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;        /* ignore simple options */
        if (!have_cmd) { cmd = argv[i]; have_cmd = 1; continue; }
        if (np < 16) prefix[np++] = argv[i];
    }
    if (!shell_stdin) { errmsg("xargs: no input\n"); return 1; }

    char line[800];
    int rc = 0;
    int count = 0;
    size_t o = 0;
    xa_rebuild(line, sizeof(line), &o, cmd, prefix, np);

    const char *p = shell_stdin;
    const char *end = shell_stdin + shell_stdin_len;
    while (p < end && !kabort_requested()) {
        while (p < end && kisspace((unsigned char)*p)) p++;
        if (p >= end) break;
        const char *w = p;
        while (p < end && !kisspace((unsigned char)*p)) p++;
        char word[UNIX_LINE];
        size_t wl = (size_t)(p - w);
        if (wl > sizeof(word) - 1) wl = sizeof(word) - 1;
        memcpy(word, w, wl); word[wl] = 0;

        /* Flush the batch before appending if it would overflow, so no
         * word is ever dropped (a quoted word is <= 5 bytes/char). */
        if (count >= 16 || o + wl * 5 + 4 >= sizeof(line)) {
            if (count > 0) {
                line[o] = 0;
                rc = shell_run_line(line);
                xa_rebuild(line, sizeof(line), &o, cmd, prefix, np);
                count = 0;
            }
        }
        if (o + 1 < sizeof(line)) line[o++] = ' ';
        xa_quote(line, sizeof(line), &o, word);
        count++;
    }
    if (!kabort_requested() && count > 0) {
        line[o] = 0;
        rc = shell_run_line(line);
    }
    return rc;
}

/* ===================================================================== expr */

struct eval { int is_int; long long ival; char sval[64]; };

static int expr_argc;
static char **expr_argv;
static int expr_pos;
static int expr_error;

static int str_to_ll(const char *t, long long *v)
{
    const char *p = t;
    int neg = 0;
    long long x = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;
    if (!kisdigit(*p)) return -1;
    while (kisdigit(*p)) x = x * 10 + (*p++ - '0');
    if (*p) return -1;
    *v = neg ? -x : x;
    return 0;
}

static void expr_get(struct eval *e)
{
    if (expr_pos >= expr_argc) { e->is_int = 0; e->sval[0] = 0; return; }
    const char *t = expr_argv[expr_pos++];
    long long v;
    if (str_to_ll(t, &v) == 0) { e->is_int = 1; e->ival = v; }
    else { e->is_int = 0; strlcpy(e->sval, t, sizeof(e->sval)); }
}

static int expr_match(const char *op)
{
    if (expr_pos < expr_argc && strcmp(expr_argv[expr_pos], op) == 0) {
        expr_pos++;
        return 1;
    }
    return 0;
}

static void expr_expr(struct eval *r);

/* A single operand: number, string, or a parenthesised sub-expression. */
static void expr_atom(struct eval *r)
{
    const char *t;
    if (expr_pos >= expr_argc) { r->is_int = 0; r->sval[0] = 0; return; }
    t = expr_argv[expr_pos];
    if (!strcmp(t, "(")) {
        expr_pos++;
        if (expr_pos < expr_argc && !strcmp(expr_argv[expr_pos], ")")) { /* empty parens */ expr_pos++; expr_get(r); return; }
        expr_expr(r);
        if (expr_match(")") == 0) { expr_error = 1; }
        return;
    }
    if (!strcmp(t, "length")) {
        expr_pos++;
        struct eval s;
        expr_get(&s);
        r->is_int = 1;
        r->ival = (long long)strlen(s.sval);
        return;
    }
    if (!strcmp(t, "substr")) {
        expr_pos++;
        struct eval s, p, l;
        expr_get(&s); expr_get(&p); expr_get(&l);
        r->is_int = 0;
        long long pos = p.is_int ? p.ival : 0;
        long long len = l.is_int ? l.ival : 0;
        size_t sl = strlen(s.sval);
        r->sval[0] = 0;
        if (pos >= 1 && pos <= (long long)sl) {
            long long start = pos - 1;
            long long avail = (long long)sl - start;
            long long n = len < 0 ? avail : (len > avail ? avail : len);
            memcpy(r->sval, s.sval + start, (size_t)n);
            r->sval[n] = 0;
        }
        return;
    }
    if (!strcmp(t, "index")) {
        expr_pos++;
        struct eval s, c;
        expr_get(&s); expr_get(&c);
        r->is_int = 1;
        r->ival = 0;
        for (long long i = 0; s.sval[i]; i++)
            if (strchr(c.sval, s.sval[i])) { r->ival = i + 1; break; }
        return;
    }
    long long v;
    if (str_to_ll(t, &v) == 0) {
        expr_pos++;
        r->is_int = 1;
        r->ival = v;
        return;
    }
    expr_get(r);
    return;
}

/* Multiplicative: atom (* | / | %) atom ... (integers only). */
static void expr_mul2(struct eval *r)
{
    expr_atom(r);
    for (;;) {
        const char *op;
        if (expr_pos < expr_argc) {
            op = expr_argv[expr_pos];
            if (strcmp(op, "*") == 0 || strcmp(op, "/") == 0 || strcmp(op, "%") == 0) expr_pos++;
            else return;
        } else return;
        struct eval b;
        expr_atom(&b);
        if (!r->is_int || !b.is_int) { expr_error = 1; return; }
        if (op[0] == '/') {
            if (b.ival == 0) { expr_error = 2; return; }
            r->ival /= b.ival;
        } else if (op[0] == '%') {
            if (b.ival == 0) { expr_error = 2; return; }
            r->ival %= b.ival;
        } else r->ival *= b.ival;
    }
}

/* Additive: expr_mul2 (+ | -) expr_mul2 ... */
static void expr_expr_level2(struct eval *r)
{
    expr_mul2(r);
    for (;;) {
        int is_plus = 0;
        if (expr_pos < expr_argc) {
            const char *t = expr_argv[expr_pos];
            if (!strcmp(t, "+")) is_plus = 1;
            else if (!strcmp(t, "-")) is_plus = 0;
            else return;
        } else return;
        expr_pos++;
        struct eval b;
        expr_mul2(&b);
        if (!r->is_int || !b.is_int) { expr_error = 1; return; }
        r->ival = is_plus ? r->ival + b.ival : r->ival - b.ival;
    }
}

/* Top level: additive expression, optionally followed by one comparison
 * operator chain (=, !=, <, <=, >, >=) against another full expression. */
static void expr_expr(struct eval *r)
{
    expr_expr_level2(r);
    for (;;) {
        int op = 0;
        const char *t;
        if (expr_pos >= expr_argc) return;
        t = expr_argv[expr_pos];
        if (!strcmp(t, "=")) op = 1;
        else if (!strcmp(t, "!=")) op = 2;
        else if (!strcmp(t, "<")) op = 3;
        else if (!strcmp(t, "<=")) op = 4;
        else if (!strcmp(t, ">")) op = 5;
        else if (!strcmp(t, ">=")) op = 6;
        else return;
        expr_pos++;
        struct eval rhs;
        expr_expr_level2(&rhs);
        int c;
        if (r->is_int && rhs.is_int) c = r->ival < rhs.ival ? -1 : r->ival > rhs.ival ? 1 : 0;
        else c = strcmp(r->sval, rhs.sval);
        int res;
        switch (op) {
        case 1: res = (c == 0); break;
        case 2: res = (c != 0); break;
        case 3: res = (c < 0); break;
        case 4: res = (c <= 0); break;
        case 5: res = (c > 0); break;
        default: res = (c >= 0); break;
        }
        r->is_int = 1;
        r->ival = res ? 1 : 0;
    }
}

static int cmd_expr(int argc, char **argv)
{
    expr_argc = argc - 1;
    expr_argv = argv + 1;
    expr_pos = 0;
    expr_error = 0;
    struct eval r;
    expr_expr(&r);
    if (expr_error == 2) { errmsg("expr: division by zero\n"); return 2; }
    if (expr_error) { errmsg("expr: syntax error\n"); return 2; }
    if (r.is_int) kprintf("%lld\n", (long long)r.ival);
    else kprintf("%s\n", r.sval);
    if (r.is_int) return r.ival != 0 ? 0 : 1;
    return r.sval[0] ? 0 : 1;
}

/* ========================================================= basename/dirname */

static int cmd_basename(int argc, char **argv)
{
    if (argc < 2 || argc > 3) { errmsg("usage: basename PATH [SUFFIX]\n"); return 1; }
    char buf[VFS_PATH_MAX];
    strlcpy(buf, argv[1], sizeof(buf));
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') { buf[--n] = 0; }
    if (n == 0 || (n == 1 && buf[0] == '/')) { kprintf("/\n"); return 0; }
    const char *b = strrchr(buf, '/');
    const char *base = b ? b + 1 : buf;
    if (argc == 3 && *argv[2]) {
        size_t bl = strlen(base), sl = strlen(argv[2]);
        if (bl > sl && strcmp(base + (bl - sl), argv[2]) == 0) {
            char tmp[VFS_PATH_MAX];
            size_t tl = bl - sl;
            memcpy(tmp, base, tl); tmp[tl] = 0;
            kprintf("%s\n", tmp);
            return 0;
        }
    }
    kprintf("%s\n", base);
    return 0;
}

static int cmd_dirname(int argc, char **argv)
{
    if (argc < 2) { errmsg("usage: dirname PATH\n"); return 1; }
    char buf[VFS_PATH_MAX];
    strlcpy(buf, argv[1], sizeof(buf));
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') { buf[--n] = 0; }
    if (n == 0) { kprintf("/\n"); return 0; }
    if (strchr(buf, '/') == NULL) { kprintf(".\n"); return 0; }
    char *s = strrchr(buf, '/');
    if (s == buf) { kprintf("/\n"); return 0; }
    *s = 0;
    kprintf("%s\n", buf);
    return 0;
}

/* ======================================================================= env */

static void env_print_cb(const char *name, const char *value, void *user)
{
    (void)user;
    kprintf("%s=%s\n", name, value);
}

static void export_print_cb(const char *name, const char *value, void *user)
{
    (void)user;
    kprintf("export %s=%s\n", name, value);
}

static int cmd_env(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) { errmsg("env: ignoring options (single shared environment)\n"); }
    shell_env_each(env_print_cb, NULL);
    return 0;
}

static int cmd_export(int argc, char **argv)
{
    if (argc < 2) { shell_env_each(export_print_cb, NULL); return 0; }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            char name[32];
            size_t nl = (size_t)(eq - argv[i]);
            if (nl >= sizeof(name)) nl = sizeof(name) - 1;
            memcpy(name, argv[i], nl);
            name[nl] = 0;
            if (*name) shell_setenv(name, eq + 1);
        } else if (*argv[i]) {
            /* export NAME: the shared environment is always exported. */
        }
    }
    return 0;
}

/* ======================================================================= yes */

static int cmd_yes(int argc, char **argv)
{
    const char *w = argc > 1 ? argv[1] : "y";
    uint64_t n = 0;
    while (!kabort_requested()) {
        kprintf("%s\n", w);
        if ((++n & 0x0FFF) == 0) kyield();
    }
    return 130;
}

/* ======================================================================== ln */

static int cmd_ln(int argc, char **argv)
{
    int sym = 0;
    const char *target = NULL, *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--symbolic")) sym = 1;
        else if (!target) target = argv[i];
        else if (!name) name = argv[i];
    }
    if (!target) { errmsg("usage: ln [-s] TARGET [LINK_NAME]\n"); return 1; }
    char fallback[VFS_NAME_MAX + 1];
    if (!name) {
        const char *b = strrchr(target, '/');
        b = b ? b + 1 : target;
        strlcpy(fallback, b, sizeof(fallback));
        name = fallback;
    }
    int r = sym ? vfs_symlink(name, target) : vfs_link(target, name);
    if (r) { errmsg("ln: %s: %s\n", name, vfs_strerror(r)); return 1; }
    return 0;
}

/* ======================================================================== du */

static int du_summarize;

static uint32_t du_walk(char *label, size_t lcap, int depth)
{
    (void)lcap;
    static char names[64][VFS_NAME_MAX + 1];
    static vfs_node_t info[64];
    int n = vfs_get_entries_info(names, info, 64);
    char (*local)[VFS_NAME_MAX + 1] = (char (*)[VFS_NAME_MAX + 1])kmalloc((size_t)n * (VFS_NAME_MAX + 1) + 1);
    vfs_node_t *li = (vfs_node_t *)kmalloc((size_t)n * sizeof(vfs_node_t) + 1);
    if (!local || !li) { kfree(local); kfree(li); return 0; }
    memcpy(local, names, (size_t)n * (VFS_NAME_MAX + 1));
    memcpy(li, info, (size_t)n * sizeof(vfs_node_t));

    uint32_t total = 0;
    for (int i = 0; i < n && !kabort_requested(); i++) {
        if (li[i].type == VFS_NODE_DIR) {
            if (depth < 12 && vfs_cd(local[i]) == 0) {
                char sub[64];
                ksnprintf(sub, sizeof(sub), "%s/%s", label, local[i]);
                uint32_t t = du_walk(sub, sizeof(sub), depth + 1);
                vfs_cd("..");
                total += t;
            }
        } else if (li[i].type == VFS_NODE_FILE) {
            total += li[i].size;
        }
    }
    kfree(local); kfree(li);

    /* Plain `du` prints a line for every directory; -s leaves that to
     * cmd_du, which prints just the totalled argument. */
    if (!du_summarize && label[0]) kprintf("%u\t%s\n", (unsigned)((total + 511u) >> 9), label);
    return total;
}

static int cmd_du(int argc, char **argv)
{
    du_summarize = 0;
    const char *args[16];
    int na = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--summarize")) du_summarize = 1;
        else if (argv[i][0] == '-') { /* ignore */ }
        else if (na < 16) args[na++] = argv[i];
    }
    if (na == 0) args[na++] = ".";
    int rc = 0;
    char saved[VFS_PATH_MAX];
    strlcpy(saved, vfs_get_cwd(), sizeof(saved));
    for (int a = 0; a < na && !kabort_requested(); a++) {
        vfs_node_t node;
        if (vfs_stat(args[a], &node) != 0) {
            errmsg("du: cannot access '%s'\n", args[a]);
            rc = 1;
            continue;
        }
        if (node.type != VFS_NODE_DIR) {
            kprintf("%u\t%s\n", (unsigned)((node.size + 511u) >> 9), args[a]);
            continue;
        }
        if (vfs_cd(args[a]) != 0) { errmsg("du: cannot access '%s'\n", args[a]); rc = 1; continue; }
        char label[64];
        strlcpy(label, args[a], sizeof(label));
        if (du_summarize) {
            uint32_t total = du_walk(label, sizeof(label), 0);
            kprintf("%u\t%s\n", (unsigned)((total + 511u) >> 9), label);
        } else {
            du_walk(label, sizeof(label), 0);
        }
        vfs_cd(saved);
    }
    vfs_cd(saved);
    return rc;
}

/* ======================================================================= stat */

static int cmd_stat(int argc, char **argv)
{
    if (argc < 2) { errmsg("usage: stat FILE...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        vfs_node_t n;
        int r = vfs_stat(argv[i], &n);
        if (r != 0) {
            errmsg("stat: cannot stat '%s': %s\n", argv[i], vfs_strerror(r));
            rc = 1;
            continue;
        }
        char target[VFS_PATH_MAX];
        int is_lnk = 0;
        if (vfs_readlink(argv[i], target, sizeof(target)) > 0) is_lnk = 1;
        char p[10], ob[16], gb[16];
        perm_str((uint16_t)(n.mode & 07777u), p);
        kprintf("  File: %s\n", argv[i]);
        kprintf("  Size: %u   Blocks: %u   Type: %s\n", (unsigned)n.size, (unsigned)((n.size + 511u) >> 9),
                is_lnk ? "symbolic link" : type_name(&n));
        kprintf("  Mode: %04o (%s)\n", n.mode & 07777u, p);
        kprintf("  Owner: %s\n", uid_name(n.uid, ob, sizeof(ob)));
        kprintf("  Group: %s\n", uid_name(n.gid, gb, sizeof(gb)));
        kprintf("  Links: %u\n", (unsigned)n.nlink);
        if (is_lnk) kprintf("  Target: %s\n", target);
    }
    return rc;
}

/* ================================================================ id/whoami */

static int cmd_id(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint16_t uid = vfs_get_uid(), gid = vfs_get_gid();
    char ub[16], gb[16];
    kprintf("uid=%u(%s) gid=%u(%s) groups=%u(%s)\n", uid, uid_name(uid, ub, sizeof(ub)),
            gid, uid_name(gid, gb, sizeof(gb)), gid, uid_name(gid, gb, sizeof(gb)));
    return 0;
}

static int cmd_whoami(int argc, char **argv)
{
    (void)argc; (void)argv;
    char ub[16];
    kprintf("%s\n", uid_name(vfs_get_uid(), ub, sizeof(ub)));
    return 0;
}

/* ==================================================================== test/ */

static int test_eval(char **a, int n)
{
    if (n <= 0) return 0;
    if (!strcmp(a[0], "!")) return !test_eval(a + 1, n - 1);
    if (n == 1) return a[0][0] != 0;
    if (n == 2) {
        const char *f = a[0], *p = a[1];
        vfs_node_t node;
        if (!strcmp(f, "-e")) return vfs_exists(p) ? 1 : 0;
        if (!strcmp(f, "-f")) { if (vfs_stat(p, &node) != 0) return 0; return node.type == VFS_NODE_FILE; }
        if (!strcmp(f, "-d")) { if (vfs_stat(p, &node) != 0) return 0; return node.type == VFS_NODE_DIR; }
        if (!strcmp(f, "-r")) return vfs_access(p, VFS_WANT_R) == 0;
        if (!strcmp(f, "-w")) return vfs_access(p, VFS_WANT_W) == 0;
        if (!strcmp(f, "-x")) return vfs_access(p, VFS_WANT_X) == 0;
        if (!strcmp(f, "-s")) { int32_t sz = vfs_file_size(p); return sz > 0; }
        if (!strcmp(f, "-z")) return strlen(p) == 0;
        if (!strcmp(f, "-n")) return strlen(p) != 0;
        return strlen(a[0]) != 0;
    }
    if (n == 3) {
        if (!strcmp(a[1], "="))  return strcmp(a[0], a[2]) == 0;
        if (!strcmp(a[1], "!=")) return strcmp(a[0], a[2]) != 0;
        const char *op = a[1];
        int32_t x, y;
        if (kparse_int(a[0], &x) != 0 || kparse_int(a[2], &y) != 0) return 0;
        if (!strcmp(op, "-eq")) return x == y;
        if (!strcmp(op, "-ne")) return x != y;
        if (!strcmp(op, "-lt")) return x < y;
        if (!strcmp(op, "-le")) return x <= y;
        if (!strcmp(op, "-gt")) return x > y;
        if (!strcmp(op, "-ge")) return x >= y;
        return 0;
    }
    return 0;
}

static int cmd_test(int argc, char **argv)
{
    int n = argc - 1;
    char **a = argv + 1;
    if (!strcmp(argv[0], "[")) {
        if (n == 0 || strcmp(argv[argc - 1], "]") != 0) { errmsg("[: missing ']'\n"); return 2; }
        n--;
    }
    return test_eval(a, n) ? 0 : 1;
}

/* ===================================================================== umask */

static int cmd_umask(int argc, char **argv)
{
    if (argc < 2) { kprintf("%04o\n", vfs_get_umask()); return 0; }
    const char *e;
    uint32_t v = kstrtoul(argv[1], &e, 8);
    if (e == argv[1] || v > 0777) { errmsg("umask: invalid mask '%s'\n", argv[1]); return 1; }
    vfs_set_umask((uint16_t)v);
    return 0;
}

/* ==================================================================== chmod */

static int chmod_symbolic(const char *spec, uint16_t cur, uint16_t *out)
{
    uint16_t m = cur;
    const char *p = spec;
    while (*p) {
        uint16_t who = 0;
        while (*p && strchr("ugoa", *p)) {
            if (*p == 'u') who |= 0x0100;
            else if (*p == 'g') who |= 0x0200;
            else if (*p == 'o') who |= 0x0400;
            else who |= 0x0700;
            p++;
        }
        if (!who) who = 0x0700;
        char op = *p;
        if (op != '+' && op != '-' && op != '=') return -1;
        p++;
        uint16_t perms = 0;
        while (*p && strchr("rwx", *p)) {
            if (*p == 'r') perms |= 0x0124;   /* 0o0444: r bits in std positions */
            else if (*p == 'w') perms |= 0x0092;   /* 0o0222 */
            else perms |= 0x0049;               /* 0o0111 */
            p++;
        }
        uint16_t mask = 0;
        if (who & 0x0100) mask |= 0x01C0;      /* 0o0700: owner rwx */
        if (who & 0x0200) mask |= 0x0038;      /* 0o0070: group rwx */
        if (who & 0x0400) mask |= 0x0007;      /* other rwx */
        if (op == '=') m &= ~mask;
        if (op == '-') m &= ~(perms & mask);
        else m |= (perms & mask);
        if (*p == ',') p++;
    }
    *out = m;
    return 0;
}

static int cmd_chmod(int argc, char **argv)
{
    if (argc < 3) { errmsg("usage: chmod MODE FILE...\n"); return 1; }
    const char *spec = argv[1];
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        vfs_node_t n;
        if (vfs_stat(argv[i], &n) != 0) {
            errmsg("chmod: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        uint16_t mode;
        if (kisdigit((unsigned char)spec[0])) {
            const char *e;
            uint32_t v = kstrtoul(spec, &e, 8);
            if (e == spec || v > 07777) {
                errmsg("chmod: invalid mode '%s'\n", spec);
                rc = 1;
                continue;
            }
            mode = (uint16_t)(v & 07777);
        } else {
            if (chmod_symbolic(spec, (uint16_t)(n.mode & 07777), &mode) != 0) {
                errmsg("chmod: invalid mode '%s'\n", spec);
                rc = 1;
                continue;
            }
        }
        int r = vfs_chmod(argv[i], mode);
        if (r) { errmsg("chmod: %s: %s\n", argv[i], vfs_strerror(r)); rc = 1; }
    }
    return rc;
}

/* ==================================================================== chown */

static int cmd_chown(int argc, char **argv)
{
    if (argc < 3) { errmsg("usage: chown UID[:GID] FILE...\n"); return 1; }
    uint32_t uid;
    int have_gid = 0;
    uint32_t gid = 0;
    const char *s = argv[1];
    const char *colon = strchr(s, ':');
    char num[16];
    size_t nl = (size_t)(colon - s);
    if (colon) {
        if (nl >= sizeof(num)) nl = sizeof(num) - 1;
        memcpy(num, s, nl); num[nl] = 0;
        uid = kstrtoul(num, NULL, 10);
        gid = kstrtoul(colon + 1, NULL, 10);
        have_gid = 1;
    } else {
        uid = kstrtoul(s, NULL, 10);
    }
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        uint16_t use_gid;
        if (have_gid) use_gid = (uint16_t)gid;
        else {
            vfs_node_t n;
            if (vfs_stat(argv[i], &n) == 0) use_gid = n.gid;
            else use_gid = 1000;
        }
        int r = vfs_chown(argv[i], (uint16_t)uid, use_gid);
        if (r) { errmsg("chown: %s: %s\n", argv[i], vfs_strerror(r)); rc = 1; }
    }
    return rc;
}

/* ==================================================================== table */

const struct command unix_commands[] = {
    { "printf",   "Shell",  "printf FORMAT [ARG...]", "print formatted text (%s %d %u %x %b)", cmd_printf },
    { "tee",      "Files",  "tee [-a] FILE...", "copy stdin to stdout and to files", cmd_tee },
    { "tr",       "Files",  "tr [-ds] SET1 [SET2]", "translate or delete characters", cmd_tr },
    { "cut",      "Files",  "cut -c LIST | -d D -f LIST [FILE]", "select characters or fields", cmd_cut },
    { "xargs",    "Files",  "xargs [COMMAND [ARG...]]", "run COMMAND with words from stdin", cmd_xargs },
    { "expr",     "Files",  "expr EXPRESSION", "evaluate an integer/string expression", cmd_expr },
    { "basename", "Files",  "basename PATH [SUFFIX]", "strip the directory part", cmd_basename },
    { "dirname",  "Files",  "dirname PATH", "strip the final component", cmd_dirname },
    { "env",      "Shell",  "env", "print the environment", cmd_env },
    { "export",   "Shell",  "export [NAME=VALUE|NAME]...", "set or list exported variables", cmd_export },
    { "yes",      "Shell",  "yes [STRING]", "print STRING forever", cmd_yes },
    { "ln",       "Files",  "ln [-s] TARGET [LINK_NAME]", "create hard or symbolic links", cmd_ln },
    { "du",       "Files",  "du [-s] [FILE...]", "disk usage in 512-byte blocks", cmd_du },
    { "stat",     "Files",  "stat FILE...", "show file status", cmd_stat },
    { "id",       "Shell",  "id", "print user and group ids", cmd_id },
    { "whoami",   "Shell",  "whoami", "print the current user", cmd_whoami },
    { "test",     "Shell",  "test EXPRESSION", "evaluate a test expression", cmd_test },
    { "[",        "Shell",  "[ EXPRESSION ]", "evaluate a test expression", cmd_test },
    { "umask",    "Shell",  "umask [MODE]", "show or set the file creation mask", cmd_umask },
    { "chmod",    "Files",  "chmod MODE FILE...", "change file permission bits (octal or u+rwx)", cmd_chmod },
    { "chown",    "Files",  "chown UID[:GID] FILE...", "change file owner and group", cmd_chown },
};
const int unix_command_count = (int)(sizeof(unix_commands) / sizeof(unix_commands[0]));