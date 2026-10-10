#include "font_ttf.h"

/*
 * Freestanding (no libc) sfnt parser + quadratic-Bezier scanline
 * rasterizer. Every working buffer below is `static` rather than a
 * stack local -- the kernel's stack is 16 KiB total (boot.s), and
 * these arrays are comfortably bigger than that combined, so they
 * must live in .bss instead. Not reentrant (fine: single-threaded
 * kernel, called from one shell command at a time).
 */

#define MAX_CONTOURS   32
#define MAX_POINTS     300   /* per glyph */
#define MAX_EXT_POINTS 600   /* points[] after inserting implied on-curve
                               * midpoints between consecutive off-curve
                               * control points -- worst case ~2x */
#define MAX_EDGES      900

#define SS   64   /* supersample raster grid: SS x SS, before downsampling.
                   * Bumped from 32 alongside CELL 8->16 so the
                   * block-averaging ratio (SS/CELL) -- and therefore
                   * antialiasing quality -- stays the same as before,
                   * rather than rasterizing at the same coarseness
                   * into a bigger, blockier cell. */
#define CELL 16   /* final output bitmap size. Was 8 (matching
                   * font8x8's convention); now a real 16x16 glyph,
                   * genuinely higher detail (actual outline geometry
                   * resolved at 2x, not an upscale of an 8x8 result)
                   * to look sharp on a 1920x1080 desktop instead of
                   * tiny/blocky. Packed as 2 bytes/row (see
                   * downsample_to_glyph()) since a single byte can
                   * only address 8 columns. */
#define FP   256  /* fixed-point scale for raster coordinates (8 frac bits) */
#define CURVE_STEPS 6 /* line segments per flattened quadratic Bezier */

struct ttf_point {
    int x, y;
    uint8_t on_curve;
};

/* ---- big-endian readers, all bounds-checked by the caller ---- */

static uint16_t ru16(const uint8_t* d, uint32_t off)
{
    return (uint16_t)(((uint32_t)d[off] << 8) | d[off + 1]);
}

static int16_t ri16(const uint8_t* d, uint32_t off)
{
    return (int16_t)ru16(d, off);
}

static uint32_t ru32(const uint8_t* d, uint32_t off)
{
    return ((uint32_t)d[off] << 24) | ((uint32_t)d[off + 1] << 16) |
           ((uint32_t)d[off + 2] << 8) | (uint32_t)d[off + 3];
}

/* ---- sfnt table directory ---- */

struct sfnt_tables {
    uint32_t head, maxp, cmap, loca, glyf, hhea, hmtx;
    int have_hhea;
};

static int find_tables(const uint8_t* d, uint32_t len, struct sfnt_tables* t)
{
    t->head = t->maxp = t->cmap = t->loca = t->glyf = t->hhea = t->hmtx = 0;
    t->have_hhea = 0;

    if (len < 12)
        return -1;

    uint16_t num_tables = ru16(d, 4);
    int have_head = 0, have_maxp = 0, have_cmap = 0, have_loca = 0, have_glyf = 0;

    for (uint16_t i = 0; i < num_tables; i++) {
        uint32_t rec = 12 + (uint32_t)i * 16;
        if (rec + 16 > len)
            return -1;

        uint32_t tag = ru32(d, rec);
        uint32_t off = ru32(d, rec + 8);

        switch (tag) {
            case 0x68656164: t->head = off; have_head = 1; break; /* "head" */
            case 0x6D617870: t->maxp = off; have_maxp = 1; break; /* "maxp" */
            case 0x636D6170: t->cmap = off; have_cmap = 1; break; /* "cmap" */
            case 0x6C6F6361: t->loca = off; have_loca = 1; break; /* "loca" */
            case 0x676C7966: t->glyf = off; have_glyf = 1; break; /* "glyf" */
            case 0x68686561: t->hhea = off; t->have_hhea = 1; break; /* "hhea" */
            case 0x686D7478: t->hmtx = off; break; /* "hmtx" */
            default: break;
        }
    }

    if (!have_head || !have_maxp || !have_cmap || !have_loca || !have_glyf)
        return -1; /* not a TrueType-outline sfnt (e.g. CFF-only OTF) */

    return 0;
}

/* ---- cmap format 4: Unicode BMP -> glyph id ---- */

