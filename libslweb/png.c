/*
 * libslweb/png.c -- M6: a small baseline PNG decoder.
 *
 * Static-content browsers mostly meet PNG, so this implements just enough of
 * the format to paint ordinary web images:
 *
 *   - the 8-byte signature and IHDR (width/height, bit depth, colour type);
 *   - colour types 0 (grey), 2 (RGB), 3 (palette), 4 (grey+alpha), 6 (RGBA);
 *   - bit depths 1/2/4/8 for grey and palette, 8/16 for the rest (16-bit is
 *     down-converted by keeping the high byte);
 *   - PLTE + tRNS, and the five scanline filters (None/Sub/Up/Average/Paeth).
 *
 * It does NOT support Adam7 interlacing (rare for web graphics) and does not
 * verify CRCs. The IDAT stream is inflated with the DEFLATE decoder below
 * (stored + fixed + dynamic Huffman blocks) so no external zlib is needed.
 */
#include <stdlib.h>
#include <string.h>
#include "slweb.h"

/* ------------------------------------------------------------------ inflate */
struct bits {
    const uint8_t *in;
    size_t len, pos;
    uint32_t buf;
    int nbits;
    int err;
};

static unsigned bits_get(struct bits *b, int n)
{
    while (b->nbits < n) {
        if (b->pos >= b->len) { b->err = 1; return 0; }
        b->buf |= (uint32_t)b->in[b->pos++] << b->nbits;
        b->nbits += 8;
    }
    unsigned v = b->buf & ((1u << n) - 1);
    b->buf >>= n;
    b->nbits -= n;
    return v;
}

struct huff { uint16_t count[16]; uint16_t sym[288]; };

static void huff_build(struct huff *h, const uint8_t *lengths, int n)
{
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    uint16_t offs[16];
    offs[0] = offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++) if (lengths[i]) h->sym[offs[lengths[i]]++] = (uint16_t)i;
}

static int huff_decode(struct bits *b, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)bits_get(b, 1);
        if (b->err) return -1;
        int count = h->count[len];
        if (code - first < count) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

struct outbuf { uint8_t *p; size_t len, cap; int err; };

static int out_put(struct outbuf *o, uint8_t c)
{
    if (o->len >= o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 65536;
        uint8_t *np = (uint8_t *)realloc(o->p, nc);
        if (!np) { o->err = 1; return -1; }
        o->p = np; o->cap = nc;
    }
    o->p[o->len++] = c;
    return 0;
}

static int out_copy(struct outbuf *o, size_t dist, size_t len)
{
    if (dist == 0 || dist > o->len) { o->err = 1; return -1; }
    for (size_t i = 0; i < len; i++)
        if (out_put(o, o->p[o->len - dist]) < 0) return -1;
    return 0;
}

static int inflate_fixed(struct bits *b, struct outbuf *o)
{
    uint8_t ll[288], dl[30];
    for (int i = 0; i < 144; i++) ll[i] = 8;
    for (int i = 144; i < 256; i++) ll[i] = 9;
    for (int i = 256; i < 280; i++) ll[i] = 7;
    for (int i = 280; i < 288; i++) ll[i] = 8;
    for (int i = 0; i < 30; i++) dl[i] = 5;
    struct huff lit, dist;
    huff_build(&lit, ll, 288);
    huff_build(&dist, dl, 30);

    for (;;) {
        int sym = huff_decode(b, &lit);
        if (sym < 0) return -1;
        if (sym < 256) { if (out_put(o, (uint8_t)sym) < 0) return -1; }
        else if (sym == 256) return 0;
        else {
            int li = sym - 257;
            if (li >= 29) return -1;
            size_t length = len_base[li] + bits_get(b, len_extra[li]);
            int ds = huff_decode(b, &dist);
            if (ds < 0 || ds >= 30) return -1;
            size_t d = dist_base[ds] + bits_get(b, dist_extra[ds]);
            if (out_copy(o, d, length) < 0) return -1;
        }
        if (b->err) return -1;
    }
}

