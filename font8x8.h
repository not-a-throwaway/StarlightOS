#ifndef FONT8X8_H
#define FONT8X8_H

#include <stdint.h>

/*
 * IBM PC / VGA-style 8x8 bitmap font.
 *
 * The glyph table is indexed by the 8-bit character value.
 * ASCII characters therefore use their normal CP437-compatible
 * positions, while extended characters can also be rendered.
 */

const uint8_t* font8x8_lookup(char c);

#endif
