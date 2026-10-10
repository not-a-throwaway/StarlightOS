#include <stddef.h>

#include "vga.h"
#include "font.h"
#include "vbe.h"
#include "gfx.h"

#define VGA_MISC_WRITE 0x3C2
#define VGA_MISC_READ  0x3CC

#define VGA_SEQ_INDEX 0x3C4
#define VGA_SEQ_DATA  0x3C5

#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5

#define VGA_GC_INDEX 0x3CE
#define VGA_GC_DATA  0x3CF

#define VGA_AC_INDEX_DATA 0x3C0
#define VGA_AC_READ       0x3C1
#define VGA_INPUT_STATUS1 0x3DA

#define VGA_DAC_MASK       0x3C6
#define VGA_DAC_WRITE_IDX  0x3C8
#define VGA_DAC_READ_IDX   0x3C7
#define VGA_DAC_DATA       0x3C9

#define SEQ_REG_COUNT  5
#define CRTC_REG_COUNT 25
#define GC_REG_COUNT   9
#define AC_REG_COUNT   21

/*
 * Active display state. Defaults to plain mode 13h; a successful
 * vga_try_enable_vbe() repoints active_fb at the linear
 * framebuffer BAR and grows active_w/active_h. Every drawing
 * primitive below reads these instead of the VGA_GFX_WIDTH/
 * HEIGHT compile-time constants, so the same code draws correctly
 * at either size.
 */
static uint8_t* active_fb = (uint8_t*)0xA0000;   /* raw video memory: 8 bpp mode 13h, or the 32 bpp VBE LFB */
static int active_w = VGA_GFX_WIDTH;
static int active_h = VGA_GFX_HEIGHT;
static int active_bpp = 8;                        /* 32 = TrueColor (VBE), 8 = mode 13h fallback */
/* In the 8 bpp fallback, surfaces are still gfx_t (TrueColor) buffers; this shadow stands in for "video
 * memory" so code that draws straight to vga_framebuffer() (the boot terminal) works the same everywhere.
 * vga_fb_update() / vga_present() quantise it into the real 8 bpp screen. */
static gfx_t shadow_fb[VGA_GFX_WIDTH * VGA_GFX_HEIGHT];

int vga_width(void)  { return active_w; }
int vga_height(void) { return active_h; }

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

/*
 * Standard mode 13h (320x200x256) register values, as found in
 * the VGA hardware documentation and used to reproduce this
 * mode outside of calling the (unavailable, post-boot) BIOS.
 */
static const uint8_t mode13h_misc = 0x63;

static const uint8_t mode13h_seq[SEQ_REG_COUNT] = {
    0x03, 0x01, 0x0F, 0x00, 0x0E
};

static const uint8_t mode13h_crtc[CRTC_REG_COUNT] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x8E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF
};

static const uint8_t mode13h_gc[GC_REG_COUNT] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF
};

static const uint8_t mode13h_ac[AC_REG_COUNT] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};

/*
 * Saved text-mode state, filled in by vga_enter_mode13h() and
 * replayed by vga_exit_mode13h(). This round-trips whatever the
 * previous mode actually was instead of assuming it was exactly
 * the standard 80x25 text mode table.
 */
static uint8_t saved_misc;
static uint8_t saved_seq[SEQ_REG_COUNT];
static uint8_t saved_crtc[CRTC_REG_COUNT];
static uint8_t saved_gc[GC_REG_COUNT];
static uint8_t saved_ac[AC_REG_COUNT];
static uint8_t saved_dac_mask;
static uint8_t saved_palette[256 * 3];
static int have_saved_state = 0;

static void ac_write_all(const uint8_t* values)
{
    (void)inb(VGA_INPUT_STATUS1); /* reset flip-flop to "index" */

    for (int i = 0; i < AC_REG_COUNT; i++) {
        (void)inb(VGA_INPUT_STATUS1);
        outb(VGA_AC_INDEX_DATA, (uint8_t)i);
        outb(VGA_AC_INDEX_DATA, values[i]);
    }

    (void)inb(VGA_INPUT_STATUS1);
    outb(VGA_AC_INDEX_DATA, 0x20); /* re-enable video output */
}

static void ac_read_all(uint8_t* out)
{
    for (int i = 0; i < AC_REG_COUNT; i++) {
        (void)inb(VGA_INPUT_STATUS1);
        outb(VGA_AC_INDEX_DATA, (uint8_t)i);
        out[i] = inb(VGA_AC_READ);
    }

    (void)inb(VGA_INPUT_STATUS1);
    outb(VGA_AC_INDEX_DATA, 0x20);
}

