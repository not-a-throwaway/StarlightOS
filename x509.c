/*
 * x509.c -- X.509 parsing and path validation. See x509.h.
 *
 * The DER reader is a few lines (der_tlv); everything else walks the structure
 * with it. Signature checks delegate to rsa.c / ecdsa.c, and hashing to
 * crypto.c, so this file has no crypto of its own.
 */
#include "x509.h"
#include "crypto.h"
#include "rsa.h"
#include "ecdsa.h"
#include "kstring.h"

/* ------------------------------------------------------------- DER reader */
/* Read the TLV at *p (which must match `tag`), point *val at its contents and
 * advance *p past it. Returns 0 on success, -1 on any malformation. */
static int der_tlv(const uint8_t **p, const uint8_t *end, uint8_t tag,
                   const uint8_t **val, size_t *len)
{
    if (*p >= end || **p != tag) return -1;
    (*p)++;
    if (*p >= end) return -1;
    size_t l;
    uint8_t first = *(*p)++;
    if (first & 0x80) {
        size_t nb = first & 0x7f;
        if (nb == 0 || nb > 4 || (size_t)(end - *p) < nb) return -1;
        l = 0;
        for (size_t i = 0; i < nb; i++) l = (l << 8) | *(*p)++;
    } else {
        l = first;
    }
    if ((size_t)(end - *p) < l) return -1;
    *val = *p;
    *len = l;
    *p += l;
    return 0;
}

/* --------------------------------------------------------------- OIDs */
static int oid_eq(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    return alen == blen && memcmp(a, b, alen) == 0;
}

static const uint8_t OID_RSA_ENC[]      = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01 }; /* 1.2.840.113549.1.1.1 */
static const uint8_t OID_SHA256_RSA[]   = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b }; /* 1.2.840.113549.1.1.11 */
static const uint8_t OID_SHA384_RSA[]   = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0c }; /* 1.2.840.113549.1.1.12 */
static const uint8_t OID_RSA_PSS[]      = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0a }; /* 1.2.840.113549.1.1.10 */
static const uint8_t OID_EC_PUBKEY[]    = { 0x2a,0x86,0x48,0xce,0x3d,0x02,0x01 };          /* 1.2.840.10045.2.1 */
static const uint8_t OID_ECDSA_SHA256[] = { 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02 };    /* 1.2.840.10045.4.3.2 */
static const uint8_t OID_ECDSA_SHA384[] = { 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03 };    /* 1.2.840.10045.4.3.3 */
static const uint8_t OID_P256[]         = { 0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07 };    /* 1.2.840.10045.3.1.7 */
static const uint8_t OID_P384[]         = { 0x2b,0x81,0x04,0x00,0x22 };                   /* 1.3.132.0.34 */
static const uint8_t OID_CN[]           = { 0x55,0x04,0x03 };                            /* 2.5.4.3 */
static const uint8_t OID_SAN[]          = { 0x55,0x1d,0x11 };                             /* 2.5.29.17 */
static const uint8_t OID_BASIC[]        = { 0x55,0x1d,0x13 };                             /* 2.5.29.19 */
static const uint8_t OID_KEYUSE[]       = { 0x55,0x1d,0x0f };                             /* 2.5.29.15 */
static const uint8_t OID_EKU[]          = { 0x55,0x1d,0x25 };                             /* 2.5.29.37 */
static const uint8_t OID_SERVER_AUTH[]  = { 0x2b,0x06,0x01,0x05,0x05,0x07,0x03,0x01 };    /* 1.3.6.1.5.5.7.3.1 */
static const uint8_t OID_SHA256[]       = { 0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01 }; /* 2.16.840.1.101.3.4.2.1 */
static const uint8_t OID_SHA384[]       = { 0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02 }; /* 2.16.840.1.101.3.4.2.2 */

/* ------------------------------------------------------- time conversion */
static int atoi_digits(const uint8_t *p, int n)
{
    int r = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        r = r * 10 + (p[i] - '0');
    }
    return r;
}

