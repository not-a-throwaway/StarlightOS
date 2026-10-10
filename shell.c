/*
 * shell.c -- the StarlightOS shell.
 *
 *  - quoting ("..." '...' \x), $VAR ${VAR:-d} $? $# $@ $(cmd) globs, aliases
 *  - operators:  ; && || !  |  > >> < 2> 2>&1 &>   and  if/while/until/for scripts
 *  - line editor: cursor keys, Home/End/Del, Ctrl-A/E/U/K/W/L/C, history, tab completion
 *  - scripts (`source FILE`, AUTOEXEC.SH at boot)
 *
 * Commands live in tables: builtins here, network commands in cmd_net.c,
 * sound commands in cmd_sound.c, file/text tools in cmd_text.c.
 */
#include <stdint.h>
#include <stddef.h>

#include "shell.h"
#include "vfs.h"
#include "keyboard.h"
#include "rtc.h"
#include "speaker.h"
#include "gui.h"
#include "mouse.h"
#include "font.h"
#include "vga.h"
#include "process.h"
#include "net.h"
#include "ac97.h"
#include "audio.h"
#include "terminal.h"
#include "kstring.h"
#include "kprintf.h"
#include "kheap.h"
#include "ktime.h"
#include "ksched.h"
#include "pci.h"
#include "version.h"

volatile int gui_requested = 0;

extern void terminal_redraw(void);
extern volatile uint32_t keyboard_irq_count;
extern volatile uint32_t mouse_irq_count;
extern uint32_t timer_get_ticks(void);
extern int multiboot_get_memory(uint32_t *lower_kb, uint32_t *upper_kb);

const char *shell_stdin = NULL;
size_t shell_stdin_len = 0;

#define LINE_MAX 256
#define HISTORY_SIZE 32
#define MAX_ARGS 64
#define MAX_TABLES 6

/* VGA palette indices for terminal colours. */
enum { C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_LGRAY,
       C_DGRAY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA, C_YELLOW, C_WHITE };

static void fg(int c) { terminal_set_color((uint8_t)c, (uint8_t)(terminal_get_color() >> 4)); }
static void fg_reset(void) { fg(C_LGRAY); }

/* ------------------------------------------------------------ variables */
struct var { char name[16]; char value[256]; };
#define MAX_VARS 48
static struct var vars[MAX_VARS];
static int last_status;

const char *shell_getenv(const char *name)
{
    for (int i = 0; i < MAX_VARS; i++) if (vars[i].name[0] && strcmp(vars[i].name, name) == 0) return vars[i].value;
    return NULL;
}

void shell_setenv(const char *name, const char *value)
{
    int free_slot = -1;
    for (int i = 0; i < MAX_VARS; i++) {
        if (vars[i].name[0] && strcmp(vars[i].name, name) == 0) { strlcpy(vars[i].value, value, sizeof(vars[i].value)); return; }
        if (!vars[i].name[0] && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) { strlcpy(vars[free_slot].name, name, sizeof(vars[0].name)); strlcpy(vars[free_slot].value, value, sizeof(vars[0].value)); }
}

void shell_env_each(void (*cb)(const char *name, const char *value, void *user), void *user)
{
    for (int i = 0; i < MAX_VARS; i++)
        if (vars[i].name[0]) cb(vars[i].name, vars[i].value, user);
}

static void unsetenv_(const char *name)
{
    for (int i = 0; i < MAX_VARS; i++) if (vars[i].name[0] && strcmp(vars[i].name, name) == 0) vars[i].name[0] = 0;
}

struct alias { char name[12]; char text[80]; };
#define MAX_ALIASES 16
static struct alias aliases[MAX_ALIASES];

static const char *alias_lookup(const char *name)
{
    for (int i = 0; i < MAX_ALIASES; i++) if (aliases[i].name[0] && strcmp(aliases[i].name, name) == 0) return aliases[i].text;
    return NULL;
}

static void alias_set(const char *name, const char *text)
{
    int slot = -1;
    for (int i = 0; i < MAX_ALIASES; i++) {
        if (aliases[i].name[0] && strcmp(aliases[i].name, name) == 0) { slot = i; break; }
        if (!aliases[i].name[0] && slot < 0) slot = i;
    }
    if (slot < 0) return;
    strlcpy(aliases[slot].name, name, sizeof(aliases[0].name));
    strlcpy(aliases[slot].text, text, sizeof(aliases[0].text));
}

/* ---------------------------------------------------------- command table */
static const struct command *tables[MAX_TABLES];
static int table_counts[MAX_TABLES];
static int table_n;

void shell_register(const struct command *t, int n)
{
    if (table_n < MAX_TABLES) { tables[table_n] = t; table_counts[table_n] = n; table_n++; }
}

static const struct command *find_command(const char *name)
{
    for (int t = 0; t < table_n; t++)
        for (int i = 0; i < table_counts[t]; i++)
            if (strcmp(tables[t][i].name, name) == 0) return &tables[t][i];
    return NULL;
}

/* ---------------------------------------------------------------- helpers */
/* Bound on what shell_read_file() takes from a file that reports no size:
 * a real empty file still gives 0 bytes, but the device nodes in /dev
 * (/dev/zero, /dev/urandom, /dev/tty) report size 0 while producing
 * endless or user-typed data, so they hand back a bounded sample that
 * filters like hexdump/cp/grep can work on. */
#define SHELL_STREAM_SAMPLE 4096

char *shell_read_file(const char *name, size_t *len_out)
{
    int32_t size = vfs_file_size(name);
    if (size < 0) return NULL;

    if (size == 0) {
        char *buf = (char *)kmalloc(SHELL_STREAM_SAMPLE + 1);
        uint32_t got = 0;
        if (!buf) return NULL;
        if (vfs_read(name, 0, buf, SHELL_STREAM_SAMPLE, &got) != 0) { kfree(buf); return NULL; }
        buf[got] = 0;
        if (len_out) *len_out = (size_t)got;
        return buf;
    }

    char *buf = (char *)kmalloc((size_t)size + 1);
    if (!buf) return NULL;
    uint32_t got = 0;
    if (size > 0 && vfs_read_all(name, buf, (uint32_t)size, &got) != 0) { kfree(buf); return NULL; }
    buf[size] = 0;
    if (len_out) *len_out = (size_t)size;
    return buf;
}

int shell_getc(uint32_t timeout_ms)
{
    uint32_t start = ktime_ms();
    for (;;) {
        int c = keyboard_getchar_nonblocking();
        if (c >= 0) { if (c == KEY_CTRL_C) return -1; return c; }
        if (timeout_ms && (uint32_t)(ktime_ms() - start) >= timeout_ms) return -1;
        kidle();
    }
}

static void err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void err(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fg(C_LRED); kvprintf_err(fmt, ap); fg_reset();
    va_end(ap);
}

static int has_flag(int argc, char **argv, const char *flag)
{
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], flag) == 0) return 1;
    return 0;
}

/* ============================================================ tokenising */
#define TOKBUF_SIZE 4096
static char tokbuf[TOKBUF_SIZE];

/* ---- calc: recursive-descent integer expressions (cmd_calc and $(( )) ) ----
 * The parser is defined lower in this file; these globals are shared so
 * expand_var() can evaluate $(( ... )) during tokenising. */
static const char *cp;
static int calc_fail;
static int64_t calc_expr(void);

/* ---- positional parameters ($0 $1 ... $# $@ $*), set while a script runs ---- */
#define MAX_POS 16
static char *pos_arg[MAX_POS];          /* pos_arg[0] is $0 */
static int   pos_n;                     /* how many of $1.. are set */
static int   shell_pid_value = 100;     /* what $$ expands to */

/* ---- control flow state shared by builtins (break/continue/exit/return) and the script interpreter ---- */
enum { CTL_NONE = 0, CTL_BREAK, CTL_CONTINUE, CTL_EXIT };
static int g_ctl, g_ctl_rc;
static int g_script_depth;              /* >0 while a script is executing */
static int g_loop_depth;                /* >0 while inside while/for/until */
static int g_run_depth;                 /* nesting of shell_run_line/script entry points */
static int g_subst_used, g_subst_rc;    /* did the last expansion run $( ), and its exit status */

/* ---- stderr routing (see shell_stderr_write) ---- */
enum { ERR_TERM = 0, ERR_STDOUT, ERR_BUF, ERR_NULL };
#define ERR_BUF_SIZE 4096
static int    err_mode = ERR_TERM;
static char  *err_buf;
static size_t err_len;

/* Where every err()/errmsg() message ends up: normally straight to the
 * screen (so `cmd | wc` never counts error text), or into the stdout stream
 * for 2>&1, or into a file for 2>file. */
void shell_stderr_write(const char *text)
{
    switch (err_mode) {
    case ERR_STDOUT: terminal_writestring(text); break;
    case ERR_BUF:
        if (err_buf) {
            size_t l = strlen(text);
            if (err_len + l >= ERR_BUF_SIZE) l = ERR_BUF_SIZE - 1 - err_len;
            memcpy(err_buf + err_len, text, l); err_len += l; err_buf[err_len] = 0;
        }
        break;
    case ERR_NULL: break;
    default: terminal_write_direct(text); break;
    }
}

static void put_str(const char *v, char *out, size_t *o, size_t cap)
{
    while (*v && *o + 1 < cap) out[(*o)++] = *v++;
}

static int run_line_inner(const char *line);
static int run_simple_line(const char *line);

static void expand_var(const char **pp, char *out, size_t *o, size_t cap)
{
    const char *p = *pp;                    /* at char after '$' */
    char name[20]; size_t n = 0;
    int brace = 0;
    char num[24];

    /* Arithmetic expansion: $(( expression )) */
    if (*p == '(' && p[1] == '(') {
        p += 2;
        const char *q = p;
        while (*q && !(*q == ')' && q[1] == ')')) q++;
        if (!*q) { *pp = p; return; }       /* unterminated: silently drop */
        if ((size_t)(q - p) < LINE_MAX) {
            char expr[LINE_MAX];
            memcpy(expr, p, (size_t)(q - p));
            expr[q - p] = 0;
            cp = expr; calc_fail = 0;
            int64_t v = calc_expr();
            if (!calc_fail) {
                ksnprintf(num, sizeof(num), "%lld", (long long)v);
                put_str(num, out, o, cap);
            }
        }
        *pp = q + 2;
        return;
    }

    if (*p == '{') { brace = 1; p++; }

    /* ${#NAME}: length of a variable's value */
    if (brace && *p == '#' && p[1] && p[1] != '}') {
        p++;
        while ((kisalnum(*p) || *p == '_') && n < sizeof(name) - 1) name[n++] = *p++;
        name[n] = 0;
        if (*p == '}') p++;
        const char *v = shell_getenv(name);
        ksnprintf(num, sizeof(num), "%u", (unsigned)(v ? strlen(v) : 0));
        put_str(num, out, o, cap);
        *pp = p; return;
    }

    /* special parameters: $# $@ $* $$ */
    if (*p == '#' || *p == '@' || *p == '*' || *p == '$') {
        char c = *p++;
        if (brace && *p == '}') p++;
        if (c == '#') { ksnprintf(num, sizeof(num), "%d", pos_n); put_str(num, out, o, cap); }
        else if (c == '$') { ksnprintf(num, sizeof(num), "%d", shell_pid_value); put_str(num, out, o, cap); }
        else for (int i = 1; i <= pos_n; i++) {
            if (i > 1) put_str(" ", out, o, cap);
            if (pos_arg[i]) put_str(pos_arg[i], out, o, cap);
        }
        *pp = p; return;
    }

    /* positional parameters: $0 .. $9 and ${10} */
    if (kisdigit(*p)) {
        int idx = 0;
        char dflt[96]; int has_dflt = 0; dflt[0] = 0;
        if (brace) {
            while (kisdigit(*p)) idx = idx * 10 + (*p++ - '0');
            if (*p == ':' && p[1] == '-') {                          /* ${1:-default} */
                size_t d = 0; p += 2; has_dflt = 1;
                while (*p && *p != '}' && d < sizeof(dflt) - 1) dflt[d++] = *p++;
                dflt[d] = 0;
            }
            if (*p == '}') p++;
        } else idx = *p++ - '0';
        const char *pv = NULL;
        if (idx == 0 && !pos_arg[0]) pv = "starlightsh";
        else if (idx < MAX_POS && idx <= pos_n && pos_arg[idx]) pv = pos_arg[idx];
        if (has_dflt && (!pv || !*pv)) pv = dflt;
        if (pv) put_str(pv, out, o, cap);
        *pp = p; return;
    }

    if (*p == '?') { name[n++] = '?'; p++; }
    else while ((kisalnum(*p) || *p == '_') && n < sizeof(name) - 1) name[n++] = *p++;
    name[n] = 0;

    /* ${NAME:-default} */
    char defv[96]; int has_def = 0; defv[0] = 0;
    if (brace && n && *p == ':' && p[1] == '-') {
        p += 2;
        size_t d = 0;
        while (*p && *p != '}' && d < sizeof(defv) - 1) defv[d++] = *p++;
        defv[d] = 0; has_def = 1;
    }
    if (brace && *p == '}') p++;
    if (!n) { if (*o + 1 < cap) out[(*o)++] = '$'; *pp = p; return; }
    const char *val;
    if (name[0] == '?') { ksnprintf(num, sizeof(num), "%d", last_status); val = num; }
    else val = shell_getenv(name);
    if (has_def && (!val || !*val)) val = defv;
    if (val) put_str(val, out, o, cap);
    *pp = p;
}

/* ---------------------------------------------------------------- globbing */
static int glob_match(const char *p, const char *s)
{
    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            for (; *s; s++) if (glob_match(p, s)) return 1;
            return glob_match(p, s);
        } else if (*p == '?') {
            if (!*s) return 0;
            p++; s++;
        } else if (*p == '[') {
            const char *q = p + 1; int neg = 0, ok = 0, first = 1;
            if (*q == '!' || *q == '^') { neg = 1; q++; }
            while (*q && (*q != ']' || first)) {
                first = 0;
                if (q[1] == '-' && q[2] && q[2] != ']') { if (*s >= q[0] && *s <= q[2]) ok = 1; q += 3; }
                else { if (*s == *q) ok = 1; q++; }
            }
            if (!*q) { if (*s != '[') return 0; p++; s++; continue; }   /* no closing ']': literal '[' */
            if (!*s || ok == neg) return 0;
            p = q + 1; s++;
        } else {
            if (*p != *s) return 0;
            p++; s++;
        }
    }
    return *s == 0;
}

static int word_has_glob(const char *w)
{
    for (; *w; w++) if (*w == '*' || *w == '?' || *w == '[') return 1;
    return 0;
}

#define GLOB_MAX 128
static char  globbuf[8192];
static char *globv[GLOB_MAX];

/* Expands a wildcard in the LAST path component (for example *.c or /bin/g?).
 * Matches land in globv[] sorted; dotfiles only match patterns starting with '.'.
 * Returns the match count (0: no match or unsupported, keep the literal word). */
