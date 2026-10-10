/*
 * cmd_text.c -- text/file filters that work in pipelines:
 *   head tail grep wc sort uniq more hexdump tree seq find
 * Each reads shell_stdin when no file is given.
 */
#include "shell.h"
#include "vfs.h"
#include "terminal.h"
#include "kstring.h"
#include "kprintf.h"
#include "kheap.h"
#include "ksched.h"
#include "keyboard.h"

static void errmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void errmsg(const char *fmt, ...)
{
    uint8_t old = terminal_get_color();
    terminal_set_color(12, (uint8_t)(old >> 4));
    va_list ap; va_start(ap, fmt); kvprintf_err(fmt, ap); va_end(ap);
    terminal_set_color((uint8_t)(old & 15), (uint8_t)(old >> 4));
}

/* Gets the input text: first non-option arg is a file, else stdin. Caller frees *owned if non-NULL. */
static const char *get_input(int argc, char **argv, int first_arg, size_t *len, char **owned, const char *cmd)
{
    *owned = NULL;
    for (int i = first_arg; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) continue;
        char *t = shell_read_file(argv[i], len);
        if (!t) { errmsg("%s: cannot read '%s'\n", cmd, argv[i]); return NULL; }
        *owned = t;
        return t;
    }
    if (shell_stdin) { *len = shell_stdin_len; return shell_stdin; }
    errmsg("%s: no input (give a file or pipe something in)\n", cmd);
    return NULL;
}

struct lines { const char **p; size_t *n; int count; };

static int split_lines(const char *text, size_t len, struct lines *L, int max)
{
    L->count = 0;
    size_t i = 0;
    while (i < len && L->count < max) {
        size_t s = i;
        while (i < len && text[i] != '\n') i++;
        size_t e = i;
        if (e > s && text[e - 1] == '\r') e--;
        L->p[L->count] = text + s; L->n[L->count] = e - s; L->count++;
        if (i < len) i++;
    }
    return L->count;
}

#define MAX_LINES 4096
static const char *line_ptr[MAX_LINES];
static size_t line_len[MAX_LINES];

static int parse_count(int argc, char **argv, int def)
{
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') return (int)kstrtoul(argv[i] + 1, NULL, 10);
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) return (int)kstrtoul(argv[i + 1], NULL, 10);
    }
    return def;
}

static int cmd_head_tail(int argc, char **argv, int is_tail)
{
    int n = parse_count(argc, argv, 10);
    /* build a filtered argv without the count option so get_input finds the file */
    char *fa[16]; int fc = 0; fa[fc++] = argv[0];
    for (int i = 1; i < argc && fc < 15; i++) {
        if (argv[i][0] == '-' && (argv[i][1] >= '0' && argv[i][1] <= '9')) continue;
        if (strcmp(argv[i], "-n") == 0) { i++; continue; }
        fa[fc++] = argv[i];
    }
    size_t len; char *owned;
    const char *text = get_input(fc, fa, 1, &len, &owned, argv[0]);
    if (!text) return 1;
    struct lines L = { line_ptr, line_len, 0 };
    split_lines(text, len, &L, MAX_LINES);
    int from = 0, to = L.count;
    if (is_tail) from = L.count > n ? L.count - n : 0; else to = n < L.count ? n : L.count;
    for (int i = from; i < to; i++) { for (size_t k = 0; k < line_len[i]; k++) terminal_putchar(line_ptr[i][k]); terminal_putchar('\n'); }
    kfree(owned);
    return 0;
}
static int cmd_head(int argc, char **argv) { return cmd_head_tail(argc, argv, 0); }
static int cmd_tail(int argc, char **argv) { return cmd_head_tail(argc, argv, 1); }

