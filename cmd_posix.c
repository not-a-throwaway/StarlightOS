/*
 * cmd_posix.c -- more unix-style tools: diff sed rev tac nl cmp fold paste file readlink realpath
 * mktemp chgrp printenv mount ps su.
 *
 * Like the other cmd_*.c files these read FILE arguments or, with none, the previous pipeline stage
 * (shell_stdin), and write to the terminal (which the shell may be capturing).
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "shell.h"
#include "vfs.h"
#include "kstring.h"
#include "kprintf.h"
#include "kheap.h"
#include "terminal.h"
#include "ktime.h"
#include "ksched.h"

static void errmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void errmsg(const char *fmt, ...)
{
    uint8_t old = terminal_get_color();
    terminal_set_color(12, (uint8_t)(old >> 4));
    va_list ap; va_start(ap, fmt); kvprintf_err(fmt, ap); va_end(ap);
    terminal_set_color((uint8_t)(old & 15), (uint8_t)(old >> 4));
}

/* Loads the text to process: file argv[ai], or stdin when there is no file argument (or it is "-").
 * *owned is set when the caller must kfree it. Returns NULL (after printing) on failure. */
static const char *load(const char *cmd, const char *file, size_t *len, char **owned)
{
    *owned = NULL;
    if (!file || !strcmp(file, "-")) {
        if (shell_stdin) { *len = shell_stdin_len; return shell_stdin; }
        errmsg("%s: no input (give a FILE or pipe something in)\n", cmd);
        return NULL;
    }
    char *t = shell_read_file(file, len);
    if (!t) { errmsg("%s: %s: cannot read\n", cmd, file); return NULL; }
    *owned = t;
    return t;
}

/* Cuts text into lines: returns the count; ls[i]/ll[i] point into text (no terminators). */
static int split_lines(const char *text, size_t len, const char ***ls, size_t **ll)
{
    int n = 0, cap = 64;
    const char **L = (const char **)kmalloc(sizeof(char *) * (size_t)cap);
    size_t *N = (size_t *)kmalloc(sizeof(size_t) * (size_t)cap);
    if (!L || !N) { kfree(L); kfree(N); return -1; }
    size_t i = 0;
    while (i < len) {
        size_t j = i; while (j < len && text[j] != '\n') j++;
        if (n == cap) {
            cap *= 2;
            const char **L2 = (const char **)krealloc(L, sizeof(char *) * (size_t)cap);
            size_t *N2 = (size_t *)krealloc(N, sizeof(size_t) * (size_t)cap);
            if (!L2 || !N2) { kfree(L2 ? L2 : L); kfree(N2 ? N2 : N); return -1; }
            L = L2; N = N2;
        }
        L[n] = text + i; N[n] = j - i; n++;
        i = j + 1;
    }
    *ls = L; *ll = N;
    return n;
}

static void put_line(const char *s, size_t l) { for (size_t i = 0; i < l; i++) terminal_putchar(s[i]); terminal_putchar('\n'); }

/* ---- rev / tac / nl ---- */
static int cmd_rev(int argc, char **argv)
{
    size_t len; char *own; const char *t = load("rev", argc > 1 ? argv[1] : NULL, &len, &own);
    if (!t) return 1;
    const char **L; size_t *N; int n = split_lines(t, len, &L, &N);
    if (n < 0) { kfree(own); return 1; }
    for (int i = 0; i < n; i++) { for (size_t k = N[i]; k > 0; k--) terminal_putchar(L[i][k - 1]); terminal_putchar('\n'); }
    kfree(L); kfree(N); kfree(own);
    return 0;
}

static int cmd_tac(int argc, char **argv)
{
    size_t len; char *own; const char *t = load("tac", argc > 1 ? argv[1] : NULL, &len, &own);
    if (!t) return 1;
    const char **L; size_t *N; int n = split_lines(t, len, &L, &N);
    if (n < 0) { kfree(own); return 1; }
    for (int i = n - 1; i >= 0; i--) put_line(L[i], N[i]);
    kfree(L); kfree(N); kfree(own);
    return 0;
}

