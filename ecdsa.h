#ifndef ECDSA_H
#define ECDSA_H
/*
 * ecdsa.h -- ECDSA signature verification on NIST P-256 and P-384.
 *
 * Used by X.509 certificate checking and by the TLS CertificateVerify /
 * ServerKeyExchange signature check. The public key is the uncompressed point
 * 0x04 || X || Y and the signature is r || s, each coordinate/component the
 * field size in bytes (32 for P-256, 48 for P-384). The digest is SHA-256 for
 * P-256 and SHA-384 for P-384 (longer digests are truncated, per ECDSA).
 *
 * Returns 0 if the signature verifies, -1 otherwise (including malformed input,
 * points off the curve, or r/s out of range).
 */
#include <stdint.h>
#include <stddef.h>

int ecdsa_verify_p256(const uint8_t *pub, size_t publen,
                      const uint8_t *sig, size_t siglen,
                      const uint8_t *hash, size_t hlen);
int ecdsa_verify_p384(const uint8_t *pub, size_t publen,
                      const uint8_t *sig, size_t siglen,
                      const uint8_t *hash, size_t hlen);

#endif
