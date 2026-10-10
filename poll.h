#ifndef STARLIGHTOS_POLL_H
#define STARLIGHTOS_POLL_H
/*
 * poll.h -- readiness notification for StarlightOS (poll(2) / select(2) style).
 *
 * Three small layers:
 *
 *   1. kwq    wait queues. A producer (TCP input, keyboard IRQ, ...) calls
 *             kwq_wake_all(&queue) when a descriptor's readiness may have
 *             changed. Waiters are plain stack objects: no allocation.
 *
 *   2. kfd    a tiny descriptor table. Sockets already have integer handles
 *             and files are path-based, so there was nothing a pollfd could
 *             name; kfd gives every pollable object a small integer fd and a
 *             vtable { poll, waitq, read, write, close }.
 *
 *   3. kpoll  the sys_poll core. Registers the caller on the wait queue of
 *             every fd, then sleeps (hlt) until a producer wakes it, the
 *             timeout expires, or Ctrl-C is pressed. No busy spinning.
 *
 * StarlightOS is cooperative (see ksched.h), so "blocking the process"
 * means: park the waiter on the queues, then run the background pollers
 * (net_poll, audio, desktop redraw) and halt the CPU between events,
 * instead of re-scanning in a hot loop. The wake-up flag is checked with
 * interrupts disabled right before `sti; hlt`, so a wake-up that races the
 * sleep can never be lost.
 */
#include <stdint.h>
#include <stddef.h>

/* ---- the pollfd structure (values match Linux) ---- */
#define POLLIN    0x0001   /* data to read, a connection to accept, or EOF   */
#define POLLPRI   0x0002   /* (unused, accepted for source compatibility)    */
#define POLLOUT   0x0004   /* a write of at least one byte will not block    */
#define POLLERR   0x0008   /* error pending (output only)                    */
#define POLLHUP   0x0010   /* peer closed / connection gone (output only)    */
#define POLLNVAL  0x0020   /* fd is not open (output only)                   */

struct pollfd {
    int   fd;        /* kfd descriptor; negative fds are ignored (revents = 0) */
    short events;    /* requested events: POLLIN | POLLOUT                      */
    short revents;   /* returned events, filled in by kpoll()                   */
};

#define KPOLL_MAX_FDS 64          /* per kpoll() call */

/* errno-style results (negative) */
#define KE_AGAIN  (-11)           /* would block (non-blocking read/write/accept) */
#define KE_BADF   (-9)
#define KE_INTR   (-4)            /* kpoll interrupted by Ctrl-C */
#define KE_INVAL  (-22)
#define KE_NFILE  (-23)           /* descriptor table full */

/* ------------------------------------------------------------- wait queues */
struct kpoll_waiter;

struct kwq_entry {
    struct kwq_entry    *next;
    struct kwq_entry   **pprev;
    struct kpoll_waiter *w;
};

struct kwq { struct kwq_entry *head; };

#define KWQ_INIT { 0 }
void kwq_init(struct kwq *q);
void kwq_add(struct kwq *q, struct kwq_entry *e, struct kpoll_waiter *w);
void kwq_remove(struct kwq_entry *e);
void kwq_wake_all(struct kwq *q);        /* safe from IRQ context */

struct kpoll_waiter { volatile uint8_t woken; };

/* ------------------------------------------------------------- descriptors */
struct kfd;
struct kfd_ops {
    unsigned (*poll)(struct kfd *f);                          /* current POLL* mask  */
    struct kwq *(*waitq)(struct kfd *f);                      /* queue to sleep on   */
    int  (*read)(struct kfd *f, void *buf, size_t n);         /* never blocks        */
    int  (*write)(struct kfd *f, const void *buf, size_t n);  /* never blocks        */
    void (*close)(struct kfd *f);
};

enum { KFD_NONE = 0, KFD_TCP, KFD_TCP_LISTEN, KFD_TTY };

struct kfd {
    uint8_t                type;
    int                    handle;      /* tcp socket index, ... */
    const struct kfd_ops  *ops;
};

#define KFD_MAX 64

void kfd_init(void);
int  kfd_open_tcp(int tcp_sock);        /* wrap an established socket        */
int  kfd_open_listener(int tcp_sock);   /* wrap a listening socket           */
int  kfd_open_tty(void);                /* keyboard (stdin)                  */
int  kfd_close(int fd);                 /* closes the underlying object too  */
int  kfd_read(int fd, void *buf, size_t n);          /* >0 bytes, 0 EOF, KE_AGAIN */
int  kfd_write(int fd, const void *buf, size_t n);   /* bytes accepted (may be short), KE_AGAIN */
int  kfd_accept(int listen_fd);                      /* new fd, KE_AGAIN if none queued */
int  kfd_tcp_handle(int fd);                         /* raw socket index or -1 */
struct kwq *kfd_tty_waitq(void);                     /* for the keyboard IRQ  */

/* ------------------------------------------------------------------- poll */
/*
 * sys_poll core.
 *   timeout_ms  <0  wait forever (until an event or Ctrl-C)
 *               =0  just check
 *               >0  wait at most that long
 * Returns the number of entries with non-zero revents, 0 on timeout,
 * KE_INTR if Ctrl-C was raised, KE_INVAL for a bad nfds.
 */
int kpoll(struct pollfd *fds, unsigned nfds, int timeout_ms);

/* select(2) flavour built on kpoll(): fd bitmaps of up to KFD_MAX bits. */
typedef struct { uint64_t bits; } kfd_set;          /* KFD_MAX == 64 */
#define KFD_ZERO(s)     ((s)->bits = 0)
#define KFD_SET(fd, s)  ((s)->bits |=  (1ull << (fd)))
#define KFD_CLR(fd, s)  ((s)->bits &= ~(1ull << (fd)))
#define KFD_ISSET(fd,s) (((s)->bits >> (fd)) & 1ull)
int kselect(int nfds, kfd_set *rd, kfd_set *wr, kfd_set *ex, int timeout_ms);

#endif