static int inflate_dynamic(struct bits *b, struct outbuf *o)
{
    static const int order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    int hlit = (int)bits_get(b, 5) + 257;
    int hdist = (int)bits_get(b, 5) + 1;
    int hclen = (int)bits_get(b, 4) + 4;
    if (b->err || hlit > 286 || hdist > 30 || hlit + hdist > 288) return -1;

    uint8_t clen[19];
    memset(clen, 0, sizeof clen);
    for (int i = 0; i < hclen; i++) clen[order[i]] = (uint8_t)bits_get(b, 3);
    struct huff clh;
    huff_build(&clh, clen, 19);

    uint8_t lens[288 + 32];
    memset(lens, 0, sizeof lens);
    int i = 0, total = hlit + hdist;
    while (i < total) {
        int c = huff_decode(b, &clh);
        if (c < 0) return -1;
        if (c < 16) lens[i++] = (uint8_t)c;
        else if (c == 16) {
            if (i == 0) return -1;
            int n = (int)bits_get(b, 2) + 3;
            uint8_t prev = lens[i - 1];
            while (n-- && i < total) lens[i++] = prev;
        } else if (c == 17) {
            int n = (int)bits_get(b, 3) + 3;
            while (n-- && i < total) lens[i++] = 0;
        } else {
            int n = (int)bits_get(b, 7) + 11;
            while (n-- && i < total) lens[i++] = 0;
        }
    }
    if (b->err) return -1;

    struct huff lit, dist;
    huff_build(&lit, lens, hlit);
    huff_build(&dist, lens + hlit, hdist);

    for (;;) {
        int sym = huff_decode(b, &lit);
        if (sym < 0) return -1;
        if (sym < 256) { if (out_put(o, (uint8_t)sym) < 0) return -1; }
        else if (sym == 256) return 0;
        else {
            int li = sym - 257;
            if (li >= 29) return -1;
            size_t length = len_base[li] + bits_get(b, len_extra[li]);
            int ds = huff_decode(b, &dist);
            if (ds < 0 || ds >= 30) return -1;
            size_t d = dist_base[ds] + bits_get(b, dist_extra[ds]);
            if (out_copy(o, d, length) < 0) return -1;
        }
        if (b->err) return -1;
    }
}

/* Inflate a zlib stream (2-byte header + DEFLATE; Adler-32 not verified). */
static int zlib_inflate(const uint8_t *in, size_t len, struct outbuf *o)
{
    if (len < 2 || (in[0] & 0x0f) != 8) return -1;
    struct bits b;
    memset(&b, 0, sizeof b);
    b.in = in; b.len = len; b.pos = 2;
    for (;;) {
        int last = (int)bits_get(&b, 1);
        int type = (int)bits_get(&b, 2);
        if (b.err) return -1;
        if (type == 0) {
            b.buf = 0; b.nbits = 0;                  /* align to byte */
            if (b.pos + 4 > b.len) return -1;
            unsigned l = b.in[b.pos] | (b.in[b.pos + 1] << 8);
            unsigned nl = b.in[b.pos + 2] | (b.in[b.pos + 3] << 8);
            b.pos += 4;
            if ((l ^ 0xffffu) != nl) return -1;
            if (b.pos + l > b.len) return -1;
            for (unsigned k = 0; k < l; k++)
                if (out_put(o, b.in[b.pos++]) < 0) return -1;
        } else if (type == 1) {
            if (inflate_fixed(&b, o) < 0) return -1;
        } else if (type == 2) {
            if (inflate_dynamic(&b, o) < 0) return -1;
        } else {
            return -1;
        }
        if (last) break;
    }
    return o->err ? -1 : 0;
}

/* --------------------------------------------------------------------- PNG */
/* 8-bit value of sample `idx` in a row of `depth`-bit samples. */
static unsigned sample8(const uint8_t *row, int idx, int depth)
{
    if (depth == 8) return row[idx];
    if (depth == 16) return row[(size_t)idx * 2];    /* big-endian: high byte */
    int per = 8 / depth;
    int byte = idx / per;
    int shift = 8 - depth * ((idx % per) + 1);
    return (unsigned)((row[byte] >> shift) & ((1u << depth) - 1));
}

static unsigned scale_gray(unsigned v, int depth)
{
    if (depth >= 8) return v;                        /* 8, or high byte of 16 */
    return v * 255u / ((1u << depth) - 1);
}

