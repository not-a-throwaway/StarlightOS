/*
 * crypto.c -- SHA-256, HMAC, HKDF, AES-128-GCM and X25519 for the TLS client.
 * See crypto.h. X25519 follows the compact TweetNaCl formulation (16 x 16-bit
 * limbs in int64_t), which needs only add/sub/mul/shift -- no division.
 */
#include "crypto.h"
#include "kstring.h"

int ct_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d;
}

/* =========================================================== SHA-256 */
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(struct sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i-15], 7) ^ ROR32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR32(w[i-2], 17) ^ ROR32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_init(struct sha256_ctx *c)
{
    static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    for (int i = 0; i < 8; i++) c->h[i] = iv[i];
    c->buflen = 0; c->total = 0;
}

void sha256_update(struct sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c->total += len;
    if (c->buflen) {
        size_t take = 64 - c->buflen; if (take > len) take = len;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += (uint32_t)take; p += take; len -= take;
        if (c->buflen == 64) { sha256_block(c, c->buf); c->buflen = 0; }
    }
    while (len >= 64) { sha256_block(c, p); p += 64; len -= 64; }
    if (len) { memcpy(c->buf, p, len); c->buflen = (uint32_t)len; }
}

void sha256_final(struct sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->buflen != 56) sha256_update(c, &z, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4] = (uint8_t)(c->h[i] >> 24); out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8); out[i*4+3] = (uint8_t)c->h[i];
    }
}

void sha256(const void *data, size_t len, uint8_t out[32])
{
    struct sha256_ctx c; sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, out);
}

