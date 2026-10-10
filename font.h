#ifndef FONT_H
#define FONT_H

#include <stdint.h>

/*
 * Small font manager sitting on top of font8x8_lookup(). vga.c's
 * vga_draw_char() calls font_get_coverage() instead of
 * font8x8_lookup() directly, so switching fonts here changes
 * rendering everywhere at once -- the terminal and every GUI
 * window alike.
 *
 * Glyphs render from 16x16 coverage masks (0..15 per pixel). TTF
 * outlines retain their 4x supersampled edge coverage for smooth
 * blending into the indexed framebuffer. Legacy raw fonts stay 8192
 * bytes on disk and expand their 1-bit pixels when loaded.
 */

#define FONT_GLYPH_BYTES 8192 /* 256 glyphs * 16 rows * 2 bytes */

enum font_source {
    FONT_SOURCE_BUILTIN = 0, /* font8x8_basic, the default */
    FONT_SOURCE_CUSTOM       /* whatever font_load() last loaded */
};

/* Switches back to the compiled-in default font. */
void font_use_builtin(void);

/* Loads a 8192-byte raw glyph file from the filesystem and
 * makes it the active font. Returns 0 on success, -1 if the file
 * doesn't exist or isn't exactly FONT_GLYPH_BYTES long (the
 * previous active font is left untouched on failure). */
int font_load(const char* filename);

/* Loads a real .ttf/.otf file from the filesystem, rasterizes
 * ASCII 32-126 from its actual outlines with fractional edge
 * coverage (see font_ttf.h for exactly what that does and doesn't cover), and makes the result the active
 * font. Codepoints outside 32-126, or with no outline in the file,
 * keep whatever the builtin font already draws for them, so the
 * result is never blank -- just builtin-for-that-glyph. Max file
 * size is FONT_TTF_MAX_BYTES; anything bigger, anything that isn't a
 * parseable sfnt file, or a file that yields zero usable glyphs
 * returns -1 and leaves the previous active font untouched. */
#define FONT_TTF_MAX_BYTES (512 * 1024)
int font_load_ttf(const char* filename);

/* Which source is currently active, and the filename last passed
 * to font_load() (valid only when the source is FONT_SOURCE_CUSTOM). */
enum font_source font_active_source(void);
const char* font_active_name(void);

/* Used by vga_draw_char(). Never returns NULL. */
const uint8_t* font_get_coverage(char c);
int font_get_advance(char c);

#endif