static int glob_expand(const char *pat)
{
    char dir[VFS_PATH_MAX], saved[VFS_PATH_MAX];
    const char *fp = pat; size_t dl = 0;
    const char *sl = strrchr(pat, '/');
    if (sl) { dl = (size_t)(sl - pat) + 1; fp = sl + 1; }
    if (dl >= sizeof(dir)) return 0;
    memcpy(dir, pat, dl); dir[dl] = 0;
    if (word_has_glob(dir) || !word_has_glob(fp)) return 0;

    char (*names)[VFS_NAME_MAX + 1] = (char (*)[VFS_NAME_MAX + 1])kmalloc(GLOB_MAX * (VFS_NAME_MAX + 1));
    if (!names) return 0;
    strlcpy(saved, vfs_get_cwd(), sizeof(saved));
    if (dl) {
        char cd[VFS_PATH_MAX]; strlcpy(cd, dir, sizeof(cd));
        if (dl > 1) cd[dl - 1] = 0;                      /* strip the trailing '/' (keep "/" itself) */
        if (vfs_cd(cd) != 0) { kfree(names); return 0; }
    }
    int n = vfs_get_entries(names, NULL, GLOB_MAX);
    if (dl) vfs_cd(saved);

    int ng = 0; size_t used = 0;
    for (int i = 0; i < n && ng < GLOB_MAX; i++) {
        const char *nm = names[i];
        if (nm[0] == '.' && fp[0] != '.') continue;
        if (!glob_match(fp, nm)) continue;
        size_t need = dl + strlen(nm) + 1;
        if (used + need > sizeof(globbuf)) break;
        memcpy(globbuf + used, dir, dl);
        strcpy(globbuf + used + dl, nm);
        globv[ng++] = globbuf + used;
        used += need;
    }
    kfree(names);
    for (int i = 1; i < ng; i++) {                       /* insertion sort */
        char *v = globv[i]; int j = i - 1;
        while (j >= 0 && strcmp(globv[j], v) > 0) { globv[j + 1] = globv[j]; j--; }
        globv[j + 1] = v;
    }
    return ng;
}

/* Splits `line` into argv in tokbuf. Returns argc, or -1 on error. Redirection operators are returned as plain args
 * that start with a marker byte 0x01 ("\x01" ">" , ">>", "<", "2>", "2>>", "2>&1", "&>", "&>>", ">&2") so quoting can escape
 * them. Unquoted * ? [..] words are expanded against the filesystem. */
static int tokenize(const char *line, char **argv, int max)
{
    size_t o = 0; int argc = 0;
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '#') break;                            /* comment to end of line */
        if (argc >= max - 1) return -1;
        size_t wstart = o;
        argv[argc++] = tokbuf + o;
        int has_glob = 0;

        /* redirection operators */
        {
            const char *op = NULL; int adv = 0;
            if      (p[0] == '2' && p[1] == '>' && p[2] == '>')              { op = "2>>";  adv = 3; }
            else if (p[0] == '2' && p[1] == '>' && p[2] == '&' && p[3] == '1') { op = "2>&1"; adv = 4; }
            else if (p[0] == '2' && p[1] == '>')                             { op = "2>";   adv = 2; }
            else if (p[0] == '&' && p[1] == '>' && p[2] == '>')              { op = "&>>";  adv = 3; }
            else if (p[0] == '&' && p[1] == '>')                             { op = "&>";   adv = 2; }
            else if (p[0] == '>' && p[1] == '&' && p[2] == '2')              { op = ">&2";  adv = 3; }
            else if (p[0] == '>' && p[1] == '>')                             { op = ">>";   adv = 2; }
            else if (p[0] == '>')                                            { op = ">";    adv = 1; }
            else if (p[0] == '<')                                            { op = "<";    adv = 1; }
            if (op) {
                size_t l = strlen(op);
                if (o + l + 3 >= TOKBUF_SIZE) return -1;
                tokbuf[o++] = 1; memcpy(tokbuf + o, op, l); o += l; tokbuf[o++] = 0;
                p += adv;
                continue;
            }
        }

        /* "$@" on its own expands to one word per positional parameter (and to nothing when there are none) */
        if (p[0] == '"' && p[1] == '$' && p[2] == '@' && p[3] == '"' && (p[4] == 0 || p[4] == ' ' || p[4] == '\t')) {
            argc--;
            for (int i = 1; i <= pos_n; i++) {
                size_t l = pos_arg[i] ? strlen(pos_arg[i]) : 0;
                if (argc >= max - 1 || o + l + 2 >= TOKBUF_SIZE) return -1;
                argv[argc++] = tokbuf + o;
                if (l) memcpy(tokbuf + o, pos_arg[i], l);
                o += l; tokbuf[o++] = 0;
            }
            p += 4;
            continue;
        }

        /* Tilde expansion: a leading `~` (followed by end-of-word or '/')
         * becomes $HOME. */
        if (*p == '~' && (p[1] == '/' || p[1] == '\0' || p[1] == ' ' || p[1] == '\t')) {
            const char *h = shell_getenv("HOME");
            if (h) {
                size_t hl = strlen(h);
                if (o + hl + 2 < TOKBUF_SIZE) { memcpy(tokbuf + o, h, hl); o += hl; }
            }
            p++;
        }
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '>' || *p == '<') break;           /* unquoted here: starts a redirection */
            if (o + 2 >= TOKBUF_SIZE) return -1;
            if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) p++;
                    else if (*p == '$') { p++; expand_var(&p, tokbuf, &o, TOKBUF_SIZE - 1); continue; }
                    if (o + 2 >= TOKBUF_SIZE) return -1;
                    tokbuf[o++] = *p++;
                }
                if (*p != '"') return -1;
                p++;
            } else if (*p == '\'') {
                p++;
                while (*p && *p != '\'') { if (o + 2 >= TOKBUF_SIZE) return -1; tokbuf[o++] = *p++; }
                if (*p != '\'') return -1;
                p++;
            } else if (*p == '\\' && p[1]) {
                p++; tokbuf[o++] = *p++;
            } else if (*p == '$') {
                p++; expand_var(&p, tokbuf, &o, TOKBUF_SIZE - 1);
            } else {
                if (*p == '*' || *p == '?' || *p == '[') has_glob = 1;
                tokbuf[o++] = *p++;
            }
        }
        tokbuf[o++] = 0;

        if (has_glob) {
            int ng = glob_expand(tokbuf + wstart);
            if (ng > 0) {
                argc--; o = wstart;
                for (int g = 0; g < ng; g++) {
                    size_t l = strlen(globv[g]);
                    if (argc >= max - 1 || o + l + 2 >= TOKBUF_SIZE) return -1;
                    argv[argc++] = tokbuf + o;
                    memcpy(tokbuf + o, globv[g], l + 1);
                    o += l + 1;
                }
            }
        }
    }
    argv[argc] = NULL;
    return argc;
}

/* Finds the end of a segment up to the next unquoted separator. `seps` may contain:
 *   ';'  the ';' operator          '&'  the "&&" operator          'O'  the "||" operator
 *   '|'  a single pipe '|'
 * Quotes, backslashes, $( ) and `backticks` hide separators. */
static const char *find_sep(const char *s, const char *seps, int *which_len)
{
    char q = 0; int depth = 0;
    for (; *s; s++) {
        if (q) {
            if (*s == q) q = 0;
            else if (q == '"' && *s == '\\' && s[1]) s++;
            continue;
        }
        if (*s == '\\' && s[1]) { s++; continue; }
        if (*s == '"' || *s == '\'' || *s == '`') { q = *s; continue; }
        if (*s == '$' && s[1] == '(') { depth++; s++; continue; }
        if (depth) { if (*s == '(') depth++; else if (*s == ')') depth--; continue; }
        if (*s == '&' && s[1] == '&' && strchr(seps, '&')) { *which_len = 2; return s; }
        if (*s == '|' && s[1] == '|' && strchr(seps, 'O')) { *which_len = 2; return s; }
        if (*s == ';' && strchr(seps, ';')) { *which_len = 1; return s; }
        if (*s == '|' && s[1] != '|' && strchr(seps, '|')) { *which_len = 1; return s; }
    }
    *which_len = 0;
    return s;
}

/* ------------------------------------------------- command substitution */
#define SUBST_MAX (16 * 1024)
#define PRE_MAX   4096

/* Runs `cmd` (cmdlen bytes) and returns its standard output, trailing newlines
 * removed, in a kmalloc'd string (NULL on out-of-memory). */
static char *run_subst(const char *cmd, size_t cmdlen)
{
    char *line = (char *)kmalloc(cmdlen + 1);
    char *buf = (char *)kmalloc(SUBST_MAX);
    if (!line || !buf) { kfree(line); kfree(buf); return NULL; }
    memcpy(line, cmd, cmdlen); line[cmdlen] = 0;

    struct terminal_capture_state st; terminal_capture_save(&st);
    int saved_status = last_status, saved_ctl = g_ctl, saved_ctl_rc = g_ctl_rc;
    g_ctl = 0;
    terminal_capture_begin(buf, SUBST_MAX);
    int rc = run_line_inner(line);
    size_t n = terminal_capture_end();
    terminal_capture_restore(&st);
    if (n + 1 >= SUBST_MAX) kabort_clear();            /* the output filled the buffer: not a real Ctrl-C */
    last_status = saved_status; g_ctl = saved_ctl; g_ctl_rc = saved_ctl_rc;
    g_subst_used = 1; g_subst_rc = rc;

    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n] = 0;
    kfree(line);
    return buf;
}

static int is_assign_start(const char *p)
{
    if (!(kisalpha(*p) || *p == '_')) return 0;
    while (kisalnum(*p) || *p == '_') p++;
    return *p == '=';
}

/* Pre-pass over a command string: replaces $(cmd) and `cmd` by the command's output. The
 * substituted text is escaped so the tokenizer sees it as plain data (it is word-split on
 * whitespace when unquoted, kept as one piece inside "..." and in NAME=$(cmd) assignments). */
