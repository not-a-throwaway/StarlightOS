/*
 * procfs.c -- the StarlightOS /proc filesystem, mounted at /proc.
 *
 * procfs implements the vfs_ops_t vector declared in vfs.h: every file
 * has no backing store and its contents are generated on read from
 * kernel state (version, uptime, memory, hostname) the way unix derives
 * /proc files from the running kernel. See procfs.h for the node list.
 *
 * All mutations are refused with VFS_EPERM because the files are
 * kernel-owned virtual views, and the directory is world-readable.
 */

#include <stdint.h>
#include <stddef.h>

#include "procfs.h"
#include "vfs.h"
#include "kstring.h"
#include "kprintf.h"
#include "kheap.h"
#include "ktime.h"
#include "net.h"
#include "version.h"

/* ------------------------------------------------------------- node table */

#define PROC_ROOT_INO 2u

enum proc_file {
    PROC_VERSION = 0,
    PROC_UPTIME,
    PROC_MEMINFO,
    PROC_HOSTNAME,
    PROC_CPUINFO,
    PROC_MOUNTS,
    PROC_FILESYSTEMS,
};

static const struct {
    const char* name;
    uint32_t    ino;
} s_nodes[] = {
    { "version",  3 },
    { "uptime",   4 },
    { "meminfo",  5 },
    { "hostname", 6 },
    { "cpuinfo",  7 },
    { "mounts",   8 },
    { "filesystems", 9 },
};

#define PROC_COUNT ((uint32_t)(sizeof(s_nodes) / sizeof(s_nodes[0])))

/* The VFS hands us absolute paths with the mount point prefix ("/proc")
 * already stripped, so the root is "/" and a file is "/name". */
static int is_root(const char* path)
{
    return path && path[0] == '/' && path[1] == '\0';
}

static int lookup(const char* path)
{
    if (!path || path[0] != '/')
        return -1;
    for (uint32_t i = 0; i < PROC_COUNT; i++)
        if (strcmp(path + 1, s_nodes[i].name) == 0)
            return (int)i;
    return -1;
}

/* -------------------------------------------------------------- content */

extern int multiboot_get_memory(uint32_t* lower_kb, uint32_t* upper_kb);

/* Renders the current contents of proc file `id`. Returns the byte count,
 * or -1 if it doesn't fit `cap`. */
