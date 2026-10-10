/*
 * process_host.c -- exercises the REAL process.c syscall layer on the host: argv/stack
 * construction, the fd table (open/read/write/lseek/close/fstat), brk, cooked console stdin,
 * path syscalls, exit status and pointer validation. Hardware (paging, TSS, keyboard, VFS
 * storage) is stubbed; the 64 MiB user window is a real mmap() wherever the OS puts it
 * (process_host_base replaces the kernel's fixed 0x40000000 window).
 *
 *   gcc -O1 -g -w -fsanitize=address,undefined -DPROCESS_HOST_TEST -I. -include string.h \
 *       -o /tmp/process_host tests/host/process_host.c && /tmp/process_host
 *
 * Works on any host architecture: -DPROCESS_HOST_TEST compiles process.c's syscall layer
 * while stripping its privileged x86 instructions (which are guarded by #ifdef __x86_64__).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "net.h"          /* tcp_*/dns_* prototypes + TCP_ERR_* (stubbed below) */
#include "poll.h"         /* struct pollfd + POLL* + KE_* */

uint64_t p4_table[512], p3_table[512], p2_table1[512], gdt64_tss_descriptor[2];
void process_enter_user(void) {}
void process_return_to_kernel(void) {}
static char termbuf[4096]; static int termlen;
void terminal_putchar(char c) { if (termlen < (int)sizeof termbuf - 1) termbuf[termlen++] = c, termbuf[termlen] = 0; }
void terminal_write_uint(uint32_t v) { (void)v; }
typedef uint32_t gfx_t;
gfx_t *terminal_get_render_target(void) { return 0; }
void terminal_set_render_target(gfx_t *t) { (void)t; }
static const char *kb_script; static size_t kb_pos; static int abort_flag;
int keyboard_getchar_nonblocking(void) { return kb_script && kb_script[kb_pos] ? (unsigned char)kb_script[kb_pos++] : -1; }
void keyboard_flush_queue(void) {}
void (*keyboard_get_dispatch(void))(char) { return 0; }
void keyboard_set_dispatch(void (*h)(char)) { (void)h; }
void kabort_raise(void) { abort_flag = 1; }
void kabort_clear(void) { abort_flag = 0; }
int kabort_requested(void) { return abort_flag; }
uint32_t ktime_unix(void) { return 1790000000u; }
uint64_t ktime_us(void) { return 123456789ull; }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
size_t strlcpy(char *d, const char *s, size_t n) { size_t l = strlen(s); if (n) { size_t c = l >= n ? n - 1 : l; memcpy(d, s, c); d[c] = 0; } return l; }

/* ---- simulated TCP stack + kfd table (net.c/tcp.c/poll.c are not in the host build) ----
 * Enough of the kernel network objects to drive process.c's dispatcher: connect()
 * records the address, send() records bytes, recv() replays a scripted stream, and
 * kpoll() reports readiness from net_poll_mask_out. Each knob is reset per case. */
#define HOST_TCP_MAX 8
static int      net_connect_ret;                 /* non-zero => tcp_connect returns it */
static uint8_t  net_last_ip[4];
static uint16_t net_last_port;
static int      net_send_ret = -1000;            /* -1000 => echo the byte count */
static int      net_send_calls;
static char     net_sent[256]; static size_t net_sent_len;
static const char *net_rx; static size_t net_rx_len, net_rx_off;
static int      net_rx_mode;                     /* 0 = script, 1 = time out (-1), 2 = error */
static int      net_rx_err = -2;                 /* TCP_ERR_RESET by default */
static unsigned net_poll_mask_out = POLLIN;      /* revents = events & mask */
static int      net_poll_ret;                    /* set to KE_INTR to simulate Ctrl-C */
static int      net_poll_last_timeout = -12345;
static int      net_kfd_tty = 70;
static int      host_kfd_used[HOST_TCP_MAX];

int tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (net_connect_ret) return net_connect_ret;
    memcpy(net_last_ip, ip, 4); net_last_port = port;
    return 3;
}
int tcp_send(int s, const void *data, size_t len)
{
    (void)s; net_send_calls++;
    size_t room = sizeof(net_sent) - net_sent_len; if (len > room) len = room;
    memcpy(net_sent + net_sent_len, data, len); net_sent_len += len;
    return net_send_ret != -1000 ? net_send_ret : (int)len;
}
int tcp_recv(int s, void *buf, size_t cap, uint32_t timeout_ms)
{
    (void)s; (void)timeout_ms;
    if (net_rx_mode == 1) return -1;                          /* "nothing arrived in time" */
    if (net_rx_mode == 2) return net_rx_err;
    if (net_rx_off >= net_rx_len) return 0;                   /* EOF */
    size_t n = net_rx_len - net_rx_off; if (n > cap) n = cap;
    memcpy(buf, net_rx + net_rx_off, n); net_rx_off += n;
    return (int)n;
}
void tcp_abort(int s) { (void)s; }

int kfd_open_tcp(int s) { (void)s; for (int i = 0; i < HOST_TCP_MAX; i++) if (!host_kfd_used[i]) { host_kfd_used[i] = 1; return 100 + i; } return -1; }
int kfd_open_tty(void) { return net_kfd_tty; }
int kfd_close(int fd) { if (fd >= 100 && fd < 100 + HOST_TCP_MAX) host_kfd_used[fd - 100] = 0; return 0; }
int kfd_tcp_handle(int fd) { return (fd >= 100 && fd < 100 + HOST_TCP_MAX && host_kfd_used[fd - 100]) ? 3 : -1; }
int kpoll(struct pollfd *fds, unsigned nfds, int timeout_ms)
{
    net_poll_last_timeout = timeout_ms;
    if (net_poll_ret) return net_poll_ret;
    int ready = 0;
    for (unsigned i = 0; i < nfds; i++) { fds[i].revents = (short)(fds[i].events & net_poll_mask_out); if (fds[i].revents) ready++; }
    return ready;
}
int dns_resolve(const char *name, uint8_t ip[4])
{
    if (!strcmp(name, "host.test")) { ip[0] = 1; ip[1] = 2; ip[2] = 3; ip[3] = 4; return 0; }
    if (!strcmp(name, "10.0.2.2"))  { ip[0] = 10; ip[1] = 0; ip[2] = 2; ip[3] = 2; return 0; }
    return -1;
}

#include "process.c"      /* the code under test */

uint64_t process_host_base;   /* window base handed to process.c's USER_BASE (set in main) */

/* ---- gui guest-window stubs (gui.c is not part of the host build) ----
 * gui_guest_open applies gui.c's screen-fit rule against a notional 1920x1080
 * desktop; gui_guest_wait hands back the canned events (or -1 while the host's
 * abort_flag stands in for kabort_requested()). */
static int g_open, g_w, g_h, g_present_calls, g_event_count;
static struct sl_event g_events[8];
static uint32_t g_surface[1024 * 512] __attribute__((aligned(4096)));   /* 2 MiB, page-aligned like gui.c's */

int gui_guest_open(int w, int h)
{
    if (g_open) return -2;
    if (w + 6 > 1920 || h + 39 + 30 > 1080) return -1;
    g_open = 1; g_w = w; g_h = h;
    return 0;
}
void gui_guest_close(void) { g_open = 0; g_w = g_h = 0; }
int gui_guest_is_open(void) { return g_open; }
void gui_guest_dims(int *w, int *h) { if (w) *w = g_w; if (h) *h = g_h; }
void *gui_guest_surface(void) { return g_surface; }
int gui_guest_present(void) { g_present_calls++; return 0; }
int gui_guest_wait(struct sl_event *buf, int count, int timeout_ms)
{
    (void)timeout_ms;
    if (abort_flag) return -1;
    int n = g_event_count < count ? g_event_count : count;
    for (int i = 0; i < n; i++) buf[i] = g_events[i];
    g_event_count = 0;
    return n;
}

/* ---- in-memory VFS ---- */
struct mf { char path[64]; int dir; uint8_t *data; uint32_t size; uint16_t mode; };
static struct mf files[16]; static int nfiles; static char cwd[64] = "/";
static struct mf *find(const char *p) { for (int i = 0; i < nfiles; i++) if (files[i].path[0] && !strcmp(files[i].path, p)) return &files[i]; return 0; }
static struct mf *add(const char *p, int dir, const char *d, uint32_t n) {
    struct mf *f = find(p); if (!f) { f = &files[nfiles++]; } strcpy(f->path, p); f->dir = dir; free(f->data);
    f->data = malloc(n + 1); memcpy(f->data, d, n); f->size = n; f->mode = dir ? 0x41ED : 0x81A4; return f; }