static int expand_cmdsubst(const char *in, char *out, size_t cap)
{
    size_t o = 0; char q = 0; int word_start = 1, assign_word = 0;
    const char *p = in;
    while (*p) {
        if (o + 8 >= cap) return -1;
        if (!q && (*p == ' ' || *p == '\t')) { out[o++] = *p++; word_start = 1; assign_word = 0; continue; }
        if (word_start && !q) { assign_word = is_assign_start(p); word_start = 0; }
        if (q != '\'' && *p == '\\' && p[1]) { out[o++] = *p++; out[o++] = *p++; continue; }
        if (*p == '\'' && q != '"') { q = q ? 0 : '\''; out[o++] = *p++; continue; }
        if (*p == '"'  && q != '\'') { q = q ? 0 : '"';  out[o++] = *p++; continue; }
        if (q != '\'' && *p == '$' && p[1] == '(' && p[2] == '(') {          /* $(( )) is arithmetic: leave it */
            const char *e = strstr(p, "))");
            size_t l = e ? (size_t)(e + 2 - p) : strlen(p);
            if (o + l + 8 >= cap) return -1;
            memcpy(out + o, p, l); o += l; p += l; continue;
        }
        if (q != '\'' && ((*p == '$' && p[1] == '(') || *p == '`')) {
            int bt = (*p == '`');
            const char *start = p + (bt ? 1 : 2), *end = start;
            if (bt) { while (*end && *end != '`') { if (*end == '\\' && end[1]) end++; end++; } }
            else {
                int d = 1; char qq = 0;
                for (; *end; end++) {
                    if (qq) { if (*end == qq) qq = 0; continue; }
                    if (*end == '\'' || *end == '"') { qq = *end; continue; }
                    if (*end == '(') d++;
                    else if (*end == ')' && --d == 0) break;
                }
            }
            if (!*end) return -1;                                           /* unterminated */
            char *res = run_subst(start, (size_t)(end - start));
            if (!res) return -1;
            int quote_it = (!q && assign_word);                              /* NAME=$(cmd): no word splitting */
            if (quote_it) out[o++] = '"';
            int in_dq = (q == '"') || quote_it;
            int pending_space = 0;
            for (const char *r = res; *r; r++) {
                if (o + 8 >= cap) { kfree(res); return -1; }
                unsigned char c = (unsigned char)*r;
                if (!in_dq && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) { pending_space = 1; continue; }
                if (pending_space) { out[o++] = ' '; pending_space = 0; }
                if (in_dq) { if (c == '"' || c == '\\' || c == '$') out[o++] = '\\'; }
                else if (!(kisalnum(c) || c == '_' || c == '.' || c == '/' || c == ':' || c == '+' ||
                           c == ',' || c == '=' || c == '@' || c == '%' || c == '-')) out[o++] = '\\';
                out[o++] = (char)c;
            }
            if (pending_space && !in_dq) out[o++] = ' ';
            if (quote_it) out[o++] = '"';
            kfree(res);
            p = end + 1;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    return 0;
}

/* Splits a string into words the way a command line is (quotes, $VAR, $( ), globs); returns
 * the count and a kmalloc'd array (one block: free it with kfree) or -1 on a syntax error. */
static int shell_words(const char *s, char ***out)
{
    char *pre = NULL;
    *out = NULL;
    if (strchr(s, '`') || strstr(s, "$(")) {
        pre = (char *)kmalloc(PRE_MAX);
        if (!pre || expand_cmdsubst(s, pre, PRE_MAX) != 0) { kfree(pre); return -1; }
        s = pre;
    }
    char *argv[MAX_ARGS];
    int argc = tokenize(s, argv, MAX_ARGS);
    kfree(pre);
    if (argc < 0) return -1;
    size_t total = 0;
    for (int i = 0; i < argc; i++) total += strlen(argv[i]) + 1;
    char **w = (char **)kmalloc(sizeof(char *) * (size_t)(argc + 1) + total + 1);
    if (!w) return -1;
    char *dst = (char *)(w + argc + 1);
    for (int i = 0; i < argc; i++) { size_t l = strlen(argv[i]) + 1; memcpy(dst, argv[i], l); w[i] = dst; dst += l; }
    w[argc] = NULL;
    *out = w;
    return argc;
}

/* ================================================================ running */
#define PIPE_BUF_SIZE (96 * 1024)
#define NESTED_PIPE_BUF (32 * 1024)
static char pipe_a[PIPE_BUF_SIZE], pipe_b[PIPE_BUF_SIZE];
static int pl_depth;

/* Writes `len` bytes to `file`, replacing it or appending (for > and >>, 2> and 2>>). */
static int write_redirect(const char *file, const char *data, size_t len, int append)
{
    char *merged = NULL;
    if (append && strcmp(file, "/dev/null") != 0) {
        size_t old = 0; char *oldtxt = shell_read_file(file, &old);
        if (oldtxt) {
            merged = (char *)kmalloc(old + len + 1);
            if (merged) { memcpy(merged, oldtxt, old); memcpy(merged + old, data, len); data = merged; len += old; }
            kfree(oldtxt);
        }
    }
    int w = vfs_write(file, data, (uint32_t)len);
    kfree(merged);
    if (w != 0) err("cannot write %s (%s)\n", file, vfs_strerror(w));
    return w;
}


/* Tries to run `path` as a program: an ELF app via the ring-3 loader, or a
 * text script run with `argv` as its positional parameters. Returns the exit
 * status, 126 on a permission/format error, or -1 when nothing exists at `path`. */
static int try_exec_file(const char *path, int argc, char **argv)
{
    if (!vfs_exists(path)) return -1;
    if (vfs_is_dir(path)) { err("%s: is a directory\n", path); return 126; }
    if (vfs_access(path, VFS_WANT_X) != 0) { err("%s: permission denied\n", path); return 126; }
    unsigned char magic[4]; uint32_t got = 0;
    if (vfs_read(path, 0, magic, 4, &got) != 0) { err("%s: cannot read\n", path); return 126; }
#if defined(__x86_64__)
    if (got >= 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
        if (process_execute_argv(path, argc, argv) != 0) { err("%s: bad or unsupported ELF\n", path); return 126; }
        return process_exit_status();
    }
#endif
    int st = 0;
    if (shell_run_script_args(path, argc, argv, &st) != 0) { err("%s: cannot read script\n", path); return 126; }
    return st;
}

/* Looks `name` up like a unix shell does: a name with '/' is used as-is, otherwise each
 * directory in $PATH is tried. Returns 1 and the full path in `out` when a file is found. */
static int path_lookup(const char *name, char *out, size_t cap)
{
    if (strchr(name, '/')) {
        if (vfs_exists(name) && !vfs_is_dir(name)) { strlcpy(out, name, cap); return 1; }
        return 0;
    }
    const char *path = shell_getenv("PATH");
    if (!path) return 0;
    const char *p = path;
    while (*p) {
        const char *sep = strchr(p, ':');
        size_t dl = sep ? (size_t)(sep - p) : strlen(p);
        char dir[VFS_PATH_MAX];
        if (dl >= sizeof(dir)) dl = sizeof(dir) - 1;
        memcpy(dir, p, dl); dir[dl] = 0;
        if (dl == 0) ksnprintf(out, cap, "%s", name);
        else ksnprintf(out, cap, "%s/%s", dir, name);
        if (vfs_exists(out) && !vfs_is_dir(out)) return 1;
        if (!sep) break;
        p = sep + 1;
    }
    return 0;
}

/* Executes an unknown command word: explicit path first, then the current directory,
 * then $PATH dirs. Returns the status, or -2 when the command was not found anywhere. */
static int exec_external(int argc, char **argv)
{
    const char *name = argv[0];
    if (strchr(name, '/')) {
        int r = try_exec_file(name, argc, argv);
        if (r == -1) return -2;
        return r;
    }
    if (vfs_exists(name)) {                 /* ./-style: run by bare name */
        int r = try_exec_file(name, argc, argv);
        if (r != -1) return r;
    }
    char cand[VFS_PATH_MAX];
    if (path_lookup(name, cand, sizeof(cand))) {
        int r = try_exec_file(cand, argc, argv);
        if (r != -1) return r;
    }
    return -2;
}

static int is_assignment(const char *w) { return w[0] != 1 && is_assign_start(w); }

static int run_stage_tokens(const char *s, const char *stdin_text, size_t stdin_len, int capture_to, char *capture_buf,
                            size_t capture_cap, size_t *captured)
{
    char *argv[MAX_ARGS];
    int argc = tokenize(s, argv, MAX_ARGS);
    if (argc < 0) { err("syntax error: unterminated quote or too many arguments\n"); return 2; }
    if (argc == 0) return 0;

    /* pull out redirections */
    const char *out_file = NULL, *err_file = NULL;
    int out_app = 0, err_app = 0, err_to_out = 0, out_to_err = 0;
    char *in_text = NULL, *argblk = NULL;
    int rc = 0, n = 0;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] != 1) { argv[n++] = argv[i]; continue; }
        const char *op = argv[i] + 1;
        if (!strcmp(op, "2>&1")) { err_to_out = 1; continue; }
        if (!strcmp(op, ">&2"))  { out_to_err = 1; continue; }
        if (i + 1 >= argc) { err("syntax error: missing file after '%s'\n", op); rc = 2; goto done; }
        const char *f = argv[++i];
        if      (!strcmp(op, ">"))   { out_file = f; out_app = 0; }
        else if (!strcmp(op, ">>"))  { out_file = f; out_app = 1; }
        else if (!strcmp(op, "2>"))  { err_file = f; err_app = 0; }
        else if (!strcmp(op, "2>>")) { err_file = f; err_app = 1; }
        else if (!strcmp(op, "&>"))  { out_file = f; out_app = 0; err_to_out = 1; }
        else if (!strcmp(op, "&>>")) { out_file = f; out_app = 1; err_to_out = 1; }
        else {                                                  /* < file : read stdin from a file */
            size_t flen; kfree(in_text);
            in_text = shell_read_file(f, &flen);
            if (!in_text) { err("cannot open %s\n", f); rc = 1; goto done; }
            stdin_text = in_text; stdin_len = flen;
        }
    }
    argc = n; argv[argc] = NULL;
    if (argc == 0) goto done;
    /* tokbuf is shared and gets reused by anything the command runs (scripts, source, xargs...),
     * so keep private copies of the redirect targets and, below, of the argument strings. */
    char ofile[VFS_PATH_MAX], efile[VFS_PATH_MAX];
    if (out_file) { strlcpy(ofile, out_file, sizeof(ofile)); out_file = ofile; }
    if (err_file) { strlcpy(efile, err_file, sizeof(efile)); err_file = efile; }
    char **av = argv;

    /* leading NAME=value words: set variables (for the command only, when one follows) */
    {
        int na = 0;
        while (na < argc && is_assignment(argv[na])) na++;
        struct { char name[16]; char old[256]; int had; } sv[4];
        if (na > 4 && na < argc) { err("too many variable assignments before a command\n"); rc = 2; goto done; }
        for (int i = 0; i < na; i++) {
            char *eq = strchr(argv[i], '='); *eq = 0;
            if (na < argc && i < 4) {
                const char *old = shell_getenv(argv[i]);
                strlcpy(sv[i].name, argv[i], sizeof(sv[i].name));
                sv[i].had = old != NULL; sv[i].old[0] = 0;
                if (old) strlcpy(sv[i].old, old, sizeof(sv[i].old));
            }
            shell_setenv(argv[i], eq + 1);
        }
        if (na == argc) { rc = g_subst_used ? g_subst_rc : 0; goto done; }
        av = argv + na; argc -= na;
        {
            size_t tot = 0;
            for (int i = 0; i < argc; i++) tot += strlen(av[i]) + 1;
            argblk = (char *)kmalloc(sizeof(char *) * (size_t)(argc + 1) + tot);
            if (!argblk) { err("out of memory\n"); rc = 1; goto done; }
            char **nv = (char **)argblk; char *d = argblk + sizeof(char *) * (size_t)(argc + 1);
            for (int i = 0; i < argc; i++) { size_t l = strlen(av[i]) + 1; memcpy(d, av[i], l); nv[i] = d; d += l; }
            nv[argc] = NULL; av = nv;
        }

        /* output plumbing */
        int need_cap = capture_to || out_file || out_to_err;
        struct terminal_capture_state outer;
        int saved_mode = err_mode; char *saved_ebuf = err_buf; size_t saved_elen = err_len;
        const char *saved_in = shell_stdin; size_t saved_inlen = shell_stdin_len;
        char *my_ebuf = NULL;
        if (err_file) {
            if (!strcmp(err_file, "/dev/null")) err_mode = ERR_NULL;
            else {
                my_ebuf = (char *)kmalloc(ERR_BUF_SIZE);
                if (my_ebuf) { my_ebuf[0] = 0; err_buf = my_ebuf; err_len = 0; err_mode = ERR_BUF; }
            }
        } else if (err_to_out) err_mode = ERR_STDOUT;

        shell_stdin = stdin_text; shell_stdin_len = stdin_len;
        if (need_cap) { terminal_capture_save(&outer); terminal_capture_begin(capture_buf, capture_cap); }

        const struct command *cmd = find_command(av[0]);
        if (cmd) rc = cmd->fn(argc, av);
        else {
            rc = exec_external(argc, av);
            if (rc == -2) { err("%s: command not found (try 'help')\n", av[0]); rc = 127; }
        }

        size_t cap = 0;
        if (need_cap) { cap = terminal_capture_end(); terminal_capture_restore(&outer); }
        shell_stdin = saved_in; shell_stdin_len = saved_inlen;
        err_mode = saved_mode; err_buf = saved_ebuf; err_len = saved_elen;
        if (my_ebuf) { write_redirect(err_file, my_ebuf, strlen(my_ebuf), err_app); kfree(my_ebuf); }

        if (out_file) { if (write_redirect(out_file, capture_buf, cap, out_app) != 0) rc = 1; *captured = 0; }
        else if (out_to_err) { shell_stderr_write(capture_buf); *captured = 0; }
        else if (capture_to) *captured = cap;

        for (int i = 0; i < na && i < 4; i++) {                 /* VAR=x cmd: put the old values back */
            if (sv[i].had) shell_setenv(sv[i].name, sv[i].old); else unsetenv_(sv[i].name);
        }
    }
done:
    kfree(in_text); kfree(argblk);
    return rc;
}

static int run_stage(const char *stage, const char *stdin_text, size_t stdin_len, int capture_to, char *capture_buf,
                     size_t capture_cap, size_t *captured)
{
    char expanded[LINE_MAX];
    /* alias expansion on the first word (once) */
    const char *s = stage;
    while (*s == ' ') s++;
    char first[16]; size_t fi = 0;
    while (s[fi] && s[fi] != ' ' && fi < sizeof(first) - 1) { first[fi] = s[fi]; fi++; }
    first[fi] = 0;
    const char *al = alias_lookup(first);
    if (al && !(s[fi] && s[fi] != ' ')) { ksnprintf(expanded, sizeof(expanded), "%s%s", al, s + fi); s = expanded; }

    char *pre = NULL;
    g_subst_used = 0;
    if (strchr(s, '`') || strstr(s, "$(")) {
        pre = (char *)kmalloc(PRE_MAX);
        if (!pre || expand_cmdsubst(s, pre, PRE_MAX) != 0) {
            kfree(pre);
            err("syntax error: bad command substitution\n");
            return 2;
        }
        s = pre;
    }
    int rc = run_stage_tokens(s, stdin_text, stdin_len, capture_to, capture_buf, capture_cap, captured);
    kfree(pre);
    return rc;
}

static int run_pipeline(const char *pl)
{
    /* split on '|' */
    const char *stages[8]; size_t lens[8]; int ns = 0;
    const char *p = pl;
    for (;;) {
        int w;
        const char *e = find_sep(p, "|", &w);
        if (ns >= 8) { err("pipeline too long\n"); return 2; }
        stages[ns] = p; lens[ns] = (size_t)(e - p); ns++;
        if (!w) break;
        p = e + 1;
    }
    /* A pipeline that runs while another is mid-flight (inside $( ), a script, a loop body) needs its own buffers. */
    int nested = (pl_depth > 0);
    size_t bsz = PIPE_BUF_SIZE;
    char *bufs[2] = { pipe_a, pipe_b };
    if (nested) {
        bsz = NESTED_PIPE_BUF;
        bufs[0] = (char *)kmalloc(bsz); bufs[1] = (char *)kmalloc(bsz);
        if (!bufs[0] || !bufs[1]) { kfree(bufs[0]); kfree(bufs[1]); err("out of memory\n"); return 1; }
    }
    pl_depth++;
    const char *in = NULL; size_t in_len = 0;
    int rc = 0;
    for (int i = 0; i < ns; i++) {
        char line[LINE_MAX];
        size_t l = lens[i] < sizeof(line) - 1 ? lens[i] : sizeof(line) - 1;
        memcpy(line, stages[i], l); line[l] = 0;
        int last = (i == ns - 1);
        size_t cap_len = 0;
        char *cbuf = bufs[i & 1];
        rc = run_stage(line, in, in_len, !last, cbuf, bsz, &cap_len);
        if (kabort_requested()) {
            if (last) { rc = 130; break; }
            rc = 130;                   /* producer ran out of pipe buffer (like SIGPIPE):
                                           let downstream stages consume what was captured */
            kabort_clear();
        }
        if (!last) { in = cbuf; in_len = cap_len; }
    }
    pl_depth--;
    if (nested) { kfree(bufs[0]); kfree(bufs[1]); }
    return rc;
}

/* ============================================================ control flow
 * `if/then/elif/else/fi`, `while|until/do/done`, `for X in WORDS/do/done`, plus
 * `break continue exit return`. Scripts (and one-liners with ';') are first cut into
 * "items" (one command list each, split on newlines and ';'), then interpreted. */
#define MAX_ITEMS 1024
typedef struct { char **it; int n; } prog_t;

static int kw_is(const char *s, const char *kw)
{
    size_t l = strlen(kw);
    return strncmp(s, kw, l) == 0 && (s[l] == 0 || s[l] == ' ' || s[l] == '\t');
}
static const char *kw_rest(const char *s, const char *kw)
{
    s += strlen(kw);
    while (*s == ' ' || *s == '\t') s++;
    return s;
}
static int is_terminator(const char *s)
{
    return kw_is(s, "then") || kw_is(s, "else") || kw_is(s, "elif") || kw_is(s, "fi") ||
           kw_is(s, "do") || kw_is(s, "done");
}
static int is_opener(const char *s)
{
    return kw_is(s, "if") || kw_is(s, "while") || kw_is(s, "until") || kw_is(s, "for");
}

/* If item `s` is "LEFT | while COND" (a loop fed by a pipe), returns the "while COND" part. */
static const char *pipe_loop_part(const char *s)
{
    const char *p = s, *last = NULL;
    for (;;) {
        int w; const char *e = find_sep(p, "|", &w);
        if (!w) break;
        p = e + 1; last = p;
    }
    if (!last) return NULL;
    while (*last == ' ' || *last == '\t') last++;
    if (kw_is(last, "while") || kw_is(last, "until")) return last;
    return NULL;
}

static int line_has_keyword(const char *line)
{
    const char *p = line;
    for (;;) {
        int w; const char *e = find_sep(p, ";", &w);
        const char *t = p; while (t < e && (*t == ' ' || *t == '\t')) t++;
        char tmp[16]; size_t l = 0;
        while (t + l < e && t[l] != ' ' && t[l] != '\t' && l < sizeof(tmp) - 1) { tmp[l] = t[l]; l++; }
        tmp[l] = 0;
        if (l && (is_opener(tmp) || is_terminator(tmp))) return 1;
        if (l) { char seg[LINE_MAX]; size_t sl = (size_t)(e - t); if (sl >= sizeof(seg)) sl = sizeof(seg) - 1;
                 memcpy(seg, t, sl); seg[sl] = 0; if (pipe_loop_part(seg)) return 1; }
        if (!w) return 0;
        p = e + 1;
    }
}

/* Removes a trailing `# comment` (a '#' that starts a word, outside quotes). */
static void strip_comment(char *line)
{
    char q = 0;
    for (char *s = line; *s; s++) {
        if (q) { if (*s == q) q = 0; else if (q == '"' && *s == '\\' && s[1]) s++; continue; }
        if (*s == '\\' && s[1]) { s++; continue; }
        if (*s == '"' || *s == '\'' || *s == '`') { q = *s; continue; }
        if (*s == '#' && (s == line || s[-1] == ' ' || s[-1] == '\t' || s[-1] == ';')) { *s = 0; return; }
    }
}