static int cmd_grep(int argc, char **argv)
{
    int icase = 0, number = 0, invert = 0, count_only = 0;
    const char *pat = NULL; int file_idx = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] && !pat) {
            for (const char *f = argv[i] + 1; *f; f++) { if (*f == 'i') icase = 1; else if (*f == 'n') number = 1; else if (*f == 'v') invert = 1; else if (*f == 'c') count_only = 1; }
        } else if (!pat) pat = argv[i];
        else if (!file_idx) file_idx = i;
    }
    if (!pat) { errmsg("usage: grep [-i] [-n] [-v] [-c] PATTERN [FILE]\n"); return 2; }
    char *fa[3] = { argv[0], file_idx ? argv[file_idx] : NULL, NULL };
    size_t len; char *owned;
    const char *text = get_input(file_idx ? 2 : 1, fa, 1, &len, &owned, "grep");
    if (!text) return 2;
    struct lines L = { line_ptr, line_len, 0 };
    split_lines(text, len, &L, MAX_LINES);
    int hits = 0;
    size_t pl = strlen(pat);
    for (int i = 0; i < L.count; i++) {
        int found = 0;
        for (size_t s = 0; s + pl <= line_len[i] && !found; s++)
            found = (icase ? strncasecmp(line_ptr[i] + s, pat, pl) : strncmp(line_ptr[i] + s, pat, pl)) == 0;
        if (pl == 0) found = 1;
        if (found == invert) continue;
        hits++;
        if (count_only) continue;
        if (number) kprintf("%d:", i + 1);
        for (size_t k = 0; k < line_len[i]; k++) terminal_putchar(line_ptr[i][k]);
        terminal_putchar('\n');
    }
    if (count_only) kprintf("%d\n", hits);
    kfree(owned);
    return hits ? 0 : 1;
}

static int cmd_wc(int argc, char **argv)
{
    size_t len; char *owned;
    const char *text = get_input(argc, argv, 1, &len, &owned, "wc");
    if (!text) return 1;
    int lines = 0, words = 0, inw = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') lines++;
        if (kisspace(text[i])) inw = 0; else if (!inw) { inw = 1; words++; }
    }
    int fl = 0, fw = 0, fc = 0;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-l")) fl = 1; else if (!strcmp(argv[i], "-w")) fw = 1; else if (!strcmp(argv[i], "-c")) fc = 1; }
    if (!fl && !fw && !fc) kprintf("%7d %7d %7u\n", lines, words, (unsigned)len);
    else { if (fl) kprintf("%d ", lines); if (fw) kprintf("%d ", words); if (fc) kprintf("%u ", (unsigned)len); kprintf("\n"); }
    kfree(owned);
    return 0;
}

static int sort_reverse, sort_numeric, sort_icase;

static int cmp_idx(int x, int y)
{
    int r;
    if (sort_numeric) {
        int64_t vx = (int64_t)kstrtoul(line_ptr[x], NULL, 10), vy = (int64_t)kstrtoul(line_ptr[y], NULL, 10);
        r = vx < vy ? -1 : vx > vy ? 1 : 0;
    } else {
        size_t m = line_len[x] < line_len[y] ? line_len[x] : line_len[y];
        r = sort_icase ? strncasecmp(line_ptr[x], line_ptr[y], m) : strncmp(line_ptr[x], line_ptr[y], m);
        if (!r) r = line_len[x] < line_len[y] ? -1 : line_len[x] > line_len[y] ? 1 : 0;
    }
    return sort_reverse ? -r : r;
}

static int cmd_sort(int argc, char **argv)
{
    sort_reverse = sort_numeric = sort_icase = 0;
    for (int i = 1; i < argc; i++) if (argv[i][0] == '-') for (const char *f = argv[i] + 1; *f; f++) { if (*f == 'r') sort_reverse = 1; else if (*f == 'n') sort_numeric = 1; else if (*f == 'f') sort_icase = 1; }
    size_t len; char *owned;
    const char *text = get_input(argc, argv, 1, &len, &owned, "sort");
    if (!text) return 1;
    struct lines L = { line_ptr, line_len, 0 };
    split_lines(text, len, &L, MAX_LINES);
    static int idx[MAX_LINES];
    for (int i = 0; i < L.count; i++) idx[i] = i;
    for (int i = 1; i < L.count; i++) {                         /* insertion sort: stable, fine for <4096 lines */
        int v = idx[i], j = i - 1;
        while (j >= 0 && cmp_idx(idx[j], v) > 0) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = v;
    }
    for (int i = 0; i < L.count; i++) { for (size_t k = 0; k < line_len[idx[i]]; k++) terminal_putchar(line_ptr[idx[i]][k]); terminal_putchar('\n'); }
    kfree(owned);
    return 0;
}