void vfs_abspath(const char *in, char *out) { if (in[0] == '/') strcpy(out, in); else snprintf(out, 256, "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", in); }
const char *vfs_get_cwd(void) { return cwd; }
int vfs_cd(const char *p) { struct mf *f = find(p); if (!strcmp(p, "/")) { strcpy(cwd, p); return 0; } if (!f) return VFS_ENOENT; if (!f->dir) return VFS_ENOTDIR; strcpy(cwd, p); return 0; }
int vfs_stat(const char *p, vfs_node_t *o) { struct mf *f = find(p); if (!strcmp(p, "/")) { memset(o, 0, sizeof *o); o->type = VFS_NODE_DIR; o->mode = 0x41ED; return 0; }
    if (!f) return VFS_ENOENT; memset(o, 0, sizeof *o); o->type = f->dir ? VFS_NODE_DIR : VFS_NODE_FILE; o->size = f->size; o->mode = f->mode; o->nlink = 1; o->ino = 7; return 0; }
int vfs_access(const char *p, int want) { if (!strcmp(p, "/")) return 0; struct mf *f = find(p); if (!f) return VFS_ENOENT; if ((want & VFS_WANT_W) && !(f->mode & VFS_IWUSR)) return VFS_EPERM; return 0; }
int vfs_read(const char *p, uint32_t off, void *b, uint32_t len, uint32_t *got) { struct mf *f = find(p); if (!f) return VFS_ENOENT; if (off >= f->size) { *got = 0; return 0; }
    uint32_t n = f->size - off; if (n > len) n = len; memcpy(b, f->data + off, n); *got = n; return 0; }
int vfs_read_all(const char *p, char *b, uint32_t cap, uint32_t *len) { struct mf *f = find(p); if (!f) return VFS_ENOENT; if (f->size > cap) return VFS_EINVAL; memcpy(b, f->data, f->size); *len = f->size; return 0; }
int vfs_write(const char *p, const char *d, uint32_t n) { add(p, 0, d, n); return 0; }
int vfs_delete(const char *p) { struct mf *f = find(p); if (!f) return VFS_ENOENT; f->path[0] = 0; return 0; }
int vfs_mkdir(const char *p) { if (find(p)) return VFS_EEXIST; add(p, 1, "", 0); return 0; }
int vfs_rmdir(const char *p) { struct mf *f = find(p); if (!f) return VFS_ENOENT; f->path[0] = 0; return 0; }
int vfs_rename(const char *a, const char *b) { struct mf *f = find(a); if (!f) return VFS_ENOENT; strcpy(f->path, b); return 0; }

/* ---- harness ---- */
static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static struct user_registers R;
static int64_t sc(uint64_t n, uint64_t a, uint64_t b, uint64_t c) { memset(&R, 0, sizeof R); R.rax = n; R.rdi = a; R.rsi = b; R.rdx = c; return (int64_t)process_syscall(&R); }
#define W(off) (USER_BASE + 0x1000 + (off))               /* scratch inside the (RW) window */
#define P(off) ((char *)(uintptr_t)W(off))
static void setstr(uint64_t off, const char *s) { strcpy(P(off), s); }

