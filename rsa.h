#ifndef RSA_H
#define RSA_H
/*
 * rsa.h -- RSA signature verification for X.509 / TLS (RFC 8017).
 *
 * Only the public operation is implemented (s^e mod n): the client never has a
 * private key. Two schemes:
 *   - RSASSA-PKCS1-v1_5 with a caller-supplied DigestInfo DER blob.
 *   - RSASSA-PSS with SHA-256 or SHA-384 and a salt the length of the hash
 *     (what TLS 1.3 negotiated via rsa_pss_rsae_*).
 * The modulus is a big-endian magnitude (leading zeros are ignored).
 */
#include <stdint.h>
#include <stddef.h>

/* 0 if the PKCS#1 v1.5 signature over digest_info (DER DigestInfo) is valid. */
int rsa_verify_v15(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                   const uint8_t *sig, size_t siglen,
                   const uint8_t *digest_info, size_t dilen);

/* 0 if the PSS signature over mhash is valid. hash_alg: HASH_SHA256 / HASH_SHA384. */
int rsa_verify_pss(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                   const uint8_t *sig, size_t siglen,
                   int hash_alg, const uint8_t *mhash, size_t mhlen);

#endif
