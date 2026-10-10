/*
 * NDEMO.ELF -- M2 demo: the ring-3 network syscalls (user/slnet.h).
 *
 *   resolve(host, ip)         DNS (or a literal IPv4 address) -> four bytes
 *   socket(AF_INET, STREAM)   allocate a process fd
 *   connect(fd, &addr)        blocking TCP handshake, Ctrl-C aware
 *   send(fd, buf, len)        blocking; blocks until the bytes are queued
 *   poll(&pfd, 1, ms)         wait for readability (also pumps the NIC)
 *   recv(fd, buf, len)        blocking; returns bytes, 0 at EOF
 *   close(fd)                 graceful FIN
 *
 * It talks to the test fixtures (plain HTTP on 10.0.2.2:18001, an echo server
 * on 10.0.2.2:18002) and prints what it sees. The serial log doubles as the
 * test's assertion surface (terminal output is mirrored to COM1).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "slnet.h"

/* The host test fixture addresses; set the port in network byte order. */
static uint16_t nd_htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

static void mkaddr(struct sl_sockaddr_in *a, const uint8_t ip[4], uint16_t port)
{
    memset(a, 0, sizeof *a);
    a->sin_family = SL_AF_INET;
    a->sin_port = nd_htons(port);
    memcpy(a->sin_addr, ip, 4);
}

int main(void)
{
    printf("NDEMO: start\n");

    uint8_t ip[4] = { 0, 0, 0, 0 };
    errno = 0;
    int rr = sl_resolve("10.0.2.2", ip);
    printf("NDEMO: resolve literal rc=%d ip=%u.%u.%u.%u\n", rr, ip[0], ip[1], ip[2], ip[3]);

    int fd = sl_socket(SL_AF_INET, SL_SOCK_STREAM, 0);
    printf("NDEMO: socket fd=%d\n", fd);
    if (fd < 3) { printf("NDEMO: socket failed errno=%d\n", errno); return 1; }

    struct sl_sockaddr_in a;
    mkaddr(&a, ip, 18001);
    errno = 0;
    if (sl_connect(fd, &a) != 0) { printf("NDEMO: connect failed errno=%d\n", errno); return 1; }
    printf("NDEMO: connect ok\n");

    const char *req = "GET /hello HTTP/1.0\r\nHost: 10.0.2.2\r\nConnection: close\r\n\r\n";
    long sent = sl_send(fd, req, strlen(req));
    printf("NDEMO: sent %ld\n", sent);

    char resp[2048];
    size_t total = 0;
    for (int i = 0; i < 200; i++) {
        struct sl_pollfd p; p.fd = fd; p.events = SL_POLLIN; p.revents = 0;
        int pr = sl_poll(&p, 1, 2000);
        if (pr < 0) { printf("NDEMO: poll failed errno=%d\n", errno); break; }
        if (pr == 0) { printf("NDEMO: poll timeout\n"); break; }
        long n = sl_recv(fd, resp + total, sizeof(resp) - 1 - total);
        if (n < 0) { printf("NDEMO: recv failed errno=%d\n", errno); break; }
        if (n == 0) { printf("NDEMO: http eof after %zu bytes\n", total); break; }
        total += (size_t)n;
        if (total >= sizeof(resp) - 1) break;
    }
    resp[total] = 0;
    printf("NDEMO: http response |%s|\n", resp);
    close(fd);

    /* A second connection, to the echo server: read the banner, then a round-trip. */
    fd = sl_socket(SL_AF_INET, SL_SOCK_STREAM, 0);
    mkaddr(&a, ip, 18002);
    errno = 0;
    if (sl_connect(fd, &a) != 0) {
        printf("NDEMO: echo connect failed errno=%d\n", errno);
    } else {
        char ebuf[256];
        struct sl_pollfd p; p.fd = fd; p.events = SL_POLLIN; p.revents = 0;
        if (sl_poll(&p, 1, 4000) > 0) {
            long n = sl_recv(fd, ebuf, sizeof(ebuf) - 1);
            if (n > 0) { ebuf[n] = 0; printf("NDEMO: banner %s", ebuf); }
        }
        sl_send(fd, "ping-pong\n", 10);
        for (int i = 0; i < 20; i++) {
            struct sl_pollfd q; q.fd = fd; q.events = SL_POLLIN; q.revents = 0;
            if (sl_poll(&q, 1, 4000) <= 0) break;
            long n = sl_recv(fd, ebuf, sizeof(ebuf) - 1);
            if (n <= 0) break;
            ebuf[n] = 0;
            printf("NDEMO: echo %s", ebuf);
            if (strstr(ebuf, "ECHO:ping-pong")) break;
        }
    }
    close(fd);

    /* Error paths: a refused connect and an unsupported address family. */
    fd = sl_socket(SL_AF_INET, SL_SOCK_STREAM, 0);
    mkaddr(&a, ip, 1);
    errno = 0;
    long rc = sl_connect(fd, &a);
    printf("NDEMO: refused rc=%ld errno=%d\n", rc, errno);
    close(fd);

    errno = 0;
    fd = sl_socket(99, SL_SOCK_STREAM, 0);
    printf("NDEMO: bad-domain fd=%d errno=%d\n", fd, errno);

    /* read()/write() on a socket take the same path as recv()/send(). */
    fd = sl_socket(SL_AF_INET, SL_SOCK_STREAM, 0);
    errno = 0;
    long urc = sl_send(fd, "x", 1);
    printf("NDEMO: unconnected send rc=%ld errno=%d\n", urc, errno);
    close(fd);

    printf("NDEMO: done\n");
    return 0;
}