/* Days since 1970-01-01 from a proleptic Gregorian date (Hinnant's algorithm). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/* UTCTime (generalized = 0, 2-digit year) or GeneralizedTime (generalized = 1). */
static int parse_time(int generalized, const uint8_t *v, size_t len, int64_t *out)
{
    int ylen = generalized ? 4 : 2;
    if (len < (size_t)ylen + 10) return -1;
    const uint8_t *p = v + ylen;
    int y = atoi_digits(v, ylen);
    int mo = atoi_digits(p, 2), d = atoi_digits(p + 2, 2);
    int hh = atoi_digits(p + 4, 2), mi = atoi_digits(p + 6, 2);
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 || hh > 23 || mi > 59) return -1;
    if (!generalized) y += (y < 50) ? 2000 : 1900;
    int ss = (len >= (size_t)ylen + 11) ? atoi_digits(p + 8, 2) : 0;
    if (ss < 0 || ss > 60) return -1;
    *out = days_from_civil(y, mo, d) * 86400 + hh * 3600 + mi * 60 + ss;
    return 0;
}

/* ------------------------------------------------------------ sub-parsers */
static int parse_validity(struct x509_cert *c, const uint8_t *val, size_t len)
{
    const uint8_t *p = val, *end = val + len;
    const uint8_t *v; size_t vl;
    if (der_tlv(&p, end, 0x17, &v, &vl) == 0) { if (parse_time(0, v, vl, &c->not_before)) return -1; }
    else if (der_tlv(&p, end, 0x18, &v, &vl) == 0) { if (parse_time(1, v, vl, &c->not_before)) return -1; }
    else return -1;
    if (der_tlv(&p, end, 0x17, &v, &vl) == 0) { if (parse_time(0, v, vl, &c->not_after)) return -1; }
    else if (der_tlv(&p, end, 0x18, &v, &vl) == 0) { if (parse_time(1, v, vl, &c->not_after)) return -1; }
    else return -1;
    return 0;
}

/* First commonName in an encoded Name (best effort; a malformed Name is ignored). */
static void parse_cn(struct x509_cert *c, const uint8_t *name, size_t len)
{
    const uint8_t *p = name, *end = name + len;
    const uint8_t *seq; size_t seqlen;
    if (der_tlv(&p, end, 0x30, &seq, &seqlen)) return;
    const uint8_t *r = seq, *rend = seq + seqlen;
    while (r < rend) {
        const uint8_t *rdn; size_t rdnlen;
        if (der_tlv(&r, rend, 0x31, &rdn, &rdnlen)) return;
        const uint8_t *a = rdn, *aend = rdn + rdnlen;
        while (a < aend) {
            const uint8_t *atv; size_t atvlen;
            if (der_tlv(&a, aend, 0x30, &atv, &atvlen)) return;
            const uint8_t *ap = atv, *apend = atv + atvlen;
            const uint8_t *oid; size_t oidlen;
            if (der_tlv(&ap, apend, 0x06, &oid, &oidlen)) return;
            if (ap >= apend) continue;
            uint8_t tag = *ap;
            const uint8_t *val; size_t vlen;
            if (der_tlv(&ap, apend, tag, &val, &vlen)) return;
            if (!c->cn && oid_eq(oid, oidlen, OID_CN, sizeof OID_CN)) {
                c->cn = val; c->cn_len = vlen;
                return;
            }
        }
    }
}