static int proc_render(int id, char* buf, size_t cap)
{
    size_t total, used;

    switch (id) {
    case PROC_VERSION: {
        int n = ksnprintf(buf, cap, "StarlightOS %s (%s) x86_64\n",
                          STARLIGHTOS_VERSION, STARLIGHTOS_BUILD_NAME);
        return n < 0 ? -1 : n;
    }
    case PROC_UPTIME: {
        int n = ksnprintf(buf, cap, "%u\n", ktime_ms() / 1000u);
        return n < 0 ? -1 : n;
    }
    case PROC_MEMINFO: {
        uint32_t lo = 0, hi = 0;
        if (multiboot_get_memory(&lo, &hi) != 0)
            lo = hi = 0;
        kheap_stats(&total, &used, NULL, NULL);
        {
            int n = ksnprintf(buf, cap,
                              "MemTotal:    %u kB\n"
                              "MemFree:     %u kB\n"
                              "KheapTotal:  %u kB\n"
                              "KheapUsed:   %u kB\n",
                              lo + hi, lo + hi - (uint32_t)(used / 1024u),
                              (unsigned)(total / 1024u),
                              (unsigned)(used / 1024u));
            return n < 0 ? -1 : n;
        }
    }
    case PROC_HOSTNAME: {
        const struct net_config* cfg = net_get_config();
        const char* h = (cfg && cfg->hostname[0]) ? cfg->hostname : "starlight";
        int n = ksnprintf(buf, cap, "%s\n", h);
        return n < 0 ? -1 : n;
    }
    case PROC_CPUINFO: {
        uint32_t r[12], max, vend[4] = {0, 0, 0, 0};
        char brand[49], vendor[13];
        __asm__ volatile("cpuid" : "=a"(vend[0]), "=b"(vend[1]), "=d"(vend[2]), "=c"(vend[3]) : "a"(0));
        memcpy(vendor, &vend[1], 4); memcpy(vendor + 4, &vend[2], 4); memcpy(vendor + 8, &vend[3], 4); vendor[12] = 0;
        __asm__ volatile("cpuid" : "=a"(max) : "a"(0x80000000) : "ebx", "ecx", "edx");
        strlcpy(brand, "unknown x86 CPU", sizeof(brand));
        if (max >= 0x80000004u) {
            for (uint32_t i = 0; i < 3; i++)
                __asm__ volatile("cpuid" : "=a"(r[i * 4]), "=b"(r[i * 4 + 1]), "=c"(r[i * 4 + 2]), "=d"(r[i * 4 + 3]) : "a"(0x80000002u + i));
            memcpy(brand, r, 48); brand[48] = 0;
        }
        const char* b = brand; while (*b == ' ') b++;
        int n = ksnprintf(buf, cap, "processor\t: 0\nvendor_id\t: %s\nmodel name\t: %s\n", vendor, b);
        return n < 0 ? -1 : n;
    }
    case PROC_MOUNTS: {
        size_t o = 0; char p[VFS_PATH_MAX];
        buf[0] = 0;
        for (int i = 0; vfs_mount_path(i, p, sizeof(p)) == VFS_OK; i++) {
            const char* type = !strcmp(p, "/dev") ? "devfs" : !strcmp(p, "/proc") ? "procfs" : "ext2";
            int n = ksnprintf(buf + o, cap - o, "%s %s %s %s 0 0\n", !strcmp(type, "ext2") ? "/dev/hda" : type, p, type,
                              !strcmp(type, "procfs") ? "ro" : "rw");
            if (n < 0 || (size_t)n >= cap - o) return -1;
            o += (size_t)n;
        }
        return (int)o;
    }
    case PROC_FILESYSTEMS: {
        int n = ksnprintf(buf, cap, "\text2\nnodev\tdevfs\nnodev\tprocfs\n");
        return n < 0 ? -1 : n;
    }
    default:
        return -1;
    }
}

/* Fills the requested [offset, offset+len) window with rendered content. */
static int proc_read_content(int id, uint32_t offset, void* buf, uint32_t len,
                             uint32_t* out_len)
{
    static char scratch[512];
    int n = proc_render(id, scratch, sizeof(scratch));

    if (out_len)
        *out_len = 0;
    if (n < 0)
        return VFS_EIO;
    if (offset >= (uint32_t)n)
        return VFS_OK;

    {
        uint32_t take = (uint32_t)n - offset;
        if (take > len)
            take = len;
        if (buf)                       /* proc_op_stat sizes a file by passing buf == NULL */
            memcpy(buf, scratch + offset, take);
        if (out_len)
            *out_len = take;
    }
    return VFS_OK;
}

/* ------------------------------------------------------------ vfs_ops_t */

static int proc_op_stat(void* fs, const char* path, vfs_node_t* out)
{
    int i;

    (void)fs;
    if (!out)
        return VFS_EINVAL;

    if (is_root(path)) {
        out->type = VFS_NODE_DIR;
        out->size = 0;
        out->ino = PROC_ROOT_INO;
        out->mode = (uint16_t)(VFS_IFDIR | 0555u);
        out->uid = 0;
        out->gid = 0;
        out->nlink = 2;
        return VFS_OK;
    }

    i = lookup(path);
    if (i < 0)
        return VFS_ENOENT;

    out->type = VFS_NODE_FILE;
    out->ino = s_nodes[i].ino;
    out->mode = (uint16_t)(VFS_IFREG | 0444u);
    out->uid = 0;
    out->gid = 0;
    out->nlink = 1;
    out->size = 0;

    {
        uint32_t n = 0;
        if (proc_read_content(i, 0, NULL, 0x10000u, &n) == VFS_OK)
            out->size = n;
    }
    return VFS_OK;
}

