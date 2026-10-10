#include "process.h"
#include "gfx.h"

/* The kernel builds this file for the 64-bit target only; the host test (make proctest)
 * compiles it on any architecture with -DPROCESS_HOST_TEST, so the handful of privileged
 * x86 instructions below are additionally guarded by #ifdef __x86_64__. */
#if defined(__x86_64__) || defined(PROCESS_HOST_TEST)
#include <stddef.h>
#include "vfs.h"
#include "keyboard.h"
#include "terminal.h"
#include "kstring.h"
#include "kheap.h"
#include "ksched.h"
#include "ktime.h"
#include "gui.h"
#include "net.h"
#include "poll.h"
#include "user/slwindow.h"
#include "user/slnet.h"

#define PAGE_SIZE       4096u
/* The kernel's window is fixed at 0x40000000; the host test maps it wherever the OS lets it
 * (macOS forbids MAP_FIXED onto virgin addresses) and sets process_host_base instead. */
#ifdef PROCESS_HOST_TEST
extern uint64_t process_host_base;
#define USER_BASE (process_host_base)
#else
#define USER_BASE       0x40000000ull
#endif
/* user_image lives in the kernel's NOLOAD .heap section; on a host test it is plain .bss. */
#ifdef PROCESS_HOST_TEST
#define SL_IMAGE_SECTION __attribute__((aligned(PAGE_SIZE)))
#else
#define SL_IMAGE_SECTION __attribute__((section(".heap"), aligned(PAGE_SIZE)))
#endif
/* The user window is 64 MiB: [USER_BASE, USER_BASE+USER_WINDOW) holds the ELF image
 * followed by the brk() heap (newlib's malloc), and the top USER_STACK_SIZE bytes are
 * the stack. The window spans USER_PT_PAGES 4 KiB page tables (2 MiB each) hung off
 * p2_table1[0..USER_PT_PAGES-1] while the process runs. */
#define USER_WINDOW     (64ull * 1024ull * 1024ull)
#define USER_STACK_TOP  (USER_BASE + USER_WINDOW)
#define USER_STACK_SIZE (256u * 1024u)
#define USER_IMAGE_MAX  (USER_WINDOW - (uint64_t)USER_STACK_SIZE)
#define USER_IMAGE_PAGES (USER_IMAGE_MAX / PAGE_SIZE)
#define USER_PT_PAGES   (USER_WINDOW / PAGE_SIZE / 512u)
/* Guest-window pixel surface (M1 fbmap): a second VA region directly after the
 * 64 MiB process window, with its own USER_FB_PAGES 4 KiB page tables hung off
 * p2_table1[USER_PT_PAGES..]. USER_FB_MAX is 8 MiB, enough for the full
 * 1920x1080x32bpp surface; only the pages fbmap() actually maps are present. */
#define USER_FB_MAX     (8ull * 1024ull * 1024ull)
#define USER_FB_PAGES   (USER_FB_MAX / (2ull * 1024 * 1024))     /* 4 page tables */
#define USER_FB_BASE    (USER_BASE + USER_WINDOW)
#define ELF_FILE_MAX    (4u * 1024u * 1024u)
#define ELF_PT_LOAD     1u
#define ELF_PF_W        2u
#define MAX_ARGS_BYTES  4096u
#define MAX_FDS         32
#define FILE_BUF_MAX    (1024u * 1024u)

