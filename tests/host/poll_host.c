/*
 * Host harness for poll.c + httpd.c.
 *
 * The real kpoll()/kfd/wait-queue code and the real httpd event loop run in
 * a normal Linux process. Only the layers below them are replaced:
 *   - tcp_*  -> thin wrappers over non-blocking host sockets, with a mock
 *               net_poll() that plays the part of tcp_input(): it notices
 *               readiness changes and calls kwq_wake_all(), exactly the
 *               producer side of the real wake-up path
 *   - "hlt"  -> usleep(10 ms) (the PIT runs at 100 Hz, so hlt wakes that often)
 *   - VFS    -> a host directory
 *
 *   poll_host serve DIR PORT   run httpd_main() on DIR (as if cwd = .../httpd_test)
 *   poll_host unit             kpoll()/kselect() unit tests on socketpairs
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/sockios.h>

#include "../../poll.h"
#include "../../net.h"
#include "../../vfs.h"
/* glibc's <poll.h> would clash with the kernel's; struct pollfd and the POLL* values are identical on Linux. */
extern int poll(struct pollfd *, unsigned long, int);

/* ------------------------------------------------------------ time / sched */
static volatile int abort_flag;
static void on_sigint(int s) { (void)s; abort_flag = 1; }
uint32_t ktime_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u); }
int  kabort_requested(void) { return abort_flag; }
void kyield(void) {}
void poll_host_halt(void) { usleep(10000); }

/* ------------------------------------------------------------- mock tcp */
#define NH 16
static struct { int used, fd, listener, parent; unsigned last; struct kwq wq; } H[NH];

static int h_alloc(int fd, int listener)
{
    for (int i = 0; i < NH; i++) if (!H[i].used) { H[i].used = 1; H[i].fd = fd; H[i].listener = listener; H[i].last = 0; H[i].wq.head = H[i].wq.head; return i; }
    return -1;
}
int mock_adopt(int fd) { fcntl(fd, F_SETFL, O_NONBLOCK); return h_alloc(fd, 0); }

unsigned tcp_poll_mask(int h)
{
    if (h < 0 || h >= NH || !H[h].used) return 0x20;
    struct pollfd p = { H[h].fd, H[h].listener ? POLLIN : (POLLIN | POLLOUT), 0 };
    poll(&p, 1, 0);
    unsigned m = 0;
    if (p.revents & POLLIN)  m |= 0x1;
    if (p.revents & POLLOUT) m |= 0x4;
    if (p.revents & POLLERR) m |= 0x8;
    if (p.revents & POLLHUP) m |= 0x10;
    return m;
}
struct kwq *tcp_waitq(int h) { return (h >= 0 && h < NH) ? &H[h].wq : 0; }
int tcp_tx_space(int h)
{
    int q = 0; ioctl(H[h].fd, TIOCOUTQ, &q);
    int room = 16384 - q; return room < 0 ? 0 : room;
}
int tcp_listen(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(fd, (void *)&a, sizeof a) || listen(fd, 16)) return -1;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return h_alloc(fd, 1);
}
int tcp_accept(int l, uint32_t t) { (void)t; int fd = accept(H[l].fd, 0, 0); if (fd < 0) return -1; fcntl(fd, F_SETFL, O_NONBLOCK); return h_alloc(fd, 0); }
int tcp_available(int h) { int n = 0; ioctl(H[h].fd, FIONREAD, &n); return n; }
int tcp_recv(int h, void *b, size_t n, uint32_t t) { (void)t; int r = (int)recv(H[h].fd, b, n, 0); return r < 0 ? -1 : r; }
int tcp_send(int h, const void *b, size_t n) { int r = (int)send(H[h].fd, b, n, MSG_NOSIGNAL); return r < 0 ? TCP_ERR_RESET : r; }
void tcp_close(int h) { if (h >= 0 && H[h].used) { close(H[h].fd); H[h].used = 0; } }
void tcp_close_nowait(int h) { tcp_close(h); }
void tcp_abort(int h) { tcp_close(h); }
int tcp_peer(int h, uint8_t ip[4], uint16_t *port)
{
    struct sockaddr_in a; socklen_t l = sizeof a;
    if (getpeername(H[h].fd, (void *)&a, &l)) return -1;
    memcpy(ip, &a.sin_addr, 4); *port = ntohs(a.sin_port); return 0;
}
/* Plays tcp_input()'s role: detect readiness changes, wake the queue. */
void net_poll(void)
{
    for (int i = 0; i < NH; i++) {
        if (!H[i].used) continue;
        unsigned m = tcp_poll_mask(i);
        if (m != H[i].last) { H[i].last = m; kwq_wake_all(&H[i].wq); }
        else if (m & 0x1) kwq_wake_all(&H[i].wq);        /* level-triggered data still queued */
    }
}
int  net_configured(void) { return 1; }
const struct net_config *net_get_config(void) { static struct net_config c; return &c; }
void net_fmt_ip(char *o, const uint8_t ip[4]) { sprintf(o, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]); }
int  keyboard_has_char(void) { return 0; }
int  keyboard_getchar_nonblocking(void) { return -1; }