static uint32_t find_cmap_subtable(const uint8_t* d, uint32_t len, uint32_t cmap_off)
{
    if (cmap_off + 4 > len)
        return 0;

    uint16_t num_sub = ru16(d, cmap_off + 2);
    uint32_t best = 0;
    int best_score = -1;

    for (uint16_t i = 0; i < num_sub; i++) {
        uint32_t rec = cmap_off + 4 + (uint32_t)i * 8;
        if (rec + 8 > len)
            break;

        uint16_t plat = ru16(d, rec);
        uint16_t enc = ru16(d, rec + 2);
        uint32_t sub_off = cmap_off + ru32(d, rec + 4);

        if (sub_off + 2 > len)
            continue;
        if (ru16(d, sub_off) != 4) /* only format 4 supported */
            continue;

        int score;
        if (plat == 3 && enc == 1) score = 3;      /* Windows, Unicode BMP */
        else if (plat == 0)        score = 2;      /* any Unicode platform */
        else if (plat == 3 && enc == 0) score = 1; /* Windows, Symbol */
        else                        score = 0;

        if (score > best_score) {
            best_score = score;
            best = sub_off;
        }
    }

    return best; /* 0 if nothing usable found */
}

static uint16_t cmap4_lookup(const uint8_t* d, uint32_t len, uint32_t sub, uint16_t cp)
{
    if (sub == 0 || sub + 14 > len)
        return 0;

    uint16_t seg_x2 = ru16(d, sub + 6);
    uint16_t seg_count = (uint16_t)(seg_x2 / 2);

    uint32_t end_base = sub + 14;
    uint32_t start_base = end_base + seg_x2 + 2; /* +2 skips reservedPad */
    uint32_t delta_base = start_base + seg_x2;
    uint32_t range_base = delta_base + seg_x2;

    if (range_base + (uint32_t)seg_x2 > len)
        return 0;

    for (uint16_t i = 0; i < seg_count; i++) {
        uint16_t end = ru16(d, end_base + i * 2);
        if (cp > end)
            continue;

        uint16_t start = ru16(d, start_base + i * 2);
        if (cp < start)
            return 0; /* falls in a gap between segments: unmapped */

        int16_t delta = ri16(d, delta_base + i * 2);
        uint32_t range_off_addr = range_base + i * 2;
        uint16_t range_off = ru16(d, range_off_addr);

        if (range_off == 0)
            return (uint16_t)(cp + delta);

        uint32_t gid_addr = range_off_addr + range_off + (uint32_t)(cp - start) * 2;
        if (gid_addr + 2 > len)
            return 0;

        uint16_t gid = ru16(d, gid_addr);
        if (gid == 0)
            return 0;

        return (uint16_t)(gid + delta);
    }

    return 0;
}

/* ---- loca: glyph id -> byte range within 'glyf' ---- */

static int loca_range(
    const uint8_t* d, uint32_t len, uint32_t loca_off,
    int long_format, uint32_t num_glyphs, uint16_t gid,
    uint32_t* out_start, uint32_t* out_end)
{
    if (gid >= num_glyphs)
        return -1;

    if (long_format) {
        uint32_t a = loca_off + (uint32_t)gid * 4;
        if (a + 8 > len)
            return -1;
        *out_start = ru32(d, a);
        *out_end = ru32(d, a + 4);
    } else {
        uint32_t a = loca_off + (uint32_t)gid * 2;
        if (a + 4 > len)
            return -1;
        *out_start = (uint32_t)ru16(d, a) * 2;
        *out_end = (uint32_t)ru16(d, a + 2) * 2;
    }

    return 0;
}

/* ---- simple glyph outline parsing ---- */

static struct ttf_point s_points[MAX_POINTS];
static uint8_t s_flags[MAX_POINTS];
static uint16_t s_end_pts[MAX_CONTOURS];
static struct ttf_point s_ext[MAX_EXT_POINTS];