/* Cuts `text` (modified in place) into command items. Returns the count, or -1 if there are too many. */
static int split_items(char *text, char **items, int max)
{
    int n = 0; char *p = text;
    /* backslash-newline joins lines */
    for (char *s = text; *s; s++) {
        if (*s == '\\' && s[1] == '\n') { s[0] = ' '; s[1] = ' '; }
        else if (*s == '\\' && s[1] == '\r' && s[2] == '\n') { s[0] = ' '; s[1] = ' '; s[2] = ' '; }
    }
    while (*p) {
        char *e = p; while (*e && *e != '\n') e++;
        char save = *e; *e = 0;
        char *line = p;
        strip_comment(line);
        for (;;) {
            int w; char *se = (char *)find_sep(line, ";", &w);
            *se = 0;
            char *t = line; while (*t == ' ' || *t == '\t') t++;
            size_t tl = strlen(t); while (tl && (t[tl - 1] == ' ' || t[tl - 1] == '\t' || t[tl - 1] == '\r')) t[--tl] = 0;
            /* peel a leading then/else/do off the command that follows it */
            while (*t) {
                if (n >= max) return -1;
                const char *kw = NULL;
                if (kw_is(t, "then")) kw = "then"; else if (kw_is(t, "else")) kw = "else"; else if (kw_is(t, "do")) kw = "do";
                if (kw && t[strlen(kw)]) {
                    char *rest = t + strlen(kw) + 1;
                    t[strlen(kw)] = 0; items[n++] = t;
                    while (*rest == ' ' || *rest == '\t') rest++;
                    t = rest; continue;
                }
                items[n++] = t; break;
            }
            if (!w) break;
            line = se + 1;
        }
        if (!save) break;
        p = e + 1;
    }
    return n;
}

static void prog_error(const char *what, const char *near_text)
{
    err("syntax error: %s%s%s\n", what, near_text ? " near " : "", near_text ? near_text : "");
    g_ctl = CTL_EXIT; g_ctl_rc = 2; last_status = 2;
}

static void run_list(prog_t *P, int *pi, int live);

static int cond_true(const char *cond)
{
    if (!*cond) return 0;
    int rc = run_simple_line(cond);
    return rc == 0;
}

static void run_if(prog_t *P, int *pi, int live)
{
    const char *cond = kw_rest(P->it[*pi], "if");
    (*pi)++;
    int taken = 0, c = live && !g_ctl && cond_true(cond);
    for (;;) {
        if (*pi >= P->n || !kw_is(P->it[*pi], "then")) { prog_error("expected 'then'", NULL); *pi = P->n; return; }
        (*pi)++;
        int doit = live && c && !taken && !g_ctl;
        run_list(P, pi, doit);
        if (doit) taken = 1;
        if (*pi >= P->n) { prog_error("missing 'fi'", NULL); return; }
        const char *s = P->it[*pi];
        if (kw_is(s, "elif")) {
            cond = kw_rest(s, "elif"); (*pi)++;
            c = live && !taken && !g_ctl && cond_true(cond);
            continue;
        }
        if (kw_is(s, "else")) {
            (*pi)++;
            run_list(P, pi, live && !taken && !g_ctl);
            if (*pi >= P->n || !kw_is(P->it[*pi], "fi")) { prog_error("expected 'fi'", NULL); *pi = P->n; return; }
            (*pi)++;
            return;
        }
        if (kw_is(s, "fi")) { (*pi)++; return; }
        prog_error("unexpected", s); *pi = P->n; return;
    }
}

static int find_done(prog_t *P, int from)
{
    int d = 0;
    for (int i = from; i < P->n; i++) {
        const char *s = P->it[i];
        if (is_opener(s) && !kw_is(s, "if")) d++;
        else if (pipe_loop_part(s)) d++;
        else if (kw_is(s, "done")) { if (d == 0) return i; d--; }
    }
    return -1;
}

/* read(1) input for `done < file` / `cmd | while read`: lines are consumed one per `read`. */
static const char *read_src; static size_t read_pos, read_len;

static void run_while(prog_t *P, int *pi, int live, const char *loop_text, char *src)
{
    int start = *pi;
    int until = kw_is(loop_text, "until");
    const char *cond = kw_rest(loop_text, until ? "until" : "while");
    char *rd_buf = NULL;
    const char *old_src = read_src; size_t old_pos = read_pos, old_len = read_len;
    int done_idx = find_done(P, start + 1);
    if (src) { read_src = src; read_pos = 0; read_len = strlen(src); }
    else if (live && done_idx >= 0) {
        const char *dn = kw_rest(P->it[done_idx], "done");
        if (*dn == '<') {
            char **w = NULL; int nw = shell_words(dn + 1, &w);
            if (nw >= 1) { size_t l; rd_buf = shell_read_file(w[0], &l);
                           if (!rd_buf) err("cannot open %s\n", w[0]); }
            kfree(w);
            if (rd_buf) { read_src = rd_buf; read_pos = 0; read_len = strlen(rd_buf); }
        }
    }
    g_loop_depth++;
    for (;;) {
        *pi = start + 1;
        int c = live && !g_ctl && !kabort_requested();
        if (c) { int ok = cond_true(cond); c = until ? !ok : ok; if (!*cond) c = 0; }
        if (*pi >= P->n || !kw_is(P->it[*pi], "do")) { prog_error("expected 'do'", NULL); *pi = P->n; break; }
        (*pi)++;
        run_list(P, pi, c);
        if (*pi >= P->n || !kw_is(P->it[*pi], "done")) { prog_error("expected 'done'", NULL); *pi = P->n; break; }
        if (g_ctl == CTL_BREAK) { g_ctl = 0; c = 0; }
        else if (g_ctl == CTL_CONTINUE) g_ctl = 0;
        if (c && !g_ctl && !kabort_requested()) { kyield(); continue; }
        (*pi)++;
        break;
    }
    g_loop_depth--;
    read_src = old_src; read_pos = old_pos; read_len = old_len;
    kfree(rd_buf); kfree(src);
}

static void run_for(prog_t *P, int *pi, int live)
{
    int start = *pi;
    const char *h = kw_rest(P->it[start], "for");
    char var[16]; size_t vl = 0;
    while (*h && *h != ' ' && *h != '\t' && vl < sizeof(var) - 1) var[vl++] = *h++;
    var[vl] = 0;
    while (*h == ' ' || *h == '\t') h++;
    char **words = NULL; int nw = 0;
    if (live) {
        if (kw_is(h, "in")) {
            nw = shell_words(kw_rest(h, "in"), &words);
            if (nw < 0) { err("syntax error in word list\n"); nw = 0; words = NULL; }
        } else {                                                   /* `for x` loops over "$@" */
            words = (char **)kmalloc(sizeof(char *) * (MAX_POS + 1));
            if (words) { for (int i = 1; i <= pos_n; i++) words[nw++] = pos_arg[i]; }
        }
    }
    int body = start + 1;
    if (body >= P->n || !kw_is(P->it[body], "do")) { prog_error("expected 'do'", NULL); *pi = P->n; kfree(words); return; }
    body++;
    g_loop_depth++;
    for (int k = 0; ; k++) {
        int c = live && k < nw && !g_ctl && !kabort_requested();
        *pi = body;
        if (c) shell_setenv(var, words[k]);
        run_list(P, pi, c);
        if (*pi >= P->n || !kw_is(P->it[*pi], "done")) { prog_error("expected 'done'", NULL); *pi = P->n; break; }
        if (g_ctl == CTL_BREAK) { g_ctl = 0; (*pi)++; break; }
        if (g_ctl == CTL_CONTINUE) g_ctl = 0;
        if (!c || g_ctl) { (*pi)++; break; }
        kyield();
    }
    g_loop_depth--;
    kfree(words);
}

/* Runs items until a terminator keyword (then/else/elif/fi/do/done) or the end. With live == 0 the
 * items are only skipped over (their structure is still parsed so nesting stays balanced). */
static void run_list(prog_t *P, int *pi, int live)
{
    while (*pi < P->n) {
        const char *s = P->it[*pi];
        if (is_terminator(s)) return;
        int run = live && !g_ctl && !kabort_requested();
        if (kw_is(s, "if")) { run_if(P, pi, run); continue; }
        if (kw_is(s, "while") || kw_is(s, "until")) { run_while(P, pi, run, s, NULL); continue; }
        if (kw_is(s, "for")) { run_for(P, pi, run); continue; }
        const char *lp = pipe_loop_part(s);
        if (lp) {                                                  /* producer | while ... */
            char *src = NULL;
            if (run) {
                size_t llen = (size_t)(lp - s);
                char *left = (char *)kmalloc(llen + 1);
                char *buf = (char *)kmalloc(SUBST_MAX);
                if (left && buf) {
                    memcpy(left, s, llen); left[llen] = 0;
                    size_t k = llen; while (k && (left[k - 1] == ' ' || left[k - 1] == '|')) left[--k] = 0;
                    struct terminal_capture_state st; terminal_capture_save(&st);
                    terminal_capture_begin(buf, SUBST_MAX);
                    run_simple_line(left);
                    terminal_capture_end(); terminal_capture_restore(&st);
                    src = buf; buf = NULL;
                }
                kfree(left); kfree(buf);
            }
            run_while(P, pi, run && src != NULL, lp, src);
            continue;
        }
        (*pi)++;
        if (run) run_simple_line(s);
    }
}

static int run_program(char **items, int n)
{
    prog_t P; P.it = items; P.n = n;
    int i = 0;
    while (i < n && !g_ctl && !kabort_requested()) {
        run_list(&P, &i, 1);
        if (i < n && !g_ctl) { prog_error("unexpected", items[i]); break; }
    }
    return last_status;
}

/* ------------------------------------------------------------ entry points */
/* One list of commands separated by ; && || with no control-flow keywords. */
static int run_simple_line(const char *line)
{
    int rc = 0;
    const char *p = line;
    int run_next = 1;
    for (;;) {
        int w;
        const char *e = find_sep(p, ";&O", &w);
        char seg[LINE_MAX];
        size_t l = (size_t)(e - p); if (l >= sizeof(seg)) l = sizeof(seg) - 1;
        memcpy(seg, p, l); seg[l] = 0;
        const char *t = seg; while (*t == ' ' || *t == '\t') t++;
        if (*t == '#') { /* comment line */ }
        else if (run_next && *t) {
            int neg = 0;
            if (*t == '!' && (t[1] == ' ' || t[1] == '\t')) { neg = 1; t++; while (*t == ' ' || *t == '\t') t++; }
            rc = run_pipeline(t);
            if (neg) rc = !rc;
            last_status = rc;
        }
        if (!w) break;
        if (g_ctl) break;                                   /* break/continue/exit asked to stop this list */
        if (*e == '&') run_next = (rc == 0);
        else if (*e == '|') run_next = (rc != 0);
        else run_next = 1;
        p = e + w;
        if (kabort_requested()) break;
    }
    return rc;
}

static int run_structured_line(const char *line)
{
    size_t l = strlen(line);
    char *text = (char *)kmalloc(l + 2);
    char **items = (char **)kmalloc(sizeof(char *) * MAX_ITEMS);
    if (!text || !items) { kfree(text); kfree(items); err("out of memory\n"); return 1; }
    memcpy(text, line, l + 1);
    int n = split_items(text, items, MAX_ITEMS);
    int rc;
    if (n < 0) { err("line too complex\n"); rc = 2; }
    else rc = run_program(items, n);
    kfree(text); kfree(items);
    return rc;
}

static int run_line_inner(const char *line)
{
    if (line_has_keyword(line)) return run_structured_line(line);
    return run_simple_line(line);
}

static void enter_run(void)
{
    if (g_run_depth++ == 0) { ksched_set_in_command(1); kabort_clear(); g_ctl = 0; }
}
static void leave_run(void)
{
    if (--g_run_depth == 0) {
        if (kabort_requested()) keyboard_flush_queue();
        kabort_clear();
        ksched_set_in_command(0);
        g_ctl = 0;
    }
}

int shell_run_line(const char *line)
{
    enter_run();
    char buf[LINE_MAX]; strlcpy(buf, line, sizeof(buf));
    strip_comment(buf);
    int rc = run_line_inner(buf);
    leave_run();
    return rc;
}

int shell_run_script_args(const char *filename, int argc, char **argv, int *status)
{
    size_t len; char *txt = shell_read_file(filename, &len);
    if (!txt) return -1;
    enter_run();

    /* private copies of $0 $1 ... for the lifetime of the script */
    char *saved_pos[MAX_POS]; int saved_n = pos_n;
    memcpy(saved_pos, pos_arg, sizeof(saved_pos));
    size_t tot = strlen(filename) + 1;
    for (int i = 1; i < argc; i++) tot += strlen(argv[i]) + 1;
    char *blk = (char *)kmalloc(tot);
    memset(pos_arg, 0, sizeof(pos_arg)); pos_n = 0;
    if (blk) {
        char *d = blk;
        pos_arg[0] = d; strcpy(d, filename); d += strlen(filename) + 1;
        for (int i = 1; i < argc && i < MAX_POS; i++) { pos_arg[i] = d; strcpy(d, argv[i]); d += strlen(argv[i]) + 1; pos_n = i; }
    }

    int saved_ctl = g_ctl, saved_ctl_rc = g_ctl_rc, saved_loop = g_loop_depth;
    g_ctl = 0; g_loop_depth = 0; g_script_depth++;
    char **items = (char **)kmalloc(sizeof(char *) * MAX_ITEMS);
    int n = items ? split_items(txt, items, MAX_ITEMS) : -1;
    int rc;
    if (n < 0) { err("%s: script too large\n", filename); rc = 2; }
    else { run_program(items, n); rc = (g_ctl == CTL_EXIT) ? g_ctl_rc : last_status; }
    g_script_depth--;
    g_ctl = saved_ctl; g_ctl_rc = saved_ctl_rc; g_loop_depth = saved_loop;
    memcpy(pos_arg, saved_pos, sizeof(saved_pos)); pos_n = saved_n;
    kfree(blk); kfree(items); kfree(txt);
    last_status = rc;
    if (status) *status = rc;
    leave_run();
    return 0;
}

int shell_run_script(const char *filename)
{
    int st = 0;
    return shell_run_script_args(filename, 0, NULL, &st);
}

/* ============================================================ line editor */
static char input[LINE_MAX];
static size_t in_len, in_pos;
static int prompt_row, prompt_col;
static char history[HISTORY_SIZE][LINE_MAX];
static int hist_count, hist_nav = -1;
static char draft[LINE_MAX];
static int tab_streak;

static void cursor_xy(size_t idx, int *row, int *col)
{
    int cols, rows; terminal_get_size(&cols, &rows);
    int lin = prompt_col + (int)idx;
    *row = prompt_row + lin / cols; *col = lin % cols;
}

static void place_cursor(size_t idx)
{
    int r, c; cursor_xy(idx, &r, &c);
    terminal_set_cursor(r, c);
}

/* Reprints input[from..] (plus an erasing space) and leaves the cursor at in_pos. */
static void repaint_from(size_t from)
{
    place_cursor(from);
    uint32_t s0 = terminal_scroll_total();
    for (size_t i = from; i < in_len; i++) terminal_putchar(input[i]);
    terminal_putchar(' ');
    prompt_row -= (int)(terminal_scroll_total() - s0);
    place_cursor(in_pos);
}

static void print_prompt(void)
{
    /* Keep the prompt itself single-line whatever the real cwd depth. */
    char cwd[64]; strlcpy(cwd, vfs_get_cwd(), sizeof(cwd));
    const char *h = shell_getenv("HOME");
    if (h && *h && strcmp(cwd, h) == 0) strlcpy(cwd, "~", sizeof(cwd));
    fg(C_LGREEN); terminal_writestring(shell_getenv("USER") ? shell_getenv("USER") : "user");
    fg(C_LGRAY); terminal_putchar('@');
    fg(C_LGREEN); terminal_writestring(net_get_config()->hostname[0] ? net_get_config()->hostname : "starlight");
    fg(C_LGRAY); terminal_putchar(':');
    fg(C_LCYAN); terminal_writestring(cwd);
    fg(last_status ? C_LRED : C_YELLOW); terminal_writestring(" $ ");
    fg(C_WHITE);
    terminal_get_cursor(&prompt_row, &prompt_col);
    in_len = in_pos = 0; input[0] = 0; hist_nav = -1; tab_streak = 0;
}

