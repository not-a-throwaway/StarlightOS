#ifndef GFX_H
#define GFX_H
/*
 * gfx.h -- TrueColor pixel type and integer colour math.
 *
 * Every drawing surface in StarlightOS (the VBE framebuffer, the desktop cache, the frame being composed, the
 * paint canvas) is an array of gfx_t = 0x00RRGGBB, 8 bits per channel. A "colour" passed to any vga_*
 * primitive is the same 0x00RRGGBB value -- write them with RGB(r,g,b) or 0xRRGGBB, never as a palette
 * index (the 8-bit palette era is over; on a display that cannot do 32 bpp, vga.c quantises at present).
 *
 * All maths is integer-only: the kernel does not enable SSE, so no float/double anywhere.
 */
#include <stdint.h>

typedef uint32_t gfx_t;

#define RGB(r, g, b)  ((gfx_t)((((unsigned)(r) & 0xFFu) << 16) | (((unsigned)(g) & 0xFFu) << 8) | ((unsigned)(b) & 0xFFu)))
#define GFX_R(c)      ((unsigned)(((c) >> 16) & 0xFFu))
#define GFX_G(c)      ((unsigned)(((c) >> 8) & 0xFFu))
#define GFX_B(c)      ((unsigned)((c) & 0xFFu))

/* Linear blend: t = 0 gives a, t = 255 gives b. */
static inline gfx_t gfx_mix(gfx_t a, gfx_t b, unsigned t)
{
    unsigned s = 255u - t;
    unsigned r = (GFX_R(a) * s + GFX_R(b) * t + 127u) / 255u;
    unsigned g = (GFX_G(a) * s + GFX_G(b) * t + 127u) / 255u;
    unsigned bl = (GFX_B(a) * s + GFX_B(b) * t + 127u) / 255u;
    return RGB(r, g, bl);
}

/* Moves a colour towards white (amount > 0) or black (amount < 0), amount in -255..255. */
static inline gfx_t gfx_shade(gfx_t c, int amount)
{
    return amount >= 0 ? gfx_mix(c, 0xFFFFFFu, (unsigned)amount) : gfx_mix(c, 0x000000u, (unsigned)-amount);
}

/* The terminal's 16 colours (4-bit fg/bg attributes, VGA numbering: 0 black .. 7 light gray, 8 dark gray ..
 * 15 white) mapped to the GNOME/Adwaita terminal palette rather than the garish 1987 hardware values. */
static inline gfx_t gfx_vga16(unsigned i)
{
    static const gfx_t tab[16] = {
        0x1E1E24, 0x1E78E4, 0x2EC27E, 0x0AB9DC, 0xC01C28, 0x9841BB, 0xA2734C, 0xD0CFCC,
        0x5E5C64, 0x51A1FF, 0x57E389, 0x4FD2FD, 0xED333B, 0xC061CB, 0xF8E45C, 0xFFFFFF,
    };
    return tab[i & 15u];
}

/* floor(sqrt(v)) for v < 2^32, no division and no float. */
static inline unsigned gfx_isqrt(unsigned v)
{
    unsigned r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}
#endif
