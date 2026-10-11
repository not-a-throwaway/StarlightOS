/*
 * bignum.c -- generic big-integer modular arithmetic for RSA and ECDSA.
 * See bignum.h. Montgomery multiplication (CIOS) with 32-bit limbs; the
 * reduction factor is m0inv = -m^-1 mod 2^32. R = 2^(32n) is materialised by
 * repeated doubling (add/sub only), so there is no division anywhere.
 */
#include "bignum.h"
#include "kstring.h"

int bn_cmp_n(const uint32_t *a, const uint32_t *b, int n)
{
    for (int i = n - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    return 0;
}

int bn_is_zero_n(const uint32_t *a, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= a[i];
    return v == 0;
}

void bn_copy_n(uint32_t *r, const uint32_t *a, int n) { for (int i = 0; i < n; i++) r[i] = a[i]; }

void bn_sub_n(uint32_t *r, const uint32_t *a, const uint32_t *b, int n)
{
    uint32_t borrow = 0;
    for (int i = 0; i < n; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        r[i] = (uint32_t)d;
        borrow = (uint32_t)(d >> 32) & 1u;
    }
}

void bn_add_n(uint32_t *r, const uint32_t *a, const uint32_t *b, int n)
{
    uint32_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint64_t s = (uint64_t)a[i] + b[i] + carry;
        r[i] = (uint32_t)s;
        carry = (uint32_t)(s >> 32);
    }
}

void bn_mod_n(uint32_t *r, const uint32_t *a, int n, const struct bn_mont *c)
{
    bn_copy_n(r, a, n);
    while (bn_cmp_n(r, c->m, n) >= 0) bn_sub_n(r, r, c->m, n);
}

int bn_mont_init(struct bn_mont *c, const uint8_t *mod, size_t modlen)
{
    while (modlen > 1 && mod[0] == 0) { mod++; modlen--; }      /* strip leading zeros */
    if (modlen == 0 || modlen > (size_t)BN_MAX_LIMBS * 4) return -1;
    int n = (int)((modlen + 3) / 4);
    c->n = n;
    memset(c->m, 0, sizeof c->m);
    for (size_t i = 0; i < modlen; i++) c->m[i >> 2] |= (uint32_t)mod[modlen - 1 - i] << ((i & 3) * 8);
    if (!(c->m[0] & 1u)) return -1;                             /* Montgomery needs an odd modulus */
    uint32_t inv = c->m[0];
    for (int i = 0; i < 5; i++) inv *= 2u - c->m[0] * inv;      /* Newton: m^-1 mod 2^32 */
    c->m0inv = 0u - inv;

    uint32_t x[BN_MAX_LIMBS];
    memset(x, 0, sizeof x); x[0] = 1;
    for (int i = 0; i < 32 * n; i++) {                          /* x = 2^(32n) mod m = R mod m */
        uint32_t carry = 0;
        for (int j = 0; j < n; j++) { uint32_t nv = (x[j] << 1) | carry; carry = x[j] >> 31; x[j] = nv; }
        if (carry || bn_cmp_n(x, c->m, n) >= 0) bn_sub_n(x, x, c->m, n);
    }
    memcpy(c->one, x, (size_t)n * 4);
    for (int i = 0; i < 32 * n; i++) {                          /* 32n more doublings: x = R^2 mod m */
        uint32_t carry = 0;
        for (int j = 0; j < n; j++) { uint32_t nv = (x[j] << 1) | carry; carry = x[j] >> 31; x[j] = nv; }
        if (carry || bn_cmp_n(x, c->m, n) >= 0) bn_sub_n(x, x, c->m, n);
    }
    memcpy(c->r2, x, (size_t)n * 4);
    return 0;
}

void bn_load_be(const struct bn_mont *c, uint32_t *r, const uint8_t *b, size_t blen)
{
    int n = c->n;
    memset(r, 0, (size_t)n * 4);
    size_t lim = blen < (size_t)n * 4 ? blen : (size_t)n * 4;
    for (size_t i = 0; i < lim; i++) r[i >> 2] |= (uint32_t)b[blen - 1 - i] << ((i & 3) * 8);
    while (bn_cmp_n(r, c->m, n) >= 0) bn_sub_n(r, r, c->m, n);
}

void bn_store_be(uint8_t *out, size_t outlen, const uint32_t *a, int n)
{
    for (size_t i = 0; i < outlen; i++)
        out[outlen - 1 - i] = (i >> 2) < (size_t)n ? (uint8_t)(a[i >> 2] >> ((i & 3) * 8)) : 0;
}

