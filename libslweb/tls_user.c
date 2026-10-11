/*
 * tls_user.c -- ring-3 platform layer for the kernel's TLS client (M9.4).
 *
 * tls.c was written against the kernel's services (kernel sockets, the kernel
 * heap, kprintf, ktime_unix, krand_bytes, net_parse_ip). To lift it into
 * libslweb unchanged we provide those symbols here, on top of the M2 socket
 * syscalls (user/slnet.h) and newlib:
 *
 *   tcp_send/tcp_recv  ->  sl_send/sl_poll/sl_recv (blocking IPv4 sockets)
 *   kmalloc/kcalloc/... ->  malloc/calloc/realloc/free
 *   ktime_unix          ->  time()             (newlib backs it with gettimeofday)
 *   krand_bytes         ->  sl_random()        (the kernel's CSPRNG, syscall 126)
 *   net_parse_ip        ->  a small dotted-quad parser (no libc inet_pton needed)
 *
 * The same file is compiled into libslweb.a for the OS and into the host tests
 * (which supply their own sl_* sockets), so tls.c/crypto.c/x509.c have exactly
 * one build for both.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../net.h"        /* TCP_ERR_*, tcp_send/tcp_recv/net_parse_ip prototypes */
#include "../kheap.h"      /* kmalloc/kcalloc/krealloc/kfree prototypes */
#include "../kprintf.h"    /* kprintf/ksnprintf prototypes */
#include "../krand.h"      /* krand_bytes prototype */
#include "../ktime.h"      /* ktime_unix prototype */
#include "../user/slnet.h" /* sl_send/sl_recv/sl_poll/sl_random */

/* ---- sockets (tls.c reads/writes one established TCP connection) ---- */
int tcp_send(int s, const void *data, size_t len)
{
    const char *p = (const char *)data;
    size_t off = 0;
    while (off < len) {
        long r = sl_send(s, p + off, len - off);
        if (r <= 0) return TCP_ERR_ABORT;
        off += (size_t)r;
    }
    return (int)len;
}

int tcp_recv(int s, void *buf, size_t cap, uint32_t timeout_ms)
{
    struct sl_pollfd p;
    p.fd = s; p.events = SL_POLLIN; p.revents = 0;
    int pr = sl_poll(&p, 1, (int)timeout_ms);
    if (pr == 0) return -1;                 /* tls.c: -1 means "nothing arrived yet" */
    if (pr < 0) return TCP_ERR_ABORT;
    long r = sl_recv(s, buf, cap);
    if (r < 0) return TCP_ERR_ABORT;
    return (int)r;
}

/* ---- heap (tls_conn and the certificate chain are heap allocated) ---- */
void *kmalloc(size_t n) { return malloc(n); }
void *kcalloc(size_t n, size_t sz) { return calloc(n, sz); }
void *krealloc(void *p, size_t n) { return realloc(p, n); }
void  kfree(void *p) { free(p); }

/* ---- clock (certificate validity is checked against the wall clock) ---- */
uint32_t ktime_unix(void) { return (uint32_t)time(NULL); }

/* ---- entropy: the kernel's CSPRNG through SL_SYS_RANDOM ---- */
void krand_bytes(void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        size_t chunk = n > 256 ? 256 : n;
        if (sl_random(p, chunk) != 0)              /* should not happen; keep going without zeros */
            for (size_t i = 0; i < chunk; i++)
                p[i] = (uint8_t)((uint32_t)time(NULL) ^ (unsigned)(i * 2654435761u));
        p += chunk;
        n -= chunk;
    }
}

/* ---- printf for the tls.c trace (off unless tls_set_trace(1)) ---- */
int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int kprintf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- "1.2.3.4" -> four bytes; 1 on success, 0 on failure (net.c convention) ---- */
int net_parse_ip(const char *s, uint8_t ip[4])
{
    int part = 0, val = 0, digits = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if (val > 255) return 0;
            digits = 1;
        } else if (*s == '.' || *s == 0) {
            if (!digits || part > 3) return 0;
            ip[part++] = (uint8_t)val;
            val = 0; digits = 0;
            if (*s == 0) break;
        } else {
            return 0;
        }
    }
    return part == 4 ? 1 : 0;
}