static void set_line(const char *s)
{
    size_t old = in_len;
    strlcpy(input, s, sizeof(input));
    in_len = in_pos = strlen(input);
    place_cursor(0);
    for (size_t i = 0; i < in_len; i++) terminal_putchar(input[i]);
    for (size_t i = in_len; i < old; i++) terminal_putchar(' ');
    /* handle scroll while painting */
    place_cursor(in_pos);
}

static void history_add(const char *line)
{
    if (!*line) return;
    if (hist_count && strcmp(history[hist_count - 1], line) == 0) return;
    if (hist_count == HISTORY_SIZE) { memmove(history[0], history[1], sizeof(history[0]) * (HISTORY_SIZE - 1)); hist_count--; }
    strlcpy(history[hist_count++], line, sizeof(history[0]));
}

static void history_move(int dir)
{
    if (!hist_count) return;
    if (hist_nav == -1) { if (dir < 0) return; strlcpy(draft, input, sizeof(draft)); }
    int nn = hist_nav + dir;
    if (nn < -1) nn = -1;
    if (nn > hist_count - 1) nn = hist_count - 1;
    hist_nav = nn;
    set_line(nn == -1 ? draft : history[hist_count - 1 - nn]);
}

static void insert_char(char c)
{
    if (in_len >= LINE_MAX - 1) return;
    if (in_pos == in_len) {
        input[in_len++] = c; input[in_len] = 0; in_pos = in_len;
        uint32_t s0 = terminal_scroll_total();
        terminal_putchar(c);
        prompt_row -= (int)(terminal_scroll_total() - s0);
        return;
    }
    memmove(input + in_pos + 1, input + in_pos, in_len - in_pos + 1);
    input[in_pos] = c; in_len++; in_pos++;
    repaint_from(in_pos - 1);
}

static void delete_range(size_t from, size_t to)
{
    if (to > in_len) to = in_len;
    if (from >= to) return;
    memmove(input + from, input + to, in_len - to + 1);
    size_t removed = to - from;
    in_len -= removed;
    if (in_pos > to) in_pos -= removed; else if (in_pos > from) in_pos = from;
    repaint_from(from);
    for (size_t i = 0; i < removed; i++) terminal_putchar(' ');
    place_cursor(in_pos);
}

/* -------- tab completion -------- */
static char cand[48][VFS_NAME_MAX + 2];
static int cand_n;

static void cand_add(const char *s)
{
    for (int i = 0; i < cand_n; i++) if (strcmp(cand[i], s) == 0) return;
    if (cand_n < 48) strlcpy(cand[cand_n++], s, sizeof(cand[0]));
}

static void do_tab(void)
{
    size_t ws = in_pos;
    while (ws > 0 && input[ws - 1] != ' ') ws--;
    int first_word = 1;
    for (size_t i = 0; i < ws; i++) if (input[i] != ' ') first_word = 0;
    char frag[64]; size_t fl = in_pos - ws; if (fl >= sizeof(frag)) fl = sizeof(frag) - 1;
    memcpy(frag, input + ws, fl); frag[fl] = 0;
    cand_n = 0;

    if (first_word) {
        for (int t = 0; t < table_n; t++)
            for (int i = 0; i < table_counts[t]; i++)
                if (strncmp(tables[t][i].name, frag, fl) == 0) cand_add(tables[t][i].name);
        for (int i = 0; i < MAX_ALIASES; i++) if (aliases[i].name[0] && strncmp(aliases[i].name, frag, fl) == 0) cand_add(aliases[i].name);
    }
    /* file names (also for command position: lets you run MINES.ELF-style names) */
    static char names[40][VFS_NAME_MAX + 1]; static uint8_t isdir[40];
    int n = vfs_get_entries(names, isdir, 40);
    for (int i = 0; i < n; i++) {
        size_t k = 0;
        while (k < fl && names[i][k] && ktoupper(frag[k]) == ktoupper(names[i][k])) k++;
        if (k == fl && (!first_word || fl > 0)) {
            char nm[VFS_NAME_MAX + 2]; strlcpy(nm, names[i], sizeof(nm));
            if (isdir[i]) strlcat(nm, "/", sizeof(nm));
            cand_add(nm);
        }
    }
    if (!cand_n) return;
    if (cand_n == 1) {
        const char *c = cand[0];
        for (size_t k = fl; c[k]; k++) insert_char(c[k]);
        if (c[strlen(c) - 1] != '/') insert_char(' ');
        tab_streak = 0;
        return;
    }
    /* longest common prefix (case-insensitive) */
    size_t lcp = strlen(cand[0]);
    for (int i = 1; i < cand_n; i++) {
        size_t k = 0;
        while (k < lcp && cand[i][k] && ktoupper(cand[0][k]) == ktoupper(cand[i][k])) k++;
        lcp = k;
    }
    if (lcp > fl) { for (size_t k = fl; k < lcp; k++) insert_char(cand[0][k]); tab_streak = 0; return; }
    if (++tab_streak < 2) return;
    /* second Tab: list matches, then reprint the prompt + line */
    place_cursor(in_len);
    terminal_putchar('\n');
    int cols, rows; terminal_get_size(&cols, &rows);
    int per = cols / 14; if (per < 1) per = 1;
    for (int i = 0; i < cand_n; i++) {
        fg(strchr(cand[i], '/') ? C_LCYAN : C_LGRAY);
        kprintf("%-13s ", cand[i]);
        if ((i + 1) % per == 0) terminal_putchar('\n');
    }
    if (cand_n % per) terminal_putchar('\n');
    fg_reset();
    char save[LINE_MAX]; strlcpy(save, input, sizeof(save)); size_t sp = in_pos;
    print_prompt();
    strlcpy(input, save, sizeof(input)); in_len = strlen(input); in_pos = sp;
    for (size_t i = 0; i < in_len; i++) terminal_putchar(input[i]);
    place_cursor(in_pos);
    tab_streak = 0;
}

static void execute_line(void)
{
    place_cursor(in_len);
    terminal_putchar('\n');
    input[in_len] = 0;
    char line[LINE_MAX]; strlcpy(line, input, sizeof(line));
    history_add(line);
    in_len = in_pos = 0; input[0] = 0;
    fg_reset();
    if (*line) shell_run_line(line);
    fg_reset();
    print_prompt();
}

void shell_handle_character(char ch)
{
    unsigned char c = (unsigned char)ch;
    if (ksched_in_command()) return;                      /* typed-ahead keys are consumed by shell_getc or later */
    if (c != '\t') tab_streak = 0;
    switch (c) {
    case '\n': execute_line(); return;
    case '\b': if (in_pos > 0) delete_range(in_pos - 1, in_pos); return;
    case KEY_DEL: if (in_pos < in_len) delete_range(in_pos, in_pos + 1); return;
    case KEY_LEFT: if (in_pos > 0) { in_pos--; place_cursor(in_pos); } return;
    case KEY_RIGHT: if (in_pos < in_len) { in_pos++; place_cursor(in_pos); } return;
    case KEY_HOME: case 1: in_pos = 0; place_cursor(0); return;
    case KEY_END: case 5: in_pos = in_len; place_cursor(in_pos); return;
    case KEY_UP: history_move(1); return;
    case KEY_DOWN: history_move(-1); return;
    case KEY_PGUP: terminal_scroll_view(8); return;
    case KEY_PGDN: terminal_scroll_view(-8); return;
    case 21: delete_range(0, in_pos); return;             /* Ctrl-U */
    case 11: delete_range(in_pos, in_len); return;        /* Ctrl-K */
    case 23: {                                            /* Ctrl-W */
        size_t s = in_pos;
        while (s > 0 && input[s - 1] == ' ') s--;
        while (s > 0 && input[s - 1] != ' ') s--;
        delete_range(s, in_pos); return;
    }
    case 12: terminal_clear(); print_prompt(); return;    /* Ctrl-L */
    case 3:                                               /* Ctrl-C */
        place_cursor(in_len); fg(C_LRED); terminal_writestring("^C\n"); fg_reset(); last_status = 130; print_prompt(); return;
    case '\t': do_tab(); tab_streak += (tab_streak == 0); return;
    default: break;
    }
    if (c >= 32 && c < 127) insert_char((char)c);
}

/* ============================================================ builtin commands */
static int cmd_help(int argc, char **argv)
{
    static const char *groups[] = { "System", "Files", "Text", "Network", "Sound", "Shell" };
    if (argc > 1) {
        const struct command *c = find_command(argv[1]);
        if (c) { fg(C_YELLOW); kprintf("%s", c->usage); fg_reset(); kprintf("\n  %s\n", c->help); }
        for (unsigned g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
            if (strcasecmp(groups[g], argv[1]) != 0) continue;
            if (c) kprintf("\n%s commands:\n", groups[g]);
            for (int t = 0; t < table_n; t++) for (int i = 0; i < table_counts[t]; i++)
                if (strcmp(tables[t][i].group, groups[g]) == 0) {
                    fg(C_LCYAN); kprintf("  %-10s", tables[t][i].name); fg_reset(); kprintf("%s\n", tables[t][i].help);
                }
            return 0;
        }
        if (c) return 0;
        err("help: no such command or group '%s'\n", argv[1]);
        return 1;
    }
    fg(C_WHITE); kprintf("StarlightOS %s -- commands by group (help GROUP, help COMMAND)\n", STARLIGHTOS_VERSION);
    for (unsigned g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
        fg(C_YELLOW); kprintf(" %-8s ", groups[g]); fg(C_LGRAY);
        int col = 10, cols, rows; terminal_get_size(&cols, &rows);
        for (int t = 0; t < table_n; t++) for (int i = 0; i < table_counts[t]; i++) {
            if (strcmp(tables[t][i].group, groups[g]) != 0) continue;
            int l = (int)strlen(tables[t][i].name) + 1;
            if (col + l >= cols) { kprintf("\n          "); col = 10; }
            kprintf("%s ", tables[t][i].name); col += l;
        }
        kprintf("\n");
    }
    fg(C_DGRAY); kprintf("Keys: Tab complete, Up/Down history, PgUp/PgDn scrollback, Ctrl-A/E line start/end, Ctrl-U/K/W cut, Ctrl-C abort, Ctrl-L clear\n");
    kprintf("Operators: cmd1 ; cmd2   cmd1 && cmd2   cmd | cmd   cmd > file   cmd >> file   cmd < file   $VAR   alias\n");
    fg_reset();
    return 0;
}

static int cmd_clear(int argc, char **argv) { (void)argc; (void)argv; terminal_clear(); return 0; }

static int cmd_echo(int argc, char **argv)
{
    int nl = 1, i = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) { nl = 0; i = 2; }
    for (; i < argc; i++) { terminal_writestring(argv[i]); if (i + 1 < argc) terminal_putchar(' '); }
    if (nl) terminal_putchar('\n');
    return 0;
}

static int cmd_history(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int i = 0; i < hist_count; i++) kprintf("%4d  %s\n", i + 1, history[i]);
    return 0;
}

static int cmd_alias(int argc, char **argv)
{
    if (argc == 1) {
        for (int i = 0; i < MAX_ALIASES; i++) if (aliases[i].name[0]) kprintf("alias %s='%s'\n", aliases[i].name, aliases[i].text);
        return 0;
    }
    char *eq = strchr(argv[1], '=');
    if (!eq) {
        const char *a = alias_lookup(argv[1]);
        if (!a) { err("alias: %s not found\n", argv[1]); return 1; }
        kprintf("alias %s='%s'\n", argv[1], a);
        return 0;
    }
    *eq = 0;
    alias_set(argv[1], eq + 1);
    return 0;
}

static int cmd_unalias(int argc, char **argv)
{
    if (argc < 2) { err("usage: unalias NAME\n"); return 1; }
    for (int i = 0; i < MAX_ALIASES; i++) if (strcmp(aliases[i].name, argv[1]) == 0) aliases[i].name[0] = 0;
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc == 1) { for (int i = 0; i < MAX_VARS; i++) if (vars[i].name[0]) kprintf("%s=%s\n", vars[i].name, vars[i].value); return 0; }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) { err("usage: set NAME=value\n"); return 1; }
        *eq = 0; shell_setenv(argv[i], eq + 1);
    }
    return 0;
}

static int cmd_unset(int argc, char **argv) { for (int i = 1; i < argc; i++) unsetenv_(argv[i]); return 0; }

static int cmd_source(int argc, char **argv)
{
    if (argc < 2) { err("usage: %s FILE [ARGS...]\n", argv[0]); return 1; }
    int st = 0;
    if (shell_run_script_args(argv[1], argc - 1, argv + 1, &st) != 0) { err("%s: cannot read %s\n", argv[0], argv[1]); return 1; }
    return st;
}