struct elf64_header {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf64_program {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct user_registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error, rip, cs, rflags, rsp, ss;
};

extern uint64_t p4_table[512], p3_table[512], p2_table1[512];
extern uint64_t gdt64_tss_descriptor[2];
extern void process_enter_user(void);
extern void process_return_to_kernel(void);
extern void terminal_putchar(char character);
extern void terminal_write_uint(uint32_t value);
extern gfx_t *terminal_get_render_target(void);
extern void terminal_set_render_target(gfx_t *target);

static struct tss64 tss;
static uint8_t kernel_stack[16384] __attribute__((aligned(16)));
static uint8_t user_image[USER_IMAGE_MAX] SL_IMAGE_SECTION;
static uint8_t user_stack[USER_STACK_SIZE] __attribute__((aligned(PAGE_SIZE)));
static uint8_t elf_file[ELF_FILE_MAX];
static uint64_t user_page_table[USER_PT_PAGES][512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t user_fb_table[USER_FB_PAGES][512] __attribute__((aligned(PAGE_SIZE)));
static uint32_t fb_page_count;                     /* guest-surface pages currently mapped (0 = none) */
static uint8_t user_image_flags[USER_IMAGE_PAGES]; /* P=1, W=2, X=4 */
static uint64_t user_entry;
static uint64_t user_stack_pointer;
static uint32_t user_image_mapped;
static uint32_t user_brk_start, user_brk_cur;      /* offsets into user_image */
static int process_status;                          /* exit status of the last process */
static volatile uint32_t process_active;
volatile uint32_t process_exit_pending;
static void (*previous_keyboard_dispatch)(char);
static gfx_t *previous_terminal_target;

static void discard_user_key(char c) { (void)c; }

/* Entry points consumed by the user-mode interrupt stubs. */
__attribute__((used)) uint64_t process_user_entry;
__attribute__((used)) uint64_t process_user_stack;
__attribute__((used)) uint64_t process_saved_rsp;

static int range_inside(uint64_t off, uint64_t size, uint64_t total)
{
    return off <= total && size <= total - off;
}

static void write_gdt_tss_descriptor(uint64_t base, uint32_t limit)
{
    uint64_t low = (limit & 0xFFFFu) |
                   ((base & 0xFFFFFFu) << 16) |
                   (0x89ull << 40) |
                   (((uint64_t)(limit >> 16) & 0x0Fu) << 48) |
                   (((base >> 24) & 0xFFu) << 56);
    gdt64_tss_descriptor[0] = low;
    gdt64_tss_descriptor[1] = base >> 32;
}

void process_initialize(void)
{
    for (size_t i = 0; i < sizeof(tss); i++) ((uint8_t *)&tss)[i] = 0;
    tss.rsp0 = (uint64_t)(uintptr_t)(kernel_stack + sizeof(kernel_stack));
    tss.iomap_base = sizeof(tss);
    write_gdt_tss_descriptor((uint64_t)(uintptr_t)&tss, sizeof(tss) - 1);
#ifdef __x86_64__
    uint16_t selector = 0x28;
    __asm__ volatile("ltr %0" : : "rm"(selector) : "memory");
#endif
}

static int elf_load(const uint8_t *file, uint32_t length)
{
    if (length < sizeof(struct elf64_header)) return -1;
    const struct elf64_header *eh = (const struct elf64_header *)file;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != 2 || eh->ident[5] != 1 ||
        eh->machine != 62 || eh->type != 2 || eh->phentsize != sizeof(struct elf64_program) ||
        eh->phnum == 0 || !range_inside(eh->phoff,
        (uint64_t)eh->phnum * eh->phentsize, length)) return -1;

    memset(user_image, 0, sizeof(user_image));
    memset(user_stack, 0, sizeof(user_stack));
    memset(user_image_flags, 0, sizeof(user_image_flags));
    user_image_mapped = 0;
    const struct elf64_program *ph = (const struct elf64_program *)(file + eh->phoff);
    for (uint16_t i = 0; i < eh->phnum; i++) {
        if (ph[i].type != ELF_PT_LOAD || ph[i].memsz == 0) continue;
        if (ph[i].vaddr < USER_BASE || ph[i].vaddr - USER_BASE > USER_IMAGE_MAX ||
            ph[i].memsz > USER_IMAGE_MAX - (ph[i].vaddr - USER_BASE) ||
            ph[i].filesz > ph[i].memsz || !range_inside(ph[i].offset, ph[i].filesz, length))
            return -1;
        uint64_t dest = ph[i].vaddr - USER_BASE;
        for (uint64_t b = 0; b < ph[i].filesz; b++) user_image[dest + b] = file[ph[i].offset + b];
        uint64_t end = dest + ph[i].memsz;
        if (end > user_image_mapped) user_image_mapped = (uint32_t)end;
        uint32_t first_page = (uint32_t)(dest / PAGE_SIZE);
        uint32_t last_page = (uint32_t)((end - 1) / PAGE_SIZE);
        for (uint32_t page = first_page; page <= last_page; page++) {
            user_image_flags[page] |= 1u;
            if (ph[i].flags & ELF_PF_W) user_image_flags[page] |= 2u;
            if (ph[i].flags & 1u) user_image_flags[page] |= 4u;
        }
    }
    if (!user_image_mapped || eh->entry < USER_BASE || eh->entry - USER_BASE >= user_image_mapped ||
        !(user_image_flags[(eh->entry - USER_BASE) / PAGE_SIZE] & 4u))
        return -1;
    user_entry = eh->entry;
    user_stack_pointer = USER_STACK_TOP;

    /* Everything between the end of the image and the stack window is the brk() heap:
     * mapped read/write, never executable, zeroed above. */
    uint32_t heap_first = (user_image_mapped + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t page = heap_first; page < USER_IMAGE_PAGES; page++) user_image_flags[page] = 1u | 2u;
    user_brk_start = user_brk_cur = heap_first * PAGE_SIZE;
    return 0;
}

static void setup_user_pages(void)
{
    for (uint32_t t = 0; t < USER_PT_PAGES; t++)
        for (size_t i = 0; i < 512; i++) user_page_table[t][i] = 0;
    uint32_t stack_first = (uint32_t)(USER_WINDOW / PAGE_SIZE) - USER_STACK_SIZE / PAGE_SIZE;
    for (uint32_t i = 0; i < USER_IMAGE_PAGES; i++) {
        if (!(user_image_flags[i] & 1u)) continue;
        uint64_t flags = 0x5; /* present + user; default read-only */
        if (user_image_flags[i] & 2u) flags |= 0x2;
        if (!(user_image_flags[i] & 4u)) flags |= (1ull << 63); /* NX */
        user_page_table[i >> 9][i & 511u] = ((uint64_t)(uintptr_t)(user_image + i * PAGE_SIZE)) | flags;
    }
    for (uint32_t i = 0; i < USER_STACK_SIZE / PAGE_SIZE; i++) {
        uint32_t page = stack_first + i;
        user_page_table[page >> 9][page & 511u] = ((uint64_t)(uintptr_t)(user_stack + i * PAGE_SIZE)) | 0x8000000000000007ull;
    }

    /* Replace the first USER_PT_PAGES entries of the 1-2 GiB identity map with
       4 KiB tables (each covers 2 MiB of the window). Everything else remains
       supervisor-only. */
    p4_table[0] |= 0x4;
    p3_table[1] |= 0x4;
    for (uint32_t t = 0; t < USER_PT_PAGES; t++)
        p2_table1[t] = (uint64_t)(uintptr_t)&user_page_table[t][0] | 0x7;

    /* The guest-surface region starts unmapped; fbmap() installs tables here. */
    fb_page_count = 0;
    for (uint32_t t = 0; t < USER_FB_PAGES; t++) {
        for (size_t i = 0; i < 512; i++) user_fb_table[t][i] = 0;
        p2_table1[USER_PT_PAGES + t] = (USER_BASE + (uint64_t)(USER_PT_PAGES + t) * (2ull * 1024 * 1024)) | 0x83ull;
    }
#ifdef __x86_64__
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
#endif
}

static void remove_user_pages(void)
{
    p4_table[0] &= ~0x4ull;
    p3_table[1] &= ~0x4ull;
    for (uint32_t t = 0; t < USER_PT_PAGES + USER_FB_PAGES; t++)
        p2_table1[t] = (USER_BASE + (uint64_t)t * (2ull * 1024 * 1024)) | 0x83ull;
    fb_page_count = 0;
#ifdef __x86_64__
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
#endif
}

/* Is [address, address+length) memory the process may touch? `write` additionally requires
 * a writable page (image data/bss, heap, stack) so a bad read() cannot scribble on .text. */
static int user_range_valid(uint64_t address, uint64_t length, int write)
{
    if (!length) return 1;
    if (address >= USER_BASE && address - USER_BASE < USER_IMAGE_MAX &&
        length <= USER_IMAGE_MAX - (address - USER_BASE)) {
        uint64_t first = (address - USER_BASE) / PAGE_SIZE;
        uint64_t last = (address - USER_BASE + length - 1) / PAGE_SIZE;
        for (uint64_t page = first; page <= last; page++) {
            uint8_t f = user_image_flags[page];
            if (!(f & 1u)) return 0;
            if (write && !(f & 2u)) return 0;
        }
        return 1;
    }
    if (address >= USER_STACK_TOP - USER_STACK_SIZE && address < USER_STACK_TOP &&
        length <= USER_STACK_TOP - address) return 1;
    /* fbmap()'s guest-window surface: always writable while mapped. */
    if (fb_page_count && address >= USER_FB_BASE) {
        uint64_t off = address - USER_FB_BASE;
        uint64_t span = (uint64_t)fb_page_count * PAGE_SIZE;
        if (off < span && length <= span - off) return 1;
    }
    return 0;
}

static void put_string(const char *s)
{
    while (*s) terminal_putchar(*s++);
}

/* ------------------------------------------------------------------------------------------
 * Initial stack: [rsp] = argc, argv[0..argc-1], NULL, envp = NULL, then the strings.
 * rsp is 8 (mod 16) exactly as if the entry point had been `call`ed, so both the old
 * hand-written C _start() and the newlib crt0 (libc/crt0.S) work.
 * ---------------------------------------------------------------------------------------- */
#define MAX_ARGC 64

static int build_user_stack(int argc, char **argv)
{
    if (argc < 0 || argc > MAX_ARGC) return -1;
    uint64_t base = USER_STACK_TOP - USER_STACK_SIZE;
    uint64_t addrs[MAX_ARGC];
    size_t total = 0;
    for (int i = 0; i < argc; i++) total += strlen(argv[i]) + 1;
    if (total > MAX_ARGS_BYTES) return -1;

    uint64_t sp = USER_STACK_TOP;
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        memcpy(user_stack + (sp - base), argv[i], len);
        addrs[i] = sp;
    }
    sp &= ~15ull;
    sp -= (uint64_t)(argc + 3) * 8;          /* argc, argv[], NULL, envp NULL */
    if ((sp & 15) != 8) sp -= 8;
    uint64_t *w = (uint64_t *)(user_stack + (sp - base));
    w[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) w[1 + i] = addrs[i];
    w[1 + argc] = 0;
    w[2 + argc] = 0;
    user_stack_pointer = sp;
    return 0;
}

