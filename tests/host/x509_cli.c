/* Host-side driver for x509.c: reads "op args..." lines and prints one result line.
 * Used by x509_diff.py, which builds real certificates with the 'cryptography'
 * (OpenSSL) library and checks parsing, name matching, time windows and path
 * validation against the expectations OpenSSL would produce.
 *
 *   parse  <der-hex>
 *   host   <der-hex> <hostname>
 *   chain  <now> <hostname|-> <nroots> <root...> <nchain> <cert...>
 *   pem    <pem-text-hex>
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "../../x509.h"

static void unhex(const char *s, uint8_t *o, size_t *outlen)
{
    size_t n = strlen(s) / 2;
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(s + 2 * i, "%2x", &v); o[i] = (uint8_t)v; }
    *outlen = n;
}

int main(void)
{
    static char line[1 << 18];
    static uint8_t ders[48][8192];
    while (fgets(line, sizeof line, stdin)) {
        char *tok[64]; int nt = 0;
        for (char *t = strtok(line, " \r\n"); t && nt < 64; t = strtok(NULL, " \r\n")) tok[nt++] = t;
        if (!nt) continue;

        if (!strcmp(tok[0], "parse") && nt >= 2) {
            size_t dl; unhex(tok[1], ders[0], &dl);
            struct x509_cert c;
            if (x509_parse(&c, ders[0], dl)) printf("MALFORMED\n");
            else printf("type=%d curve=%d sig=%d ca=%d kcs=%d eku=%d san=%zu cn=%.*s nb=%lld na=%lld\n",
                        c.pubkey.type, c.pubkey.curve, c.sig_scheme, c.is_ca, c.key_cert_sign,
                        c.eku_server_auth, c.san_len, (int)c.cn_len, (const char *)c.cn,
                        (long long)c.not_before, (long long)c.not_after);
        } else if (!strcmp(tok[0], "host") && nt >= 3) {
            size_t dl; unhex(tok[1], ders[0], &dl);
            struct x509_cert c;
            if (x509_parse(&c, ders[0], dl)) printf("MALFORMED\n");
            else printf("%s\n", x509_check_hostname(&c, tok[2]) ? "match" : "no");
        } else if (!strcmp(tok[0], "chain") && nt >= 5) {
            long long now = atoll(tok[1]);
            const char *host = strcmp(tok[2], "-") ? tok[2] : NULL;
            int nroots = atoi(tok[3]);
            if (nroots < 0 || nt < 4 + nroots + 1) { printf("BAD\n"); continue; }
            struct x509_store store; x509_store_init(&store);
            for (int i = 0; i < nroots; i++) {
                size_t l; unhex(tok[4 + i], ders[i], &l);
                x509_store_add_der(&store, ders[i], l);
            }
            int nchain = atoi(tok[4 + nroots]);
            if (nchain < 0 || nchain > 16 || nt < 4 + nroots + 1 + nchain) { printf("BAD\n"); continue; }
            const uint8_t *cp[16]; size_t cl[16];
            for (int i = 0; i < nchain; i++) { unhex(tok[5 + nroots + i], ders[16 + i], &cl[i]); cp[i] = ders[16 + i]; }
            int r = x509_verify_chain(cp, cl, nchain, &store, host, now);
            if (r == X509_OK) printf("OK\n"); else printf("ERR %d\n", r);
        } else if (!strcmp(tok[0], "pem") && nt >= 2) {
            static uint8_t text[65536];
            size_t tl; unhex(tok[1], text, &tl);
            uint8_t scratch[8192], out[8192];
            int n = x509_pem_foreach((char *)text, tl, scratch, sizeof scratch, NULL, NULL);
            int len = x509_pem_decode((char *)text, tl, out, sizeof out);
            if (n < 0 || len < 0) { printf("PEMBAD\n"); continue; }
            struct x509_cert c;
            printf("blocks=%d len=%d parse=%d\n", n, len, x509_parse(&c, out, len));
        } else printf("BAD\n");
        fflush(stdout);
    }
    return 0;
}