static int proc_op_readdir(void* fs, const char* path, uint32_t index,
                           char* name, size_t name_cap, vfs_node_t* out)
{
    (void)fs;

    if (!is_root(path))
        return VFS_ENOENT;
    if (index >= PROC_COUNT || !name || name_cap == 0)
        return VFS_ENOENT;

    strlcpy(name, s_nodes[index].name, name_cap);
    if (out) {
        out->type = VFS_NODE_FILE;
        out->size = 0;
        out->ino = s_nodes[index].ino;
        out->mode = (uint16_t)(VFS_IFREG | 0444u);
        out->uid = 0;
        out->gid = 0;
        out->nlink = 1;
    }
    return VFS_OK;
}

static int proc_op_read(void* fs, const char* path, uint32_t offset,
                        void* buf, uint32_t len, uint32_t* out_len)
{
    int i;

    (void)fs;

    if (out_len)
        *out_len = 0;
    if (is_root(path))
        return VFS_EISDIR;

    i = lookup(path);
    if (i < 0)
        return VFS_ENOENT;
    if (!buf || len == 0)
        return VFS_OK;

    return proc_read_content(i, offset, buf, len, out_len);
}

static int proc_op_write(void* fs, const char* path, const char* data,
                         uint32_t len)
{
    (void)fs; (void)path; (void)data; (void)len;
    return VFS_EPERM;
}

/* Proc files are kernel-owned views: no creating, deleting or renaming. */
static int proc_op_mkdir(void* fs, const char* path)
{
    (void)fs; (void)path;
    return VFS_EPERM;
}

static int proc_op_unlink(void* fs, const char* path)
{
    (void)fs; (void)path;
    return VFS_EPERM;
}

static int proc_op_rmdir(void* fs, const char* path)
{
    (void)fs; (void)path;
    return VFS_EPERM;
}

static int proc_op_rename(void* fs, const char* from, const char* to)
{
    (void)fs; (void)from; (void)to;
    return VFS_EPERM;
}

static int proc_op_symlink(void* fs, const char* path, const char* target)
{
    (void)fs; (void)path; (void)target;
    return VFS_EPERM;
}

static int proc_op_readlink(void* fs, const char* path, char* buf, size_t cap)
{
    (void)buf; (void)cap;
    if (is_root(path) || lookup(path) >= 0)
        return VFS_EINVAL;
    (void)fs;
    return VFS_ENOENT;
}

static int proc_op_chmod(void* fs, const char* path, uint16_t mode)
{
    (void)fs; (void)path; (void)mode;
    return VFS_EPERM;
}

static int proc_op_chown(void* fs, const char* path, uint16_t uid, uint16_t gid)
{
    (void)fs; (void)path; (void)uid; (void)gid;
    return VFS_EPERM;
}

static int proc_op_link(void* fs, const char* from, const char* to)
{
    (void)fs; (void)from; (void)to;
    return VFS_EPERM;
}

/* statfs is NULL: /proc consumes no blocks, like unix. */

static const vfs_ops_t s_procfs_ops = {
    .stat     = proc_op_stat,
    .readdir  = proc_op_readdir,
    .read     = proc_op_read,
    .write    = proc_op_write,
    .mkdir    = proc_op_mkdir,
    .unlink   = proc_op_unlink,
    .rmdir    = proc_op_rmdir,
    .rename   = proc_op_rename,
    .statfs   = NULL,
    .symlink  = proc_op_symlink,
    .readlink = proc_op_readlink,
    .chmod    = proc_op_chmod,
    .chown    = proc_op_chown,
    .link     = proc_op_link,
};

/* -------------------------------------------------------------- public API */

const vfs_ops_t* procfs_get_ops(void)
{
    return &s_procfs_ops;
}

void* procfs_get_fs(void)
{
    return NULL;
}

int procfs_init(void)
{
    /* The mount point is a plain directory on the root filesystem so
     * `ls /` lists it like any other directory. make_disk.py puts it on
     * fresh images; this covers older ones. It has to happen before the
     * mount, while /proc still resolves to ext2. */
    if (!vfs_exists("/proc"))
        vfs_mkdir("/proc");

    return vfs_mount("/proc", procfs_get_ops(), procfs_get_fs());
}