/* ------------------------------------------------------------------------------------------
 * File descriptors. fd 0/1/2 are the console. Slots 3+ live in a typed table so that files
 * (today), sockets (when the network syscalls land) and input-event queues (when the window
 * syscalls land) can share one descriptor space: every operation dispatches on e->type, and
 * fd_file()/fd_find_free()/fd_close() are the only places that know the layout.
 *  - FD_FILE: read-only regular files are streamed with vfs_read();
 *    files opened for writing live in a kernel buffer (loaded on open unless O_TRUNC) and are
 *    written back with vfs_write() on close(), fsync() and process exit;
 *    character devices (/dev/null, /dev/urandom, ...) are passed straight through.
 * ---------------------------------------------------------------------------------------- */
enum { SL_O_WRONLY = 1, SL_O_RDWR = 2, SL_O_CREAT = 0x40, SL_O_EXCL = 0x80, SL_O_TRUNC = 0x200, SL_O_APPEND = 0x400 };
enum { E_PERM = 1, E_NOENT = 2, E_IO = 5, E_BADF = 9, E_NOMEM = 12, E_ACCES = 13, E_FAULT = 14, E_BUSY = 16,
       E_EXIST = 17, E_NOTDIR = 20, E_ISDIR = 21, E_INVAL = 22, E_MFILE = 24, E_NOTTY = 25, E_FBIG = 27,
       E_NOSPC = 28, E_SPIPE = 29, E_NOSYS = 38, E_NOTEMPTY = 39, E_LOOP = 40, E_INTR = 4 };
/* network errno (Linux values); tcp_* and kfd results map onto these in ke_to_errno/tcp_to_errno */
enum { E_AGAIN = 11, E_PROTONOSUPPORT = 92, E_AFNOSUPPORT = 97, E_NETUNREACH = 101,
       E_CONNRESET = 104, E_ISCONN = 106, E_NOTCONN = 107, E_TIMEDOUT = 110, E_CONNREFUSED = 111 };
enum { FD_NONE = 0, FD_FILE = 1, FD_SOCK = 2 };   /* future: FD_EVENT joins with the event-queue syscalls */

/* Network waits block with a generous timeout and are Ctrl-C aware: connect()
 * gives up after 10 s, send()/recv() after 30 s (polling is the multiplexing
 * primitive an event loop should use instead). */
#define SL_CONNECT_TIMEOUT_MS 10000u
#define SL_IO_TIMEOUT_MS      30000u

struct ufile {
    uint8_t writable, append, dirty, direct, is_dir;
    uint32_t off, size, cap;
    uint8_t *buf;
    char path[VFS_PATH_MAX];
};

struct fsock { int kfd; };                 /* -1 until connect() wraps an established TCP socket */

struct fdent {
    uint8_t type;                       /* FD_NONE / FD_FILE / FD_SOCK */
    union {
        struct ufile file;
        struct fsock sock;              /* network socket (see user/slnet.h) */
        /* struct fevent ev; */         /* input-event queues, with the event-queue syscalls */
    } u;
};

struct sl_stat { uint32_t mode, size, ino, nlink, uid, gid; };
struct sl_timeval { int64_t sec, usec; };

static struct fdent fds[MAX_FDS];
static char saved_cwd[VFS_PATH_MAX];
static int con_tty_kfd = -1;                /* stdin's kfd once an app poll()s fd 0 (see sys_poll) */

/* console stdin: cooked, line-buffered, with echo and backspace */
static char con_line[256];
static unsigned con_len, con_pos;

static uint64_t neg(int e) { return (uint64_t)(int64_t)-e; }

static int vfs_to_errno(int v)
{
    switch (v) {
    case VFS_ENOENT: return E_NOENT;   case VFS_EIO: return E_IO;
    case VFS_EEXIST: return E_EXIST;   case VFS_ENOSPC: return E_NOSPC;
    case VFS_EISDIR: return E_ISDIR;   case VFS_ENOTDIR: return E_NOTDIR;
    case VFS_ENOTEMPTY: return E_NOTEMPTY; case VFS_EINVAL: return E_INVAL;
    case VFS_EPERM: return E_PERM;     case VFS_ENOSYS: return E_NOSYS;
    case VFS_ELOOP: return E_LOOP;
    default: return v < 0 ? E_IO : 0;
    }
}

