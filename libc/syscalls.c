/*
 * StarlightOS system-call layer for newlib (the "libgloss" part of the port).
 *
 * newlib implements the C library; the handful of functions it expects the OS to provide
 * (_write, _read, _open, _sbrk, ...) live here and turn into `int $0x80` calls handled by
 * process_syscall() in the kernel's process.c. Kernel calls return -errno on failure.
 *
 * Built into libstarlight.a and linked after libc.a (see starlight-gcc).
 */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/time.h>
#include <unistd.h>
#include "../user/slwindow.h"   /* struct sl_event + the window syscall numbers */
#include "../user/slnet.h"      /* struct sl_sockaddr_in/sl_pollfd + the network syscall numbers */

enum {
    SYS_read = 0, SYS_write = 1, SYS_close = 3, SYS_stat = 4, SYS_fstat = 5, SYS_lseek = 8,
    SYS_brk = 12, SYS_isatty = 16, SYS_getpid = 39, SYS_exit = 60, SYS_fsync = 74, SYS_getcwd = 79,
    SYS_chdir = 80, SYS_rename = 82, SYS_mkdir = 83, SYS_rmdir = 84, SYS_unlink = 87,
    SYS_gettimeofday = 96, SYS_uptime_us = 100, SYS_open = 101,
    SYS_fbmap = SL_SYS_FBMAP, SYS_present = SL_SYS_PRESENT, SYS_getevents = SL_SYS_GETEVENTS,
    SYS_socket = SL_SYS_SOCKET, SYS_connect = SL_SYS_CONNECT, SYS_send = SL_SYS_SEND,
    SYS_recv = SL_SYS_RECV, SYS_poll = SL_SYS_POLL, SYS_resolve = SL_SYS_RESOLVE,
    SYS_random = SL_SYS_RANDOM
};

/* kernel-side open flags (Linux values; translated from newlib's <fcntl.h> below) */
enum { SL_O_WRONLY = 1, SL_O_RDWR = 2, SL_O_CREAT = 0x40, SL_O_EXCL = 0x80, SL_O_TRUNC = 0x200, SL_O_APPEND = 0x400 };

struct sl_stat { uint32_t mode, size, ino, nlink, uid, gid; };
struct sl_timeval { int64_t sec, usec; };

