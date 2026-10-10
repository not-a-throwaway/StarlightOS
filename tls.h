#ifndef TLS_H
#define TLS_H
/*
 * tls.h -- TLS 1.3 / 1.2 client on top of the kernel's TCP sockets.
 *
 *   versions      TLS 1.3 (RFC 8446) and TLS 1.2 (RFC 5246, ECDHE only, with extended master secret)
 *   key exchange  X25519, secp256r1 (P-256)
 *   ciphers       TLS 1.3: CHACHA20-POLY1305-SHA256, AES-128-GCM-SHA256, AES-256-GCM-SHA384
 *                 TLS 1.2: ECDHE-{ECDSA,RSA}-{CHACHA20-POLY1305, AES128-GCM-SHA256, AES256-GCM-SHA384}
 *
 * What it does: full handshake incl. verifying the server's Finished MAC, encrypted records, ticket skipping,
 * KeyUpdate (1.3), close_notify, SNI, a ClientHello padded past the old F5 256-511 byte bug.
 *
 * What it deliberately does NOT do: it does not validate the server's certificate chain or the
 * CertificateVerify / ServerKeyExchange signature (that needs X.509 + RSA/ECDSA + a CA store). Traffic is
 * encrypted and integrity-protected, but a man-in-the-middle can impersonate the server. Also unsupported:
 * HelloRetryRequest, client certificates, session resumption, TLS 1.1 and older, RSA key exchange.
 */
#include <stdint.h>
#include <stddef.h>

enum {
    TLS_E_ABORT       = -5,    /* same value as TCP_ERR_ABORT (Ctrl-C) */
    TLS_E_IO          = -10,   /* TCP failure / connection dropped mid-record */
    TLS_E_TIMEOUT     = -11,
    TLS_E_PROTO       = -12,   /* not TLS, or malformed */
    TLS_E_UNSUPPORTED = -13,   /* server insists on something we don't implement */
    TLS_E_ALERT       = -14,   /* server sent a fatal alert (tls_last_alert()) */
    TLS_E_MAC         = -15,   /* record MAC / server Finished did not verify */
    TLS_E_NOMEM       = -16
};

struct tls_conn;

/* Runs the handshake over an established TCP socket. server_name is sent as SNI (skipped for IP literals).
 * Returns NULL on failure and stores a TLS_E_* code in *err. The socket stays owned by the caller
 * (tls_close() does not close it). */
struct tls_conn *tls_connect(int tcp_sock, const char *server_name, uint32_t timeout_ms, int *err);

int  tls_send(struct tls_conn *c, const void *data, size_t len);                    /* len, or <0 */
int  tls_recv(struct tls_conn *c, void *buf, size_t cap, uint32_t timeout_ms);      /* >0 data, 0 EOF, -1 timeout, <-1 error (tcp_recv convention) */
void tls_close(struct tls_conn *c);                                                 /* sends close_notify, frees */

const char *tls_error_text(int err);
int         tls_last_alert(void);              /* description byte of the last alert a server sent, -1 if none */
const char *tls_alert_name(int desc);          /* "protocol_version", "handshake_failure", ... */
const char *tls_describe(const struct tls_conn *c);   /* "TLSv1.3 TLS_AES_128_GCM_SHA256 x25519" */

/* Prints each handshake step (what we offered, what the server chose, where it failed) with kprintf. */
void tls_set_trace(int on);
#endif