static int file_flush(struct ufile *f)
{
    if (!f->writable || f->direct || !f->dirty) return 0;
    int r = vfs_write(f->path, (const char *)f->buf, f->size);
    if (r != 0) return vfs_to_errno(r);
    f->dirty = 0;
    return 0;
}

static int file_close(struct ufile *f)
{
    int e = file_flush(f);
    if (f->buf) kfree(f->buf);
    memset(f, 0, sizeof(*f));
    return e;
}

/* lowest unused slot >= 3, or -1 (fd 0/1/2 are the console) */
static int fd_find_free(void)
{
    for (int i = 3; i < MAX_FDS; i++) if (fds[i].type == FD_NONE) return i;
    return -1;
}

/* the FD_FILE payload of a slot, or 0 for the console and for other/bad fd types */
static struct ufile *fd_file(uint64_t fd)
{
    if (fd < 3 || fd >= MAX_FDS || fds[fd].type != FD_FILE) return 0;
    return &fds[fd].u.file;
}

/* the FD_SOCK payload of a slot, or 0 for other/bad fd types */
static struct fsock *fd_sock(uint64_t fd)
{
    if (fd < 3 || fd >= MAX_FDS || fds[fd].type != FD_SOCK) return 0;
    return &fds[fd].u.sock;
}

/* negative kfd/poll errno -> positive process errno */
static int ke_to_errno(int e)
{
    switch (e) {
    case KE_BADF:  return E_BADF;
    case KE_AGAIN: return E_AGAIN;
    case KE_INTR:  return E_INTR;
    case KE_INVAL: return E_INVAL;
    case KE_NFILE: return E_MFILE;
    case -32:      return E_SPIPE;     /* KE_PIPE (poll.c): the connection is gone */
    default:       return E_IO;
    }
}

/* negative tcp_* result -> positive process errno. recv()'s -1 ("timed out with
 * no data") is special-cased by sock_recv(), because -1 also equals TCP_ERR_TIMEOUT. */
static int tcp_to_errno(int e)
{
    switch (e) {
    case TCP_ERR_TIMEOUT: return E_TIMEDOUT;
    case TCP_ERR_RESET:   return E_CONNRESET;
    case TCP_ERR_REFUSED: return E_CONNREFUSED;
    case TCP_ERR_NOSOCK:  return E_MFILE;
    case TCP_ERR_ABORT:   return E_INTR;
    case TCP_ERR_NOROUTE: return E_NETUNREACH;
    default:              return E_IO;
    }
}

/* A network op failed; if it was Ctrl-C, unwind the process like getevents(). */
static int sock_fail(int e)
{
    if (e == TCP_ERR_ABORT) {
        kabort_clear();
        put_string("^C\n");
        process_status = 130;
        process_exit_pending = 1;
    }
    return tcp_to_errno(e);
}

/* release a slot (flushing files, closing sockets); 0 or positive errno. Slots 0-2 are never touched. */
static int fd_close(int fd)
{
    int e = 0;
    if (fds[fd].type == FD_FILE) e = file_close(&fds[fd].u.file);
    else if (fds[fd].type == FD_SOCK && fds[fd].u.sock.kfd >= 0) {
        int r = kfd_close(fds[fd].u.sock.kfd);
        if (r < 0) e = ke_to_errno(r);
    }
    memset(&fds[fd], 0, sizeof(fds[fd]));      /* type = FD_NONE */
    return e;
}

static void close_all_fds(void)
{
    for (int i = 3; i < MAX_FDS; i++) if (fds[i].type != FD_NONE) fd_close(i);
    if (con_tty_kfd >= 0) { kfd_close(con_tty_kfd); con_tty_kfd = -1; }
}

static int file_reserve(struct ufile *f, uint32_t need)
{
    if (need > FILE_BUF_MAX) return -E_FBIG;
    if (need <= f->cap) return 0;
    uint32_t cap = f->cap ? f->cap : 4096;
    while (cap < need) cap *= 2;
    if (cap > FILE_BUF_MAX) cap = FILE_BUF_MAX;
    uint8_t *nb = (uint8_t *)kmalloc(cap);
    if (!nb) return -E_NOMEM;
    if (f->buf) { memcpy(nb, f->buf, f->size); kfree(f->buf); }
    f->buf = nb;
    f->cap = cap;
    return 0;
}

/* Copies a NUL-terminated path out of user memory and makes it absolute. */
static int user_path(uint64_t addr, char *out)
{
    char raw[VFS_PATH_MAX];
    size_t n = 0;
    for (;;) {
        if (n >= sizeof(raw) - 1 || !user_range_valid(addr + n, 1, 0)) return -E_FAULT;
        char c = *(const char *)(uintptr_t)(addr + n);
        raw[n++] = c;
        if (!c) break;
    }
    if (raw[0] == 0) return -E_NOENT;
    vfs_abspath(raw, out);
    return 0;
}

static int console_getc(void)
{
    for (;;) {
        int c = keyboard_getchar_nonblocking();
        if (c >= 0) return c;
        if (kabort_requested()) return -1;
#ifdef __x86_64__
        __asm__ volatile("sti; hlt; cli" ::: "memory");
#else
        { extern int sched_yield(void); sched_yield(); }
#endif
    }
}

static int64_t console_read(char *dst, uint64_t len)
{
    if (con_pos >= con_len) {
        con_len = con_pos = 0;
        for (;;) {
            int c = console_getc();
            if (c < 0 || kabort_requested()) {
                kabort_clear();
                put_string("^C\n");
                process_status = 130;
                process_exit_pending = 1;
                return -E_INTR;
            }
            if (c == '\r') c = '\n';
            if (c == 3) continue;
            if (c == 4) { if (con_len == 0) return 0; break; }         /* Ctrl-D: EOF */
            if (c == '\b' || c == 127) {
                if (con_len) { con_len--; put_string("\b \b"); }
                continue;
            }
            if (c == '\n') { con_line[con_len++] = '\n'; terminal_putchar('\n'); break; }
            if (c >= 32 && c < 127 && con_len < sizeof(con_line) - 1) { con_line[con_len++] = (char)c; terminal_putchar((char)c); }
        }
    }
    uint64_t n = con_len - con_pos;
    if (n > len) n = len;
    memcpy(dst, con_line + con_pos, n);
    con_pos += (unsigned)n;
    return (int64_t)n;
}

