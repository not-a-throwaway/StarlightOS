/*
 * p256.c -- ECDH on secp256r1 (NIST P-256), for servers that have no X25519 (Windows/IIS, older Java, some
 * load balancers). Field arithmetic is 8x32-bit Montgomery (CIOS; p = -1 mod 2^32 so the reduction factor is
 * just the low limb), curve arithmetic is Jacobian with a = -3. Used only with ephemeral keys, so it is a
 * plain double-and-add: correct and fast enough (a few thousand field multiplications), but NOT constant-time.
 */
#include "crypto.h"
#include "kstring.h"

typedef uint32_t fe[8];                                   /* little-endian limbs */

static const uint32_t P[8] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF };
static const uint32_t N[8] = { 0xFC632551, 0xF3B9CAC2, 0xA7179E84, 0xBCE6FAAD, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF };
static const uint8_t B_BE[32] = {
    0x5A,0xC6,0x35,0xD8,0xAA,0x3A,0x93,0xE7,0xB3,0xEB,0xBD,0x55,0x76,0x98,0x86,0xBC,
    0x65,0x1D,0x06,0xB0,0xCC,0x53,0xB0,0xF6,0x3B,0xCE,0x3C,0x3E,0x27,0xD2,0x60,0x4B };
static const uint8_t GX_BE[32] = {
    0x6B,0x17,0xD1,0xF2,0xE1,0x2C,0x42,0x47,0xF8,0xBC,0xE6,0xE5,0x63,0xA4,0x40,0xF2,
    0x77,0x03,0x7D,0x81,0x2D,0xEB,0x33,0xA0,0xF4,0xA1,0x39,0x45,0xD8,0x98,0xC2,0x96 };
static const uint8_t GY_BE[32] = {
    0x4F,0xE3,0x42,0xE2,0xFE,0x1A,0x7F,0x9B,0x8E,0xE7,0xEB,0x4A,0x7C,0x0F,0x9E,0x16,
    0x2B,0xCE,0x33,0x57,0x6B,0x31,0x5E,0xCE,0xCB,0xB6,0x40,0x68,0x37,0xBF,0x51,0xF5 };

