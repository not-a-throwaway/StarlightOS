#ifndef VFS_H
#define VFS_H

/*
 * vfs.h -- the StarlightOS Virtual File System switch.
 *
 * The rest of the kernel no longer talks to a specific filesystem. It
 * calls the path-based helpers here (vfs_read_all, vfs_cd, ...) and the
 * VFS core resolves the path against the mount table and forwards the
 * request to the mounted filesystem through its vfs_ops_t vector.
 *
 * Only two filesystems are compiled in today: ext2 on the root and the
 * synthetic devfs mounted at /dev, but the indirection is real: a driver
 * is just a vfs_ops_t table plus an opaque instance pointer, so another
 * filesystem (FAT32, tmpfs, ...) is a new table and a vfs_mount() call.
 */

#include <stdint.h>
#include <stddef.h>

/* Longest filename the listing helpers will hand back. ext2 itself
 * allows 255; 63 is plenty for the shell/desktop and keeps the static
 * name tables small. Paths passed in may still be longer than this
 * only if a component exceeds it, in which case lookups fail cleanly. */
#define VFS_NAME_MAX 63

/* Longest absolute path the VFS will normalise. */
#define VFS_PATH_MAX 256

/* Return codes. Drivers speak these too. */
enum {
    VFS_OK        =  0,
    VFS_ENOENT    = -1,   /* no such file or directory          */
    VFS_EIO       = -2,   /* low-level I/O error                */
    VFS_EEXIST    = -3,   /* already exists                     */
    VFS_ENOSPC    = -4,   /* no space left on the filesystem    */
    VFS_EISDIR    = -5,   /* is a directory                     */
    VFS_ENOTDIR   = -6,   /* a path component is not a dir      */
    VFS_ENOTEMPTY = -7,   /* directory not empty                */
    VFS_EINVAL    = -8,   /* invalid argument / path            */
    VFS_EPERM     = -9,   /* operation not permitted            */
    VFS_ENOSYS    = -10,  /* operation not supported here       */
    VFS_ELOOP     = -11,  /* too many levels of symbolic links  */
};

enum {
    VFS_NODE_FILE = 1,
    VFS_NODE_DIR  = 2,
    VFS_NODE_LNK  = 3,   /* symbolic link (reported by readdir) */
    VFS_NODE_CHR  = 4,   /* character device /dev entry        */
};

/* Unix-style file type/mode bits, stored in vfs_node_t.mode as the raw
 * ext2 mode word (lower 12 bits are the 07777 permission/sticky bits). */
#define VFS_IFMT 0xF000u
#define VFS_IFREG 0x8000u
#define VFS_IFDIR 0x4000u
#define VFS_IFCHR 0x2000u
#define VFS_IFLNK 0xA000u
#define VFS_ISVTX  0x0200u   /* sticky bit (part of the low 12 bits) */

/* Permission bits, in the standard Unix/ext2 positions (S_IRUSR = 0o0400). */
#define VFS_IRUSR  0x0100u
#define VFS_IWUSR  0x0080u
#define VFS_IXUSR  0x0040u
#define VFS_IRGRP  0x0020u
#define VFS_IWGRP  0x0010u
#define VFS_IXGRP  0x0008u
#define VFS_IROTH  0x0004u
#define VFS_IWOTH  0x0002u
#define VFS_IXOTH  0x0001u

/* Access checks (vfs_access): which permission to test on the leaf. */
enum { VFS_WANT_R = 1, VFS_WANT_W = 2, VFS_WANT_X = 4 };

typedef struct {
    int      type;    /* VFS_NODE_FILE / DIR / LNK / CHR */
    uint32_t size;    /* bytes (for a symlink: target length) */
    uint32_t ino;     /* filesystem inode number */
    uint16_t mode;    /* full ext2-style mode incl. type bits */
    uint16_t uid;
    uint16_t gid;
    uint32_t nlink;
} vfs_node_t;

/*
 * Driver operation vector. `fs` is the opaque instance pointer passed to
 * vfs_mount(). Paths are always absolute and begin with '/'.
 */
