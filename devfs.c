/*
 * devfs.c -- the StarlightOS device filesystem, mounted at /dev.
 *
 * devfs implements the vfs_ops_t vector declared in vfs.h, so once it is
 * mounted every part of the kernel that talks to the VFS (the shell, the
 * desktop file window, pipelines, redirects, filters like hexdump) sees
 * the device nodes below with real unix semantics -- nothing else has to
 * know that /dev/null is not a file. See devfs.h for the node list.
 *
 * There is no backing store: reads and writes are dispatched to kernel
 * drivers (krand.c for the entropy pool, keyboard.c/terminal for tty),
 * and all filesystem mutations are refused with VFS_EPERM because the
 * nodes are owned by the kernel.
 */

#include <stdint.h>
#include <stddef.h>

#include "devfs.h"
#include "vfs.h"
#include "kstring.h"
#include "krand.h"
#include "keyboard.h"
#include "ksched.h"

extern void terminal_putchar(char character);

/* ------------------------------------------------------------- node table */

enum dev_kind {
    DEV_NULL,
    DEV_ZERO,
    DEV_RANDOM,
    DEV_URANDOM,
    DEV_TTY,
};

/* Inodes are devfs-local (they never reach the disk); the root directory
 * borrows ext2's convention of inode 2. */
#define DEV_ROOT_INO 2u

static const struct {
    const char*   name;
    enum dev_kind kind;
    uint32_t      ino;
} s_nodes[] = {
    { "null",    DEV_NULL,    11 },
    { "zero",    DEV_ZERO,    12 },
    { "random",  DEV_RANDOM,  13 },
    { "urandom", DEV_URANDOM, 14 },
    { "tty",     DEV_TTY,     15 },
};

#define DEV_COUNT ((uint32_t)(sizeof(s_nodes) / sizeof(s_nodes[0])))

/* The VFS hands us absolute paths with the mount point prefix ("/dev")
 * already stripped, so the root is "/" and a node is "/name". */
static int is_root(const char* path)
{
    return path && path[0] == '/' && path[1] == '\0';
}

static int lookup(const char* path)
{
    if (!path || path[0] != '/')
        return -1;
    for (uint32_t i = 0; i < DEV_COUNT; i++)
        if (strcmp(path + 1, s_nodes[i].name) == 0)
            return (int)i;
    return -1;
}

/* ------------------------------------------------------------ vfs_ops_t */

static int dev_op_stat(void* fs, const char* path, vfs_node_t* out)
{
    int i;

    (void)fs;
    if (!out)
        return VFS_EINVAL;

    if (is_root(path)) {
        out->type = VFS_NODE_DIR;
        out->size = 0;
        out->ino = DEV_ROOT_INO;
        out->mode = (uint16_t)(VFS_IFDIR | 0755u);
        out->uid = 0;
        out->gid = 0;
        out->nlink = 2;
        return VFS_OK;
    }

    i = lookup(path);
    if (i < 0)
        return VFS_ENOENT;

    out->type = VFS_NODE_CHR;
    out->size = 0;   /* device nodes have no byte size, like unix */
    out->ino = s_nodes[i].ino;
    /* Kernel-owned (uid 0) but world-readable/writable device nodes. */
    out->mode = (uint16_t)(VFS_IFCHR | 0666u);
    out->uid = 0;
    out->gid = 0;
    out->nlink = 1;
    return VFS_OK;
}

static int dev_op_readdir(void* fs, const char* path, uint32_t index,
                          char* name, size_t name_cap, vfs_node_t* out)
{
    (void)fs;

    if (!is_root(path))
        return VFS_ENOENT;
    if (index >= DEV_COUNT || !name || name_cap == 0)
        return VFS_ENOENT;

    strlcpy(name, s_nodes[index].name, name_cap);
    if (out) {
        out->type = VFS_NODE_CHR;
        out->size = 0;
        out->ino = s_nodes[index].ino;
        out->mode = (uint16_t)(VFS_IFCHR | 0666u);
        out->uid = 0;
        out->gid = 0;
        out->nlink = 1;
    }
    return VFS_OK;
}