static void load_be(fe r, const uint8_t b[32]) { for (int i = 0; i < 8; i++) { const uint8_t *p = b + 28 - 4 * i; r[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; } }
static void store_be(uint8_t b[32], const fe a) { for (int i = 0; i < 8; i++) { uint8_t *p = b + 28 - 4 * i; p[0] = (uint8_t)(a[i] >> 24); p[1] = (uint8_t)(a[i] >> 16); p[2] = (uint8_t)(a[i] >> 8); p[3] = (uint8_t)a[i]; } }

static int cmp(const uint32_t *a, const uint32_t *b) { for (int i = 7; i >= 0; i--) { if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1; } return 0; }
static int is_zero(const fe a) { uint32_t v = 0; for (int i = 0; i < 8; i++) v |= a[i]; return v == 0; }
static uint32_t add_n(fe r, const uint32_t *a, const uint32_t *b) { uint64_t c = 0; for (int i = 0; i < 8; i++) { c += (uint64_t)a[i] + b[i]; r[i] = (uint32_t)c; c >>= 32; } return (uint32_t)c; }
static uint32_t sub_n(fe r, const uint32_t *a, const uint32_t *b) { int64_t c = 0; for (int i = 0; i < 8; i++) { c += (int64_t)a[i] - b[i]; r[i] = (uint32_t)c; c >>= 32; } return (uint32_t)(c & 1); }

static void fe_add(fe r, const fe a, const fe b) { uint32_t c = add_n(r, a, b); if (c || cmp(r, P) >= 0) sub_n(r, r, P); }
static void fe_sub(fe r, const fe a, const fe b) { if (sub_n(r, a, b)) add_n(r, r, P); }

/* r = a * b / 2^256 mod p */
static void mmul(fe r, const fe a, const fe b)
{
    uint32_t t[10] = { 0 };
    for (int i = 0; i < 8; i++) {
        uint64_t carry = 0, x;
        for (int j = 0; j < 8; j++) { x = (uint64_t)a[i] * b[j] + t[j] + carry; t[j] = (uint32_t)x; carry = x >> 32; }
        x = (uint64_t)t[8] + carry; t[8] = (uint32_t)x; t[9] = (uint32_t)(x >> 32);
        uint32_t m = t[0];                                         /* -p^-1 mod 2^32 == 1 */
        x = (uint64_t)m * P[0] + t[0]; carry = x >> 32;
        for (int j = 1; j < 8; j++) { x = (uint64_t)m * P[j] + t[j] + carry; t[j - 1] = (uint32_t)x; carry = x >> 32; }
        x = (uint64_t)t[8] + carry; t[7] = (uint32_t)x; carry = x >> 32;
        t[8] = t[9] + (uint32_t)carry; t[9] = 0;
    }
    for (int i = 0; i < 8; i++) r[i] = t[i];
    if (t[8] || cmp(r, P) >= 0) sub_n(r, r, P);
}

static fe R2, MONT_ONE, CB, GX, GY;
static int inited;

static void init(void)
{
    if (inited) return;
    fe x = { 1, 0, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 512; i++) fe_add(x, x, x);                /* 2^512 mod p = R^2 */
    for (int i = 0; i < 8; i++) R2[i] = x[i];
    fe one = { 1, 0, 0, 0, 0, 0, 0, 0 }, t;
    mmul(MONT_ONE, one, R2);
    load_be(t, B_BE);  mmul(CB, t, R2);
    load_be(t, GX_BE); mmul(GX, t, R2);
    load_be(t, GY_BE); mmul(GY, t, R2);
    inited = 1;
}

static void fe_inv(fe r, const fe a)                                /* a^(p-2) */
{
    fe e, acc, base;
    for (int i = 0; i < 8; i++) { e[i] = P[i]; base[i] = a[i]; acc[i] = MONT_ONE[i]; }
    e[0] -= 2;
    for (int bit = 255; bit >= 0; bit--) {
        mmul(acc, acc, acc);
        if ((e[bit >> 5] >> (bit & 31)) & 1u) mmul(acc, acc, base);
    }
    for (int i = 0; i < 8; i++) r[i] = acc[i];
}

struct pt { fe x, y, z; };                                          /* Jacobian, Montgomery domain; z == 0: infinity */

static void pt_set_inf(struct pt *p) { for (int i = 0; i < 8; i++) { p->x[i] = MONT_ONE[i]; p->y[i] = MONT_ONE[i]; p->z[i] = 0; } }

static void pt_double(struct pt *r, const struct pt *p)
{
    if (is_zero(p->z) || is_zero(p->y)) { pt_set_inf(r); return; }
    fe delta, gamma, beta, alpha, t1, t2, x3, y3, z3;
    mmul(delta, p->z, p->z);
    mmul(gamma, p->y, p->y);
    mmul(beta, p->x, gamma);
    fe_sub(t1, p->x, delta); fe_add(t2, p->x, delta);
    mmul(alpha, t1, t2);
    fe_add(t1, alpha, alpha); fe_add(alpha, t1, alpha);             /* 3 * (x-delta)(x+delta) */
    mmul(x3, alpha, alpha);
    fe_add(t1, beta, beta); fe_add(t1, t1, t1); fe_add(t2, t1, t1); /* t1 = 4beta, t2 = 8beta */
    fe_sub(x3, x3, t2);
    fe_add(t2, p->y, p->z); mmul(z3, t2, t2); fe_sub(z3, z3, gamma); fe_sub(z3, z3, delta);
    fe_sub(t1, t1, x3); mmul(y3, alpha, t1);
    mmul(t2, gamma, gamma); fe_add(t2, t2, t2); fe_add(t2, t2, t2); fe_add(t2, t2, t2);   /* 8 gamma^2 */
    fe_sub(y3, y3, t2);
    for (int i = 0; i < 8; i++) { r->x[i] = x3[i]; r->y[i] = y3[i]; r->z[i] = z3[i]; }
}

static void pt_add(struct pt *r, const struct pt *p, const struct pt *q)
{
    if (is_zero(p->z)) { *r = *q; return; }
    if (is_zero(q->z)) { *r = *p; return; }
    fe z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t, x3, y3, z3;
    mmul(z1z1, p->z, p->z); mmul(z2z2, q->z, q->z);
    mmul(u1, p->x, z2z2);   mmul(u2, q->x, z1z1);
    mmul(t, q->z, z2z2);    mmul(s1, p->y, t);
    mmul(t, p->z, z1z1);    mmul(s2, q->y, t);
    fe_sub(h, u2, u1); fe_sub(rr, s2, s1);
    if (is_zero(h)) {
        if (is_zero(rr)) { pt_double(r, p); return; }
        pt_set_inf(r); return;
    }
    fe_add(rr, rr, rr);
    fe_add(t, h, h); mmul(i, t, t);                                  /* I = (2H)^2 */
    mmul(j, h, i);
    mmul(v, u1, i);
    mmul(x3, rr, rr); fe_sub(x3, x3, j); fe_sub(x3, x3, v); fe_sub(x3, x3, v);
    fe_sub(t, v, x3); mmul(y3, rr, t);
    mmul(t, s1, j); fe_add(t, t, t); fe_sub(y3, y3, t);
    fe_add(t, p->z, q->z); mmul(z3, t, t); fe_sub(z3, z3, z1z1); fe_sub(z3, z3, z2z2); mmul(z3, z3, h);
    for (int k = 0; k < 8; k++) { r->x[k] = x3[k]; r->y[k] = y3[k]; r->z[k] = z3[k]; }
}

/* k (32 big-endian bytes) * (x, y) -> affine coordinates (big-endian). Returns -1 on infinity. */
static int scalar_mult(uint8_t ox[32], uint8_t oy[32], const uint8_t k[32], const fe x, const fe y)
{
    struct pt base, acc;
    for (int i = 0; i < 8; i++) { base.x[i] = x[i]; base.y[i] = y[i]; base.z[i] = MONT_ONE[i]; }
    pt_set_inf(&acc);
    for (int bit = 255; bit >= 0; bit--) {
        pt_double(&acc, &acc);
        if ((k[31 - (bit >> 3)] >> (bit & 7)) & 1u) pt_add(&acc, &acc, &base);
    }
    if (is_zero(acc.z)) return -1;
    fe zi, zi2, zi3, ax, ay, one = { 1, 0, 0, 0, 0, 0, 0, 0 };
    fe_inv(zi, acc.z);
    mmul(zi2, zi, zi); mmul(zi3, zi2, zi);
    mmul(ax, acc.x, zi2); mmul(ay, acc.y, zi3);
    mmul(ax, ax, one); mmul(ay, ay, one);                           /* leave the Montgomery domain */
    store_be(ox, ax); if (oy) store_be(oy, ay);
    return 0;
}

int p256_scalar_ok(const uint8_t scalar[32])
{
    fe k; load_be(k, scalar);
    return !is_zero(k) && cmp(k, N) < 0;
}

int p256_pubkey(uint8_t pub[65], const uint8_t scalar[32])
{
    init();
    if (!p256_scalar_ok(scalar)) return -1;
    if (scalar_mult(pub + 1, pub + 33, scalar, GX, GY) != 0) return -1;
    pub[0] = 4;
    return 0;
}

int p256_ecdh(uint8_t shared[32], const uint8_t scalar[32], const uint8_t peer[65])
{
    init();
    if (peer[0] != 4 || !p256_scalar_ok(scalar)) return -1;
    fe x, y, mx, my, lhs, rhs, t;
    load_be(x, peer + 1); load_be(y, peer + 33);
    if (cmp(x, P) >= 0 || cmp(y, P) >= 0) return -1;
    mmul(mx, x, R2); mmul(my, y, R2);
    mmul(lhs, my, my);                                              /* y^2 == x^3 - 3x + b ? */
    mmul(t, mx, mx); mmul(rhs, t, mx);
    fe_sub(rhs, rhs, mx); fe_sub(rhs, rhs, mx); fe_sub(rhs, rhs, mx);
    fe_add(rhs, rhs, CB);
    if (cmp(lhs, rhs) != 0) return -1;
    return scalar_mult(shared, NULL, scalar, mx, my);
}