int main(void)
{
    CHECK(USER_STACK_TOP - USER_BASE == USER_WINDOW);            /* the window maths holds together */
    CHECK(USER_IMAGE_MAX + USER_STACK_SIZE == USER_WINDOW);
    CHECK(USER_PT_PAGES * 512u * (uint64_t)PAGE_SIZE == USER_WINDOW);
    void *m = mmap(0, (size_t)USER_WINDOW, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 2; }
    process_host_base = (uint64_t)(uintptr_t)m;                /* window base handed to USER_BASE */
    for (int i = 0; i < USER_IMAGE_PAGES; i++) user_image_flags[i] = 3;     /* RW image/heap */
    user_image_flags[0] = 1 | 4;                                             /* page 0 = read-only text */
    user_brk_start = user_brk_cur = 0x20000;
    memset(fds, 0, sizeof fds);

    /* --- argv / stack layout --- */
    char *av[] = { "prog", "hello world", "", "x" };
    CHECK(build_user_stack(4, av) == 0);
    CHECK((user_stack_pointer & 15) == 8);
    uint64_t base = USER_STACK_TOP - USER_STACK_SIZE;
    uint64_t *w = (uint64_t *)(user_stack + (user_stack_pointer - base));
    CHECK(w[0] == 4 && w[5] == 0 && w[6] == 0);
    for (int i = 0; i < 4; i++) {
        CHECK(w[1 + i] >= user_stack_pointer && w[1 + i] < USER_STACK_TOP);
        CHECK(!strcmp((char *)(user_stack + (w[1 + i] - base)), av[i]));
    }
    CHECK(build_user_stack(0, av) == 0 && ((uint64_t *)(user_stack + (user_stack_pointer - base)))[0] == 0);
    char *many[MAX_ARGC + 1]; for (int i = 0; i <= MAX_ARGC; i++) many[i] = "a";
    CHECK(build_user_stack(MAX_ARGC + 1, many) == -1);
    static char huge[MAX_ARGS_BYTES + 8]; memset(huge, 'z', sizeof huge - 1); char *hv[] = { huge };
    CHECK(build_user_stack(1, hv) == -1);

    /* --- console write --- */
    setstr(0, "hi\n");
    CHECK(sc(1, 1, W(0), 3) == 3 && !strcmp(termbuf, "hi\n"));
    CHECK(sc(1, 2, W(0), 3) == 3);
    CHECK(sc(1, 1, 0x1000, 3) == -E_FAULT);                                  /* kernel/low memory */
    CHECK(sc(1, 7, W(0), 3) == -E_BADF);

    /* --- files: create, write, close, read back through the VFS --- */
    setstr(0, "OUT.TXT"); setstr(64, "hello, ");  setstr(128, "newlib");
    int64_t fd = sc(101, W(0), SL_O_WRONLY | SL_O_CREAT | SL_O_TRUNC, 0);
    CHECK(fd >= 3);
    CHECK(sc(1, fd, W(64), 7) == 7 && sc(1, fd, W(128), 6) == 6);
    CHECK(find("/OUT.TXT") && find("/OUT.TXT")->size == 0);                    /* data is buffered until close */
    CHECK(sc(3, fd, 0, 0) == 0);
    CHECK(find("/OUT.TXT") && find("/OUT.TXT")->size == 13 && !memcmp(find("/OUT.TXT")->data, "hello, newlib", 13));
    CHECK(sc(3, fd, 0, 0) == -E_BADF);                                       /* double close */
    CHECK(sc(74, fd, 0, 0) == -E_BADF);                                      /* fsync on a released slot */
    CHECK(sc(8, fd, 0, 0) == -E_BADF);                                       /* lseek on a released slot */

    fd = sc(101, W(0), 0, 0);                                                /* read-only */
    CHECK(fd >= 3);
    CHECK(sc(0, fd, W(256), 5) == 5 && !memcmp(P(256), "hello", 5));
    CHECK(sc(8, fd, 7, 0) == 7 && sc(0, fd, W(256), 100) == 6 && !memcmp(P(256), "newlib", 6));
    CHECK(sc(0, fd, W(256), 100) == 0);                                      /* EOF */
    CHECK(sc(8, fd, (uint64_t)-3, 2) == 10);                                 /* SEEK_END -3 */
    CHECK(sc(1, fd, W(64), 1) == -E_BADF);                                   /* not writable */
    struct sl_stat *st = (struct sl_stat *)P(512);
    CHECK(sc(5, fd, W(512), 0) == 0 && st->size == 13 && (st->mode & 0xF000) == VFS_IFREG);
    CHECK(sc(0, fd, 0x1000, 4) == -E_FAULT);
    CHECK(sc(0, fd, USER_BASE + 16, 4) == -E_FAULT);                         /* read() into read-only text */
    CHECK(sc(3, fd, 0, 0) == 0);

    fd = sc(101, W(0), SL_O_RDWR | SL_O_APPEND, 0);                          /* append + read buffered */
    CHECK(fd >= 3 && sc(1, fd, W(64), 7) == 7);
    CHECK(sc(8, fd, 0, 0) == 0 && sc(0, fd, W(256), 64) == 20 && !memcmp(P(256), "hello, newlibhello, ", 20));
    CHECK(sc(74, fd, 0, 0) == 0 && find("/OUT.TXT")->size == 20);            /* fsync */
    CHECK(sc(3, fd, 0, 0) == 0);

    CHECK(sc(101, W(0), SL_O_CREAT | SL_O_EXCL | SL_O_WRONLY, 0) == -E_EXIST);
    setstr(0, "NOPE.TXT");
    CHECK(sc(101, W(0), 0, 0) == -E_NOENT);
    setstr(0, "/");
    fd = sc(101, W(0), 0, 0); CHECK(fd >= 3 && sc(0, fd, W(256), 4) == -E_ISDIR); sc(3, fd, 0, 0);
    CHECK(sc(101, W(0), SL_O_WRONLY, 0) == -E_ISDIR);
    int64_t open_fds[MAX_FDS]; int n = 0; setstr(0, "OUT.TXT");
    while ((open_fds[n] = sc(101, W(0), 0, 0)) >= 0) n++;
    CHECK(n == MAX_FDS - 3 && open_fds[n] == -E_MFILE);
    for (int i = 0; i < n; i++) sc(3, open_fds[i], 0, 0);

    /* unflushed write is flushed at process exit */
    setstr(0, "LATE.TXT"); fd = sc(101, W(0), SL_O_WRONLY | SL_O_CREAT, 0); sc(1, fd, W(64), 7);
    close_all_fds();
    CHECK(find("/LATE.TXT") && find("/LATE.TXT")->size == 7);

    /* --- path syscalls --- */
    setstr(0, "DIR"); setstr(64, "OUT.TXT"); setstr(128, "MOVED.TXT");
    CHECK(sc(83, W(0), 0, 0) == 0 && sc(83, W(0), 0, 0) == -E_EXIST);
    CHECK(sc(80, W(0), 0, 0) == 0);
    CHECK(sc(79, W(256), 64, 0) == 5 && !strcmp(P(256), "/DIR"));            /* getcwd, cwd is now /DIR */
    CHECK(sc(79, W(256), 2, 0) == -34);
    setstr(0, "/OUT.TXT"); CHECK(sc(82, W(0), W(128), 0) == 0 && find("/DIR/MOVED.TXT") && !find("/OUT.TXT"));
    CHECK(sc(87, W(128), 0, 0) == 0 && sc(87, W(128), 0, 0) == -E_NOENT);
    CHECK(sc(4, W(0), W(512), 0) == -E_NOENT);

    /* --- window bounds: heap region, stack region, no spanning range --- */
    CHECK(user_range_valid(USER_STACK_TOP - 16, 16, 1));                     /* top of the stack is writable */
    CHECK(!user_range_valid(USER_STACK_TOP, 1, 0));                          /* one past the window */
    CHECK(!user_range_valid(USER_BASE + USER_IMAGE_MAX - 4, 8, 1));          /* heap->stack boundary must not span */

    /* --- guest window syscalls: fbmap / present / getevents (M1) --- */
    CHECK(sc(111, 0, 0, 0) == -E_BADF);                                      /* present before any window */
    CHECK(sc(112, W(512), 1, 0) == -E_BADF);                                 /* getevents before any window */
    CHECK(sc(110, 0, 100, 0) == -E_INVAL);                                   /* zero width */
    CHECK(sc(110, 5000, 100, 0) == -E_INVAL);                                /* beyond the 1920x1080 cap */
    CHECK(sc(110, 1917, 100, 0) == -E_INVAL);                                /* fits the cap but not the screen */

    CHECK(sc(110, 320, 200, 0) == (int64_t)USER_FB_BASE);                    /* fbmap -> surface VA */
    int gw, gh; gui_guest_dims(&gw, &gh);
    CHECK(gw == 320 && gh == 200);
    uint64_t fb_span = (uint64_t)fb_page_count * PAGE_SIZE;
    CHECK(fb_span == ((320ull * 200 * 4 + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE);
    CHECK(user_range_valid(USER_FB_BASE, 320 * 200 * 4, 0));                 /* whole surface is mapped */
    CHECK(user_range_valid(USER_FB_BASE, fb_span, 1));                       /* ... and writable */
    CHECK(!user_range_valid(USER_FB_BASE, fb_span + 1, 0));                  /* one byte past the pages */
    CHECK(!user_range_valid(USER_FB_BASE + fb_span, 1, 0));                  /* first unmapped byte */
    CHECK((p2_table1[USER_PT_PAGES] & 0xFFFull) == 0x7ull);                  /* a 4 KiB table is hung here */
    uint64_t fb_pte = user_fb_table[0][0];
    CHECK((fb_pte & 0xFFFull) == 0x7ull);                                    /* present + write + user */
    CHECK((fb_pte & 0x000FFFFFFFFFF000ull) == ((uintptr_t)gui_guest_surface() & 0x000FFFFFFFFFF000ull)); /* points at the shared surface */
    CHECK((fb_pte & (1ull << 63)) != 0);                                     /* NX: pixels, not code */

    CHECK(sc(110, 320, 200, 0) == (int64_t)USER_FB_BASE);                    /* same size again: same VA */
    CHECK(sc(110, 640, 480, 0) == -E_BUSY);                                  /* M1 windows are fixed-size */

    g_present_calls = 0;
    CHECK(sc(111, 0, 0, 0) == 0 && g_present_calls == 1);                    /* present() */
    CHECK(sc(112, W(512), 0, 0) == -E_INVAL);                                /* getevents count 0 */
    CHECK(sc(112, W(512), 65, 0) == -E_INVAL);                               /* getevents count > 64 */
    CHECK(sc(112, 0x1000, 1, 0) == -E_FAULT);                                /* getevents output not writable */

    g_events[0].type = SL_EV_KEY;  g_events[0].x = 0; g_events[0].y = 0; g_events[0].data = 'a';
    g_events[1].type = SL_EV_MOVE; g_events[1].x = 5; g_events[1].y = 6; g_events[1].data = 0;
    g_event_count = 2;
    CHECK(sc(112, W(512), 4, 0) == 2);                                       /* two events delivered */
    struct sl_event *ev = (struct sl_event *)P(512);
    CHECK(ev[0].type == SL_EV_KEY && ev[0].data == 'a');
    CHECK(ev[1].type == SL_EV_MOVE && ev[1].x == 5 && ev[1].y == 6);

    g_event_count = 0; abort_flag = 1; process_exit_pending = 0; process_status = 0;
    CHECK(sc(112, W(512), 1, 50) == -E_INTR &&                              /* Ctrl-C aborts the wait */
          process_exit_pending == 1 && process_status == 130 && !abort_flag);
    process_exit_pending = 0;

    /* --- process teardown unmaps the surface and restores the identity map --- */
    remove_user_pages();
    gui_guest_close();
    CHECK(fb_page_count == 0 && !gui_guest_is_open());
    CHECK(!user_range_valid(USER_FB_BASE, 1, 0));
    CHECK(p2_table1[USER_PT_PAGES] == ((USER_BASE + (uint64_t)USER_PT_PAGES * 2ull * 1024 * 1024) | 0x83ull));

    /* --- application network syscalls: socket/connect/send/recv/poll/resolve (M2) --- */
    net_connect_ret = 0; net_send_ret = -1000; net_sent_len = 0; net_send_calls = 0;
    net_rx = 0; net_rx_len = net_rx_off = 0; net_rx_mode = 0;
    net_poll_mask_out = POLLIN; net_poll_ret = 0;

    /* socket(): only AF_INET/SOCK_STREAM, and it shares the fd table with files */
    int64_t sfd = sc(120, SL_AF_INET, SL_SOCK_STREAM, 0);
    CHECK(sfd >= 3 && fds[sfd].type == FD_SOCK && fds[sfd].u.sock.kfd == -1);
    CHECK(sc(120, 99, SL_SOCK_STREAM, 0) == -E_AFNOSUPPORT);
    CHECK(sc(120, SL_AF_INET, 2, 0) == -E_PROTONOSUPPORT);                       /* only SOCK_STREAM */
    CHECK(sc(120, SL_AF_INET, SL_SOCK_STREAM, 6) == -E_PROTONOSUPPORT);

    /* I/O on an unconnected socket is ENOTCONN; a non-socket fd is EBADF */
    CHECK(sc(122, sfd, W(0), 4) == -E_NOTCONN);
    CHECK(sc(123, sfd, W(0), 4) == -E_NOTCONN);
    CHECK(sc(122, 1, W(0), 4) == -E_BADF);
    CHECK(sc(123, 1, W(0), 4) == -E_BADF);

    /* connect(): validates the sockaddr, records ip/port, wraps it in a kfd */
    struct sl_sockaddr_in *sa = (struct sl_sockaddr_in *)P(1024);
    memset(sa, 0, sizeof *sa);
    sa->sin_family = SL_AF_INET;
    sa->sin_port = (uint16_t)((18001u << 8) | (18001u >> 8));                    /* network order */
    sa->sin_addr[0] = 10; sa->sin_addr[1] = 0; sa->sin_addr[2] = 2; sa->sin_addr[3] = 2;
    CHECK(sc(121, sfd, W(1024), sizeof *sa) == 0);
    CHECK(net_last_port == 18001 && net_last_ip[0] == 10 && net_last_ip[3] == 2);
    CHECK(fds[sfd].u.sock.kfd >= 0 && kfd_tcp_handle(fds[sfd].u.sock.kfd) == 3);
    CHECK(sc(121, sfd, W(1024), sizeof *sa) == -E_ISCONN);                       /* already connected */

    int64_t s2 = sc(120, SL_AF_INET, SL_SOCK_STREAM, 0);
    net_connect_ret = TCP_ERR_REFUSED; CHECK(sc(121, s2, W(1024), sizeof *sa) == -E_CONNREFUSED);
    net_connect_ret = TCP_ERR_TIMEOUT; CHECK(sc(121, s2, W(1024), sizeof *sa) == -E_TIMEDOUT);
    net_connect_ret = TCP_ERR_NOROUTE; CHECK(sc(121, s2, W(1024), sizeof *sa) == -E_NETUNREACH);
    net_connect_ret = 0;
    sa->sin_family = 99; CHECK(sc(121, s2, W(1024), sizeof *sa) == -E_AFNOSUPPORT);
    sa->sin_family = SL_AF_INET;
    CHECK(sc(121, s2, W(1024), 4) == -E_INVAL);                                  /* bad addrlen */
    CHECK(sc(121, s2, 0x1000, sizeof *sa) == -E_FAULT);                          /* addr outside the window */

    /* send(): routes to tcp_send, reports bytes, maps errors */
    setstr(64, "hello");
    CHECK(sc(122, sfd, W(64), 5) == 5 && net_send_calls == 1 && net_sent_len == 5 && !memcmp(net_sent, "hello", 5));
    net_send_ret = TCP_ERR_RESET; CHECK(sc(122, sfd, W(64), 5) == -E_CONNRESET);
    net_send_ret = -1000;

    /* recv(): scripted stream, then EOF; a read timeout is EAGAIN */
    net_rx = "world"; net_rx_len = 5; net_rx_off = 0;
    CHECK(sc(123, sfd, W(256), 16) == 5 && !memcmp(P(256), "world", 5));
    CHECK(sc(123, sfd, W(256), 16) == 0);                                        /* EOF */
    net_rx_mode = 1; CHECK(sc(123, sfd, W(256), 16) == -E_AGAIN); net_rx_mode = 0;
    net_rx_mode = 2; CHECK(sc(123, sfd, W(256), 16) == -E_CONNRESET); net_rx_mode = 0;

    /* read()/write() on a socket behave like recv()/send() */
    net_sent_len = 0; net_send_calls = 0;
    CHECK(sc(1, sfd, W(64), 5) == 5 && net_send_calls == 1);
    net_rx = "abc"; net_rx_len = 3; net_rx_off = 0;
    CHECK(sc(0, sfd, W(256), 16) == 3 && !memcmp(P(256), "abc", 3));

    /* poll(): translates fds to kfds, reflects readiness, passes the timeout through */
    struct sl_pollfd *pf = (struct sl_pollfd *)P(2048);
    pf[0].fd = sfd; pf[0].events = SL_POLLIN | SL_POLLOUT; pf[0].revents = 0;
    net_poll_mask_out = SL_POLLIN;
    CHECK(sc(124, W(2048), 1, 50) == 1 && (pf[0].revents & SL_POLLIN));
    net_poll_mask_out = SL_POLLIN | SL_POLLOUT;
    CHECK(sc(124, W(2048), 1, 0) == 1 && (pf[0].revents & SL_POLLOUT));
    net_poll_mask_out = 0;
    CHECK(sc(124, W(2048), 1, 5000) == 0 && net_poll_last_timeout == 5000);
    pf[0].fd = 1; pf[0].events = SL_POLLOUT;                                    /* stdout is always writable */
    CHECK(sc(124, W(2048), 1, 0) == 1 && (pf[0].revents & SL_POLLOUT));
    pf[0].fd = 0; pf[0].events = SL_POLLIN; net_poll_mask_out = SL_POLLIN;      /* stdin gets a lazy tty kfd */
    CHECK(sc(124, W(2048), 1, 0) == 1 && con_tty_kfd == net_kfd_tty);
    pf[0].fd = -1; CHECK(sc(124, W(2048), 1, 0) == 0);                          /* ignored */
    pf[0].fd = s2; pf[0].events = SL_POLLIN;                                    /* unconnected => POLLHUP */
    CHECK(sc(124, W(2048), 1, 0) == 1 && (pf[0].revents & SL_POLLHUP));
    pf[0].fd = sfd; pf[0].events = SL_POLLIN;
    CHECK(sc(124, W(2048), KPOLL_MAX_FDS + 1, 0) == -E_INVAL);
    CHECK(sc(124, 0x1000, 1, 0) == -E_FAULT);

    /* poll() Ctrl-C propagates and unwinds the process */
    net_poll_ret = KE_INTR; process_exit_pending = 0; process_status = 0;
    CHECK(sc(124, W(2048), 1, -1) == -E_INTR && process_exit_pending == 1 && process_status == 130 && !abort_flag);
    process_exit_pending = 0; net_poll_ret = 0;

    /* resolve(): cached name, literal ip, and failure */
    uint8_t *rip = (uint8_t *)P(3072);
    setstr(0, "host.test"); CHECK(sc(125, W(0), W(3072), 0) == 0 && rip[0] == 1 && rip[1] == 2 && rip[2] == 3 && rip[3] == 4);
    setstr(0, "10.0.2.2");  CHECK(sc(125, W(0), W(3072), 0) == 0 && rip[0] == 10 && rip[3] == 2);
    setstr(0, "nope.test"); CHECK(sc(125, W(0), W(3072), 0) == -E_NOENT);

    /* close() releases the kfd and the fd slot */
    int sk = fds[sfd].u.sock.kfd;
    CHECK(sc(3, sfd, 0, 0) == 0 && fds[sfd].type == FD_NONE && kfd_tcp_handle(sk) == -1);
    CHECK(sc(3, s2, 0, 0) == 0);

    /* --- brk / time / misc --- */
    uint64_t b0 = (uint64_t)sc(12, 0, 0, 0);
    CHECK(b0 == USER_BASE + 0x20000);
    CHECK((uint64_t)sc(12, b0 + 4096, 0, 0) == b0 + 4096);
    CHECK((uint64_t)sc(12, USER_BASE + USER_IMAGE_MAX + 1, 0, 0) == b0 + 4096);   /* refused: break unchanged */
    CHECK((uint64_t)sc(12, USER_BASE + 0x100, 0, 0) == b0 + 4096);                /* below heap start: refused */
    struct sl_timeval *tv = (struct sl_timeval *)P(512);
    CHECK(sc(96, W(512), 0, 0) == 0 && tv->sec == 1790000000 && tv->usec == 456789);
    CHECK(sc(100, 0, 0, 0) == 123456789);
    CHECK(sc(16, 0, 0, 0) == 1 && sc(16, 5, 0, 0) == -E_NOTTY);
    CHECK(sc(39, 0, 0, 0) == 1 && sc(9999, 0, 0, 0) == -E_NOSYS);

    /* --- cooked stdin --- */
    kb_script = "ab\bc\nxyz\r\004"; kb_pos = 0;
    int64_t r = sc(0, 0, W(0), 100);
    CHECK(r == 3 && !memcmp(P(0), "ac\n", 3));                                  /* backspace edits the line */
    CHECK(strstr(termbuf, "ab\b \bc\n") != 0);                                   /* echoed */
    CHECK(sc(0, 0, W(0), 2) == 2 && !memcmp(P(0), "xy", 2));                     /* partial reads keep the rest */
    CHECK(sc(0, 0, W(0), 100) == 2 && !memcmp(P(0), "z\n", 2));
    CHECK(sc(0, 0, W(0), 100) == 0);                                             /* Ctrl-D on empty line = EOF */
    kb_script = "q\004"; kb_pos = 0; CHECK(sc(0, 0, W(0), 100) == 1 && P(0)[0] == 'q');   /* Ctrl-D after text flushes it */
    kb_script = ""; kb_pos = 0; abort_flag = 1; process_exit_pending = 0;
    CHECK(sc(0, 0, W(0), 100) == -E_INTR && process_exit_pending == 1 && process_status == 130 && !abort_flag);
    process_exit_pending = 0;

    /* --- exit --- */
    sc(60, 7, 0, 0);
    CHECK(process_exit_pending == 1 && process_exit_status() == 7);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
