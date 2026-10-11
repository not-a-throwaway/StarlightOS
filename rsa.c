/*
 * rsa.c -- RSA signature verification (RFC 8017): PKCS#1 v1.5 and PSS.
 * See rsa.h. The shared step is the public-key modexp s^e mod n, done with the
 * Montgomery code in bignum.c; the rest is strict padding checks.
 */
#include "rsa.h"
#include "bignum.h"
#include "crypto.h"
#include "kstring.h"

/* Strip leading zero bytes from a big-endian magnitude. */
static const uint8_t *strip(const uint8_t *p, size_t *len)
{
    while (*len > 1 && p[0] == 0) { p++; (*len)--; }
    return p;
}

/* Recover EM = s^e mod n (nlen bytes). 0 on success, -1 on bad input. */
static int rsa_public(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                      const uint8_t *sig, size_t siglen, uint8_t *em)
{
    n = strip(n, &nlen);
    e = strip(e, &elen);
    if (nlen < 64 || nlen > (size_t)BN_MAX_LIMBS * 4) return -1;   /* 512..4096-bit modulus */
    if (elen == 0 || elen > 16) return -1;                         /* public exponent is small */
    if (siglen != nlen) return -1;                                 /* signature is k octets */
    if (memcmp(sig, n, nlen) >= 0) return -1;                      /* s must be < n */
    return bn_modexp_be(em, sig, siglen, e, elen, n, nlen);
}

int rsa_verify_v15(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                   const uint8_t *sig, size_t siglen,
                   const uint8_t *digest_info, size_t dilen)
{
    uint8_t em[BN_MAX_LIMBS * 4];
    n = strip(n, &nlen);
    if (nlen < 64 || nlen > sizeof em) return -1;
    if (rsa_public(n, nlen, e, elen, sig, siglen, em) != 0) return -1;
    if (em[0] != 0x00 || em[1] != 0x01) return -1;
    size_t i = 2;
    while (i < nlen && em[i] == 0xFF) i++;
    if (i < 10 || i >= nlen || em[i] != 0x00) return -1;           /* PS >= 8 octets, then 0x00 */
    i++;
    if (nlen - i != dilen) return -1;
    return ct_memcmp(em + i, digest_info, dilen) ? -1 : 0;
}

/* MGF1 (RFC 8017 B.2.1) with SHA-256/SHA-384: generate the mask into out. */
static void mgf1(int alg, const uint8_t *seed, size_t slen, uint8_t *out, size_t olen)
{
    size_t hl = hash_len(alg), pos = 0;
    uint8_t buf[HASH_MAX_LEN + 4], h[HASH_MAX_LEN];
    uint32_t ctr = 0;
    while (pos < olen) {
        memcpy(buf, seed, slen);
        buf[slen]     = (uint8_t)(ctr >> 24);
        buf[slen + 1] = (uint8_t)(ctr >> 16);
        buf[slen + 2] = (uint8_t)(ctr >> 8);
        buf[slen + 3] = (uint8_t)ctr;
        hash_data(alg, buf, slen + 4, h);
        size_t n = olen - pos < hl ? olen - pos : hl;
        memcpy(out + pos, h, n);
        pos += n; ctr++;
    }
}

int rsa_verify_pss(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                   const uint8_t *sig, size_t siglen,
                   int hash_alg, const uint8_t *mhash, size_t mhlen)
{
    uint8_t em[BN_MAX_LIMBS * 4], mask[BN_MAX_LIMBS * 4], h2[HASH_MAX_LEN];
    uint8_t mp[8 + HASH_MAX_LEN + HASH_MAX_LEN];
    if (hash_alg != HASH_SHA256 && hash_alg != HASH_SHA384) return -1;
    n = strip(n, &nlen);
    if (nlen < 64 || nlen > sizeof em) return -1;
    size_t hl = hash_len(hash_alg);
    if (mhlen != hl) return -1;
    size_t slen = hl;                                               /* TLS always uses salt = hash len */
    size_t emlen = nlen;
    if (emlen < hl + slen + 2) return -1;
    if (rsa_public(n, nlen, e, elen, sig, siglen, em) != 0) return -1;
    if (em[emlen - 1] != 0xBC) return -1;

    size_t db_len = emlen - hl - 1;
    uint8_t *H = em + db_len;
    mgf1(hash_alg, H, hl, mask, db_len);
    for (size_t i = 0; i < db_len; i++) em[i] ^= mask[i];           /* DB = maskedDB xor dbMask */
    em[0] &= 0x7f;                                                  /* clear 8*emLen-emBits = 1 bit */
    size_t ps_len = emlen - hl - hl - 2;
    for (size_t i = 0; i < ps_len; i++) if (em[i] != 0) return -1;
    if (em[ps_len] != 0x01) return -1;
    const uint8_t *salt = em + ps_len + 1;

    memset(mp, 0, 8);
    memcpy(mp + 8, mhash, mhlen);
    memcpy(mp + 8 + mhlen, salt, hl);
    hash_data(hash_alg, mp, 8 + mhlen + hl, h2);
    return ct_memcmp(h2, H, hl) ? -1 : 0;
}
