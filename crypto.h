#ifndef CRYPTO_H
#define CRYPTO_H
/*
 * crypto.h -- the primitives the TLS client needs.
 *
 *   hashes     SHA-256, SHA-384            (generic hash_* / hmac_hash / hkdf_*_h entry points)
 *   AEAD       AES-128/256-GCM, ChaCha20-Poly1305
 *   key agree  X25519 (RFC 7748), P-256 ECDH (secp256r1)
 *
 * Portable freestanding C: no libc beyond memcpy/memset, no 64-bit division, no __int128 (the kernel links
 * without libgcc on x86-64). Everything is differential-tested against OpenSSL/hashlib (tests/host).
 */
#include <stdint.h>
#include <stddef.h>

/* ---- SHA-256 ---- */
struct sha256_ctx { uint32_t h[8]; uint8_t buf[64]; uint32_t buflen; uint64_t total; };
void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, size_t len);
void sha256_final(struct sha256_ctx *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

/* ---- SHA-512 / SHA-384 ---- */
struct sha512_ctx { uint64_t h[8]; uint8_t buf[128]; uint32_t buflen; uint64_t total; int is384; };
void sha384_init(struct sha512_ctx *c);
void sha512_init(struct sha512_ctx *c);
void sha512_update(struct sha512_ctx *c, const void *data, size_t len);
void sha512_final(struct sha512_ctx *c, uint8_t *out);          /* 48 bytes for SHA-384, 64 for SHA-512 */
void sha384(const void *data, size_t len, uint8_t out[48]);

/* ---- hash-agnostic layer (the TLS key schedule runs over SHA-256 or SHA-384) ---- */
enum { HASH_SHA256 = 0, HASH_SHA384 = 1, HASH_MAX_LEN = 48 };
struct hash_ctx { int alg; struct sha256_ctx s256; struct sha512_ctx s512; };
size_t hash_len(int alg);
void   hash_init(struct hash_ctx *c, int alg);
void   hash_update(struct hash_ctx *c, const void *data, size_t len);
void   hash_final(struct hash_ctx *c, uint8_t *out);            /* out >= hash_len(alg); ctx is consumed */
void   hash_data(int alg, const void *data, size_t len, uint8_t *out);

/* ---- HMAC / HKDF ---- */
void hmac_hash(int alg, const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t *out);
void hkdf_extract_h(int alg, const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, uint8_t *prk);
void hkdf_expand_h(int alg, const uint8_t *prk, size_t plen, const uint8_t *info, size_t ilen, uint8_t *out, size_t olen);
/* SHA-256 conveniences (the original interface; used by the tests) */
void hmac_sha256(const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32]);
void hkdf_extract(const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, uint8_t prk[32]);
void hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t ilen, uint8_t *out, size_t olen);  /* olen <= 8160 */
/* TLS 1.2 PRF (RFC 5246 5): P_hash(secret, label || seed) */
void tls12_prf(int alg, const uint8_t *secret, size_t slen, const char *label,
               const uint8_t *seed, size_t seedlen, uint8_t *out, size_t olen);

/* ---- AES (128/256) and GCM ---- */
struct aes { uint8_t rk[240]; int rounds; };
void aes_init(struct aes *a, const uint8_t *key, int keylen);           /* keylen 16 or 32 */
void aes_encrypt_block(const struct aes *a, const uint8_t in[16], uint8_t out[16]);

struct gcm { struct aes aes; uint64_t hh[16], hl[16]; };                /* 4-bit multiplication tables for H */
void gcm_init(struct gcm *g, const uint8_t *key, int keylen);
/* 96-bit IV. out may alias in. tag is 16 bytes. */
void gcm_encrypt(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen,
                 const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16]);
/* Verifies the tag over the ciphertext first; returns 0 and decrypts (out may alias in) on success,
 * -1 (and writes nothing) if authentication fails. */
int  gcm_decrypt(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen,
                 const uint8_t *in, size_t len, const uint8_t tag[16], uint8_t *out);

/* ---- ChaCha20-Poly1305 (RFC 8439) ---- */
void chachapoly_encrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                        const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16]);
int  chachapoly_decrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                        const uint8_t *in, size_t len, const uint8_t tag[16], uint8_t *out);

/* ---- X25519 (RFC 7748) ---- */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* ---- P-256 ECDH (secp256r1) ---- */
/* scalar: 32 big-endian bytes, must be in [1, n-1] (p256_scalar_ok). pub: 65-byte uncompressed point. */
int  p256_scalar_ok(const uint8_t scalar[32]);
int  p256_pubkey(uint8_t pub[65], const uint8_t scalar[32]);                       /* 0 ok */
int  p256_ecdh(uint8_t shared[32], const uint8_t scalar[32], const uint8_t peer[65]); /* 0 ok, -1 bad point */

/* constant-time compare: 0 if equal */
int  ct_memcmp(const void *a, const void *b, size_t n);
#endif
