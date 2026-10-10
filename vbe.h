#ifndef VBE_H
#define VBE_H

#include <stdint.h>

/*
 * Tries to set up a linear framebuffer of exactly want_w x
 * want_h at the requested bits-per-pixel (8 or 32) using the Bochs "DISPI" interface that QEMU's
 * default "std" VGA device implements. This is a real hardware
 * interface (a handful of I/O ports), not a software hack --
 * it's the same mechanism SeaBIOS itself uses, just done
 * ourselves since no BIOS is available this late.
 *
 * On success, returns 1 and fills *out_fb with the physical
 * address of the linear framebuffer (pitch == want_w pixels, since we
 * always set virtual width == width). On any failure -- no
 * matching PCI device, DISPI ID register doesn't respond as
 * expected, whatever -- returns 0 and touches nothing, so the
 * caller can safely keep using whatever mode was already active.
 */
int vbe_set_mode(int want_w, int want_h, int bpp, uint32_t* out_fb);

#endif
