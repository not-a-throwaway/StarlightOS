/*
 * tls.c -- TLS 1.3 / 1.2 client (RFC 8446, RFC 5246 + RFC 7627). See tls.h for scope and limits.
 *
 *   ClientHello (x25519 + P-256 key shares, 9 cipher suites, SNI, EMS, padding)
 *     -> ServerHello decides the version:
 *        1.3: derive handshake keys, stream EncryptedExtensions/Certificate/CertificateVerify/Finished
 *             through a small parser (only Finished is verified), client Finished, application keys.
 *        1.2: stream Certificate/ServerKeyExchange/ServerHelloDone, ClientKeyExchange, master secret via the
 *             PRF (extended master secret when negotiated), CCS + Finished both ways.
 *   All large state lives in the heap-allocated tls_conn: the 32-bit kernel stack is only 16 KiB.
 */
#include "tls.h"
#include "crypto.h"
#include "krand.h"
#include "net.h"
#include "kstring.h"
#include "kheap.h"
#include "kprintf.h"
#include "ktime.h"
#include "ksched.h"

#define REC_MAX_PLAIN   16384
#define REC_MAX_CIPHER  (REC_MAX_PLAIN + 2048)
#define HS_MAX_BYTES    (512u * 1024u)
#define HS_KEEP         1200                       /* largest handshake body kept in memory (ServerKeyExchange, Finished) */

enum { CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APPDATA = 23 };
enum { HS_HELLO_REQUEST = 0, HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_SESSION_TICKET = 4, HS_ENCRYPTED_EXT = 8,
       HS_CERTIFICATE = 11, HS_SERVER_KEY_EXCHANGE = 12, HS_CERT_REQUEST = 13, HS_SERVER_HELLO_DONE = 14,
       HS_CERT_VERIFY = 15, HS_CLIENT_KEY_EXCHANGE = 16, HS_FINISHED = 20, HS_CERT_STATUS = 22, HS_KEY_UPDATE = 24 };
enum { GROUP_X25519 = 0x001d, GROUP_P256 = 0x0017 };
enum { AEAD_NONE = 0, AEAD_AES128, AEAD_AES256, AEAD_CHACHA };

struct suite { uint16_t id; uint8_t aead, hash, v13; const char *name; };
static const struct suite SUITES[] = {
    { 0x1303, AEAD_CHACHA, HASH_SHA256, 1, "TLS_CHACHA20_POLY1305_SHA256" },
    { 0x1301, AEAD_AES128, HASH_SHA256, 1, "TLS_AES_128_GCM_SHA256" },
    { 0x1302, AEAD_AES256, HASH_SHA384, 1, "TLS_AES_256_GCM_SHA384" },
    { 0xCCA9, AEAD_CHACHA, HASH_SHA256, 0, "ECDHE-ECDSA-CHACHA20-POLY1305" },
    { 0xCCA8, AEAD_CHACHA, HASH_SHA256, 0, "ECDHE-RSA-CHACHA20-POLY1305" },
    { 0xC02B, AEAD_AES128, HASH_SHA256, 0, "ECDHE-ECDSA-AES128-GCM-SHA256" },
    { 0xC02F, AEAD_AES128, HASH_SHA256, 0, "ECDHE-RSA-AES128-GCM-SHA256" },
    { 0xC02C, AEAD_AES256, HASH_SHA384, 0, "ECDHE-ECDSA-AES256-GCM-SHA384" },
    { 0xC030, AEAD_AES256, HASH_SHA384, 0, "ECDHE-RSA-AES256-GCM-SHA384" },
};
#define NSUITES ((int)(sizeof(SUITES) / sizeof(SUITES[0])))

static const struct suite *find_suite(unsigned id)
{
    for (int i = 0; i < NSUITES; i++) if (SUITES[i].id == id) return &SUITES[i];
    return NULL;
}

struct aead { int kind; struct gcm gcm; uint8_t key[32]; };

static void aead_set(struct aead *a, int kind, const uint8_t *key)
{
    a->kind = kind;
    if (kind == AEAD_AES128) gcm_init(&a->gcm, key, 16);
    else if (kind == AEAD_AES256) gcm_init(&a->gcm, key, 32);
    else memcpy(a->key, key, 32);
}
static size_t aead_keylen(int kind) { return kind == AEAD_AES128 ? 16 : 32; }

static void aead_seal(const struct aead *a, const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                      const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16])
{
    if (a->kind == AEAD_CHACHA) chachapoly_encrypt(a->key, nonce, aad, alen, in, len, out, tag);
    else gcm_encrypt(&a->gcm, nonce, aad, alen, in, len, out, tag);
}
static int aead_open(const struct aead *a, const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                     const uint8_t *in, size_t len, const uint8_t tag[16], uint8_t *out)
{
    if (a->kind == AEAD_CHACHA) return chachapoly_decrypt(a->key, nonce, aad, alen, in, len, tag, out);
    return gcm_decrypt(&a->gcm, nonce, aad, alen, in, len, tag, out);
}

struct tls_conn {
    int       sock;
    int       failed;                 /* sticky error */
    int       eof;
    int       version;                /* 0 until ServerHello, then 0x0304 or 0x0303 */
    const struct suite *suite;
    int       group;
    /* record protection, per direction */
    struct aead wk, rk;
    uint8_t   wiv[12], riv[12];       /* 1.3: full IV; 1.2 GCM: first 4 bytes are the salt; 1.2 ChaCha: full IV */
    uint8_t   wsecret[48], rsecret[48];   /* 1.3 traffic secrets (KeyUpdate) */
    uint64_t  wseq, rseq;
    int       wenc, renc;
    /* receive: one record at a time; decrypted in place */
    uint8_t   rbuf[5 + REC_MAX_CIPHER];
    size_t    rlen;
    const uint8_t *pdata; size_t plen;     /* undelivered plaintext (points into rbuf) */
    uint8_t   sbuf[5 + 8 + REC_MAX_PLAIN + 1 + 16];
    /* handshake-only state */
    uint8_t   chbuf[1024];
    struct sha256_ctx th256;
    struct sha512_ctx th384;
    uint8_t   hdr[4]; unsigned hdr_len; unsigned msg_type; size_t body_len, body_got; int keep;
    uint8_t   keepbuf[HS_KEEP];
    int       got_ee, got_cert, got_cv, got_fin, got_shd, got_certreq, got_ske;
    uint8_t   th_before_fin[48];
    size_t    hs_bytes;
    uint8_t   srv_kx[65]; size_t srv_kx_len; int srv_group;     /* 1.2 ServerKeyExchange */
    uint8_t   desc[96];
};