/* ========================================================== SHA-512 / SHA-384 */
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ull,0x7137449123ef65cdull,0xb5c0fbcfec4d3b2full,0xe9b5dba58189dbbcull,0x3956c25bf348b538ull,
    0x59f111f1b605d019ull,0x923f82a4af194f9bull,0xab1c5ed5da6d8118ull,0xd807aa98a3030242ull,0x12835b0145706fbeull,
    0x243185be4ee4b28cull,0x550c7dc3d5ffb4e2ull,0x72be5d74f27b896full,0x80deb1fe3b1696b1ull,0x9bdc06a725c71235ull,
    0xc19bf174cf692694ull,0xe49b69c19ef14ad2ull,0xefbe4786384f25e3ull,0x0fc19dc68b8cd5b5ull,0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull,0x4a7484aa6ea6e483ull,0x5cb0a9dcbd41fbd4ull,0x76f988da831153b5ull,0x983e5152ee66dfabull,
    0xa831c66d2db43210ull,0xb00327c898fb213full,0xbf597fc7beef0ee4ull,0xc6e00bf33da88fc2ull,0xd5a79147930aa725ull,
    0x06ca6351e003826full,0x142929670a0e6e70ull,0x27b70a8546d22ffcull,0x2e1b21385c26c926ull,0x4d2c6dfc5ac42aedull,
    0x53380d139d95b3dfull,0x650a73548baf63deull,0x766a0abb3c77b2a8ull,0x81c2c92e47edaee6ull,0x92722c851482353bull,
    0xa2bfe8a14cf10364ull,0xa81a664bbc423001ull,0xc24b8b70d0f89791ull,0xc76c51a30654be30ull,0xd192e819d6ef5218ull,
    0xd69906245565a910ull,0xf40e35855771202aull,0x106aa07032bbd1b8ull,0x19a4c116b8d2d0c8ull,0x1e376c085141ab53ull,
    0x2748774cdf8eeb99ull,0x34b0bcb5e19b48a8ull,0x391c0cb3c5c95a63ull,0x4ed8aa4ae3418acbull,0x5b9cca4f7763e373ull,
    0x682e6ff3d6b2b8a3ull,0x748f82ee5defb2fcull,0x78a5636f43172f60ull,0x84c87814a1f0ab72ull,0x8cc702081a6439ecull,
    0x90befffa23631e28ull,0xa4506cebde82bde9ull,0xbef9a3f7b2c67915ull,0xc67178f2e372532bull,0xca273eceea26619cull,
    0xd186b8c721c0c207ull,0xeada7dd6cde0eb1eull,0xf57d4f7fee6ed178ull,0x06f067aa72176fbaull,0x0a637dc5a2c898a6ull,
    0x113f9804bef90daeull,0x1b710b35131c471bull,0x28db77f523047d84ull,0x32caab7b40c72493ull,0x3c9ebe0a15c9bebcull,
    0x431d67c49c100d4cull,0x4cc5d4becb3e42b6ull,0x597f299cfc657e2aull,0x5fcb6fab3ad6faecull,0x6c44198c4a475817ull
};
#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha512_block(struct sha512_ctx *c, const uint8_t *p)
{
    uint64_t w[80], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++) {
        uint64_t v = 0;
        for (int j = 0; j < 8; j++) v = (v << 8) | p[i*8 + j];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(w[i-15], 1) ^ ROR64(w[i-15], 8) ^ (w[i-15] >> 7);
        uint64_t s1 = ROR64(w[i-2], 19) ^ ROR64(w[i-2], 61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39);
        uint64_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint64_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha512_init(struct sha512_ctx *c)
{
    static const uint64_t iv[8] = { 0x6a09e667f3bcc908ull,0xbb67ae8584caa73bull,0x3c6ef372fe94f82bull,0xa54ff53a5f1d36f1ull,
                                    0x510e527fade682d1ull,0x9b05688c2b3e6c1full,0x1f83d9abfb41bd6bull,0x5be0cd19137e2179ull };
    for (int i = 0; i < 8; i++) c->h[i] = iv[i];
    c->buflen = 0; c->total = 0; c->is384 = 0;
}

void sha384_init(struct sha512_ctx *c)
{
    static const uint64_t iv[8] = { 0xcbbb9d5dc1059ed8ull,0x629a292a367cd507ull,0x9159015a3070dd17ull,0x152fecd8f70e5939ull,
                                    0x67332667ffc00b31ull,0x8eb44a8768581511ull,0xdb0c2e0d64f98fa7ull,0x47b5481dbefa4fa4ull };
    for (int i = 0; i < 8; i++) c->h[i] = iv[i];
    c->buflen = 0; c->total = 0; c->is384 = 1;
}

void sha512_update(struct sha512_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c->total += len;
    if (c->buflen) {
        size_t take = 128 - c->buflen; if (take > len) take = len;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += (uint32_t)take; p += take; len -= take;
        if (c->buflen == 128) { sha512_block(c, c->buf); c->buflen = 0; }
    }
    while (len >= 128) { sha512_block(c, p); p += 128; len -= 128; }
    if (len) { memcpy(c->buf, p, len); c->buflen = (uint32_t)len; }
}

void sha512_final(struct sha512_ctx *c, uint8_t *out)
{
    uint64_t bits = c->total * 8;                       /* messages < 2^61 bytes: the high length word is 0 */
    uint8_t pad = 0x80, z = 0;
    sha512_update(c, &pad, 1);
    while (c->buflen != 112) sha512_update(c, &z, 1);
    uint8_t lenb[16] = { 0 };
    for (int i = 0; i < 8; i++) lenb[8 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha512_update(c, lenb, 16);
    int words = c->is384 ? 6 : 8;
    for (int i = 0; i < words; i++)
        for (int j = 0; j < 8; j++) out[i*8 + j] = (uint8_t)(c->h[i] >> (56 - 8 * j));
}

void sha384(const void *data, size_t len, uint8_t out[48])
{
    struct sha512_ctx c; sha384_init(&c); sha512_update(&c, data, len); sha512_final(&c, out);
}

/* ================================================== hash-agnostic layer */
size_t hash_len(int alg) { return alg == HASH_SHA384 ? 48 : 32; }
static size_t hash_block(int alg) { return alg == HASH_SHA384 ? 128 : 64; }

void hash_init(struct hash_ctx *c, int alg)
{
    c->alg = alg;
    if (alg == HASH_SHA384) sha384_init(&c->s512); else sha256_init(&c->s256);
}
void hash_update(struct hash_ctx *c, const void *data, size_t len)
{
    if (c->alg == HASH_SHA384) sha512_update(&c->s512, data, len); else sha256_update(&c->s256, data, len);
}
void hash_final(struct hash_ctx *c, uint8_t *out)
{
    if (c->alg == HASH_SHA384) sha512_final(&c->s512, out); else sha256_final(&c->s256, out);
}
void hash_data(int alg, const void *data, size_t len, uint8_t *out)
{
    struct hash_ctx c; hash_init(&c, alg); hash_update(&c, data, len); hash_final(&c, out);
}

/* ================================================== HMAC / HKDF / PRF */
void hmac_hash(int alg, const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t *out)
{
    size_t bl = hash_block(alg), hl = hash_len(alg);
    uint8_t k[128], ipad[128], opad[128], inner[HASH_MAX_LEN];
    memset(k, 0, bl);
    if (klen > bl) hash_data(alg, key, klen, k); else if (klen) memcpy(k, key, klen);
    for (size_t i = 0; i < bl; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    struct hash_ctx c;
    hash_init(&c, alg); hash_update(&c, ipad, bl); hash_update(&c, msg, mlen); hash_final(&c, inner);
    hash_init(&c, alg); hash_update(&c, opad, bl); hash_update(&c, inner, hl); hash_final(&c, out);
}

void hmac_sha256(const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32])
{
    hmac_hash(HASH_SHA256, key, klen, msg, mlen, out);
}

void hkdf_extract_h(int alg, const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, uint8_t *prk)
{
    uint8_t zeros[HASH_MAX_LEN] = { 0 };
    if (!salt || !slen) { salt = zeros; slen = hash_len(alg); }
    hmac_hash(alg, salt, slen, ikm, ilen, prk);
}

void hkdf_expand_h(int alg, const uint8_t *prk, size_t plen, const uint8_t *info, size_t ilen, uint8_t *out, size_t olen)
{
    size_t hl = hash_len(alg);
    uint8_t t[HASH_MAX_LEN] = { 0 }; size_t tl = 0, pos = 0; uint8_t counter = 1;
    while (pos < olen) {
        uint8_t msg[HASH_MAX_LEN + 128 + 1]; size_t ml = 0;
        if (ilen > 128) ilen = 128;
        memcpy(msg, t, tl); ml = tl;
        if (ilen) memcpy(msg + ml, info, ilen);
        ml += ilen; msg[ml++] = counter++;
        hmac_hash(alg, prk, plen, msg, ml, t); tl = hl;
        size_t n = olen - pos < hl ? olen - pos : hl;
        memcpy(out + pos, t, n); pos += n;
    }
}

void hkdf_extract(const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, uint8_t prk[32])
{
    hkdf_extract_h(HASH_SHA256, salt, slen, ikm, ilen, prk);
}

void hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t ilen, uint8_t *out, size_t olen)
{
    hkdf_expand_h(HASH_SHA256, prk, 32, info, ilen, out, olen);
}

/* P_hash(secret, seed) with seed = label || seed: A(i) = HMAC(secret, A(i-1)), out = HMAC(secret, A(i) || seed) ... */
void tls12_prf(int alg, const uint8_t *secret, size_t slen, const char *label,
               const uint8_t *seed, size_t seedlen, uint8_t *out, size_t olen)
{
    size_t hl = hash_len(alg), ll = strlen(label), pos = 0;
    uint8_t ls[160];                                    /* label || seed: at most 24 + 64 bytes in TLS */
    if (ll + seedlen > sizeof(ls)) return;
    memcpy(ls, label, ll); memcpy(ls + ll, seed, seedlen);
    size_t lsl = ll + seedlen;
    uint8_t A[HASH_MAX_LEN], blk[HASH_MAX_LEN], buf[HASH_MAX_LEN + 160];
    hmac_hash(alg, secret, slen, ls, lsl, A);                          /* A(1) */
    while (pos < olen) {
        memcpy(buf, A, hl); memcpy(buf + hl, ls, lsl);
        hmac_hash(alg, secret, slen, buf, hl + lsl, blk);
        size_t n = olen - pos < hl ? olen - pos : hl;
        memcpy(out + pos, blk, n); pos += n;
        hmac_hash(alg, secret, slen, A, hl, A);                        /* A(i+1) */
    }
}

/* ======================================================== AES-128 / AES-256 */
static uint8_t sbox[256];
static int sbox_ready;

static uint8_t rotl8(uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); }

