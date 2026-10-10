#include <stddef.h>

#include "font.h"
#include "font8x8.h"
#include "font_ttf.h"
#include "vfs.h"

static uint8_t custom_glyphs[256][256];
static uint8_t custom_bitmap[FONT_GLYPH_BYTES];
static uint8_t custom_advance[256];
static enum font_source active_source = FONT_SOURCE_BUILTIN;
static char active_name[13] = "BUILTIN";

/* Working buffers for font_load_ttf() -- static, not stack-local,
 * same reasoning as font_ttf.c's own buffers (16 KiB kernel stack,
 * these are much bigger than that). ttf_staging is rasterized into
 * first and only copied over the *live* custom_glyphs on success, so
 * a bad/oversized/unparseable file can never corrupt whatever font
 * is currently active while this function is failing out of it. */
static uint8_t ttf_raw_buf[FONT_TTF_MAX_BYTES];
static uint8_t ttf_staging[256][256];
static uint8_t ttf_advances[256];

/* font8x8_lookup() only ever returns 8x8 glyphs -- this project's
 * builtin fallback font predates the 1920x1080 HD bump, and hand-
 * redrawing 256 glyphs as real 16x16 art isn't something to
 * improvise byte-by-byte. Nearest-neighbor 2x upscaling every
 * fallback glyph into the new 16x16/32-byte format is an honest,
 * mechanical stand-in: blockier than a font actually drawn at
 * 16x16 would be, but legitimately bigger (matches "everything
 * else bigger" for HD) and exactly as recognizable as the original
 * 8x8 shape, since no pixel invents anything the source glyph
 * didn't already have. Any real .ttf/.otf loaded via
 * font_load_ttf() gets genuine 16x16 outline detail instead (see
 * font_ttf.c) -- this upscale only ever backs the compiled-in
 * default and whatever a loaded TTF's own coverage gaps fall back
 * to. */
static void upscale_8x8_to_coverage(const uint8_t src[8], uint8_t dst[256])
{
    for (int row = 0; row < 16; row++) {
        uint8_t s = src[row / 2];
        for (int col = 0; col < 16; col++) {
            int bit = (s >> (7 - col / 2)) & 1;
            dst[row * 16 + col] = bit ? 15 : 0;
        }
    }
}

void font_use_builtin(void)
{
    active_source = FONT_SOURCE_BUILTIN;

    active_name[0] = 'B'; active_name[1] = 'U'; active_name[2] = 'I';
    active_name[3] = 'L'; active_name[4] = 'T'; active_name[5] = 'I';
    active_name[6] = 'N'; active_name[7] = '\0';
}

int font_load(const char* filename)
{
    uint32_t length = 0;

    int result = vfs_read_all(
        filename,
        (char*)custom_bitmap,
        sizeof(custom_bitmap),
        &length
    );

    if (result != 0 || length != FONT_GLYPH_BYTES) {
        return -1;
    }

    /* The on-disk legacy bitmap stays compact; expand it to coverage
     * values for the common antialiased text renderer. */
    for (int c = 0; c < 256; c++) for (int row = 0; row < 16; row++) {
        uint8_t a = custom_bitmap[c * 32 + row * 2];
        uint8_t b = custom_bitmap[c * 32 + row * 2 + 1];
        for (int col = 0; col < 16; col++) {
            uint8_t byte = col < 8 ? a : b;
            int bit = col < 8 ? col : col - 8;
            custom_glyphs[c][row * 16 + col] = (byte & (0x80u >> bit)) ? 15 : 0;
        }
    }
    for (int c = 0; c < 256; c++) custom_advance[c] = 16;

    active_source = FONT_SOURCE_CUSTOM;

    int i = 0;
    for (; filename[i] != '\0' && i < 12; i++) {
        active_name[i] = filename[i];
    }
    active_name[i] = '\0';

    return 0;
}

int font_load_ttf(const char* filename)
{
    uint32_t length = 0;

    int result = vfs_read_all(
        filename,
        (char*)ttf_raw_buf,
        sizeof(ttf_raw_buf),
        &length
    );

    if (result != 0 || length == 0 || length > sizeof(ttf_raw_buf)) {
        return -1;
    }

    /* Start the staging table from the complete builtin glyph set
     * (upscaled to 16x16), so any codepoint the TTF doesn't cover
     * (outside 32-126, or a composite glyph -- see font_ttf.h) still
     * renders as something recognizable rather than blank once this
     * becomes active. */
    for (int c = 0; c < 256; c++) {
        const uint8_t* g8 = font8x8_lookup((char)c);
        upscale_8x8_to_coverage(g8, ttf_staging[c]);
        ttf_advances[c] = 16;
    }

    if (font_ttf_rasterize(ttf_raw_buf, length, ttf_staging, ttf_advances) != 0) {
        return -1; /* unparseable file, or yielded zero usable glyphs --
                     * nothing below has touched custom_glyphs, so
                     * whatever font was active stays active exactly
                     * as font_load() promises on failure. */
    }

    for (int c = 0; c < 256; c++)
        for (int i = 0; i < 256; i++)
            custom_glyphs[c][i] = ttf_staging[c][i];
    for (int c = 0; c < 256; c++) custom_advance[c] = ttf_advances[c];

    active_source = FONT_SOURCE_CUSTOM;

    int i = 0;
    for (; filename[i] != '\0' && i < 12; i++) {
        active_name[i] = filename[i];
    }
    active_name[i] = '\0';

    return 0;
}

enum font_source font_active_source(void)
{
    return active_source;
}

const char* font_active_name(void)
{
    return active_name;
}

static uint8_t builtin_glyph_coverage[256];

const uint8_t* font_get_coverage(char c)
{
    if (active_source == FONT_SOURCE_CUSTOM) {
        return custom_glyphs[(unsigned char)c];
    }
    upscale_8x8_to_coverage(font8x8_lookup(c), builtin_glyph_coverage);
    return builtin_glyph_coverage;
}

int font_get_advance(char c)
{
    return active_source == FONT_SOURCE_CUSTOM
        ? custom_advance[(unsigned char)c]
        : 16;
}