static uint64_t sys_open(uint64_t path_addr, uint64_t flags)
{
    char path[VFS_PATH_MAX];
    int e = user_path(path_addr, path);
    if (e) return neg(-e);
    int slot = fd_find_free();
    if (slot < 0) return neg(E_MFILE);

    int acc = (int)(flags & 3);
    int writable = acc != 0;
    vfs_node_t n;
    int st = vfs_stat(path, &n);
    int created = 0;
    if (st == 0) {
        if ((flags & SL_O_CREAT) && (flags & SL_O_EXCL)) return neg(E_EXIST);
        int want = acc == 0 ? VFS_WANT_R : acc == 1 ? VFS_WANT_W : (VFS_WANT_R | VFS_WANT_W);
        if (vfs_access(path, want) != 0) return neg(E_ACCES);
        if (n.type == VFS_NODE_DIR && writable) return neg(E_ISDIR);
    } else {
        if (!(flags & SL_O_CREAT)) return neg(vfs_to_errno(st));
        int r = vfs_write(path, "", 0);                  /* create now so errors show up at open() */
        if (r != 0) return neg(vfs_to_errno(r));
        memset(&n, 0, sizeof(n));
        n.type = VFS_NODE_FILE;
        created = 1;
    }

    memset(&fds[slot], 0, sizeof(fds[slot]));
    fds[slot].type = FD_FILE;
    struct ufile *f = &fds[slot].u.file;
    f->writable = (uint8_t)writable;
    f->append = (flags & SL_O_APPEND) ? 1 : 0;
    f->is_dir = (n.type == VFS_NODE_DIR);
    f->direct = (n.type == VFS_NODE_CHR);
    strlcpy(f->path, path, sizeof(f->path));

    if (writable && !f->direct) {
        if (n.size > FILE_BUF_MAX) { memset(&fds[slot], 0, sizeof(fds[slot])); return neg(E_FBIG); }
        int rr = file_reserve(f, n.size ? n.size : 1);
        if (rr < 0) { memset(&fds[slot], 0, sizeof(fds[slot])); return neg(-rr); }
        if ((flags & SL_O_TRUNC) && !created) { f->size = 0; f->dirty = 1; }
        else if (!created && n.size) {
            uint32_t got = 0;
            int r = vfs_read_all(path, (char *)f->buf, f->cap, &got);
            if (r != 0) { kfree(f->buf); memset(&fds[slot], 0, sizeof(fds[slot])); return neg(vfs_to_errno(r)); }
            f->size = got;
        }
    } else if (!writable) {
        f->size = n.size;
    }
    f->off = 0;
    return (uint64_t)slot;
}

/* Blocking socket I/O on an FD_SOCK slot: byte count, or -errno. read()/write()
 * fall through to these too, so a socket behaves like a normal fd. */
static int64_t sock_recv(struct fsock *s, void *buf, size_t len)
{
    if (s->kfd < 0) return -E_NOTCONN;
    int h = kfd_tcp_handle(s->kfd);
    if (h < 0) return -E_BADF;
    int n = tcp_recv(h, buf, len, SL_IO_TIMEOUT_MS);
    if (n < 0) return (n == -1) ? -E_AGAIN : -(int64_t)sock_fail(n);   /* -1 = nothing arrived in time */
    return n;                                                          /* >0 data, 0 EOF */
}

static int64_t sock_send(struct fsock *s, const void *buf, size_t len)
{
    if (s->kfd < 0) return -E_NOTCONN;
    int h = kfd_tcp_handle(s->kfd);
    if (h < 0) return -E_BADF;
    int n = tcp_send(h, buf, len);                                     /* blocks until all bytes are queued */
    if (n < 0) return -(int64_t)sock_fail(n);
    return n;
}

static uint64_t sys_read(uint64_t fd, uint64_t addr, uint64_t len)
{
    if (len > 0x7FFFFFFFu) return neg(E_INVAL);
    if (!user_range_valid(addr, len, 1)) return neg(E_FAULT);
    if (!len) return 0;
    char *dst = (char *)(uintptr_t)addr;
    if (fd == 0) return (uint64_t)console_read(dst, len);
    struct fsock *s = fd_sock(fd);
    if (s) return (uint64_t)sock_recv(s, dst, len);
    struct ufile *f = fd_file(fd);
    if (!f) return neg(E_BADF);
    if (f->is_dir) return neg(E_ISDIR);
    if (f->writable && !f->direct) {                      /* buffered file */
        if (f->off >= f->size) return 0;
        uint64_t n = f->size - f->off;
        if (n > len) n = len;
        memcpy(dst, f->buf + f->off, n);
        f->off += (uint32_t)n;
        return n;
    }
    uint64_t total = 0;
    while (total < len) {
        uint32_t want = (uint32_t)((len - total) > 16384 ? 16384 : (len - total)), got = 0;
        int r = vfs_read(f->path, f->off, dst + total, want, &got);
        if (r != 0) return total ? total : neg(vfs_to_errno(r));
        total += got;
        f->off += got;
        if (got < want) break;
        if (f->direct) break;                              /* devices: one chunk per call */
    }
    return total;
}

static uint64_t sys_write(uint64_t fd, uint64_t addr, uint64_t len)
{
    if (len > 0x10000 && (fd == 1 || fd == 2)) len = 0x10000;     /* console: bounded per call, callers loop */
    if (len > 0x7FFFFFFFu) return neg(E_INVAL);
    if (!user_range_valid(addr, len, 0)) return neg(E_FAULT);
    const char *src = (const char *)(uintptr_t)addr;
    if (fd == 1 || fd == 2) {
        for (uint64_t i = 0; i < len; i++) terminal_putchar(src[i]);
        return len;
    }
    struct fsock *s = fd_sock(fd);
    if (s) return (uint64_t)sock_send(s, src, len);
    struct ufile *f = fd_file(fd);
    if (!f) return neg(E_BADF);
    if (!f->writable) return neg(E_BADF);
    if (!len) return 0;
    if (f->direct) {
        int r = vfs_write(f->path, src, (uint32_t)len);
        return r == 0 ? len : neg(vfs_to_errno(r));
    }
    uint64_t pos = f->append ? f->size : f->off;
    uint64_t end = pos + len;
    if (end > FILE_BUF_MAX) return neg(E_FBIG);
    int rr = file_reserve(f, (uint32_t)end);
    if (rr < 0) return neg(-rr);
    if (pos > f->size) memset(f->buf + f->size, 0, pos - f->size);
    memcpy(f->buf + pos, src, len);
    if (end > f->size) f->size = (uint32_t)end;
    f->off = (uint32_t)end;
    f->dirty = 1;
    return len;
}