static int dev_op_read(void* fs, const char* path, uint32_t offset,
                       void* buf, uint32_t len, uint32_t* out_len)
{
    int i;
    uint8_t* dst = (uint8_t*)buf;

    (void)fs;
    (void)offset;   /* every device is a stream: offsets mean nothing */

    if (out_len)
        *out_len = 0;
    if (is_root(path))
        return VFS_EISDIR;

    i = lookup(path);
    if (i < 0)
        return VFS_ENOENT;
    if (!buf || len == 0)
        return VFS_OK;

    switch (s_nodes[i].kind) {
    case DEV_NULL:
        return VFS_OK;                       /* end of file, immediately */

    case DEV_ZERO:
        memset(dst, 0, len);
        if (out_len)
            *out_len = len;
        return VFS_OK;

    case DEV_RANDOM:
    case DEV_URANDOM:
        /* One pool backs both nodes: it is seeded from RDRAND (when the
         * CPU has it), timer jitter, the wall clock and the NIC MAC and
         * whitened with SHA-256 (krand.c). /dev/random is the same
         * stream here -- this kernel has no separate blocking entropy
         * budget, so unlike linux the two never differ in behaviour. */
        krand_bytes(dst, len);
        if (out_len)
            *out_len = len;
        return VFS_OK;

    case DEV_TTY: {
        uint32_t n = 0;
        int c = -1;

        /* A read on the controlling terminal blocks until a key arrives
         * (unix canonical read), then drains whatever else is already
         * queued so a whole typed line comes back at once. Ctrl-C is
         * raised straight from the keyboard IRQ, so it both ends the
         * wait and reads as end-of-file; the shell clears the flag. */
        while (c < 0) {
            c = keyboard_getchar_nonblocking();
            if (c < 0 && kabort_requested())
                break;
            if (c < 0)
                kidle();
        }
        if (c >= 0 && c != KEY_CTRL_C)
            dst[n++] = (uint8_t)c;

        while (n < len) {
            c = keyboard_getchar_nonblocking();
            if (c < 0 || c == KEY_CTRL_C)
                break;
            dst[n++] = (uint8_t)c;
        }

        if (out_len)
            *out_len = n;
        return VFS_OK;
    }
    }

    return VFS_EINVAL;
}

static int dev_op_write(void* fs, const char* path, const char* data,
                        uint32_t len)
{
    int i;

    (void)fs;

    if (is_root(path))
        return VFS_EISDIR;

    i = lookup(path);
    if (i < 0)
        return VFS_ENOENT;

    if (s_nodes[i].kind == DEV_TTY) {
        /* Writing the terminal: straight to the console, no storage. */
        for (uint32_t k = 0; k < len; k++)
            terminal_putchar(data[k]);
    }
    /* null/zero/random/urandom: every byte is accepted and dropped. */
    return VFS_OK;
}

/* Device nodes are kernel-owned: no creating, deleting or renaming. */
static int dev_op_mkdir(void* fs, const char* path)
{
    (void)fs; (void)path;
    return VFS_EPERM;
}

static int dev_op_unlink(void* fs, const char* path)
{
    (void)fs; (void)path;
    return VFS_EPERM;
}

static int dev_op_rmdir(void* fs, const char* path)
{
    (void)fs; (void)path;
    return VFS_EPERM;
}

static int dev_op_rename(void* fs, const char* from, const char* to)
{
    (void)fs; (void)from; (void)to;
    return VFS_EPERM;
}

static int dev_op_symlink(void* fs, const char* path, const char* target)
{
    (void)fs; (void)path; (void)target;
    return VFS_EPERM;
}

static int dev_op_readlink(void* fs, const char* path, char* buf, size_t cap)
{
    (void)buf; (void)cap;
    if (is_root(path) || lookup(path) >= 0)
        return VFS_EINVAL;
    (void)fs;
    return VFS_ENOENT;
}

static int dev_op_chmod(void* fs, const char* path, uint16_t mode)
{
    (void)fs; (void)path; (void)mode;
    return VFS_EPERM;
}

static int dev_op_chown(void* fs, const char* path, uint16_t uid, uint16_t gid)
{
    (void)fs; (void)path; (void)uid; (void)gid;
    return VFS_EPERM;
}

static int dev_op_link(void* fs, const char* from, const char* to)
{
    (void)fs; (void)from; (void)to;
    return VFS_EPERM;
}

/* statfs is deliberately NULL: /dev consumes no blocks, and the VFS
 * reports 0 bytes for it (df inside /dev shows zeros, not garbage). */

static const vfs_ops_t s_devfs_ops = {
    .stat    = dev_op_stat,
    .readdir = dev_op_readdir,
    .read    = dev_op_read,
    .write   = dev_op_write,
    .mkdir   = dev_op_mkdir,
    .unlink  = dev_op_unlink,
    .rmdir   = dev_op_rmdir,
    .rename  = dev_op_rename,
    .statfs  = NULL,
    .symlink  = dev_op_symlink,
    .readlink = dev_op_readlink,
    .chmod    = dev_op_chmod,
    .chown    = dev_op_chown,
    .link     = dev_op_link,
};

/* -------------------------------------------------------------- public API */

const vfs_ops_t* devfs_get_ops(void)
{
    return &s_devfs_ops;
}

void* devfs_get_fs(void)
{
    return NULL;
}

int devfs_init(void)
{
    /* The mount point is a plain directory on the root filesystem so
     * `ls /` lists it like any other directory. make_disk.py puts it on
     * fresh images; this covers images built before /dev existed. It has
     * to happen before the mount, while /dev still resolves to ext2. */
    if (!vfs_exists("/dev"))
        vfs_mkdir("/dev");

    return vfs_mount("/dev", devfs_get_ops(), devfs_get_fs());
}