static void make_sbox(void)
{
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));        /* p *= 3 in GF(2^8) */
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;                                      /* q /= 3 */
        uint8_t x = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
        sbox[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
    sbox_ready = 1;
}

void aes_init(struct aes *a, const uint8_t *key, int keylen)
{
    if (!sbox_ready) make_sbox();
    int nk = keylen / 4;
    a->rounds = nk + 6;
    int total = 16 * (a->rounds + 1);
    memcpy(a->rk, key, (size_t)keylen);
    uint8_t rcon = 1;
    for (int i = keylen; i < total; i += 4) {
        uint8_t t[4]; memcpy(t, a->rk + i - 4, 4);
        int w = i / 4;
        if (w % nk == 0) {
            uint8_t k = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon); t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[k];
            rcon = (uint8_t)((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0));
        } else if (nk > 6 && w % nk == 4) {
            for (int j = 0; j < 4; j++) t[j] = sbox[t[j]];
        }
        for (int j = 0; j < 4; j++) a->rk[i + j] = (uint8_t)(a->rk[i - keylen + j] ^ t[j]);
    }
}

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

void aes_encrypt_block(const struct aes *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ a->rk[i]);
    for (int round = 1; round <= a->rounds; round++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = sbox[s[i]];
        /* ShiftRows (state is column-major: s[col*4 + row]) */
        uint8_t u[16];
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) u[c*4 + r] = t[((c + r) & 3) * 4 + r];
        if (round < a->rounds) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = u + c*4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                col[0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
                col[1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
                col[2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
                col[3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
            }
        }
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(u[i] ^ a->rk[round * 16 + i]);
    }
    memcpy(out, s, 16);
}

