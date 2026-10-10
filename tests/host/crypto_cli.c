/* Host-side driver for crypto.c / chachapoly.c / p256.c: reads "op args..." lines on stdin, prints hex.
 * Used by crypto_diff.py to cross-check against hashlib / hmac / cryptography (OpenSSL). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../crypto.h"

static size_t unhex(const char *s, uint8_t *o) { if (!strcmp(s, "-")) return 0; size_t n = strlen(s) / 2; for (size_t i = 0; i < n; i++) { unsigned v; sscanf(s + 2*i, "%2x", &v); o[i] = (uint8_t)v; } return n; }
static void phex(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", p[i]); printf("\n"); }

int main(void)
{
    static char line[1 << 20];
    static uint8_t a[1 << 19], b[1 << 19], c[1 << 19], d[1 << 19], ct[1 << 19], pt[1 << 19];
    while (fgets(line, sizeof line, stdin)) {
        char *tok[8]; int nt = 0;
        for (char *t = strtok(line, " \n"); t && nt < 8; t = strtok(NULL, " \n")) tok[nt++] = t;
        if (!nt) continue;
        const char *op = tok[0];
        if (!strcmp(op, "sha256")) { size_t n = nt > 1 ? unhex(tok[1], a) : 0; uint8_t o[32]; sha256(a, n, o); phex(o, 32); }
        else if (!strcmp(op, "sha384")) { size_t n = nt > 1 ? unhex(tok[1], a) : 0; uint8_t o[48]; sha384(a, n, o); phex(o, 48); }
        else if (!strcmp(op, "hash384split")) { /* feed in 3 pieces to test streaming */
            size_t n = unhex(tok[1], a); struct hash_ctx h; hash_init(&h, HASH_SHA384); size_t s1 = n / 3, s2 = n / 2;
            hash_update(&h, a, s1); hash_update(&h, a + s1, s2 - s1); hash_update(&h, a + s2, n - s2); uint8_t o[48]; hash_final(&h, o); phex(o, 48); }
        else if (!strcmp(op, "hmac")) { int alg = atoi(tok[1]) == 384 ? HASH_SHA384 : HASH_SHA256; size_t kl = unhex(tok[2], a), ml = nt > 3 ? unhex(tok[3], b) : 0; uint8_t o[48]; hmac_hash(alg, a, kl, b, ml, o); phex(o, hash_len(alg)); }
        else if (!strcmp(op, "hkdf")) { /* hkdf alg salt ikm info len */
            int alg = atoi(tok[1]) == 384 ? HASH_SHA384 : HASH_SHA256;
            size_t sl = unhex(tok[2], a), il = unhex(tok[3], b), nl = unhex(tok[4], c); size_t L = (size_t)atoi(tok[5]);
            uint8_t prk[48]; hkdf_extract_h(alg, a, sl, b, il, prk); hkdf_expand_h(alg, prk, hash_len(alg), c, nl, d, L); phex(d, L); }
        else if (!strcmp(op, "prf")) { /* prf alg secret label seed len */
            int alg = atoi(tok[1]) == 384 ? HASH_SHA384 : HASH_SHA256;
            size_t sl = unhex(tok[2], a), seedl = unhex(tok[4], c); size_t L = (size_t)atoi(tok[5]);
            for (char *q = tok[3]; *q; q++) if (*q == '_') *q = ' ';     /* labels contain spaces */
            tls12_prf(alg, a, sl, tok[3], c, seedl, d, L); phex(d, L); }
        else if (!strcmp(op, "aes")) { size_t kl = unhex(tok[1], a); unhex(tok[2], b); struct aes k; aes_init(&k, a, (int)kl); aes_encrypt_block(&k, b, c); phex(c, 16); }
        else if (!strcmp(op, "gcmenc")) { /* key iv aad pt */
            size_t kl = unhex(tok[1], a); unhex(tok[2], b); size_t al = unhex(tok[3], c), pl = unhex(tok[4], d);
            static struct gcm g; gcm_init(&g, a, (int)kl); uint8_t tag[16]; gcm_encrypt(&g, b, c, al, d, pl, ct, tag); phex(ct, pl); phex(tag, 16); }
        else if (!strcmp(op, "gcmdec")) { /* key iv aad ct tag */
            size_t kl = unhex(tok[1], a); unhex(tok[2], b); size_t al = unhex(tok[3], c), cl = unhex(tok[4], d); uint8_t tag[16]; unhex(tok[5], tag);
            static struct gcm g; gcm_init(&g, a, (int)kl); int r = gcm_decrypt(&g, b, c, al, d, cl, tag, pt); if (r) printf("AUTHFAIL\n"); else phex(pt, cl); }
        else if (!strcmp(op, "ccenc")) { /* key nonce aad pt */
            unhex(tok[1], a); unhex(tok[2], b); size_t al = unhex(tok[3], c), pl = unhex(tok[4], d);
            uint8_t tag[16]; chachapoly_encrypt(a, b, c, al, d, pl, ct, tag); phex(ct, pl); phex(tag, 16); }
        else if (!strcmp(op, "ccdec")) {
            unhex(tok[1], a); unhex(tok[2], b); size_t al = unhex(tok[3], c), cl = unhex(tok[4], d); uint8_t tag[16]; unhex(tok[5], tag);
            int r = chachapoly_decrypt(a, b, c, al, d, cl, tag, pt); if (r) printf("AUTHFAIL\n"); else phex(pt, cl); }
        else if (!strcmp(op, "x25519")) { unhex(tok[1], a); unhex(tok[2], b); uint8_t o[32]; x25519(o, a, b); phex(o, 32); }
        else if (!strcmp(op, "x25519base")) { unhex(tok[1], a); uint8_t o[32]; x25519_base(o, a); phex(o, 32); }
        else if (!strcmp(op, "p256pub")) { unhex(tok[1], a); uint8_t o[65]; if (p256_pubkey(o, a)) printf("FAIL\n"); else phex(o, 65); }
        else if (!strcmp(op, "p256ecdh")) { unhex(tok[1], a); unhex(tok[2], b); uint8_t o[32]; if (p256_ecdh(o, a, b)) printf("FAIL\n"); else phex(o, 32); }
        fflush(stdout);
    }
    return 0;
}
