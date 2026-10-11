#ifndef X509_H
#define X509_H
/*
 * x509.h -- X.509 certificate parsing and path validation (RFC 5280).
 *
 * Small, freestanding and allocation-free so the same code runs in the 32-bit
 * kernel (tls.c) and in libslweb (the ring-3 browser). Parsing only reads the
 * DER in place; every pointer in a struct x509_cert points into the caller's
 * buffer, which must outlive the struct.
 *
 * Verification covers what TLS needs: the SubjectPublicKeyInfo of RSA and EC
 * (P-256 / P-384) keys, the sha256/sha384WithRSAEncryption, RSASSA-PSS and
 * ecdsa-with-SHA256/384 signature algorithms, validity windows, DNS subject
 * alternative names (with single-label wildcards) and a simple chain walk that
 * terminates at a trusted root in a struct x509_store.
 */
#include <stdint.h>
#include <stddef.h>

/* Public-key algorithms. */
enum {
    X509_PK_UNKNOWN = 0,
    X509_PK_RSA,
    X509_PK_EC,
};

/* Named curves. */
enum {
    X509_CURVE_NONE = 0,
    X509_CURVE_P256,
    X509_CURVE_P384,
};

/* Signature algorithms (also used for TLS CertificateVerify / ServerKeyExchange). */
enum {
    X509_SIG_UNKNOWN = 0,
    X509_SIG_RSA_PKCS1_SHA256,
    X509_SIG_RSA_PKCS1_SHA384,
    X509_SIG_RSA_PSS_SHA256,
    X509_SIG_RSA_PSS_SHA384,
    X509_SIG_ECDSA_SHA256,
    X509_SIG_ECDSA_SHA384,
};

struct x509_pubkey {
    int            type;             /* X509_PK_*                        */
    int            curve;            /* X509_CURVE_* (EC only)           */
    const uint8_t *key;              /* RSA: RSAPublicKey DER; EC: point */
    size_t         keylen;
};

struct x509_cert {
    const uint8_t *der;   size_t der_len;
    const uint8_t *tbs;   size_t tbs_len;      /* exact DER covered by the signature */
    int            sig_scheme;                 /* X509_SIG_* */
    const uint8_t *sig;   size_t sig_len;
    const uint8_t *issuer;  size_t issuer_len; /* encoded Name */
    const uint8_t *subject; size_t subject_len;
    int64_t        not_before, not_after;      /* Unix seconds */

    struct x509_pubkey pubkey;

    int has_basic_constraints, is_ca;
    int has_key_usage, key_cert_sign;
    int has_eku, eku_server_auth;

    const uint8_t *san; size_t san_len;        /* subjectAltName extnValue (GeneralNames DER) */
    const uint8_t *cn;  size_t cn_len;         /* subject commonName (first), else 0 */
};

/* Path-validation results. */
enum {
    X509_OK            = 0,
    X509_ERR_MALFORMED = -1,
    X509_ERR_NOT_YET   = -2,
    X509_ERR_EXPIRED   = -3,
    X509_ERR_HOSTNAME  = -4,
    X509_ERR_NO_ISSUER = -5,
    X509_ERR_BAD_SIG   = -6,
    X509_ERR_NOT_CA    = -7,
    X509_ERR_UNTRUSTED = -8,
    X509_ERR_LOOP      = -9,
};

#define X509_MAX_CHAIN   8
#define X509_MAX_TRUSTED 256

/* A set of trusted root certificates (borrowed DER pointers). */
struct x509_store {
    const uint8_t *ders[X509_MAX_TRUSTED];
    size_t         lens[X509_MAX_TRUSTED];
    int            count;
};

void x509_store_init(struct x509_store *s);
int  x509_store_add_der(struct x509_store *s, const uint8_t *der, size_t len);

/* Parse one DER certificate. Returns X509_OK or X509_ERR_MALFORMED. */
int x509_parse(struct x509_cert *c, const uint8_t *der, size_t len);

/* Does the (already parsed) public key verify `sig` over `msg` with `scheme`? */
int x509_verify_sig(const struct x509_pubkey *pk, int scheme,
                    const uint8_t *msg, size_t msglen,
                    const uint8_t *sig, size_t siglen);

/* Names compare as their encoded DER (issuer == subject for the same CA). */
int x509_same_name(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen);

/* 0 if `now` (Unix seconds) is inside the validity window. */
int x509_check_time(const struct x509_cert *c, int64_t now);

/* 1 if host matches the certificate's SAN (or, absent SAN, its commonName). */
int x509_check_hostname(const struct x509_cert *c, const char *host);

/*
 * Validate a server-presented chain (ders[0] = leaf) against roots.
 * `hostname` may be NULL to skip the name check. Returns X509_OK or a X509_ERR_*.
 */
int x509_verify_chain(const uint8_t *const *ders, const size_t *lens, int n,
                      const struct x509_store *roots, const char *hostname, int64_t now);

/* Decode the first PEM CERTIFICATE block into out. Returns bytes, or -1. */
int x509_pem_decode(const char *pem, size_t len, uint8_t *out, size_t cap);

/*
 * Call cb(der, derlen, user) for every PEM CERTIFICATE block, using `scratch`
 * as temporary DER storage for each block. Returns the number of blocks, or -1
 * if the base64 is malformed.
 */
int x509_pem_foreach(const char *pem, size_t len,
                     uint8_t *scratch, size_t scap,
                     int (*cb)(const uint8_t *der, size_t derlen, void *user), void *user);

#endif
