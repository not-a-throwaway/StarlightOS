#ifndef VGA_H
#define VGA_H

#include <stdint.h>
#include "gfx.h"

/*
 * Compile-time ceiling for every offscreen buffer (desktop, paint canvas, frame being composed). Buffers are
 * allocated at this size regardless of which mode ends up active, so switching modes never risks an overflow.
 * Surfaces are TrueColor (gfx_t, 4 bytes/pixel): a full 1920x1080 surface is ~8.3 MB, and gui.c keeps three
 * of them (~25 MB of .bss; the kernel is identity-mapped over 4 GiB and runs with 256 MiB of RAM).
 */
#define VGA_MAX_WIDTH  1920
#define VGA_MAX_HEIGHT 1080

/* Old name, kept as an alias to the *default* (mode 13h) size
 * for any code that hasn't been switched over to vga_width()/
 * vga_height() yet. Don't use these for buffer sizing -- use
 * VGA_MAX_WIDTH/HEIGHT for that -- since the active mode can be
 * bigger at runtime. */
#define VGA_GFX_WIDTH  320
#define VGA_GFX_HEIGHT 200

/*
 * Switches the real VGA hardware into 320x200, 256-color mode
 * (the classic "mode 13h") by reprogramming its registers
 * directly -- there's no BIOS available to ask once we're past
 * boot. The previous register/palette state is saved so
 * vga_exit_mode13h() can put text mode back exactly as it was,
 * whatever it was.
 */
void vga_enter_mode13h(void);
void vga_exit_mode13h(void);

/*
 * Tries to get a bigger linear-framebuffer mode going via the
 * Bochs/QEMU "DISPI" VBE extension registers (no real-mode BIOS
 * call needed -- just I/O ports, which is why this works from
 * protected mode). This only does anything on a QEMU "std" VGA
 * device (or anything else that implements the same Bochs
 * interface); on real VGA hardware, or any other emulator, the
 * PCI probe simply won't find a matching device and this call
 * is a no-op.
 *
 * Always call vga_enter_mode13h() first regardless -- this
 * function assumes mode 13h is already active and only
 * *upgrades* from it. If the probe or mode-set fails at any
 * point, it leaves mode 13h exactly as it was and returns 0,
 * so there is no path where calling this can make the display
 * worse than not calling it at all.
 *
 * Returns 1 and switches vga_width()/vga_height() to the new
 * mode on success, 0 otherwise.
 */
int vga_try_enable_vbe(int want_w, int want_h);

/* Active resolution -- 320x200 until/unless vga_try_enable_vbe()
 * succeeds. Every drawing primitive clips against these, not
 * the VGA_GFX_WIDTH/HEIGHT compile-time constants. */
int vga_width(void);
int vga_height(void);

/* 8 bpp fallback only: programs the DAC with a 3-3-2 colour cube. Harmless (and unused) on a TrueColor display. */
void vga_set_palette_color(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void vga_init_default_palette(void);

/* 1 once vga_try_enable_vbe() has switched to a 32 bpp TrueColor mode; 0 in the 8 bpp mode 13h fallback
 * (where every surface is still gfx_t and is quantised to 3-3-2 when presented). */
int vga_is_truecolor(void);
int vga_bits_per_pixel(void);

/* The surface that stands for video memory. TrueColor: the real linear framebuffer. Fallback: a 320x200 shadow
 * buffer. Either way you may draw into it directly -- call vga_fb_update() afterwards for the rect you
 * touched (a no-op on TrueColor, where the pixels already hit video memory). */
gfx_t* vga_framebuffer(void);
void vga_fb_update(int x, int y, int w, int h);

/* Copies a vga_width()*vga_height() offscreen surface to video memory in one shot -- compose a whole frame
 * off-screen, then present it, so nothing flickers half-drawn. */
void vga_present(const gfx_t* buffer);

void  vga_put_pixel(gfx_t* target, int x, int y, gfx_t color);

/* Reads back a single pixel already drawn into `target` (out-of-bounds reads as 0). Used to sample "what's really
 * behind this" before rounding a corner, instead of guessing a flat backdrop colour. */
gfx_t vga_get_pixel(const gfx_t* target, int x, int y);

void vga_fill_rect(gfx_t* target, int x, int y, int w, int h, gfx_t color);
void vga_draw_rect(gfx_t* target, int x, int y, int w, int h, gfx_t color);
void vga_fill_rect_dither(gfx_t* target, int x, int y, int w, int h, gfx_t color_a, gfx_t color_b);
void vga_draw_hline_2px(gfx_t* target, int x, int y, int w, gfx_t c1, gfx_t c2);

/* TrueColor extras. alpha: 0 = untouched, 255 = opaque. */
void vga_blend_pixel(gfx_t* target, int x, int y, gfx_t color, unsigned alpha);
void vga_fill_rect_alpha(gfx_t* target, int x, int y, int w, int h, gfx_t color, unsigned alpha);
void vga_fill_gradient_v(gfx_t* target, int x, int y, int w, int h, gfx_t top, gfx_t bottom);

void vga_draw_char(gfx_t* target, int x, int y, char c, gfx_t color);
void vga_draw_char_clipped(gfx_t* target, int x, int y, char c, gfx_t color, int width);
void vga_draw_string(gfx_t* target, int x, int y, const char* text, gfx_t color);
int vga_string_width(const char* text);

#endif