static int g_last_alert = -1;
static int g_trace;

void tls_set_trace(int on) { g_trace = on; }
int tls_last_alert(void) { return g_last_alert; }
#define TR(...) do { if (g_trace) kprintf(__VA_ARGS__); } while (0)

const char *tls_alert_name(int d)
{
    switch (d) {
    case 0:   return "close_notify";          case 10:  return "unexpected_message";
    case 20:  return "bad_record_mac";        case 22:  return "record_overflow";
    case 40:  return "handshake_failure";     case 42:  return "bad_certificate";
    case 47:  return "illegal_parameter";     case 48:  return "unknown_ca";
    case 50:  return "decode_error";          case 70:  return "protocol_version";
    case 71:  return "insufficient_security"; case 80:  return "internal_error";
    case 86:  return "inappropriate_fallback";case 90:  return "user_canceled";
    case 100: return "no_renegotiation";      case 109: return "missing_extension";
    case 110: return "unsupported_extension"; case 112: return "unrecognized_name";
    case 116: return "certificate_required";  case 120: return "no_application_protocol";
    default:  return "alert";
    }
}

static const char *group_name(int g) { return g == GROUP_X25519 ? "x25519" : g == GROUP_P256 ? "secp256r1" : "?"; }

const char *tls_describe(const struct tls_conn *c)
{
    if (!c || !c->suite) return "TLS";
    ksnprintf((char *)((struct tls_conn *)c)->desc, sizeof(c->desc), "TLSv1.%d %s %s",
              c->version == 0x0304 ? 3 : 2, c->suite->name, group_name(c->group));
    return (const char *)c->desc;
}

const char *tls_error_text(int e)
{
    switch (e) {
    case 0:                return "ok";
    case TLS_E_ABORT:      return "interrupted";
    case TLS_E_IO:         return "connection lost during TLS";
    case TLS_E_TIMEOUT:    return "TLS handshake timed out";
    case TLS_E_PROTO:      return "server did not answer with valid TLS (wrong port, or a plain-HTTP server?)";
    case TLS_E_UNSUPPORTED:return "server requires something this TLS client lacks (HelloRetryRequest, client certificate, TLS 1.1 or older, unusual ciphers)";
    case TLS_E_ALERT:      return "server sent a TLS alert";
    case TLS_E_MAC:        return "TLS integrity check failed";
    case TLS_E_NOMEM:      return "out of memory";
    default:               return "TLS error";
    }
}

/* ------------------------------------------------------------ small helpers */
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static unsigned get16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint8_t *w16(uint8_t *p, unsigned v) { put16(p, v); return p + 2; }

static void th_update(struct tls_conn *c, const void *d, size_t n)
{
    sha256_update(&c->th256, d, n);
    sha512_update(&c->th384, d, n);
}
static void th_get(const struct tls_conn *c, int alg, uint8_t *out)
{
    if (alg == HASH_SHA384) { struct sha512_ctx t = c->th384; sha512_final(&t, out); }
    else { struct sha256_ctx t = c->th256; sha256_final(&t, out); }
}

static void expand_label(int alg, const uint8_t *secret, const char *label, const uint8_t *ctx, size_t ctxlen,
                         uint8_t *out, size_t olen)
{
    uint8_t info[2 + 1 + 6 + 16 + 1 + HASH_MAX_LEN]; size_t n = 0, ll = strlen(label);
    info[n++] = (uint8_t)(olen >> 8); info[n++] = (uint8_t)olen;
    info[n++] = (uint8_t)(6 + ll);
    memcpy(info + n, "tls13 ", 6); n += 6;
    memcpy(info + n, label, ll); n += ll;
    info[n++] = (uint8_t)ctxlen;
    if (ctxlen) memcpy(info + n, ctx, ctxlen);
    n += ctxlen;
    hkdf_expand_h(alg, secret, hash_len(alg), info, n, out, olen);
}

/* 1.3: traffic secret -> AEAD key + IV */
static void install_keys13(const struct suite *s, const uint8_t *secret, struct aead *a, uint8_t iv[12])
{
    uint8_t key[32];
    expand_label(s->hash, secret, "key", NULL, 0, key, aead_keylen(s->aead));
    aead_set(a, s->aead, key);
    expand_label(s->hash, secret, "iv", NULL, 0, iv, 12);
    memset(key, 0, sizeof(key));
}

static int tcp_write_all(int sock, const uint8_t *p, size_t n)
{
    int r = tcp_send(sock, p, n);
    if (r < 0) return r == TCP_ERR_ABORT ? TLS_E_ABORT : TLS_E_IO;
    return 0;
}

static void make_nonce(uint8_t nonce[12], const uint8_t iv[12], uint64_t seq)
{
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(seq >> (8 * i));
}