static int cmd_uniq(int argc, char **argv)
{
    int count = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-c")) count = 1;
    size_t len; char *owned;
    const char *text = get_input(argc, argv, 1, &len, &owned, "uniq");
    if (!text) return 1;
    struct lines L = { line_ptr, line_len, 0 };
    split_lines(text, len, &L, MAX_LINES);
    for (int i = 0; i < L.count; ) {
        int j = i + 1;
        while (j < L.count && line_len[j] == line_len[i] && strncmp(line_ptr[j], line_ptr[i], line_len[i]) == 0) j++;
        if (count) kprintf("%7d ", j - i);
        for (size_t k = 0; k < line_len[i]; k++) terminal_putchar(line_ptr[i][k]);
        terminal_putchar('\n');
        i = j;
    }
    kfree(owned);
    return 0;
}

static int cmd_more(int argc, char **argv)
{
    size_t len; char *owned;
    const char *text = get_input(argc, argv, 1, &len, &owned, "more");
    if (!text) return 1;
    int cols, rows; terminal_get_size(&cols, &rows);
    int shown = 0, col = 0;
    for (size_t i = 0; i < len; i++) {
        terminal_putchar(text[i]);
        if (text[i] == '\n' || ++col >= cols) { if (text[i] == '\n') col = 0; else col = 0; shown++; }
        if (shown >= rows - 2 && i + 1 < len) {
            uint8_t old = terminal_get_color();
            terminal_set_color(0, 7); terminal_writestring("-- more (space: page, enter: line, q: quit) --"); terminal_set_color((uint8_t)(old & 15), (uint8_t)(old >> 4));
            int c = shell_getc(0);
            terminal_writestring("\r");
            int r2, c2; terminal_get_cursor(&r2, &c2);
            terminal_set_cursor(r2, 0);
            for (int k = 0; k < 48; k++) terminal_putchar(' ');
            terminal_set_cursor(r2, 0);
            if (c < 0 || c == 'q' || c == 'Q') { terminal_putchar('\n'); break; }
            shown = (c == '\n') ? rows - 3 : 0;
        }
    }
    kfree(owned);
    return 0;
}

static int cmd_hexdump(int argc, char **argv)
{
    size_t len; char *owned;
    const char *text = get_input(argc, argv, 1, &len, &owned, "hexdump");
    if (!text) return 1;
    size_t limit = len < 4096 ? len : 4096;
    for (size_t off = 0; off < limit && !kabort_requested(); off += 16) {
        kprintf("%08x  ", (unsigned)off);
        for (size_t i = 0; i < 16; i++) { if (off + i < len) kprintf("%02x ", (uint8_t)text[off + i]); else kprintf("   "); if (i == 7) kprintf(" "); }
        kprintf(" |");
        for (size_t i = 0; i < 16 && off + i < len; i++) { char c = text[off + i]; terminal_putchar(c >= 32 && c < 127 ? c : '.'); }
        kprintf("|\n");
        if (off % 256 == 240) kyield();
    }
    if (len > limit) kprintf("... (%u more bytes)\n", (unsigned)(len - limit));
    kfree(owned);
    return 0;
}

static int cmd_seq(int argc, char **argv)
{
    int32_t a = 1, b = 0;
    if (argc == 2 && kparse_int(argv[1], &b) == 0) a = 1;
    else if (argc >= 3 && kparse_int(argv[1], &a) == 0 && kparse_int(argv[2], &b) == 0) { }
    else { errmsg("usage: seq LAST | seq FIRST LAST\n"); return 1; }
    for (int32_t i = a; i <= b && !kabort_requested(); i++) kprintf("%d\n", i);
    return 0;
}