static int cmd_nl(int argc, char **argv)
{
    size_t len; char *own; const char *t = load("nl", argc > 1 ? argv[1] : NULL, &len, &own);
    if (!t) return 1;
    const char **L; size_t *N; int n = split_lines(t, len, &L, &N);
    if (n < 0) { kfree(own); return 1; }
    int num = 1;
    for (int i = 0; i < n; i++) {
        if (N[i]) kprintf("%6d\t", num++); else kprintf("      \t");
        put_line(L[i], N[i]);
    }
    kfree(L); kfree(N); kfree(own);
    return 0;
}

/* ---- fold [-w N] ---- */
static int cmd_fold(int argc, char **argv)
{
    uint32_t w = 80; const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w") && i + 1 < argc) { if (kparse_uint(argv[++i], &w) != 0 || !w) { errmsg("fold: bad width\n"); return 1; } }
        else file = argv[i];
    }
    size_t len; char *own; const char *t = load("fold", file, &len, &own);
    if (!t) return 1;
    uint32_t col = 0;
    for (size_t i = 0; i < len; i++) {
        if (t[i] == '\n') { terminal_putchar('\n'); col = 0; continue; }
        if (col == w) { terminal_putchar('\n'); col = 0; }
        terminal_putchar(t[i]); col++;
    }
    kfree(own);
    return 0;
}

/* ---- paste FILE1 FILE2 ---- */
static int cmd_paste(int argc, char **argv)
{
    if (argc < 3) { errmsg("usage: paste FILE1 FILE2 [FILE...]\n"); return 1; }
    int nf = argc - 1; if (nf > 8) nf = 8;
    char *own[8]; const char **L[8]; size_t *N[8]; int cnt[8]; int maxn = 0;
    for (int f = 0; f < nf; f++) {
        size_t len; own[f] = NULL; L[f] = NULL; N[f] = NULL; cnt[f] = 0;
        const char *t = load("paste", argv[f + 1], &len, &own[f]);
        if (!t) return 1;
        cnt[f] = split_lines(t, len, &L[f], &N[f]);
        if (cnt[f] < 0) return 1;
        if (cnt[f] > maxn) maxn = cnt[f];
    }
    for (int i = 0; i < maxn; i++) {
        for (int f = 0; f < nf; f++) {
            if (f) terminal_putchar('\t');
            if (i < cnt[f]) for (size_t k = 0; k < N[f][i]; k++) terminal_putchar(L[f][i][k]);
        }
        terminal_putchar('\n');
    }
    for (int f = 0; f < nf; f++) { kfree(L[f]); kfree(N[f]); kfree(own[f]); }
    return 0;
}

/* ---- cmp FILE1 FILE2 ---- */
static int cmd_cmp(int argc, char **argv)
{
    if (argc < 3) { errmsg("usage: cmp FILE1 FILE2\n"); return 2; }
    size_t l1, l2; char *o1, *o2;
    const char *a = load("cmp", argv[1], &l1, &o1); if (!a) return 2;
    const char *b = load("cmp", argv[2], &l2, &o2); if (!b) { kfree(o1); return 2; }
    size_t m = l1 < l2 ? l1 : l2, i; int line = 1, rc = 0;
    for (i = 0; i < m; i++) { if (a[i] != b[i]) break; if (a[i] == '\n') line++; }
    if (i < m) { kprintf("%s %s differ: byte %u, line %d\n", argv[1], argv[2], (unsigned)(i + 1), line); rc = 1; }
    else if (l1 != l2) { kprintf("cmp: EOF on %s after byte %u\n", l1 < l2 ? argv[1] : argv[2], (unsigned)m); rc = 1; }
    kfree(o1); kfree(o2);
    return rc;
}