static int parse_simple_glyph(
    const uint8_t* d, uint32_t len, uint32_t goff,
    int16_t num_contours, int* out_num_points)
{
    if (num_contours <= 0 || num_contours > MAX_CONTOURS)
        return -1;

    uint32_t p = goff + 10;

    for (int c = 0; c < num_contours; c++) {
        if (p + 2 > len)
            return -1;
        s_end_pts[c] = ru16(d, p);
        p += 2;
    }

    int num_points = s_end_pts[num_contours - 1] + 1;
    if (num_points <= 0 || num_points > MAX_POINTS)
        return -1;

    if (p + 2 > len)
        return -1;
    uint16_t instr_len = ru16(d, p);
    p += 2 + instr_len;

    int i = 0;
    while (i < num_points) {
        if (p + 1 > len)
            return -1;
        uint8_t f = d[p++];
        s_flags[i++] = f;

        if (f & 0x08) { /* REPEAT_FLAG */
            if (p + 1 > len)
                return -1;
            uint8_t rep = d[p++];
            while (rep-- > 0 && i < num_points)
                s_flags[i++] = f;
        }
    }

    int x = 0;
    for (i = 0; i < num_points; i++) {
        uint8_t f = s_flags[i];
        int dx = 0;

        if (f & 0x02) { /* X_SHORT_VECTOR */
            if (p + 1 > len)
                return -1;
            dx = d[p++];
            if (!(f & 0x10))
                dx = -dx;
        } else if (!(f & 0x10)) { /* not short, not "same" -> full i16 delta */
            if (p + 2 > len)
                return -1;
            dx = ri16(d, p);
            p += 2;
        }

        x += dx;
        s_points[i].x = x;
        s_points[i].on_curve = (uint8_t)(f & 0x01);
    }

    int y = 0;
    for (i = 0; i < num_points; i++) {
        uint8_t f = s_flags[i];
        int dy = 0;

        if (f & 0x04) { /* Y_SHORT_VECTOR */
            if (p + 1 > len)
                return -1;
            dy = d[p++];
            if (!(f & 0x20))
                dy = -dy;
        } else if (!(f & 0x20)) {
            if (p + 2 > len)
                return -1;
            dy = ri16(d, p);
            p += 2;
        }

        y += dy;
        s_points[i].y = y;
    }

    *out_num_points = num_points;
    return 0;
}

/*
 * Inserts implied on-curve midpoints between consecutive off-curve
 * control points (standard TrueType quadratic-outline expansion),
 * and rotates the contour to start on an on-curve point, so the
 * result always alternates ON, (OFF)?, ON, (OFF)?, ... ON with the
 * first and last entries equal (a closed loop) -- which is what the
 * segment walker below relies on.
 */
static int build_extended_contour(const struct ttf_point* pts, int n)
{
    int s = -1;
    for (int i = 0; i < n; i++) {
        if (pts[i].on_curve) { s = i; break; }
    }

    struct ttf_point start_pt;
    int start_idx;

    if (s < 0) {
        /* Rare: every point in the contour is off-curve. Synthesize
         * an on-curve start at the midpoint of the last and first
         * control points, per spec. */
        start_pt.x = (pts[0].x + pts[n - 1].x) / 2;
        start_pt.y = (pts[0].y + pts[n - 1].y) / 2;
        start_pt.on_curve = 1;
        start_idx = 0;
    } else {
        start_pt = pts[s];
        start_idx = s;
    }

    int m = 0;
    if (m < MAX_EXT_POINTS)
        s_ext[m++] = start_pt;

    for (int k = 1; k <= n; k++) {
        struct ttf_point cur = pts[(start_idx + k) % n];

        if (!cur.on_curve && m > 0 && !s_ext[m - 1].on_curve) {
            struct ttf_point mid;
            mid.x = (s_ext[m - 1].x + cur.x) / 2;
            mid.y = (s_ext[m - 1].y + cur.y) / 2;
            mid.on_curve = 1;
            if (m < MAX_EXT_POINTS)
                s_ext[m++] = mid;
        }

        if (m < MAX_EXT_POINTS)
            s_ext[m++] = cur;
    }

    if (m > 0 && !s_ext[m - 1].on_curve && m < MAX_EXT_POINTS)
        s_ext[m++] = start_pt;

    return m;
}

/* ---- font units -> fixed-point raster space ---- */

static void to_raster(int x, int y, int scale_units, int top, int* rx, int* ry)
{
    int scale = scale_units > 0 ? scale_units : 1;
    /* Use one uniform scale for both axes. Fit the font's complete
     * ascent/descent span when it exceeds the em square, so common
     * descenders and low punctuation stay inside the cell. */
    *rx = (int)(((long)x * SS * FP) / scale);
    *ry = (int)((((long)top - y) * SS * FP) / scale);
}

/* ---- edge list + scanline fill ---- */

static int e_x0[MAX_EDGES], e_y0[MAX_EDGES];
static int e_x1[MAX_EDGES], e_y1[MAX_EDGES];
static int num_edges;

static uint8_t coverage[SS][SS];
static int scan_xs[MAX_EDGES];

static void add_edge(int x0, int y0, int x1, int y1)
{
    if (y0 == y1) /* horizontal: never crosses a scanline sample */
        return;
    if (num_edges >= MAX_EDGES)
        return;

    e_x0[num_edges] = x0; e_y0[num_edges] = y0;
    e_x1[num_edges] = x1; e_y1[num_edges] = y1;
    num_edges++;
}