static inline long sc(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

/* Converts a kernel "-errno" result into the libc convention (-1 + errno). */
static long fix(long r)
{
    if (r < 0 && r > -4096) { errno = (int)-r; return -1; }
    return r;
}

extern char **environ;   /* defined by newlib */
static char *empty_env[1] = { 0 };

void _exit(int status)
{
    sc(SYS_exit, status, 0, 0);
    for (;;) __asm__ volatile("pause");
}

int _write(int fd, const void *buf, size_t len)
{
    /* the kernel caps a single console write; loop so callers always see a full write */
    size_t done = 0;
    while (done < len) {
        long r = sc(SYS_write, fd, (long)((const char *)buf + done), (long)(len - done));
        if (r < 0) return done ? (int)done : (int)fix(r);
        if (r == 0) break;
        done += (size_t)r;
    }
    return (int)done;
}

int _read(int fd, void *buf, size_t len) { return (int)fix(sc(SYS_read, fd, (long)buf, (long)len)); }
int _close(int fd) { return (int)fix(sc(SYS_close, fd, 0, 0)); }
off_t _lseek(int fd, off_t off, int whence) { return (off_t)fix(sc(SYS_lseek, fd, (long)off, whence)); }
int _isatty(int fd) { return sc(SYS_isatty, fd, 0, 0) == 1; }
int fsync(int fd) { return (int)fix(sc(SYS_fsync, fd, 0, 0)); }

int _open(const char *path, int flags, ...)
{
    long k = 0;
    int acc = flags & O_ACCMODE;
    if (acc == O_WRONLY) k |= SL_O_WRONLY;
    else if (acc == O_RDWR) k |= SL_O_RDWR;
    if (flags & O_CREAT) k |= SL_O_CREAT;
    if (flags & O_EXCL) k |= SL_O_EXCL;
    if (flags & O_TRUNC) k |= SL_O_TRUNC;
    if (flags & O_APPEND) k |= SL_O_APPEND;
    return (int)fix(sc(SYS_open, (long)path, k, 0));
}

static void convert_stat(struct stat *st, const struct sl_stat *k)
{
    st->st_mode = k->mode;
    st->st_size = k->size;
    st->st_ino = k->ino;
    st->st_nlink = k->nlink;
    st->st_uid = (uid_t)k->uid;
    st->st_gid = (gid_t)k->gid;
    st->st_blksize = 4096;
    st->st_blocks = (k->size + 511) / 512;
}

int _fstat(int fd, struct stat *st)
{
    struct sl_stat k;
    long r = fix(sc(SYS_fstat, fd, (long)&k, 0));
    if (r < 0) return -1;
    for (size_t i = 0; i < sizeof(*st); i++) ((char *)st)[i] = 0;
    convert_stat(st, &k);
    return 0;
}

int _stat(const char *path, struct stat *st)
{
    struct sl_stat k;
    long r = fix(sc(SYS_stat, (long)path, (long)&k, 0));
    if (r < 0) return -1;
    for (size_t i = 0; i < sizeof(*st); i++) ((char *)st)[i] = 0;
    convert_stat(st, &k);
    return 0;
}

int lstat(const char *path, struct stat *st) { return _stat(path, st); }

int _unlink(const char *path) { return (int)fix(sc(SYS_unlink, (long)path, 0, 0)); }
int _rename(const char *from, const char *to) { return (int)fix(sc(SYS_rename, (long)from, (long)to, 0)); }
int mkdir(const char *path, mode_t mode) { (void)mode; return (int)fix(sc(SYS_mkdir, (long)path, 0, 0)); }
int rmdir(const char *path) { return (int)fix(sc(SYS_rmdir, (long)path, 0, 0)); }
int chdir(const char *path) { return (int)fix(sc(SYS_chdir, (long)path, 0, 0)); }

char *getcwd(char *buf, size_t size)
{
    char tmp[256];
    if (!buf) { buf = malloc(size ? size : sizeof(tmp)); if (!buf) { errno = ENOMEM; return 0; } if (!size) size = sizeof(tmp); }
    long r = fix(sc(SYS_getcwd, (long)buf, (long)size, 0));
    return r < 0 ? 0 : buf;
}

int _link(const char *a, const char *b) { (void)a; (void)b; errno = ENOSYS; return -1; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _fork(void) { errno = ENOSYS; return -1; }
int _execve(const char *n, char *const *a, char *const *e) { (void)n; (void)a; (void)e; errno = ENOSYS; return -1; }
int _wait(int *status) { (void)status; errno = ECHILD; return -1; }

int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    struct sl_timeval k;
    long r = fix(sc(SYS_gettimeofday, (long)&k, 0, 0));
    if (r < 0) return -1;
    if (tv) { tv->tv_sec = (time_t)k.sec; tv->tv_usec = (suseconds_t)k.usec; }
    return 0;
}

clock_t _times(struct tms *buf)
{
    /* newlib's CLOCKS_PER_SEC/CLK_TCK is 1000 on bare-metal targets: report milliseconds */
    clock_t t = (clock_t)(sc(SYS_uptime_us, 0, 0, 0) / 1000);
    if (buf) { buf->tms_utime = t; buf->tms_stime = 0; buf->tms_cutime = 0; buf->tms_cstime = 0; }
    return t;
}

/* ---- heap: newlib's malloc grows the brk() region [end of image, end of the process window) ---- */
void *_sbrk(ptrdiff_t incr)
{
    static uintptr_t cur;
    if (!cur) cur = (uintptr_t)sc(SYS_brk, 0, 0, 0);
    uintptr_t want = cur + (uintptr_t)incr;
    uintptr_t got = (uintptr_t)sc(SYS_brk, (long)want, 0, 0);
    if (got != want) { errno = ENOMEM; return (void *)-1; }
    uintptr_t old = cur;
    cur = got;
    return (void *)old;
}

/* ---- process start: called from crt0.S ---- */
extern int main(int, char **, char **);
extern void __libc_init_array(void);
extern void __libc_fini_array(void);

__attribute__((noreturn)) void __starlight_start(int argc, char **argv, char **envp)
{
    environ = (envp && envp[0]) ? envp : empty_env;
    __libc_init_array();
    int rc = main(argc, argv, environ);
    exit(rc);                       /* runs atexit handlers, flushes stdio, then _exit() */
    _exit(rc);
}

/* ---- plain POSIX names: this newlib configuration's *_r wrappers call these ---- */
int write(int fd, const void *b, size_t n) { return _write(fd, b, n); }
int read(int fd, void *b, size_t n) { return _read(fd, b, n); }
int close(int fd) { return _close(fd); }
off_t lseek(int fd, off_t o, int w) { return _lseek(fd, o, w); }
int fstat(int fd, struct stat *st) { return _fstat(fd, st); }
int stat(const char *p, struct stat *st) { return _stat(p, st); }
int isatty(int fd) { return _isatty(fd); }
int unlink(const char *p) { return _unlink(p); }
int rename(const char *a, const char *b) { return _rename(a, b); }
int link(const char *a, const char *b) { return _link(a, b); }
int kill(pid_t pid, int sig) { return _kill(pid, sig); }
pid_t getpid(void) { return _getpid(); }
pid_t fork(void) { return _fork(); }
int execve(const char *n, char *const a[], char *const e[]) { return _execve(n, a, e); }
pid_t wait(int *s) { return _wait(s); }
int gettimeofday(struct timeval *tv, void *tz) { return _gettimeofday(tv, tz); }
clock_t times(struct tms *b) { return _times(b); }
void *sbrk(ptrdiff_t i) { return _sbrk(i); }
int open(const char *p, int flags, ...) { return _open(p, flags); }

/* ---- window syscalls (see user/slwindow.h) ---- */
void *sl_fbmap(int w, int h)
{
    long r = sc(SYS_fbmap, w, h, 0);
    if (r < 0 && r > -4096) { errno = (int)-r; return NULL; }
    return (void *)r;
}

int sl_present(void) { return (int)fix(sc(SYS_present, 0, 0, 0)); }

int sl_getevents(struct sl_event *buf, int count, int timeout_ms)
{
    return (int)fix(sc(SYS_getevents, (long)buf, count, timeout_ms));
}

/* ---- network syscalls (see user/slnet.h) ---- */
int sl_socket(int domain, int type, int protocol)
{
    return (int)fix(sc(SYS_socket, domain, type, protocol));
}

int sl_connect(int fd, const struct sl_sockaddr_in *addr)
{
    return (int)fix(sc(SYS_connect, fd, (long)(uintptr_t)addr, (long)sizeof *addr));
}

long sl_send(int fd, const void *buf, size_t len)
{
    return fix(sc(SYS_send, fd, (long)(uintptr_t)buf, (long)len));
}

long sl_recv(int fd, void *buf, size_t len)
{
    return fix(sc(SYS_recv, fd, (long)(uintptr_t)buf, (long)len));
}

int sl_poll(struct sl_pollfd *fds, int nfds, int timeout_ms)
{
    return (int)fix(sc(SYS_poll, (long)(uintptr_t)fds, nfds, timeout_ms));
}

int sl_resolve(const char *host, uint8_t out_ip[4])
{
    return (int)fix(sc(SYS_resolve, (long)(uintptr_t)host, (long)(uintptr_t)out_ip, 0));
}

int sl_random(void *buf, size_t len)
{
    return (int)fix(sc(SYS_random, (long)(uintptr_t)buf, (long)len, 0));
}