typedef struct vfs_ops {
    /* Fill `out` for `path`. Returns VFS_OK or a negative code. */
    int (*stat)(void* fs, const char* path, vfs_node_t* out);

    /* Enumerate `path`: index 0,1,2,... until VFS_ENOENT. "." and ".."
     * are not reported. `name` is NUL-terminated and bounded by cap. */
    int (*readdir)(void* fs, const char* path, uint32_t index,
                   char* name, size_t name_cap, vfs_node_t* out);

    /* Read up to `len` bytes at `offset`; *out_len gets the count. */
    int (*read)(void* fs, const char* path, uint32_t offset,
                void* buf, uint32_t len, uint32_t* out_len);

    /* Create or replace `path` with exactly `len` bytes. */
    int (*write)(void* fs, const char* path, const char* data, uint32_t len);

    int (*mkdir)(void* fs, const char* path);
    int (*unlink)(void* fs, const char* path);   /* regular file */
    int (*rmdir)(void* fs, const char* path);
    int (*rename)(void* fs, const char* from, const char* to);

    /* Total and free bytes. */
    int (*statfs)(void* fs, uint32_t* total_bytes, uint32_t* free_bytes);

    /* Create a symlink `path` -> `target`. target is the literal string. */
    int (*symlink)(void* fs, const char* path, const char* target);

    /* Read the target of symlink `path` into buf; returns target length. */
    int (*readlink)(void* fs, const char* path, char* buf, size_t cap);

    /* Change permission bits (type bits in mode are ignored) or owner. */
    int (*chmod)(void* fs, const char* path, uint16_t mode);
    int (*chown)(void* fs, const char* path, uint16_t uid, uint16_t gid);

    /* Create a hard link `from` -> `to`. */
    int (*link)(void* fs, const char* from, const char* to);
} vfs_ops_t;

typedef struct vfs_mount {
    char              path[VFS_PATH_MAX];
    size_t            path_len;
    const vfs_ops_t*  ops;
    void*             fs;
    struct vfs_mount* next;
} vfs_mount_t;

/* ---- core ---- */
int         vfs_initialize(void);
int         vfs_mount(const char* path, const vfs_ops_t* ops, void* fs);

/* ---- working directory ---- */
int         vfs_cd(const char* path);
const char* vfs_get_cwd(void);

/* ---- queries ---- */
int         vfs_exists(const char* path);
int         vfs_is_dir(const char* path);
int32_t     vfs_file_size(const char* path);
int         vfs_stat(const char* path, vfs_node_t* out);
int         vfs_access(const char* path, int want);   /* VFS_WANT_* bitmask */

/* ---- symlinks / metadata ---- */
int         vfs_symlink(const char* path, const char* target);
int         vfs_readlink(const char* path, char* buf, size_t cap);
int         vfs_chmod(const char* path, uint16_t mode);
int         vfs_chown(const char* path, uint16_t uid, uint16_t gid);
int         vfs_link(const char* from, const char* to);

/* ---- paths / mounts ---- */
void        vfs_abspath(const char* in, char* out);                 /* absolute, normalised ('.', '..' resolved) */
int         vfs_mount_path(int index, char* out, size_t cap);     /* mount point of mount #index; VFS_ENOENT past the end */

/* ---- credentials / umask ---- */
uint16_t    vfs_get_uid(void);
uint16_t    vfs_get_gid(void);
void        vfs_set_cred(uint16_t uid, uint16_t gid);
uint16_t    vfs_get_umask(void);
void        vfs_set_umask(uint16_t umask);

/* ---- reading ---- */
int         vfs_read_all(const char* path, char* buffer, uint32_t buffer_size,
                         uint32_t* out_length);

/* Stream read: up to `length` bytes at `offset`, *out_read gets the count
 * (0 means end of file). Unlike vfs_read_all this does not look at the
 * node's size first, so it also works on devices (/dev/zero, /dev/urandom,
 * /dev/tty), which report size 0 but never really run out of data. */
int         vfs_read(const char* path, uint32_t offset, void* buffer,
                     uint32_t length, uint32_t* out_read);
int         vfs_cat(const char* path);
int         vfs_cat_last_char(void);   /* last byte vfs_cat() wrote ('\n' if none) */

/* ---- writing ---- */
int         vfs_write(const char* path, const char* data, uint32_t length);
int         vfs_delete(const char* path);
int         vfs_rename(const char* old_path, const char* new_path);
int         vfs_mkdir(const char* path);
int         vfs_rmdir(const char* path);

/* ---- listing (current directory) ---- */
int         vfs_get_entries(char out[][VFS_NAME_MAX + 1], uint8_t is_dir[],
                            int max_count);
int         vfs_get_names(char out[][VFS_NAME_MAX + 1], int max_count);
int         vfs_get_names_current(char out[][VFS_NAME_MAX + 1], int max_count);

/* Listing with full node info (for `ls -l`): fills `info[i]` alongside
 * the names. Returns the number of entries. */
int         vfs_get_entries_info(char out[][VFS_NAME_MAX + 1],
                                vfs_node_t info[], int max_count);

/* ---- space ---- */
uint32_t    vfs_total_bytes(void);
uint32_t    vfs_free_bytes(void);

/* Human-readable message for a VFS_* code (points at a static string). */
const char* vfs_strerror(int err);

#endif