static int parse_spki(struct x509_pubkey *pk, const uint8_t *spki, size_t len)
{
    const uint8_t *p = spki, *end = spki + len;
    const uint8_t *alg; size_t alglen;
    if (der_tlv(&p, end, 0x30, &alg, &alglen)) return -1;
    const uint8_t *ap = alg, *aend = alg + alglen;
    const uint8_t *oid; size_t oidlen;
    if (der_tlv(&ap, aend, 0x06, &oid, &oidlen)) return -1;
    const uint8_t *bits; size_t bitslen;
    if (der_tlv(&p, end, 0x03, &bits, &bitslen) || bitslen < 1 || bits[0] != 0) return -1;
    pk->key = bits + 1;
    pk->keylen = bitslen - 1;
    if (oid_eq(oid, oidlen, OID_RSA_ENC, sizeof OID_RSA_ENC)) {
        pk->type = X509_PK_RSA;
        return 0;
    }
    if (oid_eq(oid, oidlen, OID_EC_PUBKEY, sizeof OID_EC_PUBKEY)) {
        pk->type = X509_PK_EC;
        if (ap >= aend) return -1;
        uint8_t tag = *ap;
        const uint8_t *curve; size_t clen;
        if (der_tlv(&ap, aend, tag, &curve, &clen)) return -1;
        if (oid_eq(curve, clen, OID_P256, sizeof OID_P256)) pk->curve = X509_CURVE_P256;
        else if (oid_eq(curve, clen, OID_P384, sizeof OID_P384)) pk->curve = X509_CURVE_P384;
        else return -1;
        return 0;
    }
    return -1;
}

static int parse_basic_constraints(struct x509_cert *c, const uint8_t *val, size_t len)
{
    const uint8_t *p = val, *end = val + len;
    const uint8_t *seq; size_t seqlen;
    if (der_tlv(&p, end, 0x30, &seq, &seqlen)) return -1;
    const uint8_t *q = seq, *qend = seq + seqlen;
    c->has_basic_constraints = 1;
    if (q < qend && *q == 0x01) {
        const uint8_t *b; size_t bl;
        if (der_tlv(&q, qend, 0x01, &b, &bl)) return -1;
        c->is_ca = bl > 0 && b[0] != 0;
    }
    return 0;
}

static int parse_key_usage(struct x509_cert *c, const uint8_t *val, size_t len)
{
    const uint8_t *p = val, *end = val + len;
    const uint8_t *bits; size_t blen;
    if (der_tlv(&p, end, 0x03, &bits, &blen) || blen < 2) return -1;
    c->has_key_usage = 1;
    c->key_cert_sign = (bits[1] & 0x04) != 0;       /* bit 5 */
    return 0;
}

static int parse_eku(struct x509_cert *c, const uint8_t *val, size_t len)
{
    const uint8_t *p = val, *end = val + len;
    const uint8_t *seq; size_t seqlen;
    if (der_tlv(&p, end, 0x30, &seq, &seqlen)) return -1;
    const uint8_t *q = seq, *qend = seq + seqlen;
    c->has_eku = 1;
    while (q < qend) {
        const uint8_t *oid; size_t oidlen;
        if (der_tlv(&q, qend, 0x06, &oid, &oidlen)) return -1;
        if (oid_eq(oid, oidlen, OID_SERVER_AUTH, sizeof OID_SERVER_AUTH)) c->eku_server_auth = 1;
    }
    return 0;
}

static int parse_extensions(struct x509_cert *c, const uint8_t *exts, size_t len)
{
    const uint8_t *p = exts, *end = exts + len;
    const uint8_t *seq; size_t seqlen;
    if (der_tlv(&p, end, 0x30, &seq, &seqlen)) return -1;
    const uint8_t *e = seq, *eend = seq + seqlen;
    while (e < eend) {
        const uint8_t *ext; size_t extlen;
        if (der_tlv(&e, eend, 0x30, &ext, &extlen)) return -1;
        const uint8_t *ep = ext, *epend = ext + extlen;
        const uint8_t *oid; size_t oidlen;
        if (der_tlv(&ep, epend, 0x06, &oid, &oidlen)) return -1;
        if (ep < epend && *ep == 0x01) {            /* critical BOOLEAN, skip */
            const uint8_t *b; size_t bl;
            if (der_tlv(&ep, epend, 0x01, &b, &bl)) return -1;
        }
        const uint8_t *val; size_t vl;
        if (der_tlv(&ep, epend, 0x04, &val, &vl)) return -1;
        if (oid_eq(oid, oidlen, OID_KEYUSE, sizeof OID_KEYUSE)) { if (parse_key_usage(c, val, vl)) return -1; }
        else if (oid_eq(oid, oidlen, OID_BASIC, sizeof OID_BASIC)) { if (parse_basic_constraints(c, val, vl)) return -1; }
        else if (oid_eq(oid, oidlen, OID_EKU, sizeof OID_EKU)) { if (parse_eku(c, val, vl)) return -1; }
        else if (oid_eq(oid, oidlen, OID_SAN, sizeof OID_SAN)) { c->san = val; c->san_len = vl; }
        /* unknown extension: ignore */
    }
    return 0;
}