/* ------------------------------------------------------------ record layer */
static int rec_write(struct tls_conn *c, uint8_t type, const uint8_t *data, size_t len)
{
    uint8_t *p = c->sbuf;
    if (len > REC_MAX_PLAIN) return TLS_E_PROTO;
    if (!c->wenc) {
        p[0] = type; put16(p + 1, c->version ? 0x0303 : 0x0301); put16(p + 3, (unsigned)len);
        memcpy(p + 5, data, len);
        return tcp_write_all(c->sock, p, 5 + len);
    }
    uint8_t nonce[12];
    if (c->version == 0x0304) {
        size_t inner = len + 1;
        p[0] = CT_APPDATA; put16(p + 1, 0x0303); put16(p + 3, (unsigned)(inner + 16));
        memcpy(p + 5, data, len); p[5 + len] = type;
        make_nonce(nonce, c->wiv, c->wseq);
        aead_seal(&c->wk, nonce, p, 5, p + 5, inner, p + 5, p + 5 + inner);
        c->wseq++;
        return tcp_write_all(c->sock, p, 5 + inner + 16);
    }
    /* TLS 1.2: the outer type is the real type; GCM carries an 8-byte explicit nonce (we use the sequence number) */
    uint8_t aad[13];
    for (int i = 0; i < 8; i++) aad[i] = (uint8_t)(c->wseq >> (56 - 8 * i));
    aad[8] = type; aad[9] = 3; aad[10] = 3; put16(aad + 11, (unsigned)len);
    size_t pre = c->wk.kind == AEAD_CHACHA ? 0 : 8;
    if (pre) { memcpy(nonce, c->wiv, 4); memcpy(nonce + 4, aad, 8); }
    else make_nonce(nonce, c->wiv, c->wseq);
    p[0] = type; put16(p + 1, 0x0303); put16(p + 3, (unsigned)(pre + len + 16));
    if (pre) memcpy(p + 5, aad, 8);
    memcpy(p + 5 + pre, data, len);
    aead_seal(&c->wk, nonce, aad, 13, p + 5 + pre, len, p + 5 + pre, p + 5 + pre + len);
    c->wseq++;
    return tcp_write_all(c->sock, p, 5 + pre + len + 16);
}

/* Fill c->rbuf with one complete record. 0 = complete, 1 = timeout (partial state kept),
 * 2 = clean EOF between records, <0 = error. Never reads past the end of the record. */
static int rec_fill(struct tls_conn *c, uint32_t timeout_ms)
{
    for (;;) {
        size_t need;
        if (c->rlen < 5) need = 5 - c->rlen;
        else {
            size_t bl = get16(c->rbuf + 3);
            if (bl > REC_MAX_CIPHER) return TLS_E_PROTO;
            need = 5 + bl - c->rlen;
            if (need == 0) return 0;
        }
        int n = tcp_recv(c->sock, c->rbuf + c->rlen, need, timeout_ms);
        if (n > 0) { c->rlen += (size_t)n; continue; }
        if (n == -1) return 1;
        if (n == 0) return c->rlen ? TLS_E_IO : 2;
        return n == TCP_ERR_ABORT ? TLS_E_ABORT : TLS_E_IO;
    }
}

/* Decrypts the record in rbuf in place; *data points into rbuf, *type is the real content type. */
static int rec_decrypt(struct tls_conn *c, uint8_t *type, const uint8_t **data, size_t *len)
{
    size_t bl = get16(c->rbuf + 3);
    uint8_t nonce[12];
    if (c->version == 0x0304) {
        if (bl < 17) return TLS_E_PROTO;
        size_t clen = bl - 16;
        make_nonce(nonce, c->riv, c->rseq);
        if (aead_open(&c->rk, nonce, c->rbuf, 5, c->rbuf + 5, clen, c->rbuf + 5 + clen, c->rbuf + 5) != 0) return TLS_E_MAC;
        c->rseq++;
        size_t i = clen;
        while (i > 0 && c->rbuf[5 + i - 1] == 0) i--;          /* strip zero padding */
        if (i == 0) return TLS_E_PROTO;
        *type = c->rbuf[5 + i - 1]; *data = c->rbuf + 5; *len = i - 1;
        return 0;
    }
    size_t pre = c->rk.kind == AEAD_CHACHA ? 0 : 8;
    if (bl < pre + 16) return TLS_E_PROTO;
    size_t clen = bl - pre - 16;
    uint8_t aad[13];
    for (int i = 0; i < 8; i++) aad[i] = (uint8_t)(c->rseq >> (56 - 8 * i));
    aad[8] = c->rbuf[0]; aad[9] = 3; aad[10] = 3; put16(aad + 11, (unsigned)clen);
    if (pre) { memcpy(nonce, c->riv, 4); memcpy(nonce + 4, c->rbuf + 5, 8); }
    else make_nonce(nonce, c->riv, c->rseq);
    if (aead_open(&c->rk, nonce, aad, 13, c->rbuf + 5 + pre, clen, c->rbuf + 5 + pre + clen, c->rbuf + 5 + pre) != 0) return TLS_E_MAC;
    c->rseq++;
    *type = c->rbuf[0]; *data = c->rbuf + 5 + pre; *len = clen;
    return 0;
}

static int fail_alert(const uint8_t *alert, size_t len)
{
    g_last_alert = len >= 2 ? alert[1] : -1;
    TR("* TLS: server alert %d (%s)\n", g_last_alert, tls_alert_name(g_last_alert));
    return TLS_E_ALERT;
}

/* ------------------------------------------------------------ handshake message stream */
static int hs_message_done(struct tls_conn *c)
{
    int v13 = c->version == 0x0304;
    size_t flen = v13 ? hash_len(c->suite->hash) : 12;
    switch (c->msg_type) {
    case HS_FINISHED:
        if (c->body_len != flen) return TLS_E_PROTO;
        c->got_fin = 1; break;
    case HS_CERTIFICATE:
        if (v13 && (!c->got_ee || c->got_cert)) return TLS_E_PROTO;
        c->got_cert = 1; break;
    case HS_CERT_REQUEST:
        if (v13) return TLS_E_UNSUPPORTED;                                  /* we have no client certificate to give */
        c->got_certreq = 1; break;
    default:
        if (v13) {
            if (c->msg_type == HS_ENCRYPTED_EXT) { if (c->got_ee) return TLS_E_PROTO; c->got_ee = 1; }
            else if (c->msg_type == HS_CERT_VERIFY) { if (!c->got_cert || c->got_cv) return TLS_E_PROTO; c->got_cv = 1; }
            else if (c->msg_type != HS_NEW_SESSION_TICKET) return TLS_E_PROTO;
        } else {
            if (c->msg_type == HS_SERVER_KEY_EXCHANGE) {
                /* ECParameters: curve_type(1)=3 named_curve, curve(2), point_len(1), point; then the (ignored) signature */
                const uint8_t *k = c->keepbuf; size_t l = c->body_len;
                if (l < 4 || k[0] != 3) return TLS_E_UNSUPPORTED;
                c->srv_group = (int)get16(k + 1);
                size_t pl = k[3];
                if (4 + pl > l || pl > sizeof(c->srv_kx)) return TLS_E_PROTO;
                memcpy(c->srv_kx, k + 4, pl); c->srv_kx_len = pl;
                c->got_ske = 1;
            } else if (c->msg_type == HS_SERVER_HELLO_DONE) {
                if (c->body_len != 0) return TLS_E_PROTO;
                c->got_shd = 1;
            } else if (c->msg_type != HS_NEW_SESSION_TICKET && c->msg_type != HS_CERT_STATUS && c->msg_type != HS_HELLO_REQUEST) {
                return TLS_E_PROTO;
            }
        }
    }
    c->hdr_len = 0;
    return 0;
}