static void emit_quad(int x0, int y0, int cx, int cy, int x1, int y1)
{
    int px = x0, py = y0;
    const int den = CURVE_STEPS * CURVE_STEPS;

    for (int k = 1; k <= CURVE_STEPS; k++) {
        int nk = CURVE_STEPS - k;
        int qx = (nk * nk * x0 + 2 * nk * k * cx + k * k * x1) / den;
        int qy = (nk * nk * y0 + 2 * nk * k * cy + k * k * y1) / den;
        add_edge(px, py, qx, qy);
        px = qx; py = qy;
    }
}

static void rasterize_edges(void)
{
    for (int r = 0; r < SS; r++)
        for (int c = 0; c < SS; c++)
            coverage[r][c] = 0;

    for (int row = 0; row < SS; row++) {
        int sy = row * FP + FP / 2; /* sample at pixel-center */
        int n = 0;

        for (int e = 0; e < num_edges; e++) {
            int y0 = e_y0[e], y1 = e_y1[e];
            int lo = (y0 < y1) ? y0 : y1;
            int hi = (y0 < y1) ? y1 : y0;

            if (sy < lo || sy >= hi)
                continue;

            long num = (long)(e_x1[e] - e_x0[e]) * (long)(sy - y0);
            int x = e_x0[e] + (int)(num / (y1 - y0));

            if (n < MAX_EDGES)
                scan_xs[n++] = x;
        }

        for (int a = 1; a < n; a++) {
            int v = scan_xs[a], b = a - 1;
            while (b >= 0 && scan_xs[b] > v) {
                scan_xs[b + 1] = scan_xs[b];
                b--;
            }
            scan_xs[b + 1] = v;
        }

        for (int a = 0; a + 1 < n; a += 2) {
            int xstart = scan_xs[a], xend = scan_xs[a + 1];
            int col0 = xstart / FP;
            int col1 = xend / FP;

            if (col0 < 0) col0 = 0;
            if (col1 >= SS) col1 = SS - 1;

            for (int col = col0; col <= col1; col++) {
                int cx = col * FP + FP / 2;
                if (cx >= xstart && cx < xend)
                    coverage[row][col] = 1;
            }
        }
    }
}

static void downsample_to_glyph(uint8_t out[256])
{
    const int block = SS / CELL; /* 4 */

    for (int row = 0; row < CELL; row++) {
        for (int col = 0; col < CELL; col++) {
            int count = 0;
            for (int by = 0; by < block; by++)
                for (int bx = 0; bx < block; bx++)
                    count += coverage[row * block + by][col * block + bx];

            /* Preserve the actual 4x4 subpixel coverage instead of
             * thresholding every edge to solid black or transparent. */
            out[row * CELL + col] = (uint8_t)((count * 15 + 8) / 16);
        }
    }
}

static int rasterize_glyph(
    const uint8_t* d, uint32_t len, uint32_t goff, uint32_t glen,
    int units_per_em, int top, int bottom, uint8_t out[256])
{
    /* A glyph is 16x16 coverage bytes, not a packed 1-bit 16x16
     * bitmap. Clear the full output, especially for empty outlines
     * such as space: leaving bytes 32..255 untouched copied stack
     * garbage into the live font and painted noise across text rows. */
    for (int i = 0; i < 256; i++)
        out[i] = 0;

    if (glen == 0)
        return 0; /* legitimately empty outline (space, etc.) -- blank is correct */

    if (goff + 10 > len)
        return -1;

    int16_t num_contours = ri16(d, goff);
    if (num_contours <= 0)
        return -1; /* composite glyph or malformed: caller keeps its fallback */

    int num_points = 0;
    if (parse_simple_glyph(d, len, goff, num_contours, &num_points) != 0)
        return -1;

    num_edges = 0;
    int scale_units = top - bottom;
    if (scale_units < units_per_em) scale_units = units_per_em;

    int start = 0;
    for (int c = 0; c < num_contours; c++) {
        int end = s_end_pts[c];
        int n = end - start + 1;

        if (n <= 0 || n > MAX_POINTS) {
            start = end + 1;
            continue;
        }

        int m = build_extended_contour(&s_points[start], n);

        int rx0, ry0;
        to_raster(s_ext[0].x, s_ext[0].y, scale_units, top, &rx0, &ry0);

        int i = 0;
        while (i < m - 1) {
            if (s_ext[i + 1].on_curve) {
                int rx1, ry1;
                to_raster(s_ext[i + 1].x, s_ext[i + 1].y, scale_units, top, &rx1, &ry1);
                add_edge(rx0, ry0, rx1, ry1);
                rx0 = rx1; ry0 = ry1;
                i += 1;
            } else {
                if (i + 2 >= m)
                    break;

                int cx, cy, rx1, ry1;
                to_raster(s_ext[i + 1].x, s_ext[i + 1].y, scale_units, top, &cx, &cy);
                to_raster(s_ext[i + 2].x, s_ext[i + 2].y, scale_units, top, &rx1, &ry1);
                emit_quad(rx0, ry0, cx, cy, rx1, ry1);
                rx0 = rx1; ry0 = ry1;
                i += 2;
            }
        }

        start = end + 1;
    }

    rasterize_edges();
    downsample_to_glyph(out);
    return 0;
}

