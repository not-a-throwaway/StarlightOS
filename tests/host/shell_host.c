/*
 * shell_host.c -- runs the REAL StarlightOS shell, builtins, VFS, ext2, devfs
 * and procfs on the host.  Only the hardware layer is stubbed:
 *   disk_read/disk_write -> a plain file (a copy of disk.img)
 *   terminal_*           -> stdout, with the same capture semantics as kernel.c
 *   net/audio/pci/gui    -> inert stubs
 *
 * Usage:  shell_host DISK.IMG < script
 * Each stdin line is one shell line.  After it runs, the harness prints
 * "\x1e<exit status>\n" so a test driver can split the output per command.
 * A line starting with '@' is a harness directive:  @tick N  advances the clock.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"
#include "vfs.h"
#include "ksched.h"
#include "rtc.h"
#include "net.h"
#include "pci.h"
#include "audio.h"
#include "font.h"
#include "kheap.h"
#include "terminal.h"

/* ---- disk ---- */
static FILE *g_disk;
int disk_read(uint32_t lba, uint8_t count, void *buf)
{
    if (fseek(g_disk, (long)lba * 512, SEEK_SET)) return -1;
    return fread(buf, 512, count ? count : 256, g_disk) == (size_t)(count ? count : 256) ? 0 : -1;
}
int disk_write(uint32_t lba, uint8_t count, const void *buf)
{
    if (fseek(g_disk, (long)lba * 512, SEEK_SET)) return -1;
    size_t n = count ? count : 256;
    int ok = fwrite(buf, 512, n, g_disk) == n ? 0 : -1;
    fflush(g_disk);
    return ok;
}

/* ---- terminal with capture ---- */
static char *cap_buf; static size_t cap_cap, cap_len;
void terminal_capture_begin(char *b, size_t c) { cap_buf = b; cap_cap = c; cap_len = 0; if (c) b[0] = 0; }
size_t terminal_capture_end(void) { size_t n = cap_len; cap_buf = NULL; cap_cap = cap_len = 0; return n; }
int terminal_capturing(void) { return cap_buf != NULL; }
static int g_abort;
void kabort_raise(void) { g_abort = 1; }
void kabort_clear(void) { g_abort = 0; }
int kabort_requested(void) { return g_abort; }
void terminal_putchar(char c)
{
    if (cap_buf) {
        if (c != '\r' && cap_len + 1 < cap_cap) { cap_buf[cap_len++] = c; cap_buf[cap_len] = 0; }
        else if (c != '\r') kabort_raise();
        return;
    }
    if (c != '\r') putchar(c);
}
void terminal_capture_save(struct terminal_capture_state *st) { st->buf = cap_buf; st->cap = cap_cap; st->len = cap_len; }
void terminal_capture_restore(const struct terminal_capture_state *st) { cap_buf = st->buf; cap_cap = st->cap; cap_len = st->len; }
void terminal_write_direct(const char *t) { fputs(t, stdout); }
void terminal_writestring(const char *s) { while (*s) terminal_putchar(*s++); }
static uint8_t g_color = 7;
uint8_t terminal_get_color(void) { return g_color; }
int terminal_set_color(uint8_t f, uint8_t b) { g_color = (uint8_t)(f | (b << 4)); return 0; }
void terminal_clear(void) {}
void terminal_redraw(void) {}
void terminal_scroll_view(int d) { (void)d; }
void terminal_get_size(int *c, int *r) { *c = 80; *r = 32; }
void terminal_get_cursor(int *r, int *c) { *r = 0; *c = 0; }
void terminal_set_cursor(int r, int c) { (void)r; (void)c; }
uint32_t terminal_scroll_total(void) { return 0; }

/* ---- scheduler / time ---- */
static int g_in_cmd;
void kyield(void) {}
void kidle(void) {}
void ksleep_ms(uint32_t ms) { (void)ms; }
int ksched_in_command(void) { return g_in_cmd; }
void ksched_set_in_command(int v) { g_in_cmd = v; }
volatile uint32_t keyboard_irq_count, mouse_irq_count;
int keyboard_getchar_nonblocking(void) { return -1; }
void keyboard_flush_queue(void) {}
uint32_t timer_get_ticks(void) { return 12345; }
void rtc_read(struct rtc_time *o) { o->second = 5; o->minute = 4; o->hour = 3; o->day = 2; o->month = 1; o->year = 2026; }
void krand_bytes(void *b, size_t n) { unsigned char *p = b; for (size_t i = 0; i < n; i++) p[i] = (unsigned char)rand(); }
int multiboot_get_memory(uint32_t *l, uint32_t *u) { *l = 640; *u = 261120; return 0; }

/* ---- net / pci / audio / gui / font / mouse / process / vga ---- */
const struct command net_commands[1]; const int net_command_count = 0;
const struct command sound_commands[1]; const int sound_command_count = 0;
static struct net_config g_nc;
int net_configured(void) { return 0; }
const struct net_config *net_get_config(void) { return &g_nc; }
void net_fmt_ip(char *o, const uint8_t ip[4]) { sprintf(o, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]); }
void net_set_hostname(const char *n) { (void)n; }
int pci_enumerate(struct pci_info *o, int m) { (void)o; (void)m; return 0; }
const char *pci_class_name(uint8_t c, uint8_t s) { (void)c; (void)s; return "?"; }
const char *pci_vendor_name(uint16_t v) { (void)v; return "?"; }
const char *pci_device_name(uint16_t v, uint16_t d) { (void)v; (void)d; return "?"; }
void audio_get_status(struct audio_status *s) { memset(s, 0, sizeof *s); }
void audio_sfx(enum audio_sfx w) { (void)w; }
void audio_stop(void) {}
void gui_toast(const char *t) { (void)t; }
enum font_source font_active_source(void) { return FONT_SOURCE_BUILTIN; }
const char *font_active_name(void) { return "builtin"; }
int font_load(const char *f) { (void)f; return -1; }
int font_load_ttf(const char *f) { (void)f; return -1; }
void font_use_builtin(void) {}
void mouse_get_state(int *x, int *y, int *l, int *r) { *x = *y = *l = *r = 0; }
unsigned mouse_get_left_click_count(void) { return 0; }
unsigned mouse_get_right_click_count(void) { return 0; }
unsigned mouse_get_packet_count(void) { return 0; }
int process_execute(const char *f) { (void)f; return -1; }
int process_execute_argv(const char *f, int c, char **v) { (void)f; (void)c; (void)v; return -1; }
int process_exit_status(void) { return 0; }
int vga_width(void) { return 1920; }
int vga_height(void) { return 1080; }
int vga_is_truecolor(void) { return 1; }

/* ---- main ---- */
extern int vfs_initialize(void);
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s disk.img < script\n", argv[0]); return 2; }
    g_disk = fopen(argv[1], "r+b");
    if (!g_disk) { perror(argv[1]); return 2; }
    if (vfs_initialize() != 0) { fprintf(stderr, "vfs_initialize failed\n"); return 3; }
    shell_initialize();
    printf("\x1e" "READY\n");
    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        int rc = shell_run_line(line);
        printf("\x1e%d\n", rc);
        fflush(stdout);
    }
    return 0;
}