static uint64_t sys_lseek(uint64_t fd, int64_t off, uint64_t whence)
{
    if (fd < 3) return neg(E_SPIPE);
    struct ufile *f = fd_file(fd);
    if (!f) return neg(E_BADF);
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->off : whence == 2 ? (int64_t)f->size : -1;
    if (base < 0) return neg(E_INVAL);
    int64_t np = base + off;
    if (np < 0 || np > (int64_t)0x7FFFFFFF) return neg(E_INVAL);
    f->off = (uint32_t)np;
    return (uint64_t)np;
}

static void fill_stat(struct sl_stat *o, const vfs_node_t *n, uint32_t size)
{
    o->mode = n->mode; o->size = size; o->ino = n->ino; o->nlink = n->nlink; o->uid = n->uid; o->gid = n->gid;
}

static uint64_t sys_stat(uint64_t path_addr, uint64_t out_addr)
{
    char path[VFS_PATH_MAX];
    int e = user_path(path_addr, path);
    if (e) return neg(-e);
    if (!user_range_valid(out_addr, sizeof(struct sl_stat), 1)) return neg(E_FAULT);
    vfs_node_t n;
    int r = vfs_stat(path, &n);
    if (r != 0) return neg(vfs_to_errno(r));
    fill_stat((struct sl_stat *)(uintptr_t)out_addr, &n, n.size);
    return 0;
}

static uint64_t sys_fstat(uint64_t fd, uint64_t out_addr)
{
    if (!user_range_valid(out_addr, sizeof(struct sl_stat), 1)) return neg(E_FAULT);
    struct sl_stat *o = (struct sl_stat *)(uintptr_t)out_addr;
    if (fd < 3) { memset(o, 0, sizeof(*o)); o->mode = VFS_IFCHR | 0600u; o->nlink = 1; return 0; }
    struct ufile *f = fd_file(fd);
    if (!f) return neg(E_BADF);
    vfs_node_t n;
    int r = vfs_stat(f->path, &n);
    if (r != 0) return neg(vfs_to_errno(r));
    fill_stat(o, &n, f->writable && !f->direct ? f->size : n.size);
    return 0;
}

static uint64_t sys_path1(uint64_t nr, uint64_t a)
{
    char path[VFS_PATH_MAX];
    int e = user_path(a, path);
    if (e) return neg(-e);
    int r;
    switch (nr) {
    case 83: r = vfs_mkdir(path); break;
    case 84: r = vfs_rmdir(path); break;
    case 87: r = vfs_delete(path); break;
    default: r = vfs_cd(path); break;           /* 80: chdir */
    }
    return r == 0 ? 0 : neg(vfs_to_errno(r));
}

static uint64_t sys_rename(uint64_t a, uint64_t b)
{
    char from[VFS_PATH_MAX], to[VFS_PATH_MAX];
    int e = user_path(a, from);
    if (!e) e = user_path(b, to);
    if (e) return neg(-e);
    for (int i = 3; i < MAX_FDS; i++)
        if (fds[i].type == FD_FILE && fds[i].u.file.writable) file_flush(&fds[i].u.file);
    int r = vfs_rename(from, to);
    return r == 0 ? 0 : neg(vfs_to_errno(r));
}

/* ------------------------------------------------------------------------------------------
 * Network syscalls (ABI in user/slnet.h). socket() hands out an FD_SOCK slot; connect()
 * runs the kernel's blocking TCP handshake and wraps the socket in a kfd so poll() can wait
 * on it; send()/recv() block on the TCP stack; resolve() uses the kernel DNS cache/resolver.
 * ---------------------------------------------------------------------------------------- */

static uint16_t ntoh16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

static uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol)
{
    if (domain != SL_AF_INET) return neg(E_AFNOSUPPORT);
    if (type != SL_SOCK_STREAM || protocol != 0) return neg(E_PROTONOSUPPORT);
    int slot = fd_find_free();
    if (slot < 0) return neg(E_MFILE);
    memset(&fds[slot], 0, sizeof(fds[slot]));
    fds[slot].type = FD_SOCK;
    fds[slot].u.sock.kfd = -1;
    return (uint64_t)slot;
}

static uint64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t addrlen)
{
    if (addrlen != sizeof(struct sl_sockaddr_in)) return neg(E_INVAL);
    if (!user_range_valid(addr, sizeof(struct sl_sockaddr_in), 0)) return neg(E_FAULT);
    struct fsock *s = fd_sock(fd);
    if (!s) return neg(E_BADF);
    if (s->kfd >= 0) return neg(E_ISCONN);
    const struct sl_sockaddr_in *a = (const struct sl_sockaddr_in *)(uintptr_t)addr;
    if (a->sin_family != SL_AF_INET) return neg(E_AFNOSUPPORT);

    int h = tcp_connect(a->sin_addr, ntoh16(a->sin_port), SL_CONNECT_TIMEOUT_MS);
    if (h < 0) return neg(sock_fail(h));
    int k = kfd_open_tcp(h);
    if (k < 0) { tcp_abort(h); return neg(ke_to_errno(k)); }
    s->kfd = k;
    return 0;
}

static uint64_t sys_send(uint64_t fd, uint64_t addr, uint64_t len)
{
    if (len > 0x7FFFFFFFu) return neg(E_INVAL);
    if (!user_range_valid(addr, len, 0)) return neg(E_FAULT);
    struct fsock *s = fd_sock(fd);
    if (!s) return neg(E_BADF);
    return (uint64_t)sock_send(s, (const void *)(uintptr_t)addr, (size_t)len);
}

