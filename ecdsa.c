/*
 * ecdsa.c -- ECDSA signature verification (FIPS 186-4) on NIST P-256 and P-384.
 *
 * Verification is u1*G + u2*Q with u1 = e/s, u2 = r/s (mod n), then check that
 * the x-coordinate of the result equals r (mod n). Point arithmetic is Jacobian
 * with a = -3 (both curves qualify), and every field/scalar operation rides on
 * the generic Montgomery code in bignum.c -- so, like the rest of the kernel
 * crypto, there is no division and no __int128.
 */
#include "ecdsa.h"
#include "bignum.h"
#include "kstring.h"

#define EC_LIMBS 12                                   /* 384-bit / 32 = 12 */

struct curve {
    const char *p, *n, *gx, *gy, *b;
    int fs;                                            /* field size in bytes */
};

static const struct curve P256 = {
    "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
    "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
    "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5",
    "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
    32
};

static const struct curve P384 = {
    "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
    "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
    "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
    "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f",
    "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
    48
};

struct ec {
    struct bn_mont fp, fn;
    int nf, nn, fs;
    uint32_t b[EC_LIMBS], gx[EC_LIMBS], gy[EC_LIMBS], one[EC_LIMBS];
    uint8_t pbytes[EC_LIMBS * 4], nbytes[EC_LIMBS * 4];
};

static int hexnib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void hexload(uint8_t *out, const char *hex, int nbytes)
{
    for (int i = 0; i < nbytes; i++)
        out[i] = (uint8_t)((hexnib(hex[2 * i]) << 4) | hexnib(hex[2 * i + 1]));
}

/* Field ops (Montgomery domain). r may alias a or b. */
static void fe_mul(struct ec *e, uint32_t *r, const uint32_t *a, const uint32_t *b) { bn_mont_mul(&e->fp, r, a, b); }
static void fe_sqr(struct ec *e, uint32_t *r, const uint32_t *a) { bn_mont_mul(&e->fp, r, a, a); }
static void fe_add(struct ec *e, uint32_t *r, const uint32_t *a, const uint32_t *b) { bn_mont_add(&e->fp, r, a, b); }
static void fe_sub(struct ec *e, uint32_t *r, const uint32_t *a, const uint32_t *b) { bn_mont_sub(&e->fp, r, a, b); }
static void fe_from_be(struct ec *e, uint32_t *r, const uint8_t *b) { bn_load_be(&e->fp, r, b, e->fs); bn_to_mont(&e->fp, r, r); }
static void fe_from_mont(struct ec *e, uint32_t *r, const uint32_t *a) { bn_from_mont(&e->fp, r, a); }

static int ec_init(struct ec *e, const struct curve *cv)
{
    e->fs = cv->fs;
    hexload(e->pbytes, cv->p, cv->fs);
    hexload(e->nbytes, cv->n, cv->fs);
    if (bn_mont_init(&e->fp, e->pbytes, cv->fs) != 0) return -1;
    if (bn_mont_init(&e->fn, e->nbytes, cv->fs) != 0) return -1;
    e->nf = e->fp.n;
    e->nn = e->fn.n;
    uint8_t buf[EC_LIMBS * 4];
    uint32_t t[EC_LIMBS];
    hexload(buf, cv->b, cv->fs);  bn_load_be(&e->fp, t, buf, cv->fs); bn_to_mont(&e->fp, e->b, t);
    hexload(buf, cv->gx, cv->fs); bn_load_be(&e->fp, t, buf, cv->fs); bn_to_mont(&e->fp, e->gx, t);
    hexload(buf, cv->gy, cv->fs); bn_load_be(&e->fp, t, buf, cv->fs); bn_to_mont(&e->fp, e->gy, t);
    bn_copy_n(e->one, e->fp.one, e->nf);
    return 0;
}

struct pt { uint32_t x[EC_LIMBS], y[EC_LIMBS], z[EC_LIMBS]; };

static void pt_set_inf(struct ec *e, struct pt *p)
{
    bn_copy_n(p->x, e->one, e->nf);
    bn_copy_n(p->y, e->one, e->nf);
    for (int i = 0; i < e->nf; i++) p->z[i] = 0;
}