/* Signature algorithm: an OID plus optional parameters (PSS carries the hash). */
static int parse_pss(struct x509_cert *c, const uint8_t *params, size_t plen)
{
    const uint8_t *p = params, *end = params + plen;
    const uint8_t *seq; size_t seqlen;
    if (der_tlv(&p, end, 0x30, &seq, &seqlen)) return -1;
    const uint8_t *q = seq, *qend = seq + seqlen;
    int hash = -1, salt = -1;
    while (q < qend) {
        uint8_t tag = *q;
        const uint8_t *v; size_t vl;
        if (der_tlv(&q, qend, tag, &v, &vl)) return -1;
        if (tag == 0xa0) {                          /* [0] hashAlgorithm */
            const uint8_t *hp = v, *hend = v + vl;
            const uint8_t *ha; size_t hal;
            if (der_tlv(&hp, hend, 0x30, &ha, &hal)) return -1;
            const uint8_t *hpp = ha, *hpe = ha + hal;
            const uint8_t *ho; size_t hol;
            if (der_tlv(&hpp, hpe, 0x06, &ho, &hol)) return -1;
            if (oid_eq(ho, hol, OID_SHA256, sizeof OID_SHA256)) hash = HASH_SHA256;
            else if (oid_eq(ho, hol, OID_SHA384, sizeof OID_SHA384)) hash = HASH_SHA384;
            else return -1;
        } else if (tag == 0xa2) {                   /* [2] saltLength */
            const uint8_t *sp = v, *send = v + vl;
            const uint8_t *sv; size_t svl;
            if (der_tlv(&sp, send, 0x02, &sv, &svl) || svl > 4) return -1;
            salt = 0;
            for (size_t i = 0; i < svl; i++) salt = (salt << 8) | sv[i];
        }
    }
    if (hash < 0) return -1;
    if (salt >= 0 && (size_t)salt != hash_len(hash)) return -1;
    c->sig_scheme = (hash == HASH_SHA256) ? X509_SIG_RSA_PSS_SHA256 : X509_SIG_RSA_PSS_SHA384;
    return 0;
}

static int parse_sig_alg(struct x509_cert *c, const uint8_t *alg, size_t len)
{
    const uint8_t *p = alg, *end = alg + len;
    const uint8_t *oid; size_t oidlen;
    if (der_tlv(&p, end, 0x06, &oid, &oidlen)) return -1;
    if (oid_eq(oid, oidlen, OID_RSA_PSS, sizeof OID_RSA_PSS))
        return parse_pss(c, p, (size_t)(end - p));
    if (oid_eq(oid, oidlen, OID_SHA256_RSA, sizeof OID_SHA256_RSA)) c->sig_scheme = X509_SIG_RSA_PKCS1_SHA256;
    else if (oid_eq(oid, oidlen, OID_SHA384_RSA, sizeof OID_SHA384_RSA)) c->sig_scheme = X509_SIG_RSA_PKCS1_SHA384;
    else if (oid_eq(oid, oidlen, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256)) c->sig_scheme = X509_SIG_ECDSA_SHA256;
    else if (oid_eq(oid, oidlen, OID_ECDSA_SHA384, sizeof OID_ECDSA_SHA384)) c->sig_scheme = X509_SIG_ECDSA_SHA384;
    else return -1;
    return 0;
}