/* ============================================================ GCM */
static uint64_t ld64be(const uint8_t *p)
{
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) | ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) | ((uint64_t)p[6] << 8) | p[7];
}
static void st64be(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i)); }

/* GHASH with Shoup's 4-bit method: 16 table lookups per block instead of 128 conditional shifts. The
 * tables hold H * {0..15} in GCM bit order; `last4` folds the 4 bits shifted out of the bottom back in. */
static const uint64_t last4[16] = {
    0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
    0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0
};

static void gcm_tables(struct gcm *g, uint64_t vh, uint64_t vl)
{
    g->hl[0] = g->hh[0] = 0;
    g->hl[8] = vl; g->hh[8] = vh;
    for (int i = 4; i > 0; i >>= 1) {
        uint64_t t = (vl & 1u) ? 0xE100000000000000ull : 0;
        vl = (vh << 63) | (vl >> 1);
        vh = (vh >> 1) ^ t;
        g->hl[i] = vl; g->hh[i] = vh;
    }
    for (int i = 2; i < 16; i <<= 1) {
        uint64_t h = g->hh[i], l = g->hl[i];
        for (int j = 1; j < i; j++) { g->hh[i + j] = h ^ g->hh[j]; g->hl[i + j] = l ^ g->hl[j]; }
    }
}

/* y <- y * H */
static void gcm_mult(const struct gcm *g, uint8_t y[16])
{
    uint8_t lo = (uint8_t)(y[15] & 0x0F), hi = (uint8_t)(y[15] >> 4);
    uint64_t zh = g->hh[lo], zl = g->hl[lo];
    for (int i = 15; i >= 0; i--) {
        lo = (uint8_t)(y[i] & 0x0F); hi = (uint8_t)(y[i] >> 4);
        if (i != 15) {
            uint8_t rem = (uint8_t)(zl & 0x0F);
            zl = (zh << 60) | (zl >> 4); zh >>= 4; zh ^= last4[rem] << 48;
            zh ^= g->hh[lo]; zl ^= g->hl[lo];
        }
        uint8_t rem = (uint8_t)(zl & 0x0F);
        zl = (zh << 60) | (zl >> 4); zh >>= 4; zh ^= last4[rem] << 48;
        zh ^= g->hh[hi]; zl ^= g->hl[hi];
    }
    st64be(y, zh); st64be(y + 8, zl);
}

static void ghash_blocks(const struct gcm *g, uint8_t y[16], const uint8_t *data, size_t len)
{
    while (len) {
        size_t n = len < 16 ? len : 16;
        for (size_t i = 0; i < n; i++) y[i] ^= data[i];
        gcm_mult(g, y);
        data += n; len -= n;
    }
}

void gcm_init(struct gcm *g, const uint8_t *key, int keylen)
{
    uint8_t zero[16], h[16];
    memset(zero, 0, 16);
    aes_init(&g->aes, key, keylen);
    aes_encrypt_block(&g->aes, zero, h);
    gcm_tables(g, ld64be(h), ld64be(h + 8));
}