static void crtc_write_all(const uint8_t* values)
{
    /*
     * Registers 0-7 are write-protected while bit 7 of index
     * 0x11 is set. Unlock, write everything else, then write
     * 0x11 last with its real (possibly protecting) value.
     */
    outb(VGA_CRTC_INDEX, 0x11);
    uint8_t unlocked = (uint8_t)(inb(VGA_CRTC_DATA) & 0x7F);
    outb(VGA_CRTC_INDEX, 0x11);
    outb(VGA_CRTC_DATA, unlocked);

    for (int i = 0; i < CRTC_REG_COUNT; i++) {
        if (i == 0x11) {
            continue;
        }

        outb(VGA_CRTC_INDEX, (uint8_t)i);
        outb(VGA_CRTC_DATA, values[i]);
    }

    outb(VGA_CRTC_INDEX, 0x11);
    outb(VGA_CRTC_DATA, values[0x11]);
}

static void crtc_read_all(uint8_t* out)
{
    for (int i = 0; i < CRTC_REG_COUNT; i++) {
        outb(VGA_CRTC_INDEX, (uint8_t)i);
        out[i] = inb(VGA_CRTC_DATA);
    }
}

static void seq_write_all(const uint8_t* values)
{
    for (int i = 0; i < SEQ_REG_COUNT; i++) {
        outb(VGA_SEQ_INDEX, (uint8_t)i);
        outb(VGA_SEQ_DATA, values[i]);
    }
}

static void seq_read_all(uint8_t* out)
{
    for (int i = 0; i < SEQ_REG_COUNT; i++) {
        outb(VGA_SEQ_INDEX, (uint8_t)i);
        out[i] = inb(VGA_SEQ_DATA);
    }
}

static void gc_write_all(const uint8_t* values)
{
    for (int i = 0; i < GC_REG_COUNT; i++) {
        outb(VGA_GC_INDEX, (uint8_t)i);
        outb(VGA_GC_DATA, values[i]);
    }
}

static void gc_read_all(uint8_t* out)
{
    for (int i = 0; i < GC_REG_COUNT; i++) {
        outb(VGA_GC_INDEX, (uint8_t)i);
        out[i] = inb(VGA_GC_DATA);
    }
}

static void dac_save(uint8_t* out_palette, uint8_t* out_mask)
{
    *out_mask = inb(VGA_DAC_MASK);

    outb(VGA_DAC_READ_IDX, 0);

    for (int i = 0; i < 256 * 3; i++) {
        out_palette[i] = inb(VGA_DAC_DATA);
    }
}

static void dac_restore(const uint8_t* palette, uint8_t mask)
{
    outb(VGA_DAC_MASK, mask);

    outb(VGA_DAC_WRITE_IDX, 0);

    for (int i = 0; i < 256 * 3; i++) {
        outb(VGA_DAC_DATA, palette[i]);
    }
}