/* ---- diff FILE1 FILE2: classic "normal" format from a line LCS ---- */
static int cmd_diff(int argc, char **argv)
{
    if (argc < 3) { errmsg("usage: diff FILE1 FILE2\n"); return 2; }
    size_t l1, l2; char *o1, *o2;
    const char *t1 = load("diff", argv[1], &l1, &o1); if (!t1) return 2;
    const char *t2 = load("diff", argv[2], &l2, &o2); if (!t2) { kfree(o1); return 2; }
    const char **A, **B; size_t *AN, *BN;
    int n = split_lines(t1, l1, &A, &AN), m = split_lines(t2, l2, &B, &BN);
    int rc = 2;
    uint16_t *tab = NULL;
    if (n < 0 || m < 0) goto out;
    if ((uint32_t)(n + 1) * (uint32_t)(m + 1) > 2000000u) { errmsg("diff: files too large\n"); goto out; }
    tab = (uint16_t *)kmalloc(sizeof(uint16_t) * (size_t)(n + 1) * (size_t)(m + 1));
    if (!tab) goto out;
#define T(i, j) tab[(size_t)(i) * (size_t)(m + 1) + (size_t)(j)]
#define SAME(i, j) (AN[i] == BN[j] && memcmp(A[i], B[j], AN[i]) == 0)
    for (int i = n; i >= 0; i--)
        for (int j = m; j >= 0; j--) {
            if (i == n || j == m) T(i, j) = 0;
            else if (SAME(i, j)) T(i, j) = (uint16_t)(T(i + 1, j + 1) + 1);
            else T(i, j) = T(i + 1, j) > T(i, j + 1) ? T(i + 1, j) : T(i, j + 1);
        }
    rc = 0;
    int i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && SAME(i, j)) { i++; j++; continue; }
        int si = i, sj = j;                              /* start of a differing hunk */
        while ((i < n || j < m) && !(i < n && j < m && SAME(i, j))) {
            if (j >= m || (i < n && T(i + 1, j) >= T(i, j + 1))) i++; else j++;
        }
        rc = 1;
        int da = i - si, db = j - sj;
        char range1[24], range2[24];
        if (da == 0) ksnprintf(range1, sizeof(range1), "%d", si); else if (da == 1) ksnprintf(range1, sizeof(range1), "%d", si + 1); else ksnprintf(range1, sizeof(range1), "%d,%d", si + 1, i);
        if (db == 0) ksnprintf(range2, sizeof(range2), "%d", sj); else if (db == 1) ksnprintf(range2, sizeof(range2), "%d", sj + 1); else ksnprintf(range2, sizeof(range2), "%d,%d", sj + 1, j);
        kprintf("%s%c%s\n", range1, da == 0 ? 'a' : db == 0 ? 'd' : 'c', range2);
        for (int k = si; k < i; k++) { kprintf("< "); put_line(A[k], AN[k]); }
        if (da && db) kprintf("---\n");
        for (int k = sj; k < j; k++) { kprintf("> "); put_line(B[k], BN[k]); }
    }
out:
    kfree(tab);
    if (n >= 0) { kfree(A); kfree(AN); }
    if (m >= 0) { kfree(B); kfree(BN); }
    kfree(o1); kfree(o2);
    return rc;
#undef T
#undef SAME
}

/* ---- sed [-n] SCRIPT [FILE]: s/OLD/NEW/[g], d, p, with optional line addresses N, N,M, $ ----
 * Patterns are literal text (the kernel has no regex engine); '&' in NEW stands for the match. */
struct sed_cmd { int a1, a2; char op; char pat[64], rep[64]; int global; };

static const char *sed_addr(const char *p, int *out)
{
    if (*p == '$') { *out = -1; return p + 1; }
    if (kisdigit(*p)) { *out = (int)kstrtoul(p, &p, 10); return p; }
    return NULL;
}

static int sed_parse(const char *s, struct sed_cmd *c)
{
    memset(c, 0, sizeof(*c));
    const char *p = s, *q = sed_addr(p, &c->a1);
    if (q) {
        p = q; c->a2 = c->a1;
        if (*p == ',') { q = sed_addr(p + 1, &c->a2); if (!q) return -1; p = q; }
    }
    c->op = *p;
    if (c->op == 'd' || c->op == 'p') return p[1] ? -1 : 0;
    if (c->op != 's' || !p[1]) return -1;
    char d = p[1]; p += 2;
    size_t n = 0;
    while (*p && *p != d) { if (*p == '\\' && p[1] == d) p++; if (n < sizeof(c->pat) - 1) c->pat[n++] = *p; p++; }
    if (*p != d || !n) return -1;
    p++; n = 0;
    while (*p && *p != d) { if (*p == '\\' && p[1] == d) p++; if (n < sizeof(c->rep) - 1) c->rep[n++] = *p; p++; }
    if (*p != d) return -1;
    p++;
    if (*p == 'g') { c->global = 1; p++; }
    return *p ? -1 : 0;
}

