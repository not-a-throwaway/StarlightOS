/*
 * poll.c -- wait queues, the kfd descriptor table, and kpoll()/kselect().
 * See poll.h for the design overview.
 */
#include "poll.h"
#include "net.h"
#include "keyboard.h"
#include "ksched.h"
#include "ktime.h"
#include "kstring.h"

#define KE_PIPE (-32)

/* ------------------------------------------------------------------------
 * IRQ-safe critical sections. Wait-queue lists are edited in normal context
 * and walked from IRQ context (keyboard), so edits run with IF cleared.
 * Written so it assembles for both the i686 and x86_64 builds.
 * ---------------------------------------------------------------------- */
#ifdef POLL_HOST_TEST            /* tests/host/poll_host.c runs this file in a normal process */
extern void poll_host_halt(void);
static inline uintptr_t irq_save(void) { return 0; }
static inline void irq_restore(uintptr_t flags) { (void)flags; }
#define CPU_HALT() poll_host_halt()
#else
static inline uintptr_t irq_save(void)
{
    uintptr_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void irq_restore(uintptr_t flags)
{
    __asm__ volatile("push %0; popf" :: "r"(flags) : "memory", "cc");
}
#define CPU_HALT() __asm__ volatile("sti; hlt" ::: "memory")
#endif

/* ------------------------------------------------------------ wait queues */

void kwq_init(struct kwq *q) { q->head = 0; }

void kwq_add(struct kwq *q, struct kwq_entry *e, struct kpoll_waiter *w)
{
    uintptr_t f = irq_save();
    e->w = w;
    e->next = q->head;
    e->pprev = &q->head;
    if (q->head) q->head->pprev = &e->next;
    q->head = e;
    irq_restore(f);
}

void kwq_remove(struct kwq_entry *e)
{
    uintptr_t f = irq_save();
    if (e->pprev) {
        *e->pprev = e->next;
        if (e->next) e->next->pprev = e->pprev;
        e->pprev = 0;
        e->next = 0;
    }
    irq_restore(f);
}

/* Only sets flags: callable from the NIC poller, the keyboard IRQ, anywhere. */
void kwq_wake_all(struct kwq *q)
{
    for (struct kwq_entry *e = q->head; e; e = e->next)
        e->w->woken = 1;
}

/* ------------------------------------------------------------ fd table */

static struct kfd fd_table[KFD_MAX];
static struct kwq tty_wq;

struct kwq *kfd_tty_waitq(void) { return &tty_wq; }

static struct kfd *kfd_get(int fd)
{
    if (fd < 0 || fd >= KFD_MAX || fd_table[fd].type == KFD_NONE) return 0;
    return &fd_table[fd];
}

int kfd_tcp_handle(int fd)
{
    struct kfd *f = kfd_get(fd);
    return (f && (f->type == KFD_TCP || f->type == KFD_TCP_LISTEN)) ? f->handle : -1;
}

/* ---- TCP socket ---- */
static unsigned tcp_fd_poll(struct kfd *f)      { return tcp_poll_mask(f->handle); }
static struct kwq *tcp_fd_waitq(struct kfd *f)  { return tcp_waitq(f->handle); }

static int tcp_fd_read(struct kfd *f, void *buf, size_t n)
{
    if (n == 0) return 0;
    if (!tcp_available(f->handle)) {
        unsigned m = tcp_poll_mask(f->handle);
        if (!(m & (POLLIN | POLLHUP | POLLERR))) return KE_AGAIN;   /* open, nothing yet */
        return 0;                                                   /* EOF (POLLIN w/o data) */
    }
    return tcp_recv(f->handle, buf, n, 0);                          /* data is queued: returns at once */
}

static int tcp_fd_write(struct kfd *f, const void *buf, size_t n)
{
    unsigned m = tcp_poll_mask(f->handle);
    if (m & (POLLERR | POLLHUP)) return KE_PIPE;
    int room = tcp_tx_space(f->handle);
    if (room <= 0) return (m & POLLOUT) ? KE_AGAIN : KE_PIPE;
    if (n > (size_t)room) n = (size_t)room;                         /* short write, never blocks */
    return tcp_send(f->handle, buf, n);
}

static void tcp_fd_close(struct kfd *f) { tcp_close_nowait(f->handle); }   /* never blocks the event loop */

static const struct kfd_ops tcp_ops = { tcp_fd_poll, tcp_fd_waitq, tcp_fd_read, tcp_fd_write, tcp_fd_close };

/* ---- listening socket: read/write make no sense, accept via kfd_accept ---- */
static const struct kfd_ops listen_ops = { tcp_fd_poll, tcp_fd_waitq, 0, 0, tcp_fd_close };

/* ---- keyboard ---- */
static unsigned tty_poll(struct kfd *f)     { (void)f; return keyboard_has_char() ? POLLIN : 0; }
static struct kwq *tty_waitq(struct kfd *f) { (void)f; return &tty_wq; }
static int tty_read(struct kfd *f, void *buf, size_t n)
{
    (void)f;
    char *o = (char *)buf;
    size_t got = 0;
    while (got < n) {
        int c = keyboard_getchar_nonblocking();
        if (c < 0) break;
        o[got++] = (char)c;
    }
    return got ? (int)got : KE_AGAIN;
}
static const struct kfd_ops tty_ops = { tty_poll, tty_waitq, tty_read, 0, 0 };

static int fd_alloc(uint8_t type, int handle, const struct kfd_ops *ops)
{
    for (int i = 0; i < KFD_MAX; i++)
        if (fd_table[i].type == KFD_NONE) {
            fd_table[i].type = type; fd_table[i].handle = handle; fd_table[i].ops = ops;
            return i;
        }
    return KE_NFILE;
}

void kfd_init(void)
{
    memset(fd_table, 0, sizeof(fd_table));
    kwq_init(&tty_wq);
}

int kfd_open_tcp(int s)      { return s < 0 ? KE_BADF : fd_alloc(KFD_TCP, s, &tcp_ops); }
int kfd_open_listener(int s) { return s < 0 ? KE_BADF : fd_alloc(KFD_TCP_LISTEN, s, &listen_ops); }
int kfd_open_tty(void)       { return fd_alloc(KFD_TTY, 0, &tty_ops); }

int kfd_close(int fd)
{
    struct kfd *f = kfd_get(fd);
    if (!f) return KE_BADF;
    struct kfd_ops const *ops = f->ops;
    struct kfd copy = *f;
    f->type = KFD_NONE;                 /* free the slot first: close() may yield */
    if (ops->close) ops->close(&copy);
    return 0;
}

int kfd_read(int fd, void *buf, size_t n)
{
    struct kfd *f = kfd_get(fd);
    if (!f || !f->ops->read) return KE_BADF;
    return f->ops->read(f, buf, n);
}

int kfd_write(int fd, const void *buf, size_t n)
{
    struct kfd *f = kfd_get(fd);
    if (!f || !f->ops->write) return KE_BADF;
    return f->ops->write(f, buf, n);
}

int kfd_accept(int lfd)
{
    struct kfd *f = kfd_get(lfd);
    if (!f || f->type != KFD_TCP_LISTEN) return KE_BADF;
    int s = tcp_accept(f->handle, 0);          /* timeout 0: scan once, never waits */
    if (s < 0) return KE_AGAIN;
    int fd = kfd_open_tcp(s);
    if (fd < 0) { tcp_abort(s); return KE_NFILE; }
    return fd;
}

/* ------------------------------------------------------------------ poll */

/*
 * One pass over the set. With `reg` set, each fd's wait-queue entry is added
 * BEFORE its state is read. Order matters: if the state changed between the
 * read and the registration, the wake-up would be lost; registering first
 * means any change after that point sets w->woken.
 */
static int scan(struct pollfd *fds, unsigned n, struct kwq_entry *ents,
                struct kpoll_waiter *w, int reg)
{
    int ready = 0;
    for (unsigned i = 0; i < n; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        struct kfd *f = kfd_get(fds[i].fd);
        if (!f) { fds[i].revents = POLLNVAL; ready++; continue; }
        if (reg && f->ops->waitq) kwq_add(f->ops->waitq(f), &ents[i], w);
        unsigned m = f->ops->poll(f);
        unsigned r = m & ((unsigned)fds[i].events | POLLERR | POLLHUP);
        if (r) { fds[i].revents = (short)r; ready++; }
    }
    return ready;
}

/*
 * Park until something wakes `w`, the timeout lapses, or Ctrl-C.
 * Each lap runs the background pollers (this is what receives packets and
 * calls kwq_wake_all via tcp_input), then halts the CPU until the next
 * interrupt (PIT tick, keyboard, NIC if its IRQ is wired). The flag test
 * and `sti; hlt` happen with interrupts off: `sti` takes effect only after
 * the next instruction, so an IRQ cannot slip in between the test and hlt.
 */
static void sleep_until_woken(struct kpoll_waiter *w, uint32_t start, int timeout_ms)
{
    for (;;) {
        net_poll();
        kyield();
        if (w->woken || kabort_requested()) return;
        if (timeout_ms >= 0 && (uint32_t)(ktime_ms() - start) >= (uint32_t)timeout_ms) return;

        uintptr_t fl = irq_save();
        if (!w->woken) CPU_HALT();
        irq_restore(fl);
        return;            /* re-scan: cheap, and lets kpoll() re-check timeout/abort */
    }
}

int kpoll(struct pollfd *fds, unsigned nfds, int timeout_ms)
{
    if (nfds > KPOLL_MAX_FDS) return KE_INVAL;

    struct kwq_entry ents[KPOLL_MAX_FDS];
    for (unsigned i = 0; i < nfds; i++) ents[i].pprev = 0;

    struct kpoll_waiter w;
    w.woken = 0;
    uint32_t start = ktime_ms();
    int reg = (timeout_ms != 0);      /* a pure check never needs wait-queue entries */
    int ready;

    for (;;) {
        w.woken = 0;                   /* clear BEFORE scanning: a wake during the scan re-sets it */
        ready = scan(fds, nfds, ents, &w, reg);
        reg = 0;                       /* entries stay registered until we leave */
        if (ready || timeout_ms == 0) break;
        if (kabort_requested()) { ready = KE_INTR; break; }
        if (timeout_ms > 0 && (uint32_t)(ktime_ms() - start) >= (uint32_t)timeout_ms) break;
        sleep_until_woken(&w, start, timeout_ms);
    }

    for (unsigned i = 0; i < nfds; i++) kwq_remove(&ents[i]);
    return ready;
}

int kselect(int nfds, kfd_set *rd, kfd_set *wr, kfd_set *ex, int timeout_ms)
{
    if (nfds < 0 || nfds > KFD_MAX) return KE_INVAL;
    struct pollfd p[KFD_MAX];
    unsigned n = 0;
    for (int fd = 0; fd < nfds; fd++) {
        short ev = 0;
        if (rd && KFD_ISSET(fd, rd)) ev |= POLLIN;
        if (wr && KFD_ISSET(fd, wr)) ev |= POLLOUT;
        if (!ev && !(ex && KFD_ISSET(fd, ex))) continue;
        p[n].fd = fd; p[n].events = ev; p[n].revents = 0; n++;
    }
    int r = kpoll(p, n, timeout_ms);
    if (r < 0) return r;
    kfd_set r_out = {0}, w_out = {0}, e_out = {0};
    int count = 0;
    for (unsigned i = 0; i < n; i++) {
        int hit = 0;
        if ((p[i].revents & (POLLIN | POLLHUP)) && rd && KFD_ISSET(p[i].fd, rd)) { KFD_SET(p[i].fd, &r_out); hit++; }
        if ((p[i].revents & (POLLOUT | POLLERR)) && wr && KFD_ISSET(p[i].fd, wr)) { KFD_SET(p[i].fd, &w_out); hit++; }
        if ((p[i].revents & (POLLERR | POLLNVAL)) && ex && KFD_ISSET(p[i].fd, ex)) { KFD_SET(p[i].fd, &e_out); hit++; }
        if (hit) count++;
    }
    if (rd) *rd = r_out;
    if (wr) *wr = w_out;
    if (ex) *ex = e_out;
    return count;
}