/* Jacobian doubling for a = -3 (same schedule as p256.c). */
static void pt_double(struct ec *e, struct pt *r, const struct pt *p)
{
    int n = e->nf;
    if (bn_is_zero_n(p->z, n) || bn_is_zero_n(p->y, n)) { pt_set_inf(e, r); return; }
    uint32_t delta[EC_LIMBS], gamma[EC_LIMBS], beta[EC_LIMBS], alpha[EC_LIMBS];
    uint32_t t1[EC_LIMBS], t2[EC_LIMBS], x3[EC_LIMBS], y3[EC_LIMBS], z3[EC_LIMBS];
    fe_sqr(e, delta, p->z);
    fe_sqr(e, gamma, p->y);
    fe_mul(e, beta, p->x, gamma);
    fe_sub(e, t1, p->x, delta); fe_add(e, t2, p->x, delta);
    fe_mul(e, alpha, t1, t2);
    fe_add(e, t1, alpha, alpha); fe_add(e, alpha, t1, alpha);        /* alpha = 3(x2-delta)(x2+delta) */
    fe_sqr(e, x3, alpha);
    fe_add(e, t1, beta, beta); fe_add(e, t1, t1, t1); fe_add(e, t2, t1, t1);  /* t2 = 8 beta */
    fe_sub(e, x3, x3, t2);
    fe_add(e, t2, p->y, p->z); fe_sqr(e, z3, t2); fe_sub(e, z3, z3, gamma); fe_sub(e, z3, z3, delta);
    fe_sub(e, t1, t1, x3); fe_mul(e, y3, alpha, t1);
    fe_sqr(e, t2, gamma); fe_add(e, t2, t2, t2); fe_add(e, t2, t2, t2); fe_add(e, t2, t2, t2); /* 8 gamma^2 */
    fe_sub(e, y3, y3, t2);
    bn_copy_n(r->x, x3, n); bn_copy_n(r->y, y3, n); bn_copy_n(r->z, z3, n);
}

static void pt_add(struct ec *e, struct pt *r, const struct pt *p, const struct pt *q)
{
    int n = e->nf;
    if (bn_is_zero_n(p->z, n)) { *r = *q; return; }
    if (bn_is_zero_n(q->z, n)) { *r = *p; return; }
    uint32_t z1z1[EC_LIMBS], z2z2[EC_LIMBS], u1[EC_LIMBS], u2[EC_LIMBS];
    uint32_t s1[EC_LIMBS], s2[EC_LIMBS], h[EC_LIMBS], i[EC_LIMBS], j[EC_LIMBS];
    uint32_t rr[EC_LIMBS], v[EC_LIMBS], t[EC_LIMBS], x3[EC_LIMBS], y3[EC_LIMBS], z3[EC_LIMBS];
    fe_sqr(e, z1z1, p->z); fe_sqr(e, z2z2, q->z);
    fe_mul(e, u1, p->x, z2z2); fe_mul(e, u2, q->x, z1z1);
    fe_mul(e, t, q->z, z2z2);  fe_mul(e, s1, p->y, t);
    fe_mul(e, t, p->z, z1z1);  fe_mul(e, s2, q->y, t);
    fe_sub(e, h, u2, u1); fe_sub(e, rr, s2, s1);
    if (bn_is_zero_n(h, n)) {
        if (bn_is_zero_n(rr, n)) { pt_double(e, r, p); return; }
        pt_set_inf(e, r); return;
    }
    fe_add(e, rr, rr, rr);
    fe_add(e, t, h, h); fe_mul(e, i, t, t);                          /* I = (2H)^2 */
    fe_mul(e, j, h, i);
    fe_mul(e, v, u1, i);
    fe_sqr(e, x3, rr); fe_sub(e, x3, x3, j); fe_sub(e, x3, x3, v); fe_sub(e, x3, x3, v);
    fe_sub(e, t, v, x3); fe_mul(e, y3, rr, t);
    fe_mul(e, t, s1, j); fe_add(e, t, t, t); fe_sub(e, y3, y3, t);
    fe_add(e, t, p->z, q->z); fe_sqr(e, z3, t); fe_sub(e, z3, z3, z1z1); fe_sub(e, z3, z3, z2z2); fe_mul(e, z3, z3, h);
    bn_copy_n(r->x, x3, n); bn_copy_n(r->y, y3, n); bn_copy_n(r->z, z3, n);
}

/* r = k * p, with k a normal-domain scalar of up to e->nn limbs. */
static void pt_scalar_mul(struct ec *e, struct pt *r, const uint32_t *k, const struct pt *p)
{
    struct pt acc, base = *p;
    pt_set_inf(e, &acc);
    for (int bit = 32 * e->nn - 1; bit >= 0; bit--) {
        pt_double(e, &acc, &acc);
        if ((k[bit >> 5] >> (bit & 31)) & 1u) pt_add(e, &acc, &acc, &base);
    }
    *r = acc;
}

/* y^2 == x^3 - 3x + b ? (Montgomery-domain coordinates) */
static int on_curve(struct ec *e, const uint32_t *x, const uint32_t *y)
{
    uint32_t lhs[EC_LIMBS], rhs[EC_LIMBS], t[EC_LIMBS];
    fe_sqr(e, lhs, y);
    fe_sqr(e, t, x); fe_mul(e, rhs, t, x);
    fe_sub(e, rhs, rhs, x); fe_sub(e, rhs, rhs, x); fe_sub(e, rhs, rhs, x);
    fe_add(e, rhs, rhs, e->b);
    return bn_cmp_n(lhs, rhs, e->nf) == 0;
}