void vga_enter_mode13h(void)
{
    /* unsigned long + bare pushf/pop (no explicit l suffix) so this
     * assembles on both targets -- 'pushfl'/'popl' are 32-bit-only
     * forms that don't exist as valid opcodes once actually
     * targeting x86-64. See shell.c's cpuid_is_available() for the
     * fuller explanation; same fix, same reason. */
    unsigned long flags;

    __asm__ volatile (
        "pushf\n"
        "pop %0\n"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    saved_misc = inb(VGA_MISC_READ);
    seq_read_all(saved_seq);
    crtc_read_all(saved_crtc);
    gc_read_all(saved_gc);
    ac_read_all(saved_ac);
    dac_save(saved_palette, &saved_dac_mask);
    have_saved_state = 1;

    outb(VGA_MISC_WRITE, mode13h_misc);
    seq_write_all(mode13h_seq);
    crtc_write_all(mode13h_crtc);
    gc_write_all(mode13h_gc);
    ac_write_all(mode13h_ac);

    outb(VGA_DAC_MASK, 0xFF);

    if (flags & 0x200) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

void vga_exit_mode13h(void)
{
    if (!have_saved_state) {
        return;
    }

    /* unsigned long + bare pushf/pop (no explicit l suffix) so this
     * assembles on both targets -- 'pushfl'/'popl' are 32-bit-only
     * forms that don't exist as valid opcodes once actually
     * targeting x86-64. See shell.c's cpuid_is_available() for the
     * fuller explanation; same fix, same reason. */
    unsigned long flags;

    __asm__ volatile (
        "pushf\n"
        "pop %0\n"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    outb(VGA_MISC_WRITE, saved_misc);
    seq_write_all(saved_seq);
    crtc_write_all(saved_crtc);
    gc_write_all(saved_gc);
    ac_write_all(saved_ac);
    dac_restore(saved_palette, saved_dac_mask);

    have_saved_state = 0;

    if (flags & 0x200) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

void vga_set_palette_color(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    outb(VGA_DAC_WRITE_IDX, index);
    outb(VGA_DAC_DATA, (uint8_t)(r & 0x3F));
    outb(VGA_DAC_DATA, (uint8_t)(g & 0x3F));
    outb(VGA_DAC_DATA, (uint8_t)(b & 0x3F));
}

/* 8 bpp fallback palette: a 3-3-2 colour cube (index = RRRGGGBB). Only used when no VBE 32 bpp mode is
 * available; TrueColor surfaces are quantised onto it at present time. */
void vga_init_default_palette(void)
{
    for (int i = 0; i < 256; i++) {
        unsigned r3 = ((unsigned)i >> 5) & 7u, g3 = ((unsigned)i >> 2) & 7u, b2 = (unsigned)i & 3u;
        vga_set_palette_color((uint8_t)i, (uint8_t)(r3 * 63u / 7u), (uint8_t)(g3 * 63u / 7u), (uint8_t)(b2 * 63u / 3u));
    }
}

static inline uint8_t quantise332(gfx_t c)
{
    return (uint8_t)(((GFX_R(c) >> 5) << 5) | ((GFX_G(c) >> 5) << 2) | (GFX_B(c) >> 6));
}

int vga_try_enable_vbe(int want_w, int want_h)
{
    if (want_w <= 0 || want_h <= 0 ||
        want_w > VGA_MAX_WIDTH || want_h > VGA_MAX_HEIGHT) {
        return 0;
    }

    uint32_t fb_phys;

    if (!vbe_set_mode(want_w, want_h, 32, &fb_phys)) {
        return 0;
    }

    /* fb_phys is a real 32-bit physical address from a PCI BAR0 read -- routed through uintptr_t so the
     * widen is explicit on the 64-bit build. */
    active_fb = (uint8_t*)(uintptr_t)fb_phys;
    active_w = want_w;
    active_h = want_h;
    active_bpp = 32;
    return 1;
}

int vga_is_truecolor(void) { return active_bpp == 32; }
int vga_bits_per_pixel(void) { return active_bpp; }

gfx_t* vga_framebuffer(void)
{
    return active_bpp == 32 ? (gfx_t*)(void*)active_fb : shadow_fb;
}

/* Copies n 32-bit pixels. On x86-64, rep movsq moves two at a time; the 32-bit build (movsq does not exist
 * there) uses rep movsl. Either runs at memory speed. */
static void gfx_copy(gfx_t* dst, const gfx_t* src, size_t n)
{
#ifdef __x86_64__
    size_t q = n >> 1;
    __asm__ volatile("cld; rep movsq" : "+D"(dst), "+S"(src), "+c"(q) : : "memory");
    if (n & 1) *dst = *src;
#else
    __asm__ volatile("cld; rep movsl" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
#endif
}

static void gfx_fill_span(gfx_t* dst, gfx_t color, size_t n)
{
    __asm__ volatile("cld; rep stosl" : "+D"(dst), "+c"(n) : "a"(color) : "memory");
}

void vga_fb_update(int x, int y, int w, int h)
{
    if (active_bpp == 32) return;                          /* drawing already hit video memory */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > active_w) w = active_w - x;
    if (y + h > active_h) h = active_h - y;
    for (int row = 0; row < h; row++) {
        const gfx_t* src = shadow_fb + (y + row) * active_w + x;
        uint8_t* dst = active_fb + (y + row) * active_w + x;
        for (int col = 0; col < w; col++) dst[col] = quantise332(src[col]);
    }
}

void vga_present(const gfx_t* buffer)
{
    size_t total = (size_t)active_w * (size_t)active_h;

    if (active_bpp == 32) {
        gfx_copy((gfx_t*)(void*)active_fb, buffer, total);
        return;
    }
    for (size_t i = 0; i < total; i++) active_fb[i] = quantise332(buffer[i]);
}

void vga_put_pixel(gfx_t* target, int x, int y, gfx_t color)
{
    if (x < 0 || y < 0 || x >= active_w || y >= active_h) {
        return;
    }

    target[y * active_w + x] = color & 0xFFFFFFu;
}

gfx_t vga_get_pixel(const gfx_t* target, int x, int y)
{
    if (x < 0 || y < 0 || x >= active_w || y >= active_h) {
        return 0;
    }

    return target[y * active_w + x];
}

/* Clips a rect to the active screen; returns 0 if nothing is left. */
static int clip_to_screen(int* x, int* y, int* w, int* h)
{
    if (*w <= 0 || *h <= 0) return 0;
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > active_w) *w = active_w - *x;
    if (*y + *h > active_h) *h = active_h - *y;
    return *w > 0 && *h > 0;
}

void vga_fill_rect(gfx_t* target, int x, int y, int w, int h, gfx_t color)
{
    if (!clip_to_screen(&x, &y, &w, &h)) return;
    color &= 0xFFFFFFu;
    for (int row = 0; row < h; row++) gfx_fill_span(target + (y + row) * active_w + x, color, (size_t)w);
}

void vga_blend_pixel(gfx_t* target, int x, int y, gfx_t color, unsigned alpha)
{
    if (x < 0 || y < 0 || x >= active_w || y >= active_h || alpha == 0) return;
    gfx_t* p = target + y * active_w + x;
    *p = alpha >= 255 ? (color & 0xFFFFFFu) : gfx_mix(*p, color, alpha);
}

void vga_fill_rect_alpha(gfx_t* target, int x, int y, int w, int h, gfx_t color, unsigned alpha)
{
    if (alpha == 0) return;
    if (alpha >= 255) { vga_fill_rect(target, x, y, w, h, color); return; }
    if (!clip_to_screen(&x, &y, &w, &h)) return;

    unsigned inv = 255u - alpha;
    unsigned sr = GFX_R(color) * alpha + 127u, sg = GFX_G(color) * alpha + 127u, sb = GFX_B(color) * alpha + 127u;
    for (int row = 0; row < h; row++) {
        gfx_t* p = target + (y + row) * active_w + x;
        for (int col = 0; col < w; col++) {
            gfx_t d = p[col];
            p[col] = RGB((GFX_R(d) * inv + sr) / 255u, (GFX_G(d) * inv + sg) / 255u, (GFX_B(d) * inv + sb) / 255u);
        }
    }
}

void vga_fill_gradient_v(gfx_t* target, int x, int y, int w, int h, gfx_t top, gfx_t bottom)
{
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        unsigned t = h > 1 ? (unsigned)(row * 255 / (h - 1)) : 0u;
        vga_fill_rect(target, x, y + row, w, 1, gfx_mix(top, bottom, t));
    }
}

void vga_draw_rect(gfx_t* target, int x, int y, int w, int h, gfx_t color)
{
    vga_fill_rect(target, x, y, w, 1, color);
    vga_fill_rect(target, x, y + h - 1, w, 1, color);
    vga_fill_rect(target, x, y, 1, h, color);
    vga_fill_rect(target, x + w - 1, y, 1, h, color);
}

/* A 1-pixel checkerboard of two colours (kept for callers that want a stipple; shadows now use real alpha). */
void vga_fill_rect_dither(gfx_t* target, int x, int y, int w, int h, gfx_t color_a, gfx_t color_b)
{
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            gfx_t c = ((row + col) & 1) ? color_a : color_b;
            vga_put_pixel(target, x + col, y + row, c);
        }
    }
}

