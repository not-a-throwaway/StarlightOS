#ifndef FONT_TTF_H
#define FONT_TTF_H

#include <stdint.h>

/*
 * A real (if deliberately minimal) TrueType/OpenType outline
 * loader + rasterizer -- turns actual glyph outlines from a real
 * .ttf/.otf file into a 16x16 coverage mask (0..15 per pixel -- see
 * font.h), the same format font.c's custom_glyphs[] now uses. The
 * rasterizer retains 4x4 supersample coverage at glyph edges for
 * smooth compositing into the indexed framebuffer, with no
 * changes needed anywhere else. (Was 8x8 until this was bumped
 * for the 1920x1080 HD desktop -- real 16x16 outline detail, not
 * an upscale of an 8x8 result, since the whole rasterizer -- outline
 * flattening, supersampling, everything -- runs at the new size.)
 *
 * What it parses: the sfnt table directory, 'head' (unitsPerEm,
 * loca index format), 'maxp' (glyph count), 'cmap' format 4
 * (Unicode BMP -> glyph id -- what every normal Latin TTF uses),
 * 'loca' + 'glyf' simple-glyph outlines (on/off-curve points,
 * quadratic Bezier contours per the TrueType spec). Each glyph's
 * outline is flattened to line segments and scan-filled at 4x
 * supersample (64x64) using the even-odd rule, then downsampled
 * to the 16x16 output while retaining each pixel's coverage.
 *
 * What it deliberately does NOT do, rather than silently guessing:
 * - Composite glyphs (accented Latin built from two component
 *   glyphs, e.g. "e" + combining acute for "e") are left as
 *   whatever glyph was already at that slot. Plain ASCII 32-126,
 *   which is all this loads, is almost entirely simple glyphs in
 *   a normal Latin font, so this rarely matters for that range.
 * - cmap formats other than 4 aren't handled (format 4 is what
 *   effectively every modern desktop Latin TTF/OTF ships).
 * - Hinting instructions are skipped entirely -- irrelevant at an
 *   unhinted 16x16 output size.
 *
 * Arial itself is proprietary and isn't bundled or fetched by
 * this code -- it just parses whatever .ttf/.otf file you actually
 * hand it. Liberation Sans is metric-compatible with Arial under
 * a free license and is a reasonable real substitute; DejaVu Sans
 * also works.
 */

/* Parses `data` (a complete .ttf/.otf file already in memory,
 * `len` bytes) and rasterizes printable ASCII 32-126 into
 * out_glyphs[256][256] (16x16 coverage values 0..15 -- see font.h).
 * Printable glyph advances are scaled from the font's hmtx table with
 * a small readability allowance for 16px rasterized outlines; callers
 * initialize entries with fallback advances before calling so
 * unmapped glyphs keep sensible spacing.
 * Codepoints outside 32-126, or a codepoint this file has no
 * mapping/outline for, are left untouched, so pre-filling
 * out_glyphs with a fallback font first (as font.c does) means
 * "no glyph found" degrades to that fallback rather than blank
 * space.
 *
 * Returns 0 if at least one glyph was rasterized, -1 if the file
 * isn't a parseable sfnt/TrueType file at all (missing table,
 * corrupt header, etc.) -- in which case out_glyphs is left
 * completely untouched.
 *
 * Pure function: no disk/filesystem/OS dependency, so it's the exact
 * same code path whether called from the kernel or a host-side
 * test harness fed a real font file.
 */
int font_ttf_rasterize(const uint8_t* data, uint32_t len, uint8_t out_glyphs[256][256], uint8_t advances[256]);

#endif