void bn_mont_mul(const struct bn_mont *c, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    int n = c->n;
    uint32_t t[BN_MAX_LIMBS + 2];
    for (int i = 0; i <= n; i++) t[i] = 0;
    for (int i = 0; i < n; i++) {
        uint64_t cc = 0;
        uint32_t bi = b[i];
        for (int j = 0; j < n; j++) {
            uint64_t uv = (uint64_t)t[j] + (uint64_t)a[j] * bi + cc;
            t[j] = (uint32_t)uv; cc = uv >> 32;
        }
        uint64_t uv = (uint64_t)t[n] + cc;
        t[n] = (uint32_t)uv;
        t[n + 1] = (uint32_t)(uv >> 32);

        uint32_t mm = (uint32_t)((uint64_t)t[0] * c->m0inv);
        uv = (uint64_t)t[0] + (uint64_t)mm * c->m[0];
        cc = uv >> 32;
        for (int j = 1; j < n; j++) {
            uv = (uint64_t)t[j] + (uint64_t)mm * c->m[j] + cc;
            t[j - 1] = (uint32_t)uv; cc = uv >> 32;
        }
        uv = (uint64_t)t[n] + cc;
        t[n - 1] = (uint32_t)uv;
        t[n] = t[n + 1] + (uint32_t)(uv >> 32);
    }
    if (t[n] || bn_cmp_n(t, c->m, n) >= 0) bn_sub_n(t, t, c->m, n);
    for (int i = 0; i < n; i++) r[i] = t[i];
}

void bn_mont_add(const struct bn_mont *c, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    int n = c->n;
    uint32_t carry = 0;
    for (int i = 0; i < n; i++) { uint64_t s = (uint64_t)a[i] + b[i] + carry; r[i] = (uint32_t)s; carry = (uint32_t)(s >> 32); }
    if (carry || bn_cmp_n(r, c->m, n) >= 0) bn_sub_n(r, r, c->m, n);
}

void bn_mont_sub(const struct bn_mont *c, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    int n = c->n;
    uint32_t borrow = 0;
    for (int i = 0; i < n; i++) { uint64_t d = (uint64_t)a[i] - b[i] - borrow; r[i] = (uint32_t)d; borrow = (uint32_t)(d >> 32) & 1u; }
    if (borrow) {
        uint32_t carry = 0;
        for (int i = 0; i < n; i++) { uint64_t s = (uint64_t)r[i] + c->m[i] + carry; r[i] = (uint32_t)s; carry = (uint32_t)(s >> 32); }
    }
}

void bn_to_mont(const struct bn_mont *c, uint32_t *r, const uint32_t *a) { bn_mont_mul(c, r, a, c->r2); }

void bn_from_mont(const struct bn_mont *c, uint32_t *r, const uint32_t *a)
{
    uint32_t one[BN_MAX_LIMBS];
    memset(one, 0, sizeof one); one[0] = 1;
    bn_mont_mul(c, r, a, one);
}

void bn_mont_inv(const struct bn_mont *c, uint32_t *r, const uint32_t *a)
{
    int n = c->n;
    uint32_t e[BN_MAX_LIMBS], base[BN_MAX_LIMBS], acc[BN_MAX_LIMBS];
    /* e = m - 2 */
    uint64_t borrow = 2;
    for (int i = 0; i < n; i++) { uint64_t d = (uint64_t)c->m[i] - borrow; e[i] = (uint32_t)d; borrow = (d >> 32) & 1u; }
    bn_copy_n(base, a, n);
    bn_copy_n(acc, c->one, n);
    for (int i = 32 * n - 1; i >= 0; i--) {
        bn_mont_mul(c, acc, acc, acc);
        if ((e[i >> 5] >> (i & 31)) & 1u) bn_mont_mul(c, acc, acc, base);
    }
    bn_copy_n(r, acc, n);
}

int bn_modexp_be(uint8_t *out, const uint8_t *base, size_t blen,
                 const uint8_t *exp, size_t elen, const uint8_t *mod, size_t modlen)
{
    struct bn_mont c;
    if (bn_mont_init(&c, mod, modlen) != 0) return -1;
    int n = c.n;
    uint32_t b[BN_MAX_LIMBS], acc[BN_MAX_LIMBS];
    bn_load_be(&c, b, base, blen);
    bn_to_mont(&c, b, b);
    bn_copy_n(acc, c.one, n);
    for (size_t i = 0; i < elen; i++) {
        uint8_t byte = exp[i];
        for (int bit = 7; bit >= 0; bit--) {
            bn_mont_mul(&c, acc, acc, acc);
            if ((byte >> bit) & 1) bn_mont_mul(&c, acc, acc, b);
        }
    }
    bn_from_mont(&c, acc, acc);
    bn_store_be(out, modlen, acc, n);
    return 0;
}