/* A 2px-tall horizontal divider using two shades -- a cheap "highlight over shadow" bevel. */
void vga_draw_hline_2px(gfx_t* target, int x, int y, int w, gfx_t c1, gfx_t c2)
{
    vga_fill_rect(target, x, y, w, 1, c1);
    vga_fill_rect(target, x, y + 1, w, 1, c2);
}

void vga_draw_char(gfx_t* target, int x, int y, char c, gfx_t color)
{
    vga_draw_char_clipped(target, x, y, c, color, 16);
}

void vga_draw_char_clipped(gfx_t* target, int x, int y, char c, gfx_t color, int width)
{
    const uint8_t* glyph = font_get_coverage(c);
    if (width < 0) width = 0;
    if (width > 16) width = 16;
    color &= 0xFFFFFFu;

    /*
     * Glyph coverage is 16x16 with 0..15 per pixel (see font.h). On a TrueColor surface that is a real
     * anti-aliased blend of ink over whatever is already there, not a nearest-palette-entry guess.
     */
    for (int row = 0; row < 16; row++) {
        int py = y + row;
        if (py < 0 || py >= active_h) continue;
        for (int col = 0; col < width; col++) {
            uint8_t coverage = glyph[row * 16 + col];
            if (!coverage) continue;
            int px = x + col;
            if (px < 0 || px >= active_w) continue;
            gfx_t* p = target + py * active_w + px;
            *p = coverage >= 15 ? color : gfx_mix(*p, color, (unsigned)coverage * 17u);
        }
    }
}

void vga_draw_string(gfx_t* target, int x, int y, const char* text, gfx_t color)
{
    int cursor_x = x;

    for (size_t i = 0; text[i] != '\0'; i++) {
        vga_draw_char(target, cursor_x, y, text[i], color);
        cursor_x += font_get_advance(text[i]);
    }
}

int vga_string_width(const char* text)
{
    int width = 0;
    for (size_t i = 0; text[i] != '\0'; i++) width += font_get_advance(text[i]);
    return width;
}