static int cmd_which(int argc, char **argv)
{
    if (argc < 2) { err("usage: which NAME\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = alias_lookup(argv[i]);
        char path[VFS_PATH_MAX];
        if (a) kprintf("%s: aliased to '%s'\n", argv[i], a);
        else {
            const struct command *c = find_command(argv[i]);
            if (c) kprintf("%s: shell builtin (%s)\n", argv[i], c->group);
            else if (path_lookup(argv[i], path, sizeof(path))) kprintf("%s\n", path);
            else { kprintf("%s: not found\n", argv[i]); rc = 1; }
        }
    }
    return rc;
}

/* type NAME...: how the shell would interpret each name (alias, builtin, file in $PATH). */
static int cmd_type(int argc, char **argv)
{
    if (argc < 2) { err("usage: type NAME...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = alias_lookup(argv[i]);
        char path[VFS_PATH_MAX];
        if (a) kprintf("%s is aliased to '%s'\n", argv[i], a);
        else if (find_command(argv[i])) kprintf("%s is a shell builtin\n", argv[i]);
        else if (path_lookup(argv[i], path, sizeof(path))) kprintf("%s is %s\n", argv[i], path);
        else { err("type: %s: not found\n", argv[i]); rc = 1; }
    }
    return rc;
}

static int cmd_true(int argc, char **argv)  { (void)argc; (void)argv; return 0; }
static int cmd_false(int argc, char **argv) { (void)argc; (void)argv; return 1; }

static int cmd_break(int argc, char **argv)
{
    (void)argc;
    if (g_loop_depth > 0) g_ctl = (argv[0][0] == 'b') ? CTL_BREAK : CTL_CONTINUE;
    else err("%s: only meaningful in a loop\n", argv[0]);
    return 0;
}

/* exit [N] / return [N]: end the running script. At the prompt `exit` just explains the desktop window. */
static int cmd_exit(int argc, char **argv)
{
    int32_t n = last_status;
    if (argc > 1 && kparse_int(argv[1], &n) != 0) { err("%s: numeric argument required\n", argv[0]); n = 2; }
    if (g_script_depth > 0) { g_ctl = CTL_EXIT; g_ctl_rc = (int)n; return (int)n; }
    if (argv[0][0] == 'r') { err("return: only valid in a script\n"); return 1; }
    kprintf("This terminal is a desktop window: press ESC or click the red button to close it.\n");
    return 0;
}

/* shift [N]: drop the first N positional parameters. */
static int cmd_shift(int argc, char **argv)
{
    int32_t n = 1;
    if (argc > 1 && (kparse_int(argv[1], &n) != 0 || n < 0)) { err("shift: numeric argument required\n"); return 1; }
    if (n > pos_n) { err("shift: shift count out of range\n"); return 1; }
    for (int i = 1; i + n <= pos_n; i++) pos_arg[i] = pos_arg[i + n];
    for (int i = pos_n - n + 1; i <= pos_n; i++) pos_arg[i] = NULL;
    pos_n -= n;
    return 0;
}

/* eval ARGS...: join the arguments and run them as a command line. */
static int cmd_eval(int argc, char **argv)
{
    char line[LINE_MAX]; line[0] = 0;
    for (int i = 1; i < argc; i++) { if (i > 1) strlcat(line, " ", sizeof(line)); strlcat(line, argv[i], sizeof(line)); }
    return run_line_inner(line);
}

/* read [-p PROMPT] [-r] [NAME...]: read one line from `done < file`/a pipe, or from the keyboard. The line is split
 * on blanks across the NAMEs (the last one gets the remainder); with no NAME it goes to REPLY. Status 1 at end of input. */
static int cmd_read(int argc, char **argv)
{
    const char *prompt = NULL; int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) prompt = argv[++i];
        else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-s")) { /* accepted, no effect */ }
        else { err("usage: read [-p PROMPT] [NAME...]\n"); return 2; }
    }
    char line[256]; size_t ll = 0; int eof = 0;
    if (read_src) {
        if (read_pos >= read_len) eof = 1;
        else {
            while (read_pos < read_len && read_src[read_pos] != '\n') { if (ll < sizeof(line) - 1) line[ll++] = read_src[read_pos]; read_pos++; }
            if (read_pos < read_len) read_pos++;
        }
    } else if (shell_stdin) {
        if (!shell_stdin_len) eof = 1;
        else for (size_t k = 0; k < shell_stdin_len && shell_stdin[k] != '\n'; k++) if (ll < sizeof(line) - 1) line[ll++] = shell_stdin[k];
    } else {
        if (prompt) { terminal_writestring(prompt); }
        for (;;) {
            int c = shell_getc(0);
            if (c < 0) { terminal_putchar('\n'); return 130; }
            if (c == '\n' || c == '\r') { terminal_putchar('\n'); break; }
            if (c == '\b' || c == 127) { if (ll) { ll--; terminal_putchar('\b'); } continue; }
            if (c >= 32 && c < 127 && ll < sizeof(line) - 1) { line[ll++] = (char)c; terminal_putchar((char)c); }
        }
    }
    while (ll && line[ll - 1] == '\r') ll--;
    line[ll] = 0;
    if (i >= argc) { shell_setenv("REPLY", line); return eof ? 1 : 0; }
    char *p = line;
    for (; i < argc; i++) {
        while (*p == ' ' || *p == '\t') p++;
        if (i == argc - 1) { shell_setenv(argv[i], p); break; }
        char *w = p; while (*p && *p != ' ' && *p != '\t') p++;
        char save = *p; *p = 0; shell_setenv(argv[i], w); if (save) p++;
    }
    return eof ? 1 : 0;
}

static int cmd_sleep(int argc, char **argv)
{
    uint32_t n;
    if (argc < 2 || kparse_uint(argv[1], &n) != 0) { err("usage: sleep SECONDS\n"); return 1; }
    ksleep_ms(n * 1000u);
    return kabort_requested() ? 130 : 0;
}

static void fmt_duration(uint32_t secs, char *out, size_t n)
{
    uint32_t d = secs / 86400u, h = (secs % 86400u) / 3600u, m = (secs % 3600u) / 60u, s = secs % 60u;
    if (d) ksnprintf(out, n, "%ud %uh %um %us", d, h, m, s);
    else if (h) ksnprintf(out, n, "%uh %um %us", h, m, s);
    else if (m) ksnprintf(out, n, "%um %us", m, s);
    else ksnprintf(out, n, "%us", s);
}

static int cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    char b[40]; fmt_duration(ktime_ms() / 1000u, b, sizeof(b));
    kprintf("up %s\n", b);
    return 0;
}

static const char *const wdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const months_[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static int cmd_date(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint32_t t = ktime_unix();
    int y, mo, d, h, mi, s, wd;
    ktime_unix_to_fields(t + (uint32_t)(ktime_tz_offset_minutes() * 60), &y, &mo, &d, &h, &mi, &s, &wd);
    int tz = ktime_tz_offset_minutes();
    kprintf("%s %s %2d %02d:%02d:%02d UTC%+03d:%02d %d\n", wdays[wd], months_[mo - 1], d, h, mi, s, tz / 60, (tz < 0 ? -tz : tz) % 60, y);
    return 0;
}

static int cmd_time(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct rtc_time t; rtc_read(&t);
    kprintf("%02u:%02u:%02u\n", t.hour, t.minute, t.second);
    return 0;
}

static int cmd_tz(int argc, char **argv)
{
    if (argc < 2) { int m = ktime_tz_offset_minutes(); kprintf("UTC%+03d:%02d\n", m / 60, (m < 0 ? -m : m) % 60); return 0; }
    int32_t h;
    if (kparse_int(argv[1], &h) != 0 || h < -12 || h > 14) { err("usage: tz HOURS_FROM_UTC (e.g. 8, -5)\n"); return 1; }
    ktime_set_tz_offset_minutes(h * 60);
    return 0;
}

static int cmd_cal(int argc, char **argv)
{
    int y, mo, d, wd;
    uint32_t t = ktime_unix() + (uint32_t)(ktime_tz_offset_minutes() * 60);
    ktime_unix_to_fields(t, &y, &mo, &d, 0, 0, 0, &wd);
    int today = d, cm = mo, cy = y;
    if (argc >= 3) { int32_t a, b; if (kparse_int(argv[1], &a) || kparse_int(argv[2], &b) || a < 1 || a > 12 || b < 1970 || b > 2099) { err("usage: cal [MONTH [YEAR]]\n"); return 1; } cm = a; cy = b; today = 0; }
    else if (argc == 2) { int32_t a; if (kparse_int(argv[1], &a) || a < 1 || a > 12) { err("usage: cal [MONTH [YEAR]]\n"); return 1; } cm = a; }
    if (cm != mo || cy != y) today = 0;
    uint32_t first = ktime_fields_to_unix(cy, cm, 1, 0, 0, 0);
    int fw; ktime_unix_to_fields(first, 0, 0, 0, 0, 0, 0, &fw);
    static const uint8_t md[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int dim = md[cm - 1] + (cm == 2 && ((cy % 4 == 0 && cy % 100 != 0) || cy % 400 == 0) ? 1 : 0);
    static const char *const full[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    char title[32]; ksnprintf(title, sizeof(title), "%s %d", full[cm - 1], cy);
    kprintf("%*s\n", (20 + (int)strlen(title)) / 2, title);
    fg(C_LCYAN); kprintf("Su Mo Tu We Th Fr Sa\n"); fg_reset();
    for (int i = 0; i < fw; i++) kprintf("   ");
    for (int day = 1; day <= dim; day++) {
        if (day == today) { fg(C_BLACK); terminal_set_color(C_BLACK, C_LGRAY); kprintf("%2d", day); terminal_set_color(C_LGRAY, C_BLACK); kprintf(" "); }
        else kprintf("%2d ", day);
        if ((fw + day) % 7 == 0) kprintf("\n");
    }
    if ((fw + dim) % 7) kprintf("\n");
    return 0;
}

static void cpu_brand(char out[49])
{
    uint32_t r[12];
    uint32_t max;
    __asm__ volatile("cpuid" : "=a"(max) : "a"(0x80000000) : "ebx", "ecx", "edx");
    if (max < 0x80000004u) { strlcpy(out, "unknown x86 CPU", 49); return; }
    for (uint32_t i = 0; i < 3; i++)
        __asm__ volatile("cpuid" : "=a"(r[i * 4]), "=b"(r[i * 4 + 1]), "=c"(r[i * 4 + 2]), "=d"(r[i * 4 + 3]) : "a"(0x80000002u + i));
    memcpy(out, r, 48); out[48] = 0;
    char *p = out; while (*p == ' ') p++;
    if (p != out) memmove(out, p, strlen(p) + 1);
}

static uint32_t total_ram_kb(void)
{
    uint32_t cmos = ktime_cmos_ram_kb();         /* what the BIOS reports (accurate on QEMU and PCs < 4 GiB) */
    if (cmos) return cmos;
    uint32_t lo, up;
    if (multiboot_get_memory(&lo, &up) == 0 && up) return lo + up;
    return 0;
}

static int cmd_cpuinfo(int argc, char **argv)
{
    (void)argc; (void)argv;
    char b[49]; cpu_brand(b);
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    char v[13]; memcpy(v, &ebx, 4); memcpy(v + 4, &edx, 4); memcpy(v + 8, &ecx, 4); v[12] = 0;
    kprintf("Model:    %s\nVendor:   %s\n", b, v);
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    kprintf("Family:   %u  Model: %u  Stepping: %u\n", (eax >> 8) & 15, ((eax >> 4) & 15) | ((eax >> 12) & 0xF0), eax & 15);
    static const struct { int reg; uint32_t bit; const char *name; } feats[] = {
        { 0, 25, "sse" }, { 0, 26, "sse2" }, { 1, 0, "sse3" }, { 1, 9, "ssse3" }, { 1, 19, "sse4.1" },
        { 1, 20, "sse4.2" }, { 1, 28, "avx" }, { 0, 4, "tsc" }, { 0, 5, "msr" }, { 0, 9, "apic" },
        { 0, 23, "mmx" }, { 1, 30, "rdrand" },
    };
    kprintf("Features:");
    for (unsigned i = 0; i < sizeof(feats) / sizeof(feats[0]); i++) {
        uint32_t word = feats[i].reg ? ecx : edx;
        if (word & (1u << feats[i].bit)) kprintf(" %s", feats[i].name);
    }
    kprintf("\n");
    return 0;
}

static int cmd_uname(int argc, char **argv)
{
    if (has_flag(argc, argv, "-a")) kprintf("StarlightOS %s %s x86_64 %s\n", net_get_config()->hostname, STARLIGHTOS_VERSION, STARLIGHTOS_BUILD_NAME);
    else kprintf("StarlightOS\n");
    return 0;
}

static int cmd_ver(int argc, char **argv) { (void)argc; (void)argv; kprintf("StarlightOS %s (%s)\n", STARLIGHTOS_VERSION, STARLIGHTOS_BUILD_NAME); return 0; }

static int cmd_neofetch(int argc, char **argv)
{
    (void)argc; (void)argv;
    static const char *const logo[] = {
        "         .               ",
        "        /|\\              ",
        "   ----< * >----         ",
        "        \\|/              ",
        "         '               ",
        "   x86_64  Long Mode     ",
    };
    char brand[49]; cpu_brand(brand);
    char up[40]; fmt_duration(ktime_ms() / 1000u, up, sizeof(up));
    uint32_t ram = total_ram_kb();
    size_t ht, hu; kheap_stats(&ht, &hu, NULL, NULL);
    char ipb[16] = "no lease"; if (net_configured()) net_fmt_ip(ipb, net_get_config()->ip);
    struct audio_status as; audio_get_status(&as);
    const char *host = net_get_config()->hostname;
    uint32_t total = vfs_total_bytes(), freeb = vfs_free_bytes();
    char rows[12][96]; int n = 0;
    ksnprintf(rows[n++], 96, "%s@%s", shell_getenv("USER") ? shell_getenv("USER") : "user", host);
    { int rule = (int)strlen(rows[0]); if (rule > 90) rule = 90; int k = 0; for (; k < rule; k++) rows[n][k] = '-'; rows[n++][k] = 0; }
    ksnprintf(rows[n++], 96, "OS:      StarlightOS %s \"%s\" x86_64", STARLIGHTOS_VERSION, STARLIGHTOS_BUILD_NAME);
    ksnprintf(rows[n++], 96, "Uptime:  %s", up);
    ksnprintf(rows[n++], 96, "Shell:   %s", shell_getenv("SHELL") ? shell_getenv("SHELL") : "starlightsh");
    ksnprintf(rows[n++], 96, "CPU:     %s", brand);
    ksnprintf(rows[n++], 96, "Memory:  %u MiB RAM, heap %u/%u KiB", ram / 1024u, (unsigned)(hu / 1024u), (unsigned)(ht / 1024u));
    ksnprintf(rows[n++], 96, "Display: %dx%d %s  Font: %s", vga_width(), vga_height(), vga_is_truecolor() ? "TrueColor" : "8-bit", font_active_name());
    ksnprintf(rows[n++], 96, "Disk:    %u / %u KiB used", (total - freeb) / 1024u, total / 1024u);
    ksnprintf(rows[n++], 96, "Network: %s", ipb);
    ksnprintf(rows[n++], 96, "Audio:   %s, vol %d%%", as.backend == 2 ? "AC'97" : as.backend == 1 ? "PC speaker" : "none", as.volume);
    for (int i = 0; i < n; i++) {
        fg(C_LCYAN); kprintf("%-26s", i < 6 ? logo[i] : "");
        if (i == 0) fg(C_LGREEN); else if (i == 1) fg(C_DGRAY); else fg(C_LGRAY);
        kprintf("%s\n", rows[i]);
    }
    kprintf("%-26s", "");
    for (int c = 0; c < 16; c++) { terminal_set_color(C_BLACK, (uint8_t)c); kprintf("  "); }
    terminal_set_color(C_LGRAY, C_BLACK); kprintf("\n");
    return 0;
}

static int cmd_mem(int argc, char **argv)
{
    (void)argc; (void)argv;
    size_t ht, hu, hl, hb; kheap_stats(&ht, &hu, &hl, &hb);
    uint32_t ram = total_ram_kb();
    kprintf("%-14s %10s %10s %10s\n", "", "total", "used", "free");
    if (ram) kprintf("%-14s %8u K\n", "System RAM:", ram);
    kprintf("%-14s %8u K %8u K %8u K  (largest free block %u K, %u blocks)\n", "Kernel heap:",
            (unsigned)(ht / 1024), (unsigned)(hu / 1024), (unsigned)((ht - hu) / 1024), (unsigned)(hl / 1024), (unsigned)hb);
    return 0;
}

static int cmd_lspci(int argc, char **argv)
{
    (void)argc; (void)argv;
    static struct pci_info list[40];
    int n = pci_enumerate(list, 40);
    for (int i = 0; i < n; i++) {
        const char *dn = pci_device_name(list[i].vendor, list[i].device);
        fg(C_LCYAN); kprintf("%02x:%02x.%x ", list[i].addr.bus, list[i].addr.device, list[i].addr.function);
        fg(C_LGRAY); kprintf("%s: ", pci_class_name(list[i].class_code, list[i].subclass));
        kprintf("%s %s ", pci_vendor_name(list[i].vendor), dn);
        fg(C_DGRAY); kprintf("[%04x:%04x]", list[i].vendor, list[i].device);
        if (list[i].irq_line && list[i].irq_line != 0xFF) kprintf(" irq %u", list[i].irq_line);
        fg_reset(); kprintf("\n");
    }
    return 0;
}

static int cmd_color(int argc, char **argv)
{
    int32_t f, b;
    if (argc < 3 || kparse_int(argv[1], &f) || kparse_int(argv[2], &b)) { err("usage: color FG BG   (0-15 each)\n"); return 1; }
    if (terminal_set_color((uint8_t)f, (uint8_t)b) != 0) { err("color: values must be 0-15\n"); return 1; }
    return 0;
}

static int cmd_font(int argc, char **argv)
{
    if (argc == 1) {
        kprintf("Active font: %s (%s)\n", font_active_name(), font_active_source() == FONT_SOURCE_CUSTOM ? "custom" : "built-in");
        kprintf("Usage: font default | font FILE | font ttf FILE\n");
        return 0;
    }
    if (strcmp(argv[1], "default") == 0) { font_use_builtin(); terminal_redraw(); return 0; }
    if (strcmp(argv[1], "ttf") == 0) {
        if (argc < 3) { err("usage: font ttf FILE\n"); return 1; }
        if (font_load_ttf(argv[2]) != 0) { err("font: cannot load or rasterize '%s'\n", argv[2]); return 1; }
        terminal_redraw(); return 0;
    }
    if (font_load(argv[1]) != 0) { err("font: cannot load '%s'\n", argv[1]); return 1; }
    terminal_redraw();
    return 0;
}

static int ls_color(const char *name, int is_dir)
{
    if (is_dir) return C_LCYAN;
    const char *dot = strrchr(name, '.');
    if (!dot) return C_LGRAY;
    if (!strcasecmp(dot, ".ELF") || !strcasecmp(dot, ".SH")) return C_LGREEN;
    if (!strcasecmp(dot, ".WAV") || !strcasecmp(dot, ".MML")) return C_LMAGENTA;
    if (!strcasecmp(dot, ".TTF") || !strcasecmp(dot, ".HTM") || !strcasecmp(dot, ".HTML")) return C_YELLOW;
    return C_LGRAY;
}

/* Turns a uid/gid into a name: the two known accounts, else the number. */
static const char *id_name(uint16_t id, char *buf, size_t n)
{
    if (id == 0) return "root";
    if (id == 1000) return "guest";
    ksnprintf(buf, n, "%u", id);
    return buf;
}

/* "drwxr-xr-x"-style string for an ext2 mode word (incl. type char). */
static void fmt_mode(uint16_t mode, char out[11])
{
    char t = '-';
    switch (mode & VFS_IFMT) {
    case VFS_IFDIR: t = 'd'; break;
    case VFS_IFLNK: t = 'l'; break;
    case VFS_IFCHR: t = 'c'; break;
    }
    out[0] = t;
    out[1] = (mode & VFS_IRUSR) ? 'r' : '-';
    out[2] = (mode & VFS_IWUSR) ? 'w' : '-';
    out[3] = (mode & VFS_IXUSR) ? ((mode & 0x800u) ? 's' : 'x') : ((mode & 0x800u) ? 'S' : '-');
    out[4] = (mode & VFS_IRGRP) ? 'r' : '-';
    out[5] = (mode & VFS_IWGRP) ? 'w' : '-';
    out[6] = (mode & VFS_IXGRP) ? ((mode & 0x400u) ? 's' : 'x') : ((mode & 0x400u) ? 'S' : '-');
    out[7] = (mode & VFS_IROTH) ? 'r' : '-';
    out[8] = (mode & VFS_IWOTH) ? 'w' : '-';
    out[9] = (mode & VFS_IXOTH) ? ((mode & VFS_ISVTX) ? 't' : 'x') : ((mode & VFS_ISVTX) ? 'T' : '-');
    out[10] = 0;
}

static void ls_row_long(const char *name, const vfs_node_t *node)
{
    char m[11]; fmt_mode(node->mode, m);
    char ob[8], gb[8];
    fg(C_DGRAY); kprintf("%s %-5s %-5s %7u %2u ", m, id_name(node->uid, ob, sizeof(ob)),
                         id_name(node->gid, gb, sizeof(gb)), node->size, node->nlink); fg_reset();
    fg(ls_color(name, node->type == VFS_NODE_DIR)); kprintf("%s", name); fg_reset();
    if (node->type == VFS_NODE_DIR) kprintf("/");
    if (node->type == VFS_NODE_LNK) {
        char tgt[VFS_PATH_MAX];
        if (vfs_readlink(name, tgt, sizeof(tgt)) > 0) kprintf(" -> %s", tgt);
    }
    kprintf("\n");
}

/* Lists one directory (cwd-relative or absolute `dir`; NULL = current). */
static int ls_dir(const char *dir, int longfmt, int all, int one)
{
    char saved[VFS_PATH_MAX]; strlcpy(saved, vfs_get_cwd(), sizeof(saved));
    if (dir && vfs_cd(dir) != 0) { err("ls: cannot access '%s'\n", dir); return 1; }
    static char names[96][VFS_NAME_MAX + 1];
    static vfs_node_t info[96];
    int n = vfs_get_entries_info(names, info, 96);
    /* dotfiles are hidden unless -a (like unix) */
    int keep = 0;
    for (int i = 0; i < n; i++) {
        if (!all && names[i][0] == '.') continue;
        if (keep != i) { strlcpy(names[keep], names[i], sizeof(names[0])); info[keep] = info[i]; }
        keep++;
    }
    n = keep;
    if (n <= 0) { if (!longfmt && !one && !terminal_capturing()) kprintf("(empty)\n"); if (dir) vfs_cd(saved); return 0; }
    /* alphabetical order (case-insensitive), like unix */
    static int idx[96];
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 1; i < n; i++) {
        int v = idx[i], j = i - 1;
        while (j >= 0 && strcasecmp(names[idx[j]], names[v]) > 0) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = v;
    }
    if (longfmt) {
        uint32_t total = 0;
        for (int i = 0; i < n; i++) if (info[i].type != VFS_NODE_DIR) total += info[i].size;
        kprintf("total %u\n", (total + 511u) / 512u);
        if (all) {
            vfs_node_t dot, ddot;
            if (vfs_stat(".", &dot) == 0) ls_row_long(".", &dot);
            if (vfs_stat("..", &ddot) == 0) ls_row_long("..", &ddot);
        }
        for (int i = 0; i < n; i++) ls_row_long(names[idx[i]], &info[idx[i]]);
    } else if (one || terminal_capturing()) {          /* one name per line when piped or with -1 */
        for (int i = 0; i < n; i++) kprintf("%s\n", names[idx[i]]);
    } else {
        int cols, rows; terminal_get_size(&cols, &rows);
        int per = cols / 14; if (per < 1) per = 1;
        for (int i = 0; i < n; i++) {
            char nm[VFS_NAME_MAX + 2]; strlcpy(nm, names[idx[i]], sizeof(nm));
            if (info[idx[i]].type == VFS_NODE_DIR) strlcat(nm, "/", sizeof(nm));
            fg(ls_color(names[idx[i]], info[idx[i]].type == VFS_NODE_DIR)); kprintf("%-13s ", nm);
            if ((i + 1) % per == 0) kprintf("\n");
        }
        if (n % per) kprintf("\n");
        fg_reset();
    }
    if (dir) vfs_cd(saved);
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    int longfmt = 0, all = 0, one = 0, ntargets = 0, rc = 0;
    const char *targets[MAX_ARGS];
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            if (strchr(argv[i], 'l')) longfmt = 1;
            if (strchr(argv[i], 'a')) all = 1;
            if (strchr(argv[i], '1')) one = 1;
        } else targets[ntargets++] = argv[i];
    }
    if (ntargets == 0) return ls_dir(NULL, longfmt, all, one);
    /* files first (listed as given), then each directory, like unix ls */
    int nfiles = 0;
    for (int i = 0; i < ntargets; i++) {
        vfs_node_t nd;
        if (vfs_stat(targets[i], &nd) != 0) { err("ls: cannot access '%s': no such file or directory\n", targets[i]); rc = 1; continue; }
        if (nd.type == VFS_NODE_DIR) continue;
        if (longfmt) ls_row_long(targets[i], &nd);
        else kprintf("%s\n", targets[i]);
        nfiles++;
    }
    int ndirs = 0;
    for (int i = 0; i < ntargets; i++) {
        vfs_node_t nd;
        if (vfs_stat(targets[i], &nd) != 0 || nd.type != VFS_NODE_DIR) continue;
        if (ntargets > 1) kprintf("%s%s:\n", (nfiles || ndirs) ? "\n" : "", targets[i]);
        if (ls_dir(targets[i], longfmt, all, one) != 0) rc = 1;
        ndirs++;
    }
    return rc;
}