static void gcm_ctr(const struct gcm *g, const uint8_t iv[12], const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 12);
    uint32_t n = 2;                                   /* counter 1 is reserved for the tag mask */
    for (size_t off = 0; off < len; off += 16) {
        ctr[12] = (uint8_t)(n >> 24); ctr[13] = (uint8_t)(n >> 16); ctr[14] = (uint8_t)(n >> 8); ctr[15] = (uint8_t)n;
        n++;
        aes_encrypt_block(&g->aes, ctr, ks);
        size_t m = len - off < 16 ? len - off : 16;
        for (size_t i = 0; i < m; i++) out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
    }
}

static void gcm_tag(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen,
                    const uint8_t *ct, size_t clen, uint8_t tag[16])
{
    uint8_t y[16] = { 0 };
    ghash_blocks(g, y, aad, alen);
    ghash_blocks(g, y, ct, clen);
    uint8_t lens[16];
    st64be(lens, (uint64_t)alen * 8); st64be(lens + 8, (uint64_t)clen * 8);
    ghash_blocks(g, y, lens, 16);
    uint8_t j0[16], mask[16];
    memcpy(j0, iv, 12); j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    aes_encrypt_block(&g->aes, j0, mask);
    for (int i = 0; i < 16; i++) tag[i] = (uint8_t)(y[i] ^ mask[i]);
}

void gcm_encrypt(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen,
                 const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16])
{
    gcm_ctr(g, iv, in, len, out);
    gcm_tag(g, iv, aad, alen, out, len, tag);
}

int gcm_decrypt(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen,
                const uint8_t *in, size_t len, const uint8_t tag[16], uint8_t *out)
{
    uint8_t expect[16];
    gcm_tag(g, iv, aad, alen, in, len, expect);
    if (ct_memcmp(expect, tag, 16) != 0) return -1;
    gcm_ctr(g, iv, in, len, out);
    return 0;
}

/* ========================================================== X25519 */
typedef int64_t gf[16];
static const gf C121665 = { 0xDB41, 1 };

static void car25519(gf o)
{
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}

static void sel25519(gf p, gf q, int b)
{
    int64_t c = ~((int64_t)b - 1);
    for (int i = 0; i < 16; i++) { int64_t t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}

static void pack25519(uint8_t *o, const gf n)
{
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2*i] = (uint8_t)(t[i] & 0xff); o[2*i+1] = (uint8_t)(t[i] >> 8); }
}

static void unpack25519(gf o, const uint8_t *n)
{
    for (int i = 0; i < 16; i++) o[i] = n[2*i] + ((int64_t)n[2*i+1] << 8);
    o[15] &= 0x7fff;
}

static void fe_add(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fe_sub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void fe_mul(gf o, const gf a, const gf b)
{
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i+16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}

static void fe_sq(gf o, const gf a) { fe_mul(o, a, a); }

static void inv25519(gf o, const gf i)
{
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        fe_sq(c, c);
        if (a != 2 && a != 4) fe_mul(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    uint8_t z[32]; int64_t x[80]; gf a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (uint8_t)((scalar[31] & 127) | 64);
    z[0] &= 248;
    unpack25519(x, point);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r); sel25519(c, d, r);
        fe_add(e, a, c); fe_sub(a, a, c);
        fe_add(c, b, d); fe_sub(b, b, d);
        fe_sq(d, e); fe_sq(f, a);
        fe_mul(a, c, a); fe_mul(c, b, e);
        fe_add(e, a, c); fe_sub(a, a, c);
        fe_sq(b, a);
        fe_sub(c, d, f);
        fe_mul(a, c, C121665);
        fe_add(a, a, d);
        fe_mul(c, c, a); fe_mul(a, d, f);
        fe_mul(d, b, x);
        fe_sq(b, e);
        sel25519(a, b, r); sel25519(c, d, r);
    }
    for (int i = 0; i < 16; i++) { x[i+16] = a[i]; x[i+32] = c[i]; x[i+48] = b[i]; x[i+64] = d[i]; }
    inv25519(x + 32, x + 32);
    fe_mul(x + 16, x + 16, x + 32);
    pack25519(out, x + 16);
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32])
{
    uint8_t base[32] = { 9 };
    x25519(out, scalar, base);
}