/* ----------------------------------------------------------------- parse */
int x509_parse(struct x509_cert *c, const uint8_t *der, size_t len)
{
    memset(c, 0, sizeof *c);
    const uint8_t *p = der, *end = der + len;
    const uint8_t *seq; size_t seqlen;
    if (der_tlv(&p, end, 0x30, &seq, &seqlen)) return X509_ERR_MALFORMED;
    const uint8_t *q = seq, *qend = seq + seqlen;

    const uint8_t *tbs_start = q, *tbs; size_t tbslen;
    if (der_tlv(&q, qend, 0x30, &tbs, &tbslen)) return X509_ERR_MALFORMED;
    c->der = der; c->der_len = len;
    c->tbs = tbs_start; c->tbs_len = (size_t)(q - tbs_start);

    const uint8_t *alg; size_t alglen;
    if (der_tlv(&q, qend, 0x30, &alg, &alglen) || parse_sig_alg(c, alg, alglen)) return X509_ERR_MALFORMED;
    const uint8_t *bits; size_t blen;
    if (der_tlv(&q, qend, 0x03, &bits, &blen) || blen < 1 || bits[0] != 0) return X509_ERR_MALFORMED;
    c->der = der; c->der_len = len;
    c->sig = bits + 1; c->sig_len = blen - 1;

    const uint8_t *t = tbs, *tend = tbs + tbslen;
    const uint8_t *v; size_t vl;
    if (t < tend && *t == 0xa0) { if (der_tlv(&t, tend, 0xa0, &v, &vl)) return X509_ERR_MALFORMED; }  /* version */
    if (der_tlv(&t, tend, 0x02, &v, &vl)) return X509_ERR_MALFORMED;                                  /* serialNumber */
    if (der_tlv(&t, tend, 0x30, &v, &vl)) return X509_ERR_MALFORMED;                                  /* signature */
    const uint8_t *issuer_start = t;
    if (der_tlv(&t, tend, 0x30, &v, &vl)) return X509_ERR_MALFORMED;                                  /* issuer */
    c->issuer = issuer_start; c->issuer_len = (size_t)(t - issuer_start);
    const uint8_t *val; size_t valn;
    if (der_tlv(&t, tend, 0x30, &val, &valn)) return X509_ERR_MALFORMED;                              /* validity */
    if (parse_validity(c, val, valn)) return X509_ERR_MALFORMED;
    const uint8_t *subject_start = t;
    if (der_tlv(&t, tend, 0x30, &v, &vl)) return X509_ERR_MALFORMED;                                  /* subject */
    c->subject = subject_start; c->subject_len = (size_t)(t - subject_start);
    parse_cn(c, c->subject, c->subject_len);
    const uint8_t *spki; size_t spkilen;
    if (der_tlv(&t, tend, 0x30, &spki, &spkilen)) return X509_ERR_MALFORMED;                          /* subjectPublicKeyInfo */
    if (parse_spki(&c->pubkey, spki, spkilen)) return X509_ERR_MALFORMED;
    while (t < tend) {                                  /* optional unique IDs and extensions */
        uint8_t tag = *t;
        const uint8_t *u; size_t ul;
        if (der_tlv(&t, tend, tag, &u, &ul)) return X509_ERR_MALFORMED;
        if (tag == 0xa3 && parse_extensions(c, u, ul)) return X509_ERR_MALFORMED;
    }
    return X509_OK;
}