static int cmd_sed(int argc, char **argv)
{
    int quiet = 0; const char *script = NULL, *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) quiet = 1;
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) script = argv[++i];
        else if (!script) script = argv[i];
        else file = argv[i];
    }
    struct sed_cmd c;
    if (!script || sed_parse(script, &c) != 0) { errmsg("usage: sed [-n] 's/OLD/NEW/[g]' | '[N[,M]]d' | '[N[,M]]p' [FILE]\n"); return 1; }
    size_t len; char *own; const char *t = load("sed", file, &len, &own);
    if (!t) return 1;
    const char **L; size_t *N; int n = split_lines(t, len, &L, &N);
    if (n < 0) { kfree(own); return 1; }
    for (int i = 0; i < n; i++) {
        int ln = i + 1, in = 1;
        if (c.a1) { int lo = c.a1 == -1 ? n : c.a1, hi = c.a2 == -1 ? n : c.a2; in = ln >= lo && ln <= hi; }
        if (in && c.op == 'd') continue;
        if (in && c.op == 'p') { put_line(L[i], N[i]); if (!quiet) put_line(L[i], N[i]); continue; }
        if (in && c.op == 's') {
            if (quiet) continue;
            size_t pl = strlen(c.pat), rl = strlen(c.rep), k = 0; int did = 0;
            while (k < N[i]) {
                if ((c.global || !did) && k + pl <= N[i] && memcmp(L[i] + k, c.pat, pl) == 0) {
                    for (size_t r = 0; r < rl; r++) {
                        if (c.rep[r] == '&') for (size_t q = 0; q < pl; q++) terminal_putchar(c.pat[q]);
                        else terminal_putchar(c.rep[r]);
                    }
                    k += pl; did = 1;
                } else terminal_putchar(L[i][k++]);
            }
            terminal_putchar('\n');
            continue;
        }
        if (!quiet) put_line(L[i], N[i]);
    }
    kfree(L); kfree(N); kfree(own);
    return 0;
}

