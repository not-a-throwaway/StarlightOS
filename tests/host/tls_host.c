/* Host harness: runs the kernel's tls.c/crypto.c over real loopback sockets.
 * usage: tls_host HOST PORT [SNI] [PATH]   -> prints the HTTP response body bytes to stdout, status to stderr */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "../../tls.h"

/* ---- stubs for the kernel services tls.c needs ---- */
enum { TCP_ERR_ABORT = -5 };
int tcp_send(int s, const void *d, size_t n) { size_t o = 0; while (o < n) { ssize_t r = send(s, (const char *)d + o, n - o, 0); if (r <= 0) return -2; o += (size_t)r; } return (int)n; }
int tcp_recv(int s, void *b, size_t cap, uint32_t tmo) {
    struct pollfd p = { s, POLLIN, 0 };
    int pr = poll(&p, 1, (int)tmo); if (pr == 0) return -1; if (pr < 0) return -2;
    ssize_t r = recv(s, b, cap, 0); if (r < 0) return -2; return (int)r;
}
void *kcalloc(size_t a, size_t b) { return calloc(a, b); }
void kfree(void *p) { free(p); }
void krand_bytes(void *b, size_t n) { FILE *f = fopen("/dev/urandom", "rb"); if (fread(b, 1, n, f) != n) abort(); fclose(f); }
#include <stdarg.h>
int kprintf(const char *f, ...) { va_list ap; va_start(ap, f); int r = vfprintf(stderr, f, ap); va_end(ap); return r; }
int ksnprintf(char *b, size_t n, const char *f, ...) { va_list ap; va_start(ap, f); int r = vsnprintf(b, n, f, ap); va_end(ap); return r; }
int net_parse_ip(const char *s, uint8_t ip[4]) { struct in_addr a; if (inet_pton(AF_INET, s, &a) != 1) return -1; memcpy(ip, &a, 4); return 0; }

int main(int argc, char **argv)
{
    const char *host = argv[1]; int port = atoi(argv[2]);
    const char *sni = argc > 3 && strcmp(argv[3], "-") ? argv[3] : NULL;
    const char *path = argc > 4 ? argv[4] : "/";
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, host, &a.sin_addr);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { perror("connect"); return 2; }
    int err = 0;
    tls_set_trace(getenv("TLS_TRACE") != NULL);
    struct tls_conn *c = tls_connect(s, sni, 5000, &err);
    if (!c) { fprintf(stderr, "TLS_FAIL %d %s alert=%d\n", err, tls_error_text(err), tls_last_alert()); return 3; }
    if (argc > 5 && !strcmp(argv[5], "chat")) {      /* long-lived session: used to exercise KeyUpdate */
        tls_send(c, "hello\n", 6);
        for (int i = 0; i < 9; i++) {
            static uint8_t b[4096]; int k = tls_recv(c, b, sizeof b - 1, 700);
            if (k > 0) { b[k] = 0; printf("RECV[%s]\n", (char *)b); fflush(stdout); }
            else if (k == 0) { printf("EOF\n"); break; } else if (k < -1) { printf("ERR %d\n", k); break; }
            if (i == 4) tls_send(c, "second-after-keyupdate\n", 23);
        }
        tls_close(c); close(s); return 0;
    }
    char req[512];
    int rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
    if (tls_send(c, req, (size_t)rl) != rl) { fprintf(stderr, "send failed\n"); return 4; }
    static uint8_t buf[70000]; size_t total = 0; int n;
    while ((n = tls_recv(c, buf, sizeof buf, 5000)) > 0) { fwrite(buf, 1, (size_t)n, stdout); total += (size_t)n; }
    fprintf(stderr, "TLS_DONE rc=%d bytes=%zu %s\n", n, total, tls_describe(c));
    tls_close(c); close(s);
    return n == 0 ? 0 : 5;
}
