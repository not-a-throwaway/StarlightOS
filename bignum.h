#ifndef BIGNUM_H
#define BIGNUM_H
/*
 * bignum.h -- fixed-capacity big integers + Montgomery modular arithmetic.
 *
 * Built for X.509 certificate checking (rsa.c, ecdsa.c). Numbers are arrays of
 * uint32_t limbs in little-endian order, with the limb count fixed by a
 * bn_mont context (one modulus per context). Everything is add/sub/mul based --
 * no division, no __int128 -- so it compiles for the 32-bit kernel too. The
 * modulus must be odd (RSA moduli, and the P-256/P-384 field primes and group
 * orders, all are), which is what Montgomery reduction needs.
 */
#include <stdint.h>
#include <stddef.h>

#define BN_MAX_LIMBS 128                     /* 4096-bit: RSA-4096 and P-384 both fit */

struct bn_mont {
    int      n;                              /* limbs in use */
    uint32_t m[BN_MAX_LIMBS];
    uint32_t m0inv;                          /* -m^-1 mod 2^32 */
    uint32_t r2[BN_MAX_LIMBS];               /* R^2 mod m, R = 2^(32n) */
    uint32_t one[BN_MAX_LIMBS];              /* R mod m = Montgomery form of 1 */
};

/* Set up a Montgomery context (m odd, 1..BN_MAX_LIMBS limbs). 0 ok, -1 bad. */
int  bn_mont_init(struct bn_mont *c, const uint8_t *mod, size_t modlen);

/* Load <= modlen big-endian bytes into n normal limbs (fully reduced mod m). */
void bn_load_be(const struct bn_mont *c, uint32_t *r, const uint8_t *b, size_t blen);
/* Write the low `outlen` bytes of a as big-endian (outlen <= 4*n). */
void bn_store_be(uint8_t *out, size_t outlen, const uint32_t *a, int n);

/* Montgomery-domain arithmetic (a, b, r are n limbs; r may alias a or b). */
void bn_mont_mul(const struct bn_mont *c, uint32_t *r, const uint32_t *a, const uint32_t *b);
void bn_mont_add(const struct bn_mont *c, uint32_t *r, const uint32_t *a, const uint32_t *b);
void bn_mont_sub(const struct bn_mont *c, uint32_t *r, const uint32_t *a, const uint32_t *b);
void bn_mont_inv(const struct bn_mont *c, uint32_t *r, const uint32_t *a);   /* for prime m: a^(m-2) */

/* Convert between normal and Montgomery form. */
void bn_to_mont(const struct bn_mont *c, uint32_t *r, const uint32_t *a);
void bn_from_mont(const struct bn_mont *c, uint32_t *r, const uint32_t *a);

/* Plain n-limb helpers (normal domain). */
int  bn_cmp_n(const uint32_t *a, const uint32_t *b, int n);
int  bn_is_zero_n(const uint32_t *a, int n);
void bn_copy_n(uint32_t *r, const uint32_t *a, int n);
void bn_sub_n(uint32_t *r, const uint32_t *a, const uint32_t *b, int n);      /* r = a-b mod 2^(32n) */
void bn_add_n(uint32_t *r, const uint32_t *a, const uint32_t *b, int n);      /* r = a+b mod 2^(32n) */
void bn_mod_n(uint32_t *r, const uint32_t *a, int n, const struct bn_mont *c);/* reduce once mod m */

/* out = base^exp mod m, all big-endian; out gets modlen bytes. 0 ok, -1 error. */
int  bn_modexp_be(uint8_t *out, const uint8_t *base, size_t blen,
                  const uint8_t *exp, size_t elen, const uint8_t *mod, size_t modlen);

#endif