/* ---- file FILE...: guess the type ---- */
static int cmd_file(int argc, char **argv)
{
    if (argc < 2) { errmsg("usage: file FILE...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        vfs_node_t nd;
        if (vfs_stat(argv[i], &nd) != 0) { kprintf("%s: cannot open (no such file)\n", argv[i]); rc = 1; continue; }
        const char *what;
        if (nd.type == VFS_NODE_DIR) what = "directory";
        else if (nd.type == VFS_NODE_LNK) what = "symbolic link";
        else if (nd.type == VFS_NODE_CHR) what = "character special";
        else {
            unsigned char b[64]; uint32_t got = 0;
            if (vfs_read(argv[i], 0, b, sizeof(b), &got) != 0) what = "unreadable";
            else if (got == 0) what = "empty";
            else if (got >= 4 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') what = "ELF executable";
            else if (got >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) what = "WAVE audio";
            else if (got >= 2 && b[0] == '#' && b[1] == '!') what = "script text";
            else if (got >= 4 && b[0] == 0 && b[1] == 1 && b[2] == 0 && b[3] == 0) what = "TrueType font";
            else {
                int text = 1;
                for (uint32_t k = 0; k < got; k++) if (b[k] < 9 || (b[k] > 13 && b[k] < 32) || b[k] > 126) text = 0;
                what = text ? "ASCII text" : "data";
            }
        }
        kprintf("%s: %s\n", argv[i], what);
    }
    return rc;
}

static int cmd_readlink(int argc, char **argv)
{
    if (argc < 2) { errmsg("usage: readlink LINK...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char buf[VFS_PATH_MAX];
        int r = vfs_readlink(argv[i], buf, sizeof(buf));
        if (r < 0) { rc = 1; continue; }
        buf[(size_t)r < sizeof(buf) ? (size_t)r : sizeof(buf) - 1] = 0;
        kprintf("%s\n", buf);
    }
    return rc;
}

static int cmd_realpath(int argc, char **argv)
{
    if (argc < 2) { errmsg("usage: realpath PATH...\n"); return 1; }
    for (int i = 1; i < argc; i++) { char abs[VFS_PATH_MAX]; vfs_abspath(argv[i], abs); kprintf("%s\n", abs); }
    return 0;
}

static int cmd_mktemp(int argc, char **argv)
{
    int dir = argc > 1 && !strcmp(argv[1], "-d");
    static uint32_t seq;
    for (int tries = 0; tries < 100; tries++) {
        char path[VFS_PATH_MAX];
        ksnprintf(path, sizeof(path), "/tmp/tmp.%05u", (unsigned)((ktime_ms() * 7u + (++seq) * 2654435761u) % 100000u));
        if (vfs_exists(path)) continue;
        int r = dir ? vfs_mkdir(path) : vfs_write(path, "", 0);
        if (r != 0) { errmsg("mktemp: cannot create %s (%s)\n", path, vfs_strerror(r)); return 1; }
        kprintf("%s\n", path);
        return 0;
    }
    errmsg("mktemp: no free name\n");
    return 1;
}

/* ---- chgrp GID FILE... (numeric or a name from /etc/group) ---- */
static int lookup_id(const char *file, const char *name, uint32_t *id)
{
    if (kparse_uint(name, id) == 0) return 0;
    size_t len; char *t = shell_read_file(file, &len);
    if (!t) return -1;
    size_t nl = strlen(name); int found = -1;
    for (char *p = t; *p; ) {
        char *e = p; while (*e && *e != '\n') e++;
        if (!strncmp(p, name, nl) && p[nl] == ':') {
            char *f = p + nl + 1; while (*f && *f != ':') f++;     /* skip password */
            if (*f == ':') { uint32_t v; if (kparse_uint(f + 1, &v) == 0) { *id = v; found = 0; } }
            break;
        }
        p = *e ? e + 1 : e;
    }
    kfree(t);
    return found;
}

static int cmd_chgrp(int argc, char **argv)
{
    if (argc < 3) { errmsg("usage: chgrp GROUP FILE...\n"); return 1; }
    uint32_t gid;
    if (lookup_id("/etc/group", argv[1], &gid) != 0) { errmsg("chgrp: invalid group: %s\n", argv[1]); return 1; }
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        vfs_node_t nd;
        if (vfs_stat(argv[i], &nd) != 0) { errmsg("chgrp: cannot access '%s'\n", argv[i]); rc = 1; continue; }
        int r = vfs_chown(argv[i], nd.uid, (uint16_t)gid);
        if (r != 0) { errmsg("chgrp: %s: %s\n", argv[i], vfs_strerror(r)); rc = 1; }
    }
    return rc;
}

/* ---- printenv [NAME] ---- */
static void print_var(const char *n, const char *v, void *u) { (void)u; kprintf("%s=%s\n", n, v); }
static int cmd_printenv(int argc, char **argv)
{
    if (argc == 1) { shell_env_each(print_var, NULL); return 0; }
    int rc = 0;
    for (int i = 1; i < argc; i++) { const char *v = shell_getenv(argv[i]); if (v) kprintf("%s\n", v); else rc = 1; }
    return rc;
}

/* ---- mount (list) ---- */
static int cmd_mount(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int i = 0; ; i++) {
        char p[VFS_PATH_MAX];
        if (vfs_mount_path(i, p, sizeof(p)) != 0) break;
        const char *type = !strcmp(p, "/dev") ? "devfs" : !strcmp(p, "/proc") ? "procfs" : "ext2";
        const char *src = !strcmp(p, "/") ? "/dev/hda" : type;
        kprintf("%s on %s type %s (%s)\n", src, p, type, !strcmp(type, "ext2") ? "rw" : !strcmp(type, "devfs") ? "rw,nosuid" : "ro,nosuid");
    }
    return 0;
}

/* ---- ps: StarlightOS is cooperative, so there is the shell and whatever it is running right now ---- */
static int cmd_ps(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("  PID USER     TIME CMD\n");
    kprintf("    1 root     0:00 kernel\n");
    uint32_t s = ktime_ms() / 1000u;
    kprintf("  100 %-8s %u:%02u starlightsh\n", shell_getenv("USER") ? shell_getenv("USER") : "guest", s / 60u, s % 60u);
    kprintf("  101 %-8s 0:00 ps\n", shell_getenv("USER") ? shell_getenv("USER") : "guest");
    return 0;
}

/* ---- su [USER]: switch identity using /etc/passwd (this hobby OS has no passwords) ---- */
static int cmd_su(int argc, char **argv)
{
    const char *want = argc > 1 && strcmp(argv[1], "-") ? argv[1] : "root";
    if (argc > 2 && !strcmp(argv[1], "-")) want = argv[2];
    size_t len; char *t = shell_read_file("/etc/passwd", &len);
    if (!t) { errmsg("su: cannot read /etc/passwd\n"); return 1; }
    size_t wl = strlen(want); int found = 0;
    for (char *p = t; *p; ) {
        char *e = p; while (*e && *e != '\n') e++;
        char save = *e; *e = 0;
        if (!strncmp(p, want, wl) && p[wl] == ':') {
            char *f[7]; int nf = 0; char *q = p;
            while (nf < 7) { f[nf++] = q; q = strchr(q, ':'); if (!q) break; *q++ = 0; }
            if (nf >= 6) {
                uint32_t uid = 0, gid = 0;
                kparse_uint(f[2], &uid); kparse_uint(f[3], &gid);
                vfs_set_cred((uint16_t)uid, (uint16_t)gid);
                shell_setenv("USER", f[0]);
                shell_setenv("HOME", f[5]);
                if (vfs_is_dir(f[5])) vfs_cd(f[5]);
                shell_setenv("PWD", vfs_get_cwd());
                found = 1;
            }
            *e = save; break;
        }
        *e = save;
        p = *e ? e + 1 : e;
    }
    kfree(t);
    if (!found) { errmsg("su: user %s does not exist\n", want); return 1; }
    return 0;
}

const struct command posix_commands[] = {
    { "diff",     "Files",  "diff FILE1 FILE2", "compare files line by line (exit 1 if they differ)", cmd_diff },
    { "cmp",      "Files",  "cmp FILE1 FILE2", "compare files byte by byte", cmd_cmp },
    { "sed",      "Files",  "sed [-n] SCRIPT [FILE]", "s/OLD/NEW/[g], [N[,M]]d, [N[,M]]p (literal patterns)", cmd_sed },
    { "rev",      "Files",  "rev [FILE]", "reverse each line", cmd_rev },
    { "tac",      "Files",  "tac [FILE]", "print lines last to first", cmd_tac },
    { "nl",       "Files",  "nl [FILE]", "number the non-empty lines", cmd_nl },
    { "fold",     "Files",  "fold [-w N] [FILE]", "wrap lines at N columns", cmd_fold },
    { "paste",    "Files",  "paste FILE1 FILE2...", "merge lines of files side by side", cmd_paste },
    { "file",     "Files",  "file FILE...", "guess what kind of file each is", cmd_file },
    { "readlink", "Files",  "readlink LINK...", "print where a symbolic link points", cmd_readlink },
    { "realpath", "Files",  "realpath PATH...", "print the absolute, normalised path", cmd_realpath },
    { "mktemp",   "Files",  "mktemp [-d]", "create a unique file (or dir) in /tmp and print its name", cmd_mktemp },
    { "chgrp",    "Files",  "chgrp GROUP FILE...", "change a file's group (name from /etc/group or number)", cmd_chgrp },
    { "printenv", "Shell",  "printenv [NAME]", "print environment variables", cmd_printenv },
    { "mount",    "System", "mount", "list mounted filesystems", cmd_mount },
    { "ps",       "System", "ps", "list processes", cmd_ps },
    { "su",       "Shell",  "su [USER]", "become another user from /etc/passwd (no passwords here)", cmd_su },
};
const int posix_command_count = (int)(sizeof(posix_commands) / sizeof(posix_commands[0]));
