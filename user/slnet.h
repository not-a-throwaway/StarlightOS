#ifndef STARLIGHTOS_USER_SLNET_H
#define STARLIGHTOS_USER_SLNET_H

/*
 * StarlightOS network ABI -- shared verbatim by the kernel (process.c), the
 * newlib syscall layer (libc/syscalls.c) and freestanding ring-3 apps
 * (user/syscall.h), the same way user/slwindow.h shares the window ABI.
 *
 * M2 exposes blocking AF_INET/SOCK_STREAM sockets on top of the kernel's TCP
 * stack. A ring-3 app does:
 *
 *   int fd = socket(AF_INET, SOCK_STREAM, 0);
 *   connect(fd, &addr);           // resolve() a host name first if needed
 *   send(fd, request, len);       // blocks until the bytes are queued
 *   poll(&pfd, 1, timeout_ms);    // wait for readability (also pumps the NIC)
 *   recv(fd, buf, cap);           // blocks until data / EOF
 *   close(fd);                    // graceful FIN
 *
 * poll() is the multiplexing primitive: while it waits it runs the network
 * poller, the audio mixer and (when a window is open) the app's compositor,
 * because poll.c's kpoll() calls net_poll() and kyield() in its sleep loop.
 *
 * This file is deliberately freestanding: <stdint.h> only, no libc, no kernel
 * headers -- it is included by every build (kernel, host test, newlib app).
 */
#include <stdint.h>

/* ---- syscall numbers (int $0x80, process.c dispatcher) ---- */
#define SL_SYS_SOCKET   120   /* int socket(int domain, int type, int protocol) -> fd (or -errno) */
#define SL_SYS_CONNECT  121   /* int connect(int fd, const struct sl_sockaddr_in *addr, int addrlen) */
#define SL_SYS_SEND     122   /* long send(int fd, const void *buf, size_t len) -> bytes (or -errno) */
#define SL_SYS_RECV     123   /* long recv(int fd, void *buf, size_t len) -> bytes / 0 EOF (or -errno) */
#define SL_SYS_POLL     124   /* int poll(struct sl_pollfd *fds, int nfds, int timeout_ms) */
#define SL_SYS_RESOLVE  125   /* int resolve(const char *host, uint8_t out_ip[4]) */
#define SL_SYS_RANDOM   126   /* int random(void *buf, size_t len) -> 0; CSPRNG bytes (krand), for TLS keys */

/* ---- address family / socket type (Linux values) ---- */
#define SL_AF_INET       2
#define SL_SOCK_STREAM   1

/*
 * IPv4 socket address, the same 16-byte layout as POSIX sockaddr_in.
 * sin_port is in network byte order; sin_addr holds the four address bytes in
 * network order (matching resolve()'s output). Use htons()/your own swap.
 */
struct sl_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint8_t  sin_addr[4];
    uint8_t  sin_zero[8];
};

/* ---- poll flags (values match Linux and the kernel's poll.h) ---- */
#define SL_POLLIN    0x0001   /* readable: data, a connection, or EOF        */
#define SL_POLLPRI   0x0002   /* (accepted for source compatibility)         */
#define SL_POLLOUT   0x0004   /* writable                                    */
#define SL_POLLERR   0x0008   /* error pending (output only)                 */
#define SL_POLLHUP   0x0010   /* peer closed / connection gone (output only) */
#define SL_POLLNVAL  0x0020   /* fd is not open (output only)                */

struct sl_pollfd {
    int   fd;        /* socket fd (or 0 for the console); negative = ignored */
    short events;    /* requested: SL_POLLIN | SL_POLLOUT                    */
    short revents;   /* returned events                                      */
};

/* ---- newlib wrappers (libc/syscalls.c) ----
 * Freestanding apps get raw `starlightos_*` inlines from user/syscall.h
 * instead, so these names never clash with them in the same translation unit.
 * The wrappers follow newlib conventions: -1 + errno on failure, except
 * send/recv which return the byte count.
 */
#ifdef __cplusplus
extern "C" {
#endif
int  sl_socket(int domain, int type, int protocol);            /* fd, or -1 + errno */
int  sl_connect(int fd, const struct sl_sockaddr_in *addr);    /* 0, or -1 + errno */
long sl_send(int fd, const void *buf, size_t len);             /* bytes, or -1 + errno */
long sl_recv(int fd, void *buf, size_t len);                   /* bytes / 0 EOF, or -1 + errno */
int  sl_poll(struct sl_pollfd *fds, int nfds, int timeout_ms); /* ready count, 0 on timeout, -1 + errno */
int  sl_resolve(const char *host, uint8_t out_ip[4]);          /* 0, or -1 + errno */
int  sl_random(void *buf, size_t len);                         /* 0, or -1 + errno (CSPRNG bytes) */
#ifdef __cplusplus
}
#endif

#endif