/* Streams handshake bytes through the transcript hash; keeps only the bodies that are needed. */
static int hs_consume(struct tls_conn *c, const uint8_t *d, size_t n)
{
    c->hs_bytes += n;
    if (c->hs_bytes > HS_MAX_BYTES) return TLS_E_PROTO;
    while (n && !c->got_fin && !c->got_shd) {
        if (c->hdr_len < 4) {
            c->hdr[c->hdr_len++] = *d++; n--;
            if (c->hdr_len < 4) continue;
            c->msg_type = c->hdr[0];
            c->body_len = ((size_t)c->hdr[1] << 16) | ((size_t)c->hdr[2] << 8) | c->hdr[3];
            c->body_got = 0;
            c->keep = (c->msg_type == HS_FINISHED) || (c->version == 0x0303 && c->msg_type == HS_SERVER_KEY_EXCHANGE);
            if (c->keep && c->body_len > HS_KEEP) return TLS_E_PROTO;
            if (c->msg_type == HS_FINISHED) th_get(c, c->suite->hash, c->th_before_fin);   /* transcript *without* Finished */
            th_update(c, c->hdr, 4);
            if (c->body_len == 0) { int r = hs_message_done(c); if (r) return r; }
            continue;
        }
        size_t take = c->body_len - c->body_got; if (take > n) take = n;
        th_update(c, d, take);
        if (c->keep) memcpy(c->keepbuf + c->body_got, d, take);
        d += take; n -= take; c->body_got += take;
        if (c->body_got == c->body_len) { int r = hs_message_done(c); if (r) return r; }
    }
    return 0;
}

/* ------------------------------------------------------------ ClientHello / ServerHello */
static int build_client_hello(uint8_t *ch, const uint8_t random[32], const uint8_t sid[32],
                              const uint8_t xpub[32], const uint8_t ppub[65], const char *sni)
{
    static const uint16_t sigalgs[] = { 0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601, 0x0807, 0x0603, 0x0201, 0x0203 };
    uint8_t *p = ch;
    *p++ = HS_CLIENT_HELLO; p += 3;                                        /* length patched below */
    p = w16(p, 0x0303);
    memcpy(p, random, 32); p += 32;
    *p++ = 32; memcpy(p, sid, 32); p += 32;
    p = w16(p, 2 * NSUITES);
    for (int i = 0; i < NSUITES; i++) p = w16(p, SUITES[i].id);
    *p++ = 1; *p++ = 0;                                                    /* no compression */
    uint8_t *extlen = p; p += 2;
    if (sni) {
        size_t nl = strlen(sni);
        p = w16(p, 0x0000); p = w16(p, (unsigned)(nl + 5)); p = w16(p, (unsigned)(nl + 3));
        *p++ = 0; p = w16(p, (unsigned)nl); memcpy(p, sni, nl); p += nl;
    }
    p = w16(p, 0x000b); p = w16(p, 2); *p++ = 1; *p++ = 0;                  /* ec_point_formats: uncompressed */
    p = w16(p, 0x000a); p = w16(p, 6); p = w16(p, 4); p = w16(p, GROUP_X25519); p = w16(p, GROUP_P256);
    p = w16(p, 0x000d); p = w16(p, 2 + 2 * (unsigned)(sizeof(sigalgs) / 2)); p = w16(p, 2 * (unsigned)(sizeof(sigalgs) / 2));
    for (size_t i = 0; i < sizeof(sigalgs) / 2; i++) p = w16(p, sigalgs[i]);
    p = w16(p, 0x002b); p = w16(p, 5); *p++ = 4; p = w16(p, 0x0304); p = w16(p, 0x0303);   /* supported_versions: 1.3, 1.2 */
#ifndef TLS_TEST_NO_EMS                                                    /* test hook: lets the host suite exercise the non-EMS key derivation */
    p = w16(p, 0x0017); p = w16(p, 0);                                      /* extended_master_secret */
#endif
    p = w16(p, 0xff01); p = w16(p, 1); *p++ = 0;                            /* renegotiation_info (initial) */
    p = w16(p, 0x0033); p = w16(p, 2 + 4 + 32 + 4 + 65); p = w16(p, 4 + 32 + 4 + 65);   /* key_share: x25519 + P-256 */
    p = w16(p, GROUP_X25519); p = w16(p, 32); memcpy(p, xpub, 32); p += 32;
    p = w16(p, GROUP_P256);   p = w16(p, 65); memcpy(p, ppub, 65); p += 65;
    size_t total = (size_t)(p - ch);
    if (total < 512 - 4) {                                                  /* RFC 7685: keep clear of the 256..511 byte F5 bug */
        size_t pad = 512 - 4 - total;
        p = w16(p, 0x0015); p = w16(p, (unsigned)pad);
        memset(p, 0, pad); p += pad;
    }
    put16(extlen, (unsigned)(p - extlen - 2));
    size_t body = (size_t)(p - ch) - 4;
    ch[1] = (uint8_t)(body >> 16); ch[2] = (uint8_t)(body >> 8); ch[3] = (uint8_t)body;
    return (int)(p - ch);
}

