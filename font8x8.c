#include <stdint.h>

#include "font8x8.h"
#include "font8x8_basic.h"
#include "font8x8_control.h"
#include "font8x8_ext_latin.h"
#include "font8x8_greek.h"
#include "font8x8_misc.h"
#include "font8x8_box.h"
#include "font8x8_block.h"
#include "font8x8_hiragana.h"
#include "font8x8_sga.h"

/*
 * The source font is split into Unicode ranges rather than a
 * single CP437-indexed array. StarlightOS currently only ever looks up
 * plain ASCII (0-127), which font8x8_basic covers directly, so
 * that's the only table wired up here.
 *
 * Row bytes are MSB-first (bit 7 = leftmost pixel) -- vga.c's
 * vga_draw_char() reads them that way. Confirmed by rendering
 * several asymmetric letters ('F', 'L', 'P', '7', 'b', 'd') as
 * ASCII art both ways: only MSB-first produces correct,
 * unmirrored letterforms. (An earlier comment here claimed
 * LSB-first, which is wrong -- don't trust that claim if you see
 * it resurface; check by rendering a glyph, not by re-reading
 * this comment.) See font8x8_basic.h's own header comment for
 * provenance; it's public domain.
 */

static const uint8_t blank_glyph[8] = {
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

const uint8_t* font8x8_lookup(char c)
{
    unsigned char uc = (unsigned char)c;

    if (uc < 128) {
        return (const uint8_t*)font8x8_basic[uc];
    }

    return blank_glyph;
}