static int cmd_cat(int argc, char **argv)
{
    int rc = 0, last = '\n';
    if (argc < 2) {
        if (shell_stdin) { terminal_writestring(shell_stdin); if (shell_stdin_len) last = (unsigned char)shell_stdin[shell_stdin_len - 1]; }
        else { err("usage: cat FILE...\n"); return 1; }
    }
    for (int i = 1; i < argc; i++) {
        int r = vfs_cat(argv[i]);
        if (r == VFS_ENOENT) { err("cat: %s: no such file\n", argv[i]); rc = 1; }
        else if (r == VFS_EISDIR) { err("cat: %s: is a directory\n", argv[i]); rc = 1; }
        else if (r) { err("cat: %s: read error\n", argv[i]); rc = 1; }
        else last = vfs_cat_last_char();
    }
    /* Output is passed through byte for byte (so `cat a > b` is an exact copy). Only at an interactive
     * prompt do we finish a last line that lacks its newline, so the next prompt starts on a fresh line. */
    if (!terminal_capturing() && last != '\n') terminal_putchar('\n');
    return rc;
}

static int write_result(int r, const char *cmd)
{
    if (r == 0) return 0;
    err("%s: %s\n", cmd, vfs_strerror(r));
    return 1;
}

static int cmd_touch(int argc, char **argv)
{
    if (argc < 2) { err("usage: touch FILE...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) if (!vfs_exists(argv[i])) rc |= write_result(vfs_write(argv[i], "", 0), "touch");
    return rc;
}

static int cmd_write(int argc, char **argv)
{
    if (argc < 2) { err("usage: write FILE [TEXT...]   (or: cmd | write FILE)\n"); return 1; }
    char *buf = (char *)kmalloc(LINE_MAX * 2 + (shell_stdin_len ? shell_stdin_len : 0) + 2);
    if (!buf) return 1;
    size_t n = 0;
    if (argc == 2 && shell_stdin) { memcpy(buf, shell_stdin, shell_stdin_len); n = shell_stdin_len; }
    else { for (int i = 2; i < argc; i++) { size_t l = strlen(argv[i]); memcpy(buf + n, argv[i], l); n += l; if (i + 1 < argc) buf[n++] = ' '; } buf[n++] = '\n'; }
    int r = write_result(vfs_write(argv[1], buf, (uint32_t)n), "write");
    kfree(buf);
    return r;
}

static int cmd_append(int argc, char **argv)
{
    if (argc < 3) { err("usage: append FILE TEXT...\n"); return 1; }
    size_t old = 0; char *oldtxt = shell_read_file(argv[1], &old);
    char *buf = (char *)kmalloc(old + LINE_MAX * 2);
    if (!buf) { kfree(oldtxt); return 1; }
    size_t n = 0;
    if (oldtxt) { memcpy(buf, oldtxt, old); n = old; kfree(oldtxt); }
    for (int i = 2; i < argc; i++) { size_t l = strlen(argv[i]); memcpy(buf + n, argv[i], l); n += l; if (i + 1 < argc) buf[n++] = ' '; }
    buf[n++] = '\n';
    int r = write_result(vfs_write(argv[1], buf, (uint32_t)n), "append");
    kfree(buf);
    return r;
}

static int cmd_rm(int argc, char **argv)
{
    if (argc < 2) { err("usage: rm FILE...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int r = vfs_delete(argv[i]);
        if (r == VFS_ENOENT) { err("rm: %s: no such file\n", argv[i]); rc = 1; } else if (r) { err("rm: %s: %s\n", argv[i], vfs_strerror(r)); rc = 1; }
    }
    return rc;
}

static int cmd_mv(int argc, char **argv)
{
    if (argc < 3) { err("usage: mv OLD NEW\n"); return 1; }
    int r = vfs_rename(argv[1], argv[2]);
    if (r == 0) return 0;
    if (r == VFS_ENOENT) err("mv: no such file\n");
    else if (r == VFS_EEXIST) err("mv: destination exists\n");
    else err("mv: %s\n", vfs_strerror(r));
    return 1;
}

static int cmd_cp(int argc, char **argv)
{
    if (argc < 3) { err("usage: cp SRC DST\n"); return 1; }
    size_t len; char *data = shell_read_file(argv[1], &len);
    if (!data) { err("cp: cannot read %s\n", argv[1]); return 1; }
    int r = write_result(vfs_write(argv[2], data, (uint32_t)len), "cp");
    kfree(data);
    return r;
}

static int cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) { err("usage: mkdir NAME\n"); return 1; }
    int r = vfs_mkdir(argv[1]);
    if (r == 0) return 0;
    err("mkdir: %s\n", vfs_strerror(r));
    return 1;
}

static int cmd_rmdir(int argc, char **argv)
{
    if (argc < 2) { err("usage: rmdir NAME\n"); return 1; }
    int r = vfs_rmdir(argv[1]);
    if (r == 0) return 0;
    err("rmdir: %s\n", vfs_strerror(r));
    return 1;
}

static int cmd_cd(int argc, char **argv)
{
    const char *h = shell_getenv("HOME");
    const char *d = argc < 2 ? (h && *h ? h : "/") : argv[1];
    if (vfs_cd(d) != 0) { err("cd: %s: no such directory\n", d); return 1; }
    shell_setenv("PWD", vfs_get_cwd());
    return 0;
}

static int cmd_pwd(int argc, char **argv) { (void)argc; (void)argv; kprintf("%s\n", vfs_get_cwd()); return 0; }

static int cmd_df(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint32_t total = vfs_total_bytes(), fr = vfs_free_bytes(), used = total - fr;
    int pct = total ? (int)((uint64_t)used * 100 / total) : 0;
    kprintf("%-10s %10s %10s %10s %5s\n", "Filesystem", "Size", "Used", "Avail", "Use%");
    kprintf("%-10s %9uK %9uK %9uK %4d%%  ", "STARLIGHT", total / 1024, used / 1024, fr / 1024, pct);
    kprintf("[");
    for (int i = 0; i < 20; i++) { fg(i < pct / 5 ? (pct > 85 ? C_LRED : C_LGREEN) : C_DGRAY); kprintf(i < pct / 5 ? "#" : "."); }
    fg_reset(); kprintf("]\n");
    return 0;
}

static int cmd_run(int argc, char **argv)
{
#if defined(__x86_64__)
    if (argc < 2) { err("usage: run FILE.ELF [ARGS...]\n"); return 1; }
    if (process_execute_argv(argv[1], argc - 1, argv + 1) != 0) { err("run: invalid, missing or unsupported ELF64 app\n"); return 1; }
    return process_exit_status();
#else
    (void)argc; (void)argv; err("run: user-mode ELF apps require the x86_64 build\n"); return 1;
#endif
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("Rebooting...\n");
    uint8_t st;
    do { __asm__ volatile("inb %1, %0" : "=a"(st) : "Nd"((uint16_t)0x64)); } while (st & 2);
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    for (;;) __asm__ volatile("cli; hlt");
    return 0;
}

static int cmd_poweroff(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("Powering off...\n");
    audio_stop();
    __asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));     /* QEMU ACPI */
    __asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));    /* old Bochs/QEMU */
    __asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x3400), "Nd"((uint16_t)0x4004));    /* VirtualBox */
    err("No ACPI shutdown on this machine. It is safe to power off now.\n");
    return 1;
}

