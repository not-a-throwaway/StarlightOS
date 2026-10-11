/*
 * Host driver for libslweb's HTTPS path (M9.4) over real loopback sockets.
 *
 * libslweb/http.c is the code under test; libslweb/tls_user.c supplies the
 * ring-3 platform layer for the kernel's tls.c, and this file supplies the four
 * socket syscalls tls_user.c needs (sl_socket/sl_connect/sl_send/sl_recv/
 * sl_poll/sl_resolve/sl_random) with real POSIX sockets. So the whole userland
 * stack -- URL, HTTP, TLS handshake, X.509 validation -- runs against an
 * OpenSSL s_server, exactly as it does inside the OS but with real TCP.
 *
 *   slweb_tls_host URL [--insecure] [--cafile FILE] [--verbose]
 *
 * Prints one field per line (error=, status=, tls_verified=, tls_peer=, ...)
 * for tests/host/slweb_tls_test.py to assert on.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "slweb.h"
#include "../user/slnet.h"

/* ---- the M2 socket ABI, backed by real BSD sockets ---- */
int sl_socket(int domain, int type, int protocol)
{
    if (domain != SL_AF_INET) { errno = EAFNOSUPPORT; return -1; }
    return socket(AF_INET, type, protocol);
}

int sl_connect(int fd, const struct sl_sockaddr_in *addr)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)((addr->sin_port >> 8) | (addr->sin_port << 8)));
    memcpy(&a.sin_addr, addr->sin_addr, 4);
    return connect(fd, (struct sockaddr *)&a, sizeof a);
}

long sl_send(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t r = send(fd, p + off, len - off, 0);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return (long)len;
}

long sl_recv(int fd, void *buf, size_t len)
{
    ssize_t r = recv(fd, buf, len, 0);
    return r < 0 ? -1 : (long)r;
}

int sl_poll(struct sl_pollfd *fds, int nfds, int timeout_ms)
{
    if (nfds < 1) return 0;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fds[0].fd, &rd);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int r = select(fds[0].fd + 1, &rd, NULL, NULL, &tv);
    if (r > 0) fds[0].revents = FD_ISSET(fds[0].fd, &rd) ? SL_POLLIN : 0;
    return r;
}

int sl_resolve(const char *host, uint8_t out_ip[4])
{
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) == 1) { memcpy(out_ip, &a, 4); return 0; }
    if (strcmp(host, "localhost") == 0) { out_ip[0] = 127; out_ip[1] = 0; out_ip[2] = 0; out_ip[3] = 1; return 0; }
    errno = ENOENT;
    return -1;
}

int sl_random(void *buf, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    return got == len ? 0 : -1;
}

static void print_escaped(const char *tag, const void *p, size_t n)
{
    const unsigned char *s = p;
    printf("%s=", tag);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '\n') printf("\\n");
        else if (c == '\r') printf("\\r");
        else if (c == '\t') printf("\\t");
        else if (c == '\\') printf("\\\\");
        else putchar(c);
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s URL [--insecure] [--cafile FILE] [--verbose]\n", argv[0]); return 2; }
    const char *url = argv[1];

    struct slweb_http_opts o;
    memset(&o, 0, sizeof o);
    o.follow_redirects = 5;
    o.quiet = 1;
    o.timeout_ms = 5000;
    o.max_body = 2 * 1024 * 1024;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--insecure")) o.insecure = 1;
        else if (!strcmp(argv[i], "--verbose")) o.verbose = 1;
        else if (!strcmp(argv[i], "--cafile") && i + 1 < argc) {
            int n = slweb_http_add_ca_file(argv[++i]);
            printf("cafile_roots=%d\n", n);
        }
    }

    struct slweb_response r;
    memset(&r, 0, sizeof r);
    int err = slweb_http_request(url, &o, &r);

    printf("error=%d\n", err);
    printf("error_text=%s\n", slweb_http_error_text(err));
    printf("status=%d\n", r.status);
    printf("status_text=%s\n", r.status_text);
    printf("content_type=%s\n", r.content_type);
    printf("final_url=%s\n", r.final_url);
    printf("tls_verified=%d\n", r.tls_verified);
    printf("tls_peer=%s\n", r.tls_peer);
    printf("tls_error=%d\n", r.tls_error);
    if (r.tls_error) printf("tls_error_text=%s\n", slweb_http_tls_error_text(r.tls_error));
    printf("tls_alert=%d\n", r.tls_alert);
    printf("body_len=%zu\n", r.body_len);
    if (r.body) print_escaped("body", r.body, r.body_len);
    else printf("body=\n");
    slweb_http_response_free(&r);
    return 0;
}