/* ------------------------------------------------------- kernel services */
void *kmalloc(size_t n) { return malloc(n); }
void  kfree(void *p) { free(p); }
int kprintf(const char *f, ...) { va_list a; va_start(a, f); int r = vprintf(f, a); va_end(a); fflush(stdout); return r; }
int ksnprintf(char *b, size_t n, const char *f, ...) { va_list a; va_start(a, f); int r = vsnprintf(b, n, f, a); va_end(a); return r; }
int kisalnum(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int kparse_uint(const char *s, uint32_t *o) { char *e; unsigned long v = strtoul(s, &e, 0); if (*e || e == s) return -1; *o = (uint32_t)v; return 0; }
uint32_t kstrtoul(const char *s, const char **end, int base) { char *e; unsigned long v = strtoul(s, &e, base); if (end) *end = e; return (uint32_t)v; }
#ifndef __GLIBC_USE_LIB_EXT2
size_t strlcpy(char *d, const char *s, size_t n) { size_t l = strlen(s); if (n) { size_t c = l >= n ? n - 1 : l; memcpy(d, s, c); d[c] = 0; } return l; }
#endif

static char root[512];
const char *vfs_get_cwd(void) { static char b[600]; snprintf(b, sizeof b, "/home/guest/httpd_test"); return b; }
static void full(const char *n, char *o, size_t cap) { snprintf(o, cap, "%s/%s", root, n); }
int vfs_exists(const char *n) { char p[800]; full(n, p, sizeof p); struct stat s; return !stat(p, &s); }
int vfs_is_dir(const char *n) { char p[800]; full(n, p, sizeof p); struct stat s; return !stat(p, &s) && S_ISDIR(s.st_mode); }
int vfs_cd(const char *n) { (void)n; return 0; }
int vfs_get_entries(char out[][VFS_NAME_MAX + 1], uint8_t isdir[], int max)
{
    DIR *d = opendir(root); int n = 0; struct dirent *e;
    while (d && (e = readdir(d)) && n < max) { if (e->d_name[0] == '.') continue; strlcpy(out[n], e->d_name, VFS_NAME_MAX + 1); isdir[n++] = e->d_type == DT_DIR; }
    if (d) closedir(d);
    return n;
}
char *shell_read_file(const char *name, size_t *len)
{
    char p[800]; full(name, p, sizeof p);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(sz + 1); if (fread(b, 1, sz, f) != (size_t)sz) { fclose(f); return 0; }
    b[sz] = 0; fclose(f); *len = (size_t)sz; return b;
}
extern int httpd_main(int, char **);

/* ------------------------------------------------------------ unit tests */
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int mkpair(int *peer)
{
    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    *peer = sv[1];
    int h = mock_adopt(sv[0]);
    return kfd_open_tcp(h);
}

static int unit(void)
{
    kfd_init();
    int peer, fd = mkpair(&peer);
    struct pollfd p = { fd, POLLIN, 0 };
    uint32_t t0;

    CHECK(kpoll(&p, 1, 0) == 0, "idle fd, timeout 0 -> 0");
    t0 = ktime_ms(); int r = kpoll(&p, 1, 200); uint32_t dt = ktime_ms() - t0;
    CHECK(r == 0 && dt >= 190 && dt < 260, "timeout 200ms: r=%d dt=%u", r, dt);

    p.events = POLLOUT;
    CHECK(kpoll(&p, 1, 0) == 1 && (p.revents & POLLOUT), "writable immediately");
    p.events = POLLIN;

    /* wake-up from another process while we sleep in kpoll */
    if (!fork()) { usleep(100000); (void)!write(peer, "x", 1); _exit(0); }
    t0 = ktime_ms(); r = kpoll(&p, 1, 2000); dt = ktime_ms() - t0;
    CHECK(r == 1 && (p.revents & POLLIN) && dt >= 90 && dt < 150, "woken by data after ~100ms: r=%d dt=%u", r, dt);
    char c; CHECK(kfd_read(fd, &c, 1) == 1 && c == 'x', "read the byte");
    CHECK(kfd_read(fd, &c, 1) == KE_AGAIN, "empty read is EAGAIN, not blocking");

    /* data already queued before the call: must not sleep at all */
    (void)!write(peer, "yz", 2);
    t0 = ktime_ms(); r = kpoll(&p, 1, 5000); dt = ktime_ms() - t0;
    CHECK(r == 1 && dt < 5, "pre-queued data returns at once: dt=%u", dt);
    CHECK(kfd_read(fd, &c, 1) == 1 && kfd_read(fd, &c, 1) == 1, "drain");

    /* two fds: only the second becomes ready */
    int peer2, fd2 = mkpair(&peer2);
    struct pollfd two[3] = { { fd, POLLIN, 0 }, { fd2, POLLIN, 0 }, { -1, POLLIN, 7 } };
    if (!fork()) { usleep(60000); (void)!write(peer2, "q", 1); _exit(0); }
    r = kpoll(two, 3, 2000);
    CHECK(r == 1 && two[0].revents == 0 && (two[1].revents & POLLIN) && two[2].revents == 0, "multi-fd: r=%d %d %d %d", r, two[0].revents, two[1].revents, two[2].revents);

    /* select() wrapper */
    kfd_set rd; KFD_ZERO(&rd); KFD_SET(fd, &rd); KFD_SET(fd2, &rd);
    r = kselect(fd2 + 1, &rd, 0, 0, 100);
    CHECK(r == 1 && !KFD_ISSET(fd, &rd) && KFD_ISSET(fd2, &rd), "select: r=%d", r);

    /* peer hangs up -> POLLHUP/POLLIN (EOF) */
    close(peer2);
    two[1].events = POLLIN; r = kpoll(&two[1], 1, 500);
    CHECK(r == 1 && (two[1].revents & (POLLIN | POLLHUP)), "peer close seen: revents=%x", two[1].revents);
    CHECK(kfd_read(fd2, &c, 1) == 1 || 1, "(drain q)");
    CHECK(kfd_read(fd2, &c, 1) == 0, "then EOF reads 0");

    /* bad fd */
    struct pollfd bad = { 55, POLLIN, 0 };
    CHECK(kpoll(&bad, 1, 0) == 1 && bad.revents == POLLNVAL, "POLLNVAL");

    /* Ctrl-C interrupts an infinite wait */
    abort_flag = 0; signal(SIGALRM, on_sigint); alarm(1);
    p.events = POLLIN; t0 = ktime_ms(); r = kpoll(&p, 1, -1); dt = ktime_ms() - t0;
    CHECK(r == KE_INTR && dt < 1200, "Ctrl-C: r=%d dt=%u", r, dt);

    /* registrations are all removed: queue heads must be empty */
    CHECK(H[0].wq.head == 0 && H[1].wq.head == 0, "wait queues are clean after kpoll");

    printf(fails ? "UNIT FAILED (%d)\n" : "UNIT OK\n", fails);
    return fails != 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (argc >= 2 && !strcmp(argv[1], "unit")) return unit();
    if (argc >= 4 && !strcmp(argv[1], "serve")) {
        strlcpy(root, argv[2], sizeof root);
        signal(SIGINT, on_sigint);
        kfd_init();
        char *a[] = { "httpd", argv[3], 0 };
        int rc = httpd_main(2, a);
        printf("exit %d\n", rc);
        return rc;
    }
    fprintf(stderr, "usage: poll_host unit | serve DIR PORT\n");
    return 2;
}