static uint64_t sys_recv(uint64_t fd, uint64_t addr, uint64_t len)
{
    if (len > 0x7FFFFFFFu) return neg(E_INVAL);
    if (!user_range_valid(addr, len, 1)) return neg(E_FAULT);
    struct fsock *s = fd_sock(fd);
    if (!s) return neg(E_BADF);
    return (uint64_t)sock_recv(s, (void *)(uintptr_t)addr, (size_t)len);
}

/* poll(fds, nfds, timeout): translate app fds to kfds, then park in kpoll(), which
 * runs net_poll()/kyield() while it waits (so the NIC, audio and app window stay live).
 * Regular files and stdout report ready immediately; an unconnected socket is POLLHUP. */
static uint64_t sys_poll(uint64_t ufds_addr, uint64_t nfds, int64_t timeout)
{
    if (nfds > KPOLL_MAX_FDS) return neg(E_INVAL);
    if (nfds && !user_range_valid(ufds_addr, nfds * sizeof(struct sl_pollfd), 1)) return neg(E_FAULT);
    if (timeout > 0x7FFFFFFF) timeout = 0x7FFFFFFF;

    struct sl_pollfd *u = (struct sl_pollfd *)(uintptr_t)ufds_addr;
    struct pollfd kp[KPOLL_MAX_FDS];
    int map[KPOLL_MAX_FDS];
    unsigned n = 0;
    int count = 0;

    for (uint64_t i = 0; i < nfds; i++) {
        u[i].revents = 0;
        int fd = u[i].fd;
        if (fd < 0) continue;
        if (fd == 0) {                                              /* console stdin */
            if (con_tty_kfd < 0) con_tty_kfd = kfd_open_tty();
            if (con_tty_kfd < 0) { u[i].revents = SL_POLLNVAL; count++; continue; }
            kp[n].fd = con_tty_kfd;
        } else if (fd == 1 || fd == 2) {                            /* always writable */
            u[i].revents = (short)(u[i].events & SL_POLLOUT); count++; continue;
        } else if (fd >= MAX_FDS) {
            u[i].revents = SL_POLLNVAL; count++; continue;
        } else if (fds[fd].type == FD_FILE) {                       /* files never block */
            u[i].revents = (short)(u[i].events & (SL_POLLIN | SL_POLLOUT)); count++; continue;
        } else if (fds[fd].type == FD_SOCK) {
            if (fds[fd].u.sock.kfd < 0) { u[i].revents = SL_POLLHUP; count++; continue; }
            kp[n].fd = fds[fd].u.sock.kfd;
        } else {
            u[i].revents = SL_POLLNVAL; count++; continue;
        }
        kp[n].events = u[i].events;
        kp[n].revents = 0;
        map[n] = (int)i;
        n++;
    }

    int r = kpoll(kp, n, count ? 0 : (int)timeout);      /* something is already ready: just check */
    if (r < 0) {
        if (r == KE_INTR) {
            kabort_clear();
            put_string("^C\n");
            process_status = 130;
            process_exit_pending = 1;
        }
        return neg(ke_to_errno(r));
    }
    for (unsigned j = 0; j < n; j++)
        if (kp[j].revents) { u[map[j]].revents = kp[j].revents; count++; }
    return (uint64_t)count;
}

static uint64_t sys_resolve(uint64_t host_addr, uint64_t out_addr)
{
    char host[256];
    size_t len = 0;
    for (;;) {
        if (len >= sizeof(host) - 1 || !user_range_valid(host_addr + len, 1, 0)) return neg(E_FAULT);
        char c = *(const char *)(uintptr_t)(host_addr + len);
        host[len++] = c;
        if (!c) break;
    }
    if (!user_range_valid(out_addr, 4, 1)) return neg(E_FAULT);
    uint8_t ip[4];
    if (dns_resolve(host, ip) != 0) return neg(E_NOENT);
    memcpy((void *)(uintptr_t)out_addr, ip, 4);
    return 0;
}