static void tree_walk(int depth, int *files, int *dirs, const char *prefix)
{
    static char names[64][VFS_NAME_MAX + 1]; static uint8_t isdir[64];
    int n = vfs_get_entries(names, isdir, 64);
    char (*local)[VFS_NAME_MAX + 1] = (char (*)[VFS_NAME_MAX + 1])kmalloc((size_t)n * (VFS_NAME_MAX + 1) + 1);
    uint8_t *ld = (uint8_t *)kmalloc((size_t)n + 1);
    if (!local || !ld) { kfree(local); kfree(ld); return; }
    memcpy(local, names, (size_t)n * (VFS_NAME_MAX + 1)); memcpy(ld, isdir, (size_t)n);
    for (int i = 0; i < n; i++) {
        int last = i == n - 1;
        kprintf("%s%s", prefix, last ? "`-- " : "|-- ");
        if (ld[i]) { terminal_set_color(11, 0); kprintf("%s/\n", local[i]); terminal_set_color(7, 0); (*dirs)++;
            if (depth < 4 && vfs_cd(local[i]) == 0) {
                char np[64]; ksnprintf(np, sizeof(np), "%s%s", prefix, last ? "    " : "|   ");
                tree_walk(depth + 1, files, dirs, np);
                vfs_cd("..");
            }
        } else { kprintf("%s\n", local[i]); (*files)++; }
    }
    kfree(local); kfree(ld);
}

static int cmd_tree(int argc, char **argv)
{
    (void)argc; (void)argv;
    int files = 0, dirs = 0;
    kprintf("%s\n", vfs_get_cwd());
    tree_walk(0, &files, &dirs, "");
    kprintf("\n%d directories, %d files\n", dirs, files);
    return 0;
}

static int find_walk(const char *pat, int depth, char *path, int *hits)
{
    static char names[48][VFS_NAME_MAX + 1]; static uint8_t isdir[48];
    int n = vfs_get_entries(names, isdir, 48);
    char (*local)[VFS_NAME_MAX + 1] = (char (*)[VFS_NAME_MAX + 1])kmalloc((size_t)n * (VFS_NAME_MAX + 1) + 1);
    uint8_t *ld = (uint8_t *)kmalloc((size_t)n + 1);
    if (!local || !ld) { kfree(local); kfree(ld); return 0; }
    memcpy(local, names, (size_t)n * (VFS_NAME_MAX + 1)); memcpy(ld, isdir, (size_t)n);
    size_t plen = strlen(path);
    for (int i = 0; i < n && !kabort_requested(); i++) {
        if (!*pat || strcasestr(local[i], pat)) { kprintf("%s%s%s\n", path, local[i], ld[i] ? "/" : ""); (*hits)++; }
        if (ld[i] && depth < 4 && vfs_cd(local[i]) == 0) {
            ksnprintf(path + plen, 64 - plen, "%s/", local[i]);
            find_walk(pat, depth + 1, path, hits);
            path[plen] = 0;
            vfs_cd("..");
        }
    }
    kfree(local); kfree(ld);
    return 0;
}

static int cmd_find(int argc, char **argv)
{
    char path[64] = "";
    int hits = 0;
    find_walk(argc > 1 ? argv[1] : "", 0, path, &hits);
    return hits ? 0 : 1;
}

const struct command text_commands[] = {
    { "head",    "Text", "head [-n N] [FILE]", "first N lines (default 10)", cmd_head },
    { "tail",    "Text", "tail [-n N] [FILE]", "last N lines (default 10)", cmd_tail },
    { "grep",    "Text", "grep [-invc] PATTERN [FILE]", "print lines containing PATTERN", cmd_grep },
    { "wc",      "Text", "wc [-lwc] [FILE]", "count lines, words, bytes", cmd_wc },
    { "sort",    "Text", "sort [-rnf] [FILE]", "sort lines", cmd_sort },
    { "uniq",    "Text", "uniq [-c] [FILE]", "collapse adjacent duplicate lines", cmd_uniq },
    { "more",    "Text", "more [FILE]", "page through text", cmd_more },
    { "hexdump", "Text", "hexdump [FILE]", "hex + ASCII dump", cmd_hexdump },
    { "xxd",     "Text", "xxd [FILE]", "hex + ASCII dump", cmd_hexdump },
    { "seq",     "Text", "seq [FIRST] LAST", "print a sequence of numbers", cmd_seq },
    { "tree",    "Files", "tree", "directory tree of the current directory", cmd_tree },
    { "find",    "Files", "find [NAME]", "search file names recursively", cmd_find },
};
const int text_command_count = (int)(sizeof(text_commands) / sizeof(text_commands[0]));