/* ------------------------------------------------------------- signature */
/* X.509 ECDSA signatures are DER SEQUENCE { r INTEGER, s INTEGER }. */
static int decode_ecdsa_sig(const uint8_t *sig, size_t len, uint8_t *r, uint8_t *s, int fs)
{
    const uint8_t *p = sig, *end = sig + len;
    const uint8_t *seq; size_t seqlen;
    if (der_tlv(&p, end, 0x30, &seq, &seqlen)) return -1;
    const uint8_t *q = seq, *qend = seq + seqlen;
    const uint8_t *rb; size_t rbl, sbl;
    const uint8_t *sb;
    if (der_tlv(&q, qend, 0x02, &rb, &rbl)) return -1;
    if (der_tlv(&q, qend, 0x02, &sb, &sbl)) return -1;
    while (rbl > 1 && rb[0] == 0) { rb++; rbl--; }
    while (sbl > 1 && sb[0] == 0) { sb++; sbl--; }
    if (rbl > (size_t)fs || sbl > (size_t)fs) return -1;
    memset(r, 0, fs); memset(s, 0, fs);
    memcpy(r + fs - rbl, rb, rbl);
    memcpy(s + fs - sbl, sb, sbl);
    return 0;
}

static int hash_for_scheme(int scheme)
{
    switch (scheme) {
    case X509_SIG_RSA_PKCS1_SHA256:
    case X509_SIG_RSA_PSS_SHA256:
    case X509_SIG_ECDSA_SHA256: return HASH_SHA256;
    case X509_SIG_RSA_PKCS1_SHA384:
    case X509_SIG_RSA_PSS_SHA384:
    case X509_SIG_ECDSA_SHA384: return HASH_SHA384;
    default: return -1;
    }
}

int x509_verify_sig(const struct x509_pubkey *pk, int scheme,
                    const uint8_t *msg, size_t msglen,
                    const uint8_t *sig, size_t siglen)
{
    int alg = hash_for_scheme(scheme);
    if (alg < 0) return -1;
    uint8_t hash[HASH_MAX_LEN];
    hash_data(alg, msg, msglen, hash);
    size_t hl = hash_len(alg);

    if (pk->type == X509_PK_RSA) {
        const uint8_t *p = pk->key, *end = pk->key + pk->keylen;
        const uint8_t *seq; size_t seqlen;
        if (der_tlv(&p, end, 0x30, &seq, &seqlen)) return -1;
        const uint8_t *q = seq, *qend = seq + seqlen;
        const uint8_t *n; size_t nlen;
        const uint8_t *e; size_t elen;
        if (der_tlv(&q, qend, 0x02, &n, &nlen)) return -1;
        if (der_tlv(&q, qend, 0x02, &e, &elen)) return -1;
        while (nlen > 1 && n[0] == 0) { n++; nlen--; }
        while (elen > 1 && e[0] == 0) { e++; elen--; }
        if (scheme == X509_SIG_RSA_PKCS1_SHA256 || scheme == X509_SIG_RSA_PKCS1_SHA384) {
            static const uint8_t di256[] = { 0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20 };
            static const uint8_t di384[] = { 0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30 };
            const uint8_t *pre = (alg == HASH_SHA256) ? di256 : di384;
            uint8_t di[19 + HASH_MAX_LEN];
            memcpy(di, pre, 19);
            memcpy(di + 19, hash, hl);
            return rsa_verify_v15(n, nlen, e, elen, sig, siglen, di, 19 + hl);
        }
        return rsa_verify_pss(n, nlen, e, elen, sig, siglen, alg, hash, hl);
    }

    if (pk->type == X509_PK_EC) {
        int fs = (pk->curve == X509_CURVE_P256) ? 32 : (pk->curve == X509_CURVE_P384) ? 48 : 0;
        if (!fs) return -1;
        uint8_t raw[96];
        if (decode_ecdsa_sig(sig, siglen, raw, raw + fs, fs)) return -1;
        if (pk->curve == X509_CURVE_P256)
            return ecdsa_verify_p256(pk->key, pk->keylen, raw, 2 * fs, hash, hl);
        return ecdsa_verify_p384(pk->key, pk->keylen, raw, 2 * fs, hash, hl);
    }
    return -1;
}

/* ---------------------------------------------------------------- names */
int x509_same_name(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    return alen == blen && memcmp(a, b, alen) == 0;
}

