/*
 * libslweb/image.c -- M6: bitmap images for the browser.
 *
 * The layout engine only needs pixels, so this module turns a blob of bytes
 * into a struct slweb_image (0xAARRGGBB, row-major):
 *
 *   - PNG:        baseline, non-interlaced (see png.c for the DEFLATE decoder);
 *   - "BS64":     the StarlightOS raw "basesize64" bitmap -- a 12-byte header
 *                 (magic "BS64", version, flags, 16-bit width/height) followed by
 *                 width*height RGB (bpp 3) or RGBA (bpp 4) bytes;
 *   - base64 + data: URIs: decode("data:image/png;base64,....") inline.
 *
 * slweb_image_format() sniffs a blob so the browser can pick a decoder from the
 * moved bytes without trusting the Content-Type.
 */
#include <stdlib.h>
#include <string.h>
#include "slweb.h"

/* ---------------------------------------------------------------- base64 */
long slweb_base64_decode(const char *in, size_t inlen, uint8_t *out, size_t cap)
{
    size_t o = 0;
    int quad = 0;
    unsigned acc = 0;
    for (size_t i = 0; i < inlen; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        if (c == '=') break;
        int v = 0;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else return -1;                       /* invalid byte */
        acc = (acc << 6) | (unsigned)v;
        if (++quad == 4) {
            if (o < cap) out[o] = (uint8_t)(acc >> 16); else return -1;
            o++;
            if (o < cap) out[o] = (uint8_t)(acc >> 8); else return -1;
            o++;
            if (o < cap) out[o] = (uint8_t)acc; else return -1;
            o++;
            quad = 0; acc = 0;
        }
    }
    /* trailing partial quantum */
    if (quad == 2) { if (o + 1 > cap) return -1; out[o++] = (uint8_t)(acc >> 4); }
    else if (quad == 3) {
        if (o + 2 > cap) return -1;
        out[o++] = (uint8_t)(acc >> 10);
        out[o++] = (uint8_t)(acc >> 2);
    } else if (quad == 1) return -1;          /* 6 bits is not a byte */
    return (long)o;
}

/* ------------------------------------------------------------------ sniff */
const char *slweb_image_format(const uint8_t *data, size_t len)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (len >= 8 && memcmp(data, sig, 8) == 0) return "png";
    if (len >= 4 && data[0] == 'B' && data[1] == 'S' && data[2] == '6' && data[3] == '4')
        return "bs64";
    return NULL;
}

int slweb_image_decode(const uint8_t *data, size_t len, struct slweb_image *out)
{
    const char *fmt = slweb_image_format(data, len);
    if (!fmt) return -1;
    if (fmt[0] == 'p') return slweb_png_decode(data, len, out);
    return slweb_bs64_decode(data, len, out);
}

void slweb_image_free(struct slweb_image *img)
{
    if (!img) return;
    free(img->pixels);
    img->pixels = NULL;
    img->w = img->h = img->has_alpha = 0;
}

/* ------------------------------------------------------------- BS64 raw ---- */
/*
 * The "basesize64" raw bitmap: a tiny little-endian header followed by tightly
 * packed samples. It is the format the browser's own test fixtures use so the
 * image path can be exercised without a full PNG on disk.
 *
 *   0  'B' 'S' '6' '4'      magic
 *   4  version             1
 *   5  flags               bit0 = 4 channels (RGBA), else 3 (RGB)
 *   6  reserved (0)
 *   7  reserved (0)
 *   8  width               uint16 little-endian
 *  10  height              uint16 little-endian
 *  12  pixels              w*h*(3|4) bytes, row-major, top-left first
 */
int slweb_bs64_decode(const uint8_t *data, size_t len, struct slweb_image *out)
{
    if (len < 12 || slweb_image_format(data, len) == NULL) return -1;
    if (data[0] != 'B' || data[1] != 'S') return -1;
    if (data[4] != 1) return -1;
    int flags = data[5];
    int alpha = flags & 1;
    int w = data[8] | (data[9] << 8);
    int h = data[10] | (data[11] << 8);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return -1;
    int bpp = alpha ? 4 : 3;
    size_t need = (size_t)w * (size_t)h * (size_t)bpp;
    if (12 + need > len) return -1;
    uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4);
    if (!px) return -1;
    const uint8_t *p = data + 12;
    int has_alpha = 0;
    for (int i = 0; i < w * h; i++) {
        uint32_t r = p[0], g = p[1], b = p[2], a = alpha ? p[3] : 255;
        p += bpp;
        if (a != 255) has_alpha = 1;
        px[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    out->w = w; out->h = h; out->has_alpha = has_alpha; out->pixels = px;
    return 0;
}

/* ------------------------------------------------------------- data: URI */
int slweb_image_data_uri(const char *uri, struct slweb_image *out)
{
    if (!uri || strncmp(uri, "data:", 5) != 0) return -1;
    const char *comma = strchr(uri + 5, ',');
    if (!comma) return -1;
    const char *meta = uri + 5;
    size_t metalen = (size_t)(comma - meta);
    int is_b64 = 0;
    for (size_t i = 0; i + 6 <= metalen; i++)
        if (strncmp(meta + i, "base64", 6) == 0) { is_b64 = 1; break; }
    if (!is_b64) return -1;

    const char *b = comma + 1;
    size_t blen = strlen(b);
    size_t cap = blen / 4 * 3 + 4;
    uint8_t *buf = (uint8_t *)malloc(cap ? cap : 1);
    if (!buf) return -1;
    long n = slweb_base64_decode(b, blen, buf, cap);
    int rc = (n > 0) ? slweb_image_decode(buf, (size_t)n, out) : -1;
    free(buf);
    return rc;
}