struct shello {
    unsigned suite; int tls13; int group; const uint8_t *kx; size_t kxlen; int ems;
    const uint8_t *random;
};

static int parse_server_hello(const uint8_t *m, size_t ml, struct shello *sh)
{
    static const uint8_t hrr[32] = { 0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
                                     0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C };
    memset(sh, 0, sizeof(*sh));
    if (ml < 4 || m[0] != HS_SERVER_HELLO) return TLS_E_PROTO;
    size_t bl = ((size_t)m[1] << 16) | ((size_t)m[2] << 8) | m[3];
    if (bl + 4 != ml || bl < 2 + 32 + 1 + 2 + 1) return TLS_E_PROTO;
    const uint8_t *p = m + 4, *end = m + ml;
    unsigned legacy = get16(p);
    if (legacy < 0x0303) return TLS_E_UNSUPPORTED;                         /* TLS 1.0 / 1.1 */
    sh->random = p + 2;
    if (memcmp(p + 2, hrr, 32) == 0) { TR("* TLS: server sent HelloRetryRequest (not supported)\n"); return TLS_E_UNSUPPORTED; }
    p += 34;
    size_t sl = *p++;
    if (p + sl + 2 + 1 > end) return TLS_E_PROTO;
    p += sl;
    sh->suite = get16(p); p += 2;
    if (*p++ != 0) return TLS_E_PROTO;
    if (p < end) {
        if (p + 2 > end) return TLS_E_PROTO;
        size_t el = get16(p); p += 2;
        if (p + el != end) return TLS_E_PROTO;
        while (p + 4 <= end) {
            unsigned et = get16(p), elen = get16(p + 2); p += 4;
            if (p + elen > end) return TLS_E_PROTO;
            if (et == 0x002b) { if (elen != 2) return TLS_E_PROTO; if (get16(p) == 0x0304) sh->tls13 = 1; }
            else if (et == 0x0033) {
                if (elen < 4) return TLS_E_PROTO;
                sh->group = (int)get16(p); size_t kl = get16(p + 2);
                if (4 + kl != elen) return TLS_E_PROTO;
                sh->kx = p + 4; sh->kxlen = kl;
            }
            else if (et == 0x0017) sh->ems = 1;
            p += elen;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ TLS 1.3 */
static int handshake13(struct tls_conn *c, const struct shello *sh, const uint8_t xpriv[32], const uint8_t ppriv[32], uint32_t tmo)
{
    const struct suite *s = c->suite;
    int alg = s->hash; size_t hl = hash_len(alg);
    uint8_t shared[32], nz = 0;

    if (sh->group == GROUP_X25519 && sh->kxlen == 32) {
        x25519(shared, xpriv, sh->kx);
    } else if (sh->group == GROUP_P256 && sh->kxlen == 65) {
        if (p256_ecdh(shared, ppriv, sh->kx) != 0) return TLS_E_PROTO;       /* invalid curve point */
    } else return TLS_E_UNSUPPORTED;
    for (int i = 0; i < 32; i++) nz |= shared[i];
    if (!nz) return TLS_E_PROTO;                                           /* low-order point */
    c->group = sh->group;

    uint8_t zeros[HASH_MAX_LEN] = { 0 }, empty[HASH_MAX_LEN], early[HASH_MAX_LEN], derived[HASH_MAX_LEN], hs[HASH_MAX_LEN];
    uint8_t h1[HASH_MAX_LEN], c_hs[HASH_MAX_LEN], s_hs[HASH_MAX_LEN];
    hash_data(alg, "", 0, empty);
    hkdf_extract_h(alg, NULL, 0, zeros, hl, early);
    expand_label(alg, early, "derived", empty, hl, derived, hl);
    hkdf_extract_h(alg, derived, hl, shared, 32, hs);
    th_get(c, alg, h1);
    expand_label(alg, hs, "c hs traffic", h1, hl, c_hs, hl);
    expand_label(alg, hs, "s hs traffic", h1, hl, s_hs, hl);
    install_keys13(s, s_hs, &c->rk, c->riv);
    install_keys13(s, c_hs, &c->wk, c->wiv);
    c->rseq = c->wseq = 0; c->renc = 1;

    while (!c->got_fin) {
        int r = rec_fill(c, tmo);
        if (r == 1) return TLS_E_TIMEOUT;
        if (r == 2) return TLS_E_IO;
        if (r < 0) return r;
        uint8_t t = c->rbuf[0];
        if (t == CT_CCS) { c->rlen = 0; continue; }
        if (t == CT_ALERT) return fail_alert(c->rbuf + 5, get16(c->rbuf + 3));
        if (t != CT_APPDATA) return TLS_E_PROTO;
        uint8_t it; const uint8_t *d; size_t dl;
        r = rec_decrypt(c, &it, &d, &dl);
        if (r) return r;
        if (it == CT_ALERT) return fail_alert(d, dl);
        if (it != CT_HANDSHAKE) return TLS_E_PROTO;
        r = hs_consume(c, d, dl);
        if (r) return r;
        c->rlen = 0;
    }
    TR("* TLS: server flight received (%u handshake bytes)\n", (unsigned)c->hs_bytes);

    uint8_t fk[HASH_MAX_LEN], expect[HASH_MAX_LEN];
    expand_label(alg, s_hs, "finished", NULL, 0, fk, hl);
    hmac_hash(alg, fk, hl, c->th_before_fin, hl, expect);
    if (ct_memcmp(expect, c->keepbuf, hl) != 0) return TLS_E_MAC;

    uint8_t h2[HASH_MAX_LEN], derived2[HASH_MAX_LEN], master[HASH_MAX_LEN], c_ap[HASH_MAX_LEN], s_ap[HASH_MAX_LEN];
    th_get(c, alg, h2);
    expand_label(alg, hs, "derived", empty, hl, derived2, hl);
    hkdf_extract_h(alg, derived2, hl, zeros, hl, master);
    expand_label(alg, master, "c ap traffic", h2, hl, c_ap, hl);
    expand_label(alg, master, "s ap traffic", h2, hl, s_ap, hl);

    uint8_t cfin[4 + HASH_MAX_LEN];
    expand_label(alg, c_hs, "finished", NULL, 0, fk, hl);
    cfin[0] = HS_FINISHED; cfin[1] = 0; cfin[2] = 0; cfin[3] = (uint8_t)hl;
    hmac_hash(alg, fk, hl, h2, hl, cfin + 4);
    static const uint8_t ccs[6] = { CT_CCS, 3, 3, 0, 1, 1 };               /* middlebox-compat dummy ChangeCipherSpec */
    int r = tcp_write_all(c->sock, ccs, sizeof(ccs));
    if (r) return r;
    c->wenc = 1;
    r = rec_write(c, CT_HANDSHAKE, cfin, 4 + hl);
    if (r) return r;

    memcpy(c->rsecret, s_ap, hl); memcpy(c->wsecret, c_ap, hl);
    install_keys13(s, s_ap, &c->rk, c->riv); install_keys13(s, c_ap, &c->wk, c->wiv);
    c->rseq = c->wseq = 0;

    memset(shared, 0, sizeof(shared)); memset(hs, 0, sizeof(hs)); memset(master, 0, sizeof(master));
    memset(c_hs, 0, sizeof(c_hs)); memset(s_hs, 0, sizeof(s_hs)); memset(c_ap, 0, sizeof(c_ap)); memset(s_ap, 0, sizeof(s_ap)); memset(fk, 0, sizeof(fk));
    return 0;
}

/* ------------------------------------------------------------ TLS 1.2 */
static int read_plain_handshake(struct tls_conn *c, uint32_t tmo, int until_shd)
{
    while (until_shd ? !c->got_shd : 0) {
        int r = rec_fill(c, tmo);
        if (r == 1) return TLS_E_TIMEOUT;
        if (r == 2) return TLS_E_IO;
        if (r < 0) return r;
        uint8_t t = c->rbuf[0];
        if (t == CT_ALERT) return fail_alert(c->rbuf + 5, get16(c->rbuf + 3));
        if (t != CT_HANDSHAKE) return TLS_E_PROTO;
        r = hs_consume(c, c->rbuf + 5, get16(c->rbuf + 3));
        if (r) return r;
        c->rlen = 0;
    }
    return 0;
}

static int handshake12(struct tls_conn *c, const struct shello *sh, const uint8_t crandom[32], const uint8_t srandom[32],
                       const uint8_t xpriv[32], const uint8_t xpub[32], const uint8_t ppriv[32], const uint8_t ppub[65],
                       uint32_t tmo)
{
    const struct suite *s = c->suite;
    int alg = s->hash; size_t hl = hash_len(alg);
    int r;

    r = read_plain_handshake(c, tmo, 1);
    if (r) return r;
    TR("* TLS: server flight received (%u handshake bytes%s)\n", (unsigned)c->hs_bytes, c->got_certreq ? ", client certificate requested" : "");
    if (!c->got_ske) { TR("* TLS: no ServerKeyExchange (static RSA/DH suite?)\n"); return TLS_E_UNSUPPORTED; }

    uint8_t pms[32], nz = 0;
    const uint8_t *cke_point; size_t cke_len;
    if (c->srv_group == GROUP_X25519 && c->srv_kx_len == 32) {
        x25519(pms, xpriv, c->srv_kx); cke_point = xpub; cke_len = 32;
    } else if (c->srv_group == GROUP_P256 && c->srv_kx_len == 65) {
        if (p256_ecdh(pms, ppriv, c->srv_kx) != 0) return TLS_E_PROTO;
        cke_point = ppub; cke_len = 65;
    } else { TR("* TLS: server chose unsupported curve 0x%04x\n", (unsigned)c->srv_group); return TLS_E_UNSUPPORTED; }
    for (int i = 0; i < 32; i++) nz |= pms[i];
    if (!nz) return TLS_E_PROTO;
    c->group = c->srv_group;
    TR("* TLS: ECDHE over %s, cipher %s%s\n", group_name(c->group), s->name, sh->ems ? ", extended master secret" : "");

    if (c->got_certreq) {                                                  /* we have no certificate: send an empty list */
        static const uint8_t empty_cert[7] = { HS_CERTIFICATE, 0, 0, 3, 0, 0, 0 };
        th_update(c, empty_cert, sizeof(empty_cert));
        r = rec_write(c, CT_HANDSHAKE, empty_cert, sizeof(empty_cert));
        if (r) return r;
    }
    uint8_t cke[4 + 1 + 65];
    cke[0] = HS_CLIENT_KEY_EXCHANGE; cke[1] = 0; cke[2] = 0; cke[3] = (uint8_t)(1 + cke_len); cke[4] = (uint8_t)cke_len;
    memcpy(cke + 5, cke_point, cke_len);
    th_update(c, cke, 5 + cke_len);
    r = rec_write(c, CT_HANDSHAKE, cke, 5 + cke_len);
    if (r) return r;

    uint8_t master[48], seed[64];
    if (sh->ems) {
        uint8_t sess[HASH_MAX_LEN];
        th_get(c, alg, sess);
        tls12_prf(alg, pms, 32, "extended master secret", sess, hl, master, 48);
    } else {
        memcpy(seed, crandom, 32); memcpy(seed + 32, srandom, 32);
        tls12_prf(alg, pms, 32, "master secret", seed, 64, master, 48);
    }
    memset(pms, 0, sizeof(pms));

    /* key_block = client key | server key | client IV | server IV  (GCM: 4-byte salts; ChaCha: 12-byte IVs) */
    size_t kl = aead_keylen(s->aead), il = s->aead == AEAD_CHACHA ? 12 : 4;
    uint8_t kb[2 * 32 + 2 * 12];
    memcpy(seed, srandom, 32); memcpy(seed + 32, crandom, 32);
    tls12_prf(alg, master, 48, "key expansion", seed, 64, kb, 2 * kl + 2 * il);
    aead_set(&c->wk, s->aead, kb);
    aead_set(&c->rk, s->aead, kb + kl);
    memset(c->wiv, 0, 12); memset(c->riv, 0, 12);
    memcpy(c->wiv, kb + 2 * kl, il); memcpy(c->riv, kb + 2 * kl + il, il);
    memset(kb, 0, sizeof(kb));

    static const uint8_t ccs[6] = { CT_CCS, 3, 3, 0, 1, 1 };
    r = tcp_write_all(c->sock, ccs, sizeof(ccs));
    if (r) return r;
    c->wenc = 1; c->wseq = 0;

    uint8_t th[HASH_MAX_LEN], fin[4 + 12];
    th_get(c, alg, th);
    fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = 12;
    tls12_prf(alg, master, 48, "client finished", th, hl, fin + 4, 12);
    th_update(c, fin, sizeof(fin));                                        /* the server's Finished covers ours */
    r = rec_write(c, CT_HANDSHAKE, fin, sizeof(fin));
    if (r) return r;

    /* server: [NewSessionTicket], ChangeCipherSpec, encrypted Finished */
    c->got_shd = 0;                                                        /* the flight parser stops on ServerHelloDone; re-arm it */
    while (!c->got_fin) {
        r = rec_fill(c, tmo);
        if (r == 1) return TLS_E_TIMEOUT;
        if (r == 2) return TLS_E_IO;
        if (r < 0) return r;
        uint8_t t = c->rbuf[0];
        if (t == CT_ALERT) {
            if (!c->renc) return fail_alert(c->rbuf + 5, get16(c->rbuf + 3));
        }
        if (t == CT_CCS) { c->renc = 1; c->rseq = 0; c->rlen = 0; continue; }
        if (!c->renc) { if (t == CT_HANDSHAKE) { r = hs_consume(c, c->rbuf + 5, get16(c->rbuf + 3)); if (r) return r; c->rlen = 0; continue; } return TLS_E_PROTO; }
        uint8_t it; const uint8_t *d; size_t dl;
        r = rec_decrypt(c, &it, &d, &dl);
        if (r) return r;
        if (it == CT_ALERT) return fail_alert(d, dl);
        if (it != CT_HANDSHAKE) return TLS_E_PROTO;
        r = hs_consume(c, d, dl);
        if (r) return r;
        c->rlen = 0;
    }
    uint8_t expect[12];
    tls12_prf(alg, master, 48, "server finished", c->th_before_fin, hl, expect, 12);
    memset(master, 0, sizeof(master));
    if (ct_memcmp(expect, c->keepbuf, 12) != 0) return TLS_E_MAC;
    return 0;
}

/* ------------------------------------------------------------ handshake driver */
static int handshake(struct tls_conn *c, const char *host, uint32_t tmo)
{
    uint8_t xpriv[32], xpub[32], ppriv[32], ppub[65], crandom[32], sid[32];
    krand_bytes(xpriv, 32); krand_bytes(crandom, 32); krand_bytes(sid, 32);
    x25519_base(xpub, xpriv);
    do { krand_bytes(ppriv, 32); } while (!p256_scalar_ok(ppriv));
    if (p256_pubkey(ppub, ppriv) != 0) return TLS_E_PROTO;

    uint8_t ip[4];
    const char *sni = (host && !net_parse_ip(host, ip)) ? host : NULL;
    int len = build_client_hello(c->chbuf, crandom, sid, xpub, ppub, sni);
    sha256_init(&c->th256); sha384_init(&c->th384);
    th_update(c, c->chbuf, (size_t)len);
    TR("* TLS: ClientHello (%d bytes): %d cipher suites, groups x25519+secp256r1, TLS 1.3/1.2, SNI %s\n", len, NSUITES, sni ? sni : "(none)");
    int r = rec_write(c, CT_HANDSHAKE, c->chbuf, (size_t)len);
    if (r) return r;

    /* ---- ServerHello (plaintext; a 1.2 server often puts Certificate etc. in the same record) ---- */
    for (;;) {
        r = rec_fill(c, tmo);
        if (r == 1) return TLS_E_TIMEOUT;
        if (r == 2) return TLS_E_IO;
        if (r < 0) return r;
        if (c->rbuf[0] == CT_CCS) { c->rlen = 0; continue; }
        break;
    }
    if (c->rbuf[0] == CT_ALERT) return fail_alert(c->rbuf + 5, get16(c->rbuf + 3));
    if (c->rbuf[0] != CT_HANDSHAKE) return TLS_E_PROTO;
    const uint8_t *m = c->rbuf + 5; size_t ml = get16(c->rbuf + 3);
    if (ml < 4) return TLS_E_PROTO;
    size_t sh_total = 4 + (((size_t)m[1] << 16) | ((size_t)m[2] << 8) | m[3]);
    if (sh_total > ml) return TLS_E_PROTO;                                 /* ServerHello split over records: not seen in practice */
    struct shello sh;
    r = parse_server_hello(m, sh_total, &sh);
    if (r) return r;
    const struct suite *s = find_suite(sh.suite);
    if (!s || (sh.tls13 != s->v13)) { TR("* TLS: server picked suite 0x%04x we did not offer (or wrong version)\n", sh.suite); return TLS_E_UNSUPPORTED; }
    c->suite = s;
    th_update(c, m, sh_total);

    uint8_t srandom[32];
    memcpy(srandom, sh.random, 32);
    if (!sh.tls13 && memcmp(srandom + 24, "DOWNGRD", 7) == 0) { TR("* TLS: downgrade sentinel in ServerHello random\n"); return TLS_E_PROTO; }
    c->version = sh.tls13 ? 0x0304 : 0x0303;
    TR("* TLS: ServerHello: TLS 1.%d, %s%s\n", sh.tls13 ? 3 : 2, s->name, sh.tls13 ? "" : (sh.ems ? ", EMS" : ", no EMS"));

    if (sh.tls13) {
        if (ml != sh_total) return TLS_E_PROTO;                            /* nothing may follow a 1.3 ServerHello in the record */
        c->rlen = 0;
        return handshake13(c, &sh, xpriv, ppriv, tmo);
    }
    /* TLS 1.2 servers (Go, Java, Schannel...) often put Certificate / ServerKeyExchange / ServerHelloDone in the same
     * record as the ServerHello: feed the remainder to the handshake parser right out of the receive buffer. */
    r = hs_consume(c, m + sh_total, ml - sh_total);
    c->rlen = 0;
    if (r) return r;
    return handshake12(c, &sh, crandom, srandom, xpriv, xpub, ppriv, ppub, tmo);
}

struct tls_conn *tls_connect(int tcp_sock, const char *server_name, uint32_t timeout_ms, int *err)
{
    int dummy; if (!err) err = &dummy;
    g_last_alert = -1;
    struct tls_conn *c = (struct tls_conn *)kcalloc(1, sizeof(*c));
    if (!c) { *err = TLS_E_NOMEM; return NULL; }
    c->sock = tcp_sock;
    int r = handshake(c, server_name, timeout_ms ? timeout_ms : 8000);
    if (r) {
        TR("* TLS: handshake failed: %s\n", tls_error_text(r));
        memset(c, 0, sizeof(*c)); kfree(c);
        *err = r;
        return NULL;
    }
    *err = 0;
    c->rlen = 0; c->plen = 0;
    TR("* TLS: handshake complete: %s\n", tls_describe(c));
    return c;
}

/* ------------------------------------------------------------ application data */
static void update_keys_read(struct tls_conn *c)
{
    uint8_t n[HASH_MAX_LEN];
    expand_label(c->suite->hash, c->rsecret, "traffic upd", NULL, 0, n, hash_len(c->suite->hash));
    memcpy(c->rsecret, n, hash_len(c->suite->hash)); install_keys13(c->suite, c->rsecret, &c->rk, c->riv); c->rseq = 0;
    memset(n, 0, sizeof(n));
}

static void update_keys_write(struct tls_conn *c)
{
    uint8_t n[HASH_MAX_LEN];
    expand_label(c->suite->hash, c->wsecret, "traffic upd", NULL, 0, n, hash_len(c->suite->hash));
    memcpy(c->wsecret, n, hash_len(c->suite->hash)); install_keys13(c->suite, c->wsecret, &c->wk, c->wiv); c->wseq = 0;
    memset(n, 0, sizeof(n));
}

static void post_handshake(struct tls_conn *c, const uint8_t *d, size_t len)
{
    if (c->version != 0x0304) return;                                      /* 1.2: HelloRequest etc. -- no renegotiation, ignore */
    while (len >= 4) {
        size_t ml = ((size_t)d[1] << 16) | ((size_t)d[2] << 8) | d[3];
        if (4 + ml > len) break;                                           /* fragmented across records: ignore */
        if (d[0] == HS_KEY_UPDATE && ml == 1) {
            update_keys_read(c);
            if (d[4] == 1) {                                               /* peer asked us to update too */
                static const uint8_t ku[5] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
                if (rec_write(c, CT_HANDSHAKE, ku, sizeof(ku)) == 0) update_keys_write(c);
            }
        }                                                                  /* NewSessionTicket & co: ignored */
        d += 4 + ml; len -= 4 + ml;
    }
}

int tls_recv(struct tls_conn *c, void *buf, size_t cap, uint32_t timeout_ms)
{
    if (!c) return TLS_E_IO;
    for (;;) {
        if (c->plen) {
            size_t n = c->plen < cap ? c->plen : cap;
            memcpy(buf, c->pdata, n);
            c->pdata += n; c->plen -= n;
            if (!c->plen) c->rlen = 0;
            return (int)n;
        }
        if (c->eof) return 0;
        if (c->failed) return c->failed;
        int r = rec_fill(c, timeout_ms);
        if (r == 1) return -1;
        if (r == 2) { c->eof = 1; return 0; }                              /* TCP FIN without close_notify: HTTP framing decides */
        if (r < 0) { c->failed = r; return r; }
        uint8_t t = c->rbuf[0];
        if (t == CT_CCS) { c->rlen = 0; continue; }
        if (c->version == 0x0304 && t != CT_APPDATA) {                     /* 1.3: everything after the handshake is encrypted */
            c->failed = t == CT_ALERT ? fail_alert(c->rbuf + 5, get16(c->rbuf + 3)) : TLS_E_PROTO;
            return c->failed;
        }
        if (t != CT_APPDATA && t != CT_ALERT && t != CT_HANDSHAKE) { c->failed = TLS_E_PROTO; return c->failed; }
        uint8_t it; const uint8_t *d; size_t dl;
        r = rec_decrypt(c, &it, &d, &dl);
        if (r) { c->failed = r; return r; }
        if (it == CT_APPDATA) {
            if (dl == 0) { c->rlen = 0; continue; }
            c->pdata = d; c->plen = dl;
            continue;
        }
        if (it == CT_HANDSHAKE) { post_handshake(c, d, dl); c->rlen = 0; continue; }
        if (it == CT_ALERT) {
            if (dl >= 2 && d[1] == 0) { c->eof = 1; c->rlen = 0; return 0; }   /* close_notify */
            c->failed = fail_alert(d, dl);
            return c->failed;
        }
        c->failed = TLS_E_PROTO;
        return c->failed;
    }
}

int tls_send(struct tls_conn *c, const void *data, size_t len)
{
    if (!c) return TLS_E_IO;
    if (c->failed) return c->failed;
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0;
    while (off < len) {
        size_t n = len - off < REC_MAX_PLAIN ? len - off : REC_MAX_PLAIN;
        int r = rec_write(c, CT_APPDATA, p + off, n);
        if (r) { c->failed = r; return r; }
        off += n;
    }
    return (int)len;
}

void tls_close(struct tls_conn *c)
{
    if (!c) return;
    if (!c->failed && c->wenc) {
        static const uint8_t close_notify[2] = { 1, 0 };
        rec_write(c, CT_ALERT, close_notify, 2);
    }
    memset(c, 0, sizeof(*c));
    kfree(c);
}