int x509_check_time(const struct x509_cert *c, int64_t now)
{
    if (now < c->not_before) return X509_ERR_NOT_YET;
    if (now > c->not_after) return X509_ERR_EXPIRED;
    return X509_OK;
}

static int tolower_ascii(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; }

static int host_eq_ci(const uint8_t *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++)
        if (tolower_ascii(a[i]) != tolower_ascii((uint8_t)b[i])) return 0;
    return 1;
}

/* DNS name match with a single leading "*." wildcard (RFC 6125). */
static int dns_match(const uint8_t *pat, size_t plen, const char *host, size_t hlen)
{
    if (plen >= 2 && pat[0] == '*' && pat[1] == '.') {
        const uint8_t *suf = pat + 2;
        size_t sl = plen - 2;
        if (hlen <= sl) return 0;
        if (host[hlen - sl - 1] != '.') return 0;                     /* need a dot before the suffix */
        for (size_t i = 0; i + sl + 1 < hlen; i++)
            if (host[i] == '.') return 0;                             /* wildcard covers one label only */
        return host_eq_ci(suf, sl, host + hlen - sl, sl);
    }
    return host_eq_ci(pat, plen, host, hlen);
}

int x509_check_hostname(const struct x509_cert *c, const char *host)
{
    size_t hlen = strlen(host);
    if (c->san_len) {
        const uint8_t *p = c->san, *end = c->san + c->san_len;
        const uint8_t *seq; size_t seqlen;
        if (der_tlv(&p, end, 0x30, &seq, &seqlen)) return 0;
        const uint8_t *g = seq, *gend = seq + seqlen;
        while (g < gend) {
            uint8_t tag = *g;
            const uint8_t *v; size_t vl;
            if (der_tlv(&g, gend, tag, &v, &vl)) return 0;
            if (tag == 0x82 && dns_match(v, vl, host, hlen)) return 1;   /* [2] dNSName */
        }
        return 0;
    }
    return c->cn_len ? dns_match(c->cn, c->cn_len, host, hlen) : 0;
}

/* --------------------------------------------------------------- store */
void x509_store_init(struct x509_store *s) { s->count = 0; }

int x509_store_add_der(struct x509_store *s, const uint8_t *der, size_t len)
{
    if (s->count >= X509_MAX_TRUSTED) return -1;
    s->ders[s->count] = der;
    s->lens[s->count] = len;
    s->count++;
    return 0;
}

/* ---------------------------------------------------------------- chain */
static int issued_by(const struct x509_cert *child, const struct x509_cert *issuer)
{
    if (!x509_same_name(child->issuer, child->issuer_len, issuer->subject, issuer->subject_len))
        return 0;
    return x509_verify_sig(&issuer->pubkey, child->sig_scheme,
                           child->tbs, child->tbs_len, child->sig, child->sig_len) == 0;
}

int x509_verify_chain(const uint8_t *const *ders, const size_t *lens, int n,
                      const struct x509_store *roots, const char *hostname, int64_t now)
{
    if (n < 1 || n > X509_MAX_CHAIN) return X509_ERR_MALFORMED;
    struct x509_cert chain[X509_MAX_CHAIN];
    for (int i = 0; i < n; i++)
        if (x509_parse(&chain[i], ders[i], lens[i]) != X509_OK) return X509_ERR_MALFORMED;

    int rc = x509_check_time(&chain[0], now);
    if (rc != X509_OK) return rc;
    if (hostname && !x509_check_hostname(&chain[0], hostname)) return X509_ERR_HOSTNAME;

    int used[X509_MAX_CHAIN] = { 0 };
    used[0] = 1;
    const struct x509_cert *cur = &chain[0];
    for (int depth = 0; depth <= X509_MAX_CHAIN; depth++) {
        for (int i = 0; i < roots->count; i++) {          /* terminate at a trusted root */
            struct x509_cert rc2;
            if (x509_parse(&rc2, roots->ders[i], roots->lens[i]) != X509_OK) continue;
            if (issued_by(cur, &rc2)) return X509_OK;
        }
        int found = -1;
        for (int j = 1; j < n; j++) {
            if (used[j]) continue;
            if (issued_by(cur, &chain[j])) { found = j; break; }
        }
        if (found < 0) return X509_ERR_UNTRUSTED;
        const struct x509_cert *iss = &chain[found];
        if (iss->has_basic_constraints && !iss->is_ca) return X509_ERR_NOT_CA;
        if (iss->has_key_usage && !iss->key_cert_sign) return X509_ERR_NOT_CA;
        rc = x509_check_time(iss, now);
        if (rc != X509_OK) return rc;
        used[found] = 1;
        cur = iss;
    }
    return X509_ERR_LOOP;
}

