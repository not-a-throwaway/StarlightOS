/* Host render harness: compiles the REAL gui.c + vga.c + starlightos_gui.c and renders desktop frames to PPM.
 * The kernel services gui.c calls are stubbed below; the VBE framebuffer is an mmap'd low-memory buffer.
 * usage: render_demo FONT.TTF outprefix */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <sys/mman.h>
#include "../../font_ttf.h"
#include "../../vga.h"
#include "../../gfx.h"
#include "../../vfs.h"

/* ---- font: real JetBrains Mono through the kernel's own rasteriser ---- */
static uint8_t g_cov[256][256]; static uint8_t g_adv[256];
const uint8_t* font_get_coverage(char c) { return g_cov[(uint8_t)c]; }
int font_get_advance(char c) { return g_adv[(uint8_t)c]; }

/* ---- vbe: hand out a framebuffer below 4 GiB, like the real BAR ---- */
int vbe_set_mode(int w, int h, int bpp, uint32_t* out_fb)
{
    void* p = mmap(NULL, (size_t)w * h * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED || bpp != 32) return 0;
    *out_fb = (uint32_t)(uintptr_t)p;
    return 1;
}

/* ---- stubs for everything gui.c touches ---- */
#include "../../rtc.h"
void rtc_read(struct rtc_time* t) { memset(t, 0, sizeof *t); t->hour = 21; t->minute = 42; t->second = 7; t->day = 6; t->month = 10; t->year = 2026; }
void mouse_get_state(int* x, int* y, int* l, int* r) { *x = 980; *y = 520; *l = *r = 0; }
void mouse_set_bounds(int w, int h) { (void)w; (void)h; }
int mouse_take_dirty(void) { return 0; }
uint32_t ktime_ms(void) { return 100000; }
int net_configured(void) { return 1; }
int net_ready(void) { return 1; }
int audio_busy(void) { return 0; }
int audio_get_volume(void) { return 60; }
int audio_available(void) { return 1; }
int ksnprintf(char* b, size_t n, const char* f, ...) { va_list ap; va_start(ap, f); int r = vsnprintf(b, n, f, ap); va_end(ap); return r; }
#include "../../audio.h"
#include "../../net.h"
void audio_get_status(struct audio_status* s) { memset(s, 0, sizeof *s); s->volume = 60; s->level_left = 55; s->level_right = 85; s->backend = 2; }
void audio_sfx(enum audio_sfx w) { (void)w; }
int vfs_cd(const char* p) { (void)p; return 0; }
const char* vfs_get_cwd(void) { return "/"; }
uint32_t vfs_free_bytes(void) { return 12u << 20; }
uint32_t vfs_total_bytes(void) { return 16u << 20; }
int vfs_get_entries(char out[][VFS_NAME_MAX + 1], uint8_t is_dir[], int max) {
    static const char* n[] = { "..", "DOCS", "BIN", "HELLO.TXT", "FONT.TTF", "MINES.ELF", "NOTES.TXT", "ROM.BIN" };
    int c = 8 < max ? 8 : max; for (int i = 0; i < c; i++) { strcpy(out[i], n[i]); is_dir[i] = i < 3; } return c; }
int keyboard_pump(void) { return 0; }
void keyboard_set_dispatch(void (*h)(char)) { (void)h; }
void kyield(void) {}
void net_fmt_ip(char* o, const uint8_t ip[4]) { sprintf(o, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]); }
static struct net_config g_cfg = { .ip = { 10, 0, 2, 15 } };
const struct net_config* net_get_config(void) { return &g_cfg; }
int process_execute(const char* f) { (void)f; return 0; }
int process_execute_argv(const char* f, int c, char** v) { (void)f; (void)c; (void)v; return 0; }
int process_exit_status(void) { return 0; }
void shell_handle_character(char c) { (void)c; }
void terminal_writestring(const char* s) { (void)s; }
uint32_t terminal_dirty_count(void) { return 0; }
static int t_ox, t_oy, t_cols = 80, t_rows = 30;
void terminal_set_origin(int x, int y) { t_ox = x; t_oy = y; }
void terminal_set_size(int c, int r) { t_cols = c; t_rows = r; }
void terminal_get_size(int* c, int* r) { *c = t_cols; *r = t_rows; }
static gfx_t* t_target;
void terminal_set_render_target(gfx_t* t) { t_target = t; }
void terminal_redraw(void) {   /* a mock shell session drawn with the same primitives kernel.c uses */
    static const char* lines[] = {
        "StarlightOS kernel starting...", "Display: 1920x1080, 32-bit TrueColor (VBE linear framebuffer).",
        "Network: RTL8139 52:54:00:12:34:56, link up, DHCP started.", "StarlightOS is running.", "",
        "starlight:/> fetch https://example.com", "200 OK  text/html  1256 bytes in 214 ms", "", "# Example Domain", "",
        "This domain is for use in illustrative examples in documents.", "", "More information...[1]", "", "starlight:/> _" };
    const unsigned fgs[] = { 7, 10, 11, 14, 7, 15, 10, 7, 15, 7, 7, 7, 9, 7, 15 };
    int cw = 9, ch = 16;
    gfx_t* tgt = t_target ? t_target : vga_framebuffer();   /* like kernel.c's term_fb() */
    for (int r = 0; r < t_rows; r++) for (int c = 0; c < t_cols; c++) {
        int px = t_ox + c * cw, py = t_oy + r * ch; char chr = ' ';
        if (r < 15 && c < (int)strlen(lines[r])) chr = lines[r][c];
        vga_fill_rect(tgt, px, py, cw, ch, gfx_vga16(0));
        vga_draw_char_clipped(tgt, px, py, chr, gfx_vga16(fgs[r < 15 ? r : 0]), cw);
    }
}

#include "../../gui.c"

static void dump(const char* path)
{
    FILE* f = fopen(path, "wb"); int w = vga_width(), h = vga_height();
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { gfx_t c = frame_buffer[i]; fputc((c >> 16) & 255, f); fputc((c >> 8) & 255, f); fputc(c & 255, f); }
    fclose(f);
}

int main(int argc, char** argv)
{
    FILE* f = fopen(argv[1], "rb"); static uint8_t ttf[1 << 20]; size_t n = fread(ttf, 1, sizeof ttf, f); fclose(f);
    for (int i = 0; i < 256; i++) g_adv[i] = 9;
    if (font_ttf_rasterize(ttf, (uint32_t)n, g_cov, g_adv) != 0) { fprintf(stderr, "font load failed\n"); return 1; }
    if (!vga_try_enable_vbe(1920, 1080)) { fprintf(stderr, "no vbe\n"); return 1; }
    printf("truecolor=%d bpp=%d %dx%d\n", vga_is_truecolor(), vga_bits_per_pixel(), vga_width(), vga_height());
    build_desktop();
    struct rtc_time t; rtc_read(&t);
    char path[256];
    static const int apps[] = { APP_DESKTOP, APP_TERMINAL, APP_FILES, APP_ABOUT };
    static const char* names[] = { "desktop", "terminal", "files", "about" };
    for (int i = 0; i < 4; i++) {
        current_app = apps[i];
        if (apps[i] != APP_DESKTOP) open_app(apps[i]);
        render_frame(&t, 1010, 560, 0, 0);
        snprintf(path, sizeof path, "%s_%s.ppm", argv[2], names[i]); dump(path);
    }
    return 0;
}
