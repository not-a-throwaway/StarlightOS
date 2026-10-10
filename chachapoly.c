/*
 * chachapoly.c -- ChaCha20-Poly1305 AEAD (RFC 8439). Poly1305 uses 5x26-bit limbs so every product fits in
 * 64 bits (no __int128). In software ChaCha20 is several times faster than our table-less AES, which is why
 * the TLS client offers it first.
 */
#include "crypto.h"
#include "kstring.h"

static uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t ld32le(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void st32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

#define QR(a, b, c, d) \
    a += b; d ^= a; d = rotl32(d, 16); c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d, 8);  c += d; b ^= c; b = rotl32(b, 7)

static void chacha_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64])
{
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = ld32le(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = ld32le(nonce + 4 * i);
    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int r = 0; r < 10; r++) {
        QR(x[0], x[4], x[8],  x[12]); QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]); QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) st32le(out + 4 * i, x[i] + s[i]);
}

static void chacha_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                       const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t ks[64];
    for (size_t off = 0; off < len; off += 64, counter++) {
        chacha_block(key, counter, nonce, ks);
        size_t n = len - off < 64 ? len - off : 64;
        for (size_t i = 0; i < n; i++) out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
    }
}

/* ---------------------------------------------------------------- Poly1305 */
struct poly { uint32_t r[5], h[5], pad[4]; uint8_t buf[16]; size_t left; };

static void poly_init(struct poly *p, const uint8_t key[32])
{
    uint32_t t0 = ld32le(key), t1 = ld32le(key + 4), t2 = ld32le(key + 8), t3 = ld32le(key + 12);
    p->r[0] = t0 & 0x3ffffff;
    p->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
    p->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
    p->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
    p->r[4] = (t3 >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) p->h[i] = 0;
    for (int i = 0; i < 4; i++) p->pad[i] = ld32le(key + 16 + 4 * i);
    p->left = 0;
}

static void poly_blocks(struct poly *p, const uint8_t *m, size_t bytes, uint32_t hibit)
{
    uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    while (bytes >= 16) {
        uint32_t t0 = ld32le(m), t1 = ld32le(m + 4), t2 = ld32le(m + 8), t3 = ld32le(m + 12);
        h0 += t0 & 0x3ffffff;
        h1 += ((t0 >> 26) | (t1 << 6)) & 0x3ffffff;
        h2 += ((t1 >> 20) | (t2 << 12)) & 0x3ffffff;
        h3 += ((t2 >> 14) | (t3 << 18)) & 0x3ffffff;
        h4 += (t3 >> 8) | hibit;
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff; d1 += c;
        c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff; d2 += c;
        c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff; d3 += c;
        c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff; d4 += c;
        c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
        m += 16; bytes -= 16;
    }
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

static void poly_update(struct poly *p, const uint8_t *m, size_t n)
{
    if (p->left) {
        size_t want = 16 - p->left; if (want > n) want = n;
        memcpy(p->buf + p->left, m, want); p->left += want; m += want; n -= want;
        if (p->left < 16) return;
        poly_blocks(p, p->buf, 16, 1u << 24); p->left = 0;
    }
    if (n >= 16) { size_t b = n & ~(size_t)15; poly_blocks(p, m, b, 1u << 24); m += b; n -= b; }
    if (n) { memcpy(p->buf, m, n); p->left = n; }
}

static void poly_finish(struct poly *p, uint8_t mac[16])
{
    if (p->left) {
        p->buf[p->left] = 1;
        for (size_t i = p->left + 1; i < 16; i++) p->buf[i] = 0;
        poly_blocks(p, p->buf, 16, 0);
    }
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);

    uint32_t mask = (g4 >> 31) - 1;                    /* all ones if h >= p (g4 did not go negative) */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;

    h0 = (h0 | (h1 << 26));
    h1 = ((h1 >> 6) | (h2 << 20));
    h2 = ((h2 >> 12) | (h3 << 14));
    h3 = ((h3 >> 18) | (h4 << 8));

    uint64_t f;
    f = (uint64_t)h0 + p->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + p->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + p->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + p->pad[3] + (f >> 32); h3 = (uint32_t)f;
    st32le(mac, h0); st32le(mac + 4, h1); st32le(mac + 8, h2); st32le(mac + 12, h3);
}

/* ---------------------------------------------------------------- AEAD */
static void aead_tag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                     const uint8_t *ct, size_t clen, uint8_t tag[16])
{
    uint8_t block0[64], zeros[16] = { 0 }, lens[16];
    chacha_block(key, 0, nonce, block0);
    struct poly p; poly_init(&p, block0);
    poly_update(&p, aad, alen);
    if (alen & 15) poly_update(&p, zeros, 16 - (alen & 15));
    poly_update(&p, ct, clen);
    if (clen & 15) poly_update(&p, zeros, 16 - (clen & 15));
    for (int i = 0; i < 8; i++) { lens[i] = (uint8_t)((uint64_t)alen >> (8 * i)); lens[8 + i] = (uint8_t)((uint64_t)clen >> (8 * i)); }
    poly_update(&p, lens, 16);
    poly_finish(&p, tag);
    memset(block0, 0, sizeof(block0));
}

void chachapoly_encrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                        const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16])
{
    chacha_xor(key, 1, nonce, in, len, out);
    aead_tag(key, nonce, aad, alen, out, len, tag);
}

int chachapoly_decrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                       const uint8_t *in, size_t len, const uint8_t tag[16], uint8_t *out)
{
    uint8_t expect[16];
    aead_tag(key, nonce, aad, alen, in, len, expect);
    if (ct_memcmp(expect, tag, 16) != 0) return -1;
    chacha_xor(key, 1, nonce, in, len, out);
    return 0;
}