/* ------------------------------------------------------------------ PEM */
static int b64val(int ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

/* Decode base64 in [s, end) into out; returns bytes or -1. */
static int b64decode(const char *s, const char *end, uint8_t *out, size_t cap)
{
    int quad[4], qn = 0, pad = 0;
    size_t o = 0;
    for (const char *p = s; p < end; p++) {
        int ch = (unsigned char)*p;
        if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') continue;
        if (ch == '=') { quad[qn++] = 0; pad++; }
        else {
            int val = b64val(ch);
            if (val < 0) return -1;
            quad[qn++] = val;
        }
        if (qn == 4) {
            uint32_t w = ((uint32_t)quad[0] << 18) | ((uint32_t)quad[1] << 12) |
                         ((uint32_t)quad[2] << 6) | (uint32_t)quad[3];
            int n = 3 - pad;
            if (n < 1 || o + (size_t)n > cap) return -1;
            out[o++] = (uint8_t)(w >> 16);
            if (n >= 2) out[o++] = (uint8_t)(w >> 8);
            if (n >= 3) out[o++] = (uint8_t)w;
            qn = 0; pad = 0;
        }
    }
    if (qn != 0) return -1;                      /* truncated group */
    return (int)o;
}

static const char BEGIN[] = "-----BEGIN CERTIFICATE-----";
static const char END[]   = "-----END CERTIFICATE-----";

int x509_pem_decode(const char *pem, size_t len, uint8_t *out, size_t cap)
{
    const char *end = pem + len;
    const char *b = NULL;
    for (const char *p = pem; p + sizeof BEGIN - 1 <= end; p++) {
        if (memcmp(p, BEGIN, sizeof BEGIN - 1) == 0) { b = p + sizeof BEGIN - 1; break; }
    }
    if (!b) return -1;
    const char *e = NULL;
    for (const char *p = b; p + sizeof END - 1 <= end; p++) {
        if (memcmp(p, END, sizeof END - 1) == 0) { e = p; break; }
    }
    if (!e) return -1;
    return b64decode(b, e, out, cap);
}

int x509_pem_foreach(const char *pem, size_t len,
                     uint8_t *scratch, size_t scap,
                     int (*cb)(const uint8_t *der, size_t derlen, void *user), void *user)
{
    const char *end = pem + len;
    const char *p = pem;
    int count = 0;
    while (p + sizeof BEGIN - 1 <= end) {
        const char *b = NULL;
        for (; p + sizeof BEGIN - 1 <= end; p++)
            if (memcmp(p, BEGIN, sizeof BEGIN - 1) == 0) { b = p + sizeof BEGIN - 1; break; }
        if (!b) break;
        p = b;
        const char *e = NULL;
        for (; p + sizeof END - 1 <= end; p++)
            if (memcmp(p, END, sizeof END - 1) == 0) { e = p; break; }
        if (!e) return -1;
        int dlen = b64decode(b, e, scratch, scap);
        if (dlen < 0) return -1;
        if (cb && cb(scratch, (size_t)dlen, user)) return count;
        count++;
        p = e + sizeof END - 1;
    }
    return count;
}