static int is_zero_bytes(const uint8_t *b, int n)
{
    uint8_t v = 0;
    for (int i = 0; i < n; i++) v |= b[i];
    return v == 0;
}

static int ecdsa_verify(const struct curve *cv, const uint8_t *pub, size_t publen,
                        const uint8_t *sig, size_t siglen, const uint8_t *hash, size_t hlen)
{
    struct ec e;
    if (ec_init(&e, cv) != 0) return -1;
    int fs = cv->fs;
    if (publen != (size_t)(1 + 2 * fs) || pub[0] != 0x04) return -1;
    if (siglen != (size_t)(2 * fs)) return -1;
    const uint8_t *rb = sig, *sb = sig + fs;
    if (is_zero_bytes(rb, fs) || is_zero_bytes(sb, fs)) return -1;
    if (memcmp(rb, e.nbytes, fs) >= 0 || memcmp(sb, e.nbytes, fs) >= 0) return -1;  /* 1 <= r,s < n */

    uint32_t r[EC_LIMBS], s[EC_LIMBS], wm[EC_LIMBS], u1m[EC_LIMBS], u2m[EC_LIMBS];
    uint32_t u1[EC_LIMBS], u2[EC_LIMBS], em[EC_LIMBS], eb[EC_LIMBS];
    bn_load_be(&e.fn, r, rb, fs);
    bn_load_be(&e.fn, s, sb, fs);
    bn_load_be(&e.fn, eb, hash, hlen < (size_t)fs ? hlen : (size_t)fs);   /* e = leftmost bits */
    bn_to_mont(&e.fn, em, eb);
    uint32_t sm[EC_LIMBS], rm[EC_LIMBS];
    bn_to_mont(&e.fn, sm, s);
    bn_to_mont(&e.fn, rm, r);
    bn_mont_inv(&e.fn, wm, sm);                                     /* w = s^-1 mod n */
    bn_mont_mul(&e.fn, u1m, em, wm);
    bn_mont_mul(&e.fn, u2m, rm, wm);
    bn_from_mont(&e.fn, u1, u1m);
    bn_from_mont(&e.fn, u2, u2m);

    /* Public point. */
    if (memcmp(pub + 1, e.pbytes, fs) >= 0 || memcmp(pub + 1 + fs, e.pbytes, fs) >= 0) return -1;
    uint32_t qx[EC_LIMBS], qy[EC_LIMBS];
    fe_from_be(&e, qx, pub + 1);
    fe_from_be(&e, qy, pub + 1 + fs);
    if (!on_curve(&e, qx, qy)) return -1;

    struct pt G, Q, R, Qm;
    bn_copy_n(G.x, e.gx, e.nf); bn_copy_n(G.y, e.gy, e.nf); bn_copy_n(G.z, e.one, e.nf);
    bn_copy_n(Q.x, qx, e.nf);   bn_copy_n(Q.y, qy, e.nf);   bn_copy_n(Q.z, e.one, e.nf);
    pt_scalar_mul(&e, &R, u1, &G);
    pt_scalar_mul(&e, &Qm, u2, &Q);
    pt_add(&e, &R, &R, &Qm);
    if (bn_is_zero_n(R.z, e.nf)) return -1;                          /* result at infinity */

    uint32_t zi[EC_LIMBS], zi2[EC_LIMBS], x[EC_LIMBS], xn[EC_LIMBS], v[EC_LIMBS];
    bn_mont_inv(&e.fp, zi, R.z);
    bn_mont_mul(&e.fp, zi2, zi, zi);
    bn_mont_mul(&e.fp, x, R.x, zi2);                                 /* affine x */
    fe_from_mont(&e, xn, x);
    uint8_t xb[EC_LIMBS * 4];
    bn_store_be(xb, fs, xn, e.nf);
    bn_load_be(&e.fn, v, xb, fs);                                    /* v = x mod n */
    return bn_cmp_n(v, r, e.nn) == 0 ? 0 : -1;
}

int ecdsa_verify_p256(const uint8_t *pub, size_t publen, const uint8_t *sig, size_t siglen, const uint8_t *hash, size_t hlen)
{
    return ecdsa_verify(&P256, pub, publen, sig, siglen, hash, hlen);
}

int ecdsa_verify_p384(const uint8_t *pub, size_t publen, const uint8_t *sig, size_t siglen, const uint8_t *hash, size_t hlen)
{
    return ecdsa_verify(&P384, pub, publen, sig, siglen, hash, hlen);
}