uint64_t process_syscall(void *frame)
{
    struct user_registers *r = (struct user_registers *)frame;
    switch (r->rax) {
    case 0:  return sys_read(r->rdi, r->rsi, r->rdx);
    case 1:  return sys_write(r->rdi, r->rsi, r->rdx);
    case 2: /* legacy: read one raw character from the keyboard (used by MINES.ELF) */
        for (;;) {
            int c = keyboard_getchar_nonblocking();
            if (c >= 0) return (uint64_t)c;
#ifdef __x86_64__
            __asm__ volatile("sti; hlt; cli" ::: "memory");
#else
            { extern int sched_yield(void); sched_yield(); }
#endif
        }
    case 3: { /* close(fd) */
        if (r->rdi < 3) return 0;
        if (r->rdi >= MAX_FDS || fds[r->rdi].type == FD_NONE) return neg(E_BADF);
        int e = fd_close((int)r->rdi);
        return e ? neg(e) : 0;
    }
    case 4:  return sys_stat(r->rdi, r->rsi);
    case 5:  return sys_fstat(r->rdi, r->rsi);
    case 8:  return sys_lseek(r->rdi, (int64_t)r->rsi, r->rdx);
    case 12: { /* brk(addr): 0 queries; returns the (possibly unchanged) break, like Linux */
        if (r->rdi >= USER_BASE) {
            uint64_t off = r->rdi - USER_BASE;
            if (off >= user_brk_start && off <= USER_IMAGE_MAX) user_brk_cur = (uint32_t)off;
        }
        return USER_BASE + user_brk_cur;
    }
    case 16: return r->rdi < 3 ? 1 : neg(E_NOTTY);                 /* isatty(fd) */
    case 39: return 1;                                             /* getpid */
    case 60: /* exit(status) */
        process_status = (int)(r->rdi & 0xFF);
        process_exit_pending = 1;
        return r->rdi;
    case 74: { /* fsync(fd) */
        if (r->rdi < 3) return 0;
        if (r->rdi >= MAX_FDS || fds[r->rdi].type == FD_NONE) return neg(E_BADF);
        if (fds[r->rdi].type != FD_FILE) return 0;          /* nothing to flush */
        int e = file_flush(&fds[r->rdi].u.file);
        return e ? neg(e) : 0;
    }
    case 79: { /* getcwd(buf, size) -> length including NUL */
        const char *cwd = vfs_get_cwd();
        uint64_t need = strlen(cwd) + 1;
        if (need > r->rsi) return neg(34);                         /* ERANGE */
        if (!user_range_valid(r->rdi, need, 1)) return neg(E_FAULT);
        memcpy((void *)(uintptr_t)r->rdi, cwd, need);
        return need;
    }
    case 80: case 83: case 84: case 87: return sys_path1(r->rax, r->rdi);
    case 82: return sys_rename(r->rdi, r->rsi);
    case 96: { /* gettimeofday(struct sl_timeval *) */
        if (!user_range_valid(r->rdi, sizeof(struct sl_timeval), 1)) return neg(E_FAULT);
        struct sl_timeval *tv = (struct sl_timeval *)(uintptr_t)r->rdi;
        tv->sec = ktime_unix();
        tv->usec = (int64_t)(ktime_us() % 1000000ull);
        return 0;
    }
    case 100: return ktime_us();                                   /* microseconds since boot */
    case 101: return sys_open(r->rdi, r->rsi);                     /* open(path, SL_O_* flags) */
    case 110: { /* fbmap(w, h) -> user VA of the window pixel surface (see slwindow.h) */
        uint64_t w = r->rdi, h = r->rsi;
        if (!w || !h || w > 1920 || h > 1080 || w * h * 4ull > USER_FB_MAX) return neg(E_INVAL);

        if (gui_guest_is_open()) {
            int ew, eh;
            gui_guest_dims(&ew, &eh);
            if (w != (uint64_t)ew || h != (uint64_t)eh) return neg(E_BUSY);  /* no resize in M1 */
            return USER_FB_BASE;
        }

        int e = gui_guest_open((int)w, (int)h);
        if (e == -1) return neg(E_INVAL);                          /* would not fit the screen */
        if (e) return neg(E_NOMEM);

        uint64_t bytes = w * h * 4ull;
        uintptr_t surface = (uintptr_t)gui_guest_surface();
        fb_page_count = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
        for (uint64_t i = 0; i < fb_page_count; i++)
            user_fb_table[i >> 9][i & 511u] = (surface + i * PAGE_SIZE) | 0x8000000000000007ull;
        for (uint32_t t = 0; t < (fb_page_count + 511u) / 512u; t++)
            p2_table1[USER_PT_PAGES + t] = (uint64_t)(uintptr_t)&user_fb_table[t][0] | 0x7ull;
#ifdef __x86_64__
        __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
#endif
        return USER_FB_BASE;
    }
    case 111: /* present(): pump input, composite the desktop, show the frame */
        if (!gui_guest_is_open()) return neg(E_BADF);
        return (uint64_t)gui_guest_present();
    case 112: { /* getevents(buf, count, timeout_ms): blocking event read */
        uint64_t count = r->rsi;
        if (!count || count > 64) return neg(E_INVAL);
        if (!user_range_valid(r->rdi, count * sizeof(struct sl_event), 1)) return neg(E_FAULT);
        if (!gui_guest_is_open()) return neg(E_BADF);

        int n = gui_guest_wait((struct sl_event *)(uintptr_t)r->rdi, (int)count, (int32_t)r->rdx);
        if (n < 0) {                                               /* Ctrl-C while waiting */
            kabort_clear();
            put_string("^C\n");
            process_status = 130;
            process_exit_pending = 1;
            return neg(E_INTR);
        }
        return (uint64_t)n;
    }
    case 120: return sys_socket(r->rdi, r->rsi, r->rdx);             /* see user/slnet.h */
    case 121: return sys_connect(r->rdi, r->rsi, r->rdx);
    case 122: return sys_send(r->rdi, r->rsi, r->rdx);
    case 123: return sys_recv(r->rdi, r->rsi, r->rdx);
    case 124: return sys_poll(r->rdi, r->rsi, (int64_t)r->rdx);
    case 125: return sys_resolve(r->rdi, r->rsi);
    default: return neg(E_NOSYS);
    }
}

int process_execute_argv(const char *filename, int argc, char **argv)
{
    if (!filename || process_active) return -1;
    uint32_t file_length = 0;
    if (vfs_read_all(filename, (char *)elf_file, sizeof(elf_file), &file_length) != 0 ||
        file_length > sizeof(elf_file) || elf_load(elf_file, file_length) != 0) return -1;

    char *fallback[1] = { (char *)filename };
    if (argc <= 0 || !argv) { argc = 1; argv = fallback; }
    if (build_user_stack(argc, argv) != 0) return -1;

    memset(fds, 0, sizeof(fds));
    if (con_tty_kfd >= 0) { kfd_close(con_tty_kfd); con_tty_kfd = -1; }
    con_len = con_pos = 0;
    strlcpy(saved_cwd, vfs_get_cwd(), sizeof(saved_cwd));
    kabort_clear();
    process_status = 0;
    process_exit_pending = 0;
    keyboard_flush_queue();
    previous_keyboard_dispatch = keyboard_get_dispatch();
    keyboard_set_dispatch(discard_user_key);
    previous_terminal_target = terminal_get_render_target();
    terminal_set_render_target(0);
    process_active = 1;
    setup_user_pages();
    process_user_entry = user_entry;
    process_user_stack = user_stack_pointer;
    process_enter_user();
    remove_user_pages();
    gui_guest_close();                      /* the app window dies with the process */
    process_active = 0;
    process_exit_pending = 0;
    close_all_fds();                        /* flush anything the program forgot to close */
    vfs_cd(saved_cwd);                       /* a child's chdir() must not move the shell */
    kabort_clear();
    keyboard_set_dispatch(previous_keyboard_dispatch);
    terminal_set_render_target(previous_terminal_target);
    return 0;
}

int process_execute(const char *filename)
{
    return process_execute_argv(filename, 0, 0);
}

int process_exit_status(void)
{
    return process_status;
}

void process_handle_user_fault(uint64_t vector, uint64_t error, uint64_t rip)
{
    (void)error; (void)rip;
    put_string("\nUser process terminated by CPU exception ");
    terminal_write_uint((uint32_t)vector);
    terminal_putchar('\n');
    process_status = 139;
    process_exit_pending = 1;
}

#endif