static int cmd_crash(int argc, char **argv) { (void)argc; (void)argv; kprintf("Triggering INT3...\n"); __asm__ volatile("int3"); return 0; }

static int cmd_irqdebug(int argc, char **argv)
{
    (void)argc; (void)argv;
    int mx, my, lb, rb; mouse_get_state(&mx, &my, &lb, &rb);
    kprintf("Keyboard IRQs: %u\nMouse IRQs: %u  packets: %u\nMouse: x=%d y=%d buttons=%u\nClicks: L=%u R=%u\nTimer ticks: %u\n",
            keyboard_irq_count, mouse_irq_count, mouse_get_packet_count(), mx, my, (lb ? 1u : 0u) | (rb ? 2u : 0u),
            mouse_get_left_click_count(), mouse_get_right_click_count(), timer_get_ticks());
    return 0;
}

static int cmd_notify(int argc, char **argv)
{
    if (argc < 2) { err("usage: notify TEXT...\n"); return 1; }
    char msg[72]; msg[0] = 0;
    for (int i = 1; i < argc; i++) { strlcat(msg, argv[i], sizeof(msg)); if (i + 1 < argc) strlcat(msg, " ", sizeof(msg)); }
    gui_toast(msg);
    audio_sfx(SFX_NOTIFY);
    return 0;
}

static int cmd_banner(int argc, char **argv)
{
    if (argc < 2) { err("usage: banner TEXT\n"); return 1; }
    size_t len = 0;
    for (int i = 1; i < argc; i++) len += strlen(argv[i]) + 1;
    len--;
    kprintf("+"); for (size_t i = 0; i < len + 2; i++) kprintf("-"); kprintf("+\n| ");
    for (int i = 1; i < argc; i++) kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
    kprintf(" |\n+"); for (size_t i = 0; i < len + 2; i++) kprintf("-"); kprintf("+\n");
    return 0;
}

/* ---- calc: recursive-descent integer expressions ---- */
static int64_t calc_expr(void);
static void cskip(void) { while (*cp == ' ') cp++; }
static int64_t calc_atom(void)
{
    cskip();
    if (*cp == '(') { cp++; int64_t v = calc_expr(); cskip(); if (*cp == ')') cp++; else calc_fail = 1; return v; }
    if (*cp == '-') { cp++; return -calc_atom(); }
    if (*cp == '+') { cp++; return calc_atom(); }
    if (*cp == '~') { cp++; return ~calc_atom(); }
    if (kisdigit(*cp)) { const char *e; uint32_t v = kstrtoul(cp, &e, 0); cp = e; return v; }
    if (kisalpha(*cp) || *cp == '_') {                      /* variable name: its numeric value (0 if unset) */
        char nm[20]; size_t n = 0;
        while ((kisalnum(*cp) || *cp == '_') && n < sizeof(nm) - 1) nm[n++] = *cp++;
        nm[n] = 0;
        const char *v = shell_getenv(nm); int32_t iv = 0;
        if (v && *v) kparse_int(v, &iv);
        return iv;
    }
    calc_fail = 1; return 0;
}
static int64_t calc_term(void)
{
    int64_t v = calc_atom();
    for (;;) {
        cskip();
        if (*cp == '*') { cp++; v *= calc_atom(); }
        else if (*cp == '/') { cp++; int64_t d = calc_atom(); if (!d) { calc_fail = 2; return 0; } v /= d; }
        else if (*cp == '%') { cp++; int64_t d = calc_atom(); if (!d) { calc_fail = 2; return 0; } v %= d; }
        else return v;
    }
}
static int64_t calc_expr(void)
{
    int64_t v = calc_term();
    for (;;) {
        cskip();
        if (*cp == '+') { cp++; v += calc_term(); }
        else if (*cp == '-') { cp++; v -= calc_term(); }
        else if (*cp == '<' && cp[1] == '<') { cp += 2; v <<= calc_term(); }
        else if (*cp == '>' && cp[1] == '>') { cp += 2; v >>= calc_term(); }
        else if (*cp == '&') { cp++; v &= calc_term(); }
        else if (*cp == '|') { cp++; v |= calc_term(); }
        else if (*cp == '^') { cp++; v ^= calc_term(); }
        else return v;
    }
}

static int cmd_calc(int argc, char **argv)
{
    char expr[LINE_MAX]; expr[0] = 0;
    for (int i = 1; i < argc; i++) { strlcat(expr, argv[i], sizeof(expr)); strlcat(expr, " ", sizeof(expr)); }
    if (!expr[0]) { err("usage: calc EXPRESSION   e.g. calc (3+4)*0x10 / 7   (quote | & < >)\n"); return 1; }
    cp = expr; calc_fail = 0;
    int64_t v = calc_expr(); cskip();
    if (calc_fail == 2) { err("calc: division by zero\n"); return 1; }
    if (calc_fail || *cp) { err("calc: syntax error near '%s'\n", cp); return 1; }
    kprintf("%lld  (0x%llx)\n", (long long)v, (unsigned long long)v);
    return 0;
}

static int cmd_hostname(int argc, char **argv)
{
    if (argc < 2) { kprintf("%s\n", net_get_config()->hostname); return 0; }
    net_set_hostname(argv[1]);
    return 0;
}

static int cmd_ticks(int argc, char **argv) { (void)argc; (void)argv; kprintf("PIT ticks: %u\n", timer_get_ticks()); return 0; }

static int cmd_about(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("StarlightOS %s -- %s\n", STARLIGHTOS_VERSION, STARLIGHTOS_BUILD_NAME);
    kprintf("A small x86_64 hobby OS: ring-3 ELF apps, an ext2 filesystem, TTF fonts, and a\n");
    kprintf("TrueColor desktop (32-bit VBE), with a TCP/IP stack (ARP, ICMP, UDP, TCP, DHCP,\n");
    kprintf("DNS, NTP, HTTP, HTTPS via TLS 1.2/1.3, telnet) and an AC'97 software mixer with\n");
    kprintf("an MML sequencer.\n");
    return 0;
}

/* ---- table ---- */
extern const struct command text_commands[];
extern const int text_command_count;
extern const struct command net_commands[];
extern const int net_command_count;
extern const struct command sound_commands[];
extern const int sound_command_count;
extern const struct command unix_commands[];
extern const int unix_command_count;
extern const struct command posix_commands[];
extern const int posix_command_count;

static const struct command builtins[] = {
    { "help",     "Shell",  "help [COMMAND|GROUP]", "list commands, or show help for one", cmd_help },
    { "clear",    "Shell",  "clear", "clear the screen", cmd_clear },
    { "echo",     "Shell",  "echo [-n] TEXT...", "print text", cmd_echo },
    { "history",  "Shell",  "history", "show command history", cmd_history },
    { "alias",    "Shell",  "alias [NAME=TEXT]", "define or list aliases", cmd_alias },
    { "unalias",  "Shell",  "unalias NAME", "remove an alias", cmd_unalias },
    { "set",      "Shell",  "set [NAME=VALUE]", "set or list variables ($NAME expands)", cmd_set },
    { "unset",    "Shell",  "unset NAME", "remove a variable", cmd_unset },
    { "source",   "Shell",  "source FILE [ARGS]", "run a script; ARGS become $1 $2 ...", cmd_source },
    { ".",        "Shell",  ". FILE [ARGS]", "same as source", cmd_source },
    { "sh",       "Shell",  "sh FILE [ARGS]", "run a script of shell commands", cmd_source },
    { "which",    "Shell",  "which NAME...", "show what a name refers to (searches $PATH)", cmd_which },
    { "type",     "Shell",  "type NAME...", "describe how each NAME would be run", cmd_type },
    { "true",     "Shell",  "true", "do nothing, successfully", cmd_true },
    { "false",    "Shell",  "false", "do nothing, unsuccessfully", cmd_false },
    { ":",        "Shell",  ":", "do nothing, successfully", cmd_true },
    { "break",    "Shell",  "break", "leave the enclosing loop", cmd_break },
    { "continue", "Shell",  "continue", "next iteration of the enclosing loop", cmd_break },
    { "return",   "Shell",  "return [N]", "end the running script with status N", cmd_exit },
    { "shift",    "Shell",  "shift [N]", "drop the first N script arguments ($1..)", cmd_shift },
    { "eval",     "Shell",  "eval ARGS...", "run the arguments as a command line", cmd_eval },
    { "read",     "Shell",  "read [-p PROMPT] [NAME...]", "read a line into variables (REPLY by default)", cmd_read },
    { "exit",     "Shell",  "exit [N]", "end a script (at the prompt: how to close the terminal)", cmd_exit },
    { "sleep",    "Shell",  "sleep SECONDS", "wait (Ctrl-C aborts)", cmd_sleep },
    { "notify",   "Shell",  "notify TEXT", "show a desktop notification", cmd_notify },
    { "banner",   "Shell",  "banner TEXT", "print text in a box", cmd_banner },
    { "calc",     "Shell",  "calc EXPR", "integer calculator: + - * / % << >> & | ^ ( ) 0x..", cmd_calc },
    { "about",    "System", "about", "about StarlightOS", cmd_about },
    { "neofetch", "System", "neofetch", "system summary with logo", cmd_neofetch },
    { "sysinfo",  "System", "sysinfo", "system summary with logo (alias of neofetch)", cmd_neofetch },
    { "uname",    "System", "uname [-a]", "kernel info", cmd_uname },
    { "ver",      "System", "ver", "version", cmd_ver },
    { "uptime",   "System", "uptime", "time since boot", cmd_uptime },
    { "date",     "System", "date", "current date and time", cmd_date },
    { "time",     "System", "time", "current RTC time", cmd_time },
    { "tz",       "System", "tz [HOURS]", "show/set the timezone offset from UTC", cmd_tz },
    { "cal",      "System", "cal [MONTH [YEAR]]", "calendar", cmd_cal },
    { "cpuinfo",  "System", "cpuinfo", "CPU model and features", cmd_cpuinfo },
    { "mem",      "System", "mem", "memory and heap usage", cmd_mem },
    { "free",     "System", "free", "memory and heap usage", cmd_mem },
    { "lspci",    "System", "lspci", "list PCI devices", cmd_lspci },
    { "hostname", "System", "hostname [NAME]", "show or set the hostname", cmd_hostname },
    { "color",    "System", "color FG BG", "set terminal colours (0-15)", cmd_color },
    { "font",     "System", "font [default|FILE|ttf FILE]", "show or switch the font", cmd_font },
    { "run",      "System", "run FILE.ELF [ARGS]", "run a 64-bit user-mode app (newlib C programs get argc/argv)", cmd_run },
    { "ticks",    "System", "ticks", "PIT tick counter", cmd_ticks },
    { "irqdebug", "System", "irqdebug", "keyboard/mouse IRQ counters", cmd_irqdebug },
    { "crash",    "System", "crash", "trigger a CPU breakpoint exception", cmd_crash },
    { "reboot",   "System", "reboot", "restart the machine", cmd_reboot },
    { "poweroff", "System", "poweroff", "shut down (QEMU/ACPI)", cmd_poweroff },
    { "ls",       "Files",  "ls [-1al] [FILE|DIR...]", "list files (one per line when piped)", cmd_ls },
    { "cat",      "Files",  "cat FILE...", "print files (or stdin)", cmd_cat },
    { "touch",    "Files",  "touch FILE...", "create empty files", cmd_touch },
    { "write",    "Files",  "write FILE [TEXT]", "write text (or piped input) to a file", cmd_write },
    { "append",   "Files",  "append FILE TEXT", "append a line to a file", cmd_append },
    { "rm",       "Files",  "rm FILE...", "delete files", cmd_rm },
    { "mv",       "Files",  "mv OLD NEW", "rename a file", cmd_mv },
    { "cp",       "Files",  "cp SRC DST", "copy a file", cmd_cp },
    { "mkdir",    "Files",  "mkdir DIR", "create a directory", cmd_mkdir },
    { "rmdir",    "Files",  "rmdir DIR", "remove an empty directory", cmd_rmdir },
    { "cd",       "Files",  "cd [DIR]", "change directory", cmd_cd },
    { "pwd",      "Files",  "pwd", "print working directory", cmd_pwd },
    { "df",       "Files",  "df", "disk usage", cmd_df },
};

void shell_initialize(void)
{
    table_n = 0;
    shell_register(builtins, (int)(sizeof(builtins) / sizeof(builtins[0])));
    shell_register(text_commands, text_command_count);
    shell_register(net_commands, net_command_count);
    shell_register(sound_commands, sound_command_count);
    shell_register(unix_commands, unix_command_count);
    shell_register(posix_commands, posix_command_count);

    shell_pid_value = 100 + (int)(timer_get_ticks() % 900u);
    shell_setenv("USER", "guest");
    shell_setenv("HOME", "/home/guest");
    shell_setenv("SHELL", "starlightsh");
    shell_setenv("PATH", "/bin:/usr/bin");
    shell_setenv("PWD", "/");
    alias_set("ll", "ls -l"); alias_set("cls", "clear"); alias_set("..", "cd ..");
    alias_set("curl", "fetch"); alias_set("wget", "fetch -O"); alias_set("ifconfig", "ip");
    alias_set("host", "nslookup"); alias_set("dir", "ls -l"); alias_set("md", "mkdir"); alias_set("del", "rm");

    /* hostname from /etc/hostname (falls back to the default). */
    if (vfs_exists("/etc/hostname")) {
        size_t hl; char *hn = shell_read_file("/etc/hostname", &hl);
        if (hn) {
            char *e = hn; while (*e && *e != '\n' && *e != '\r') e++;
            *e = 0;
            if (*hn) net_set_hostname(hn);
            kfree(hn);
        }
    }

    fg(C_LCYAN);
    kprintf(" ___  _               _  _        _     _     ___   ___\n/ __|| |_  __ _  _ _ | |(_) __ _ | |_  | |_  / _ \\ / __|\n\\__ \\|  _|/ _` || '_|| || |/ _` || ' \\ |  _|| (_) |\\__ \\\n|___/ \\__|\\__,_||_|  |_||_|\\__, ||_||_| \\__| \\___/ |___/\n                           |___/");
    fg(C_WHITE); kprintf(" %s\n", STARLIGHTOS_VERSION);
    fg(C_DGRAY); kprintf("Type 'help' for commands. Tab completes, PgUp/PgDn scrolls, Ctrl-C aborts.\n\n");
    fg_reset();

    /* Unix login environment: /etc/profile, then ~/.profile, then the
     * legacy AUTOEXEC.SH that earlier releases always ran at boot. */
    if (vfs_exists("/etc/profile")) shell_run_script("/etc/profile");
    {
        char prof[VFS_PATH_MAX];
        ksnprintf(prof, sizeof(prof), "%s/.profile", shell_getenv("HOME"));
        if (vfs_exists(prof)) shell_run_script(prof);
    }
    if (vfs_exists("AUTOEXEC.SH")) shell_run_script("AUTOEXEC.SH");
    print_prompt();
}

/* Called once after the desktop is up: runs AUTOEXEC.SH if present. */
void shell_run_autoexec(void)
{
    if (vfs_exists("AUTOEXEC.SH")) { shell_run_script("AUTOEXEC.SH"); }
}