int font_ttf_rasterize(const uint8_t* d, uint32_t len, uint8_t out_glyphs[256][256], uint8_t advances[256])
{
    struct sfnt_tables t;
    if (find_tables(d, len, &t) != 0)
        return -1;

    if (t.head + 54 > len || t.maxp + 6 > len)
        return -1;

    int units_per_em = ru16(d, t.head + 18);
    if (units_per_em == 0)
        return -1;

    int16_t loca_fmt = ri16(d, t.head + 50);
    uint32_t num_glyphs = ru16(d, t.maxp + 4);
    uint16_t num_hmetrics = 0;
    if (t.have_hhea && t.hhea + 36 <= len && t.hmtx < len)
        num_hmetrics = ru16(d, t.hhea + 34);
    if (num_hmetrics > num_glyphs) num_hmetrics = 0;

    /* Real ascent/descent from 'hhea' when present -- this is what
     * actually varies between fonts and matters for legible vertical
     * placement. Falls back to an em-fraction guess (covers cap
     * height + descenders reasonably on most Latin fonts) only if
     * 'hhea' is missing or malformed. */
    int top = (units_per_em * 3) / 4;
    int bottom = -(units_per_em / 4);

    if (t.have_hhea && t.hhea + 8 <= len) {
        int16_t ascent = ri16(d, t.hhea + 4);
        int16_t descent = ri16(d, t.hhea + 6);
        if (ascent > 0 && descent < 0 && ascent - descent > 0) {
            top = ascent;
            bottom = descent;
        }
    }

    int advance_scale_units = top - bottom;
    if (advance_scale_units < units_per_em) advance_scale_units = units_per_em;

    uint32_t cmap_sub = find_cmap_subtable(d, len, t.cmap);
    if (cmap_sub == 0)
        return -1;

    int any_ok = 0;

    for (int cp = 32; cp <= 126; cp++) {
        uint16_t gid = cmap4_lookup(d, len, cmap_sub, (uint16_t)cp);
        if (gid == 0)
            continue; /* no mapping for this codepoint: leave fallback glyph */

        if (num_hmetrics && (uint32_t)t.hmtx + (uint32_t)num_hmetrics * 4 <= len) {
            uint32_t metric = gid < num_hmetrics ? gid : num_hmetrics - 1;
            uint16_t advance_units = ru16(d, t.hmtx + metric * 4);
            /* Match the outline's actual uniform scale. Avoid the old
             * synthetic tracking multiplier, which made GUI text too wide. */
            int advance = (advance_units * 16 + advance_scale_units / 2) / advance_scale_units;
            int minimum = cp == ' ' ? 5 : 7;
            if (advance < minimum) advance = minimum;
            if (advance > 16) advance = 16;
            advances[cp] = (uint8_t)advance;
        }

        uint32_t gstart, gend;
        if (loca_range(d, len, t.loca, loca_fmt, num_glyphs, gid, &gstart, &gend) != 0)
            continue;
        if (gend < gstart)
            continue;

        uint32_t glen = gend - gstart;
        uint32_t goff = t.glyf + gstart;
        if (goff + glen > len)
            continue;

        uint8_t glyph[256];
        if (rasterize_glyph(d, len, goff, glen, units_per_em, top, bottom, glyph) != 0)
            continue; /* composite/malformed glyph: leave fallback */

        for (int i = 0; i < 256; i++)
            out_glyphs[cp][i] = glyph[i];

        /* Guard against overhanging strokes (notably f/t and some
         * punctuation) colliding with the next glyph at 16px. Use
         * the actual rasterized right edge as a lower bound. */
        int rightmost = -1;
        for (int row = 0; row < 16; row++) for (int col = 0; col < 16; col++)
            if (glyph[row * 16 + col] && col > rightmost) rightmost = col;
        int visual_advance = rightmost + 2;
        if (visual_advance > advances[cp]) advances[cp] = (uint8_t)visual_advance;

        any_ok = 1;
    }

    return any_ok ? 0 : -1;
}