int slweb_png_decode(const uint8_t *data, size_t len, struct slweb_image *out)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (len < 8 || memcmp(data, sig, 8) != 0) return -1;

    int w = 0, h = 0, depth = 0, color = 0, interlace = 0;
    uint8_t pal[256 * 3];
    uint8_t trns[256];
    int have_pal = 0;
    struct outbuf idat;
    memset(&idat, 0, sizeof idat);
    memset(trns, 255, sizeof trns);

    size_t p = 8;
    while (p + 12 <= len) {
        uint32_t clen = ((uint32_t)data[p] << 24) | ((uint32_t)data[p + 1] << 16) |
                        ((uint32_t)data[p + 2] << 8) | data[p + 3];
        const uint8_t *type = data + p + 4;
        const uint8_t *cdata = data + p + 8;
        if (p + 12 + clen > len) { free(idat.p); return -1; }
        if (memcmp(type, "IHDR", 4) == 0 && clen >= 13) {
            w = (int)(((uint32_t)cdata[0] << 24) | (cdata[1] << 16) | (cdata[2] << 8) | cdata[3]);
            h = (int)(((uint32_t)cdata[4] << 24) | (cdata[5] << 16) | (cdata[6] << 8) | cdata[7]);
            depth = cdata[8];
            color = cdata[9];
            interlace = cdata[12];
            if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || interlace != 0) { free(idat.p); return -1; }
        } else if (memcmp(type, "PLTE", 4) == 0) {
            int n = (int)(clen / 3);
            if (n > 256) n = 256;
            memcpy(pal, cdata, (size_t)n * 3);
            have_pal = 1;
        } else if (memcmp(type, "tRNS", 4) == 0 && color == 3) {
            if (clen > 256) return -1;
            memcpy(trns, cdata, clen);
        } else if (memcmp(type, "IDAT", 4) == 0) {
            for (uint32_t k = 0; k < clen; k++)
                if (out_put(&idat, cdata[k]) < 0) return -1;
        }
        p += 12 + (size_t)clen;
    }

    int ok = 1;
    int channels = 1;
    if (w <= 0 || h <= 0) ok = 0;
    switch (color) {
    case 0: channels = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) ok = 0; break;
    case 2: channels = 3; if (depth != 8 && depth != 16) ok = 0; break;
    case 3: channels = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8) ok = 0; if (!have_pal) ok = 0; break;
    case 4: channels = 2; if (depth != 8 && depth != 16) ok = 0; break;
    case 6: channels = 4; if (depth != 8 && depth != 16) ok = 0; break;
    default: ok = 0; break;
    }
    if (interlace != 0) ok = 0;
    if (!ok || idat.len == 0) { free(idat.p); return -1; }

    size_t bytes_per_pixel = (size_t)(channels * depth + 7) / 8;
    if (bytes_per_pixel < 1) bytes_per_pixel = 1;
    size_t rowbytes = ((size_t)w * (size_t)channels * (size_t)depth + 7) / 8;
    size_t expected = (size_t)h * (rowbytes + 1);

    struct outbuf raw;
    memset(&raw, 0, sizeof raw);
    if (zlib_inflate(idat.p, idat.len, &raw) != 0) { free(idat.p); free(raw.p); return -1; }
    free(idat.p);
    if (raw.len < expected) { free(raw.p); return -1; }

    /* unfilter in place */
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw.p + (size_t)y * (rowbytes + 1);
        int ft = row[0];
        uint8_t *cur = row + 1;
        const uint8_t *prev = (y > 0) ? cur - (rowbytes + 1) : NULL;
        for (size_t x = 0; x < rowbytes; x++) {
            int a = (x >= bytes_per_pixel) ? cur[x - bytes_per_pixel] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= bytes_per_pixel) ? prev[x - bytes_per_pixel] : 0;
            switch (ft) {
            case 0: break;
            case 1: cur[x] = (uint8_t)(cur[x] + a); break;
            case 2: cur[x] = (uint8_t)(cur[x] + b); break;
            case 3: cur[x] = (uint8_t)(cur[x] + ((a + b) >> 1)); break;
            case 4: {
                int pp = a + b - c;
                int pa = pp - a; if (pa < 0) pa = -pa;
                int pb = pp - b; if (pb < 0) pb = -pb;
                int pc = pp - c; if (pc < 0) pc = -pc;
                int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                cur[x] = (uint8_t)(cur[x] + pr);
                break;
            }
            default: free(raw.p); return -1;
            }
        }
    }

    uint32_t *px = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!px) { free(raw.p); return -1; }
    int has_alpha = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = raw.p + (size_t)y * (rowbytes + 1) + 1;
        for (int x = 0; x < w; x++) {
            unsigned r = 0, g = 0, b = 0, a = 255;
            if (color == 0) {
                r = g = b = scale_gray(sample8(row, x, depth), depth);
            } else if (color == 2) {
                r = sample8(row, x * 3 + 0, depth);
                g = sample8(row, x * 3 + 1, depth);
                b = sample8(row, x * 3 + 2, depth);
            } else if (color == 3) {
                unsigned idx = sample8(row, x, depth);
                r = pal[idx * 3 + 0]; g = pal[idx * 3 + 1]; b = pal[idx * 3 + 2];
                a = trns[idx];
            } else if (color == 4) {
                g = r = b = sample8(row, x * 2 + 0, depth);
                a = sample8(row, x * 2 + 1, depth);
            } else { /* color == 6 */
                r = sample8(row, x * 4 + 0, depth);
                g = sample8(row, x * 4 + 1, depth);
                b = sample8(row, x * 4 + 2, depth);
                a = sample8(row, x * 4 + 3, depth);
            }
            if (a != 255) has_alpha = 1;
            px[(size_t)y * w + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    free(raw.p);
    out->w = w; out->h = h; out->has_alpha = has_alpha; out->pixels = px;
    return 0;
}
