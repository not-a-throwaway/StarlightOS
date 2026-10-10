/*
 * vfs.c -- the StarlightOS Virtual File System core.
 *
 * The rest of the kernel only ever calls the path-based helpers declared in
 * vfs.h. This file keeps the working directory, normalises relative paths
 * into absolute ones, resolves which mounted filesystem owns a path and
 * forwards the request through that filesystem's vfs_ops_t vector.
 *
 * Since the unix pass the VFS also enforces the permission model: every
 * path is checked for execute on each directory component and for the
 * requested access (read/write/execute) on the leaf, against the current
 * credentials and the mode/uid/gid the filesystem reports. Root (uid 0)
 * bypasses every check, like unix.
 *
 * Two filesystems are compiled in: ext2, mounted at the root, and devfs,
 * the synthetic device filesystem mounted over the /dev directory (plus
 * procfs over /proc). The mount table always picks the most specific
 * match, so paths under /dev reach devfs while everything else stays on
 * ext2 -- the indirection is real:
 * a driver is just a vfs_ops_t table plus an opaque instance pointer, so
 * another filesystem (FAT32, tmpfs, ...) is a new table and a
 * vfs_mount() call.
 */

#include <stdint.h>
#include <stddef.h>

#include "vfs.h"
#include "ext2.h"
#include "devfs.h"
#include "procfs.h"
#include "kstring.h"
#include "ksched.h"

extern void terminal_putchar(char character);

#define VFS_MAX_MOUNTS 8

#define VFS_DEFAULT_UID  1000u
#define VFS_DEFAULT_GID  1000u
#define VFS_DEFAULT_UMASK 022u

static vfs_mount_t s_mounts[VFS_MAX_MOUNTS];
static int         s_mount_n;
static char        s_cwd[VFS_PATH_MAX] = "/";

/* Current credentials and process umask (shared with the filesystems,
 * which apply the umask when creating inodes). */
static uint16_t s_uid = VFS_DEFAULT_UID;
static uint16_t s_gid = VFS_DEFAULT_GID;
static uint16_t s_umask = VFS_DEFAULT_UMASK;

/* --------------------------------------------------------------- mounting */

int vfs_mount(const char* path, const vfs_ops_t* ops, void* fs)
{
    vfs_mount_t* m;
    size_t len;

    if (!ops || s_mount_n >= VFS_MAX_MOUNTS)
        return VFS_ENOSPC;

    if (!path)
        path = "";

    len = strlen(path);
    if (len >= VFS_PATH_MAX)
        return VFS_EINVAL;

    /* Root is stored as "" so the prefix match is simple; otherwise strip
     * any trailing slash. */
    while (len > 1u && path[len - 1u] == '/')
        len--;
    if (len == 1u && path[0] == '/')
        len = 0;

    m = &s_mounts[s_mount_n];
    memcpy(m->path, path, len);
    m->path[len] = '\0';
    m->path_len = len;
    m->ops = ops;
    m->fs = fs;
    m->next = NULL;
    s_mount_n++;

    return VFS_OK;
}

/* Returns the most specific mount whose path prefixes `abs`, and points
 * *rest at the trailing absolute path to hand to the driver. */
static const vfs_mount_t* find_mount(const char* abs, const char** rest)
{
    const vfs_mount_t* best = NULL;
    const char* best_rest = NULL;

    for (int i = 0; i < s_mount_n; i++) {
        const vfs_mount_t* m = &s_mounts[i];

        if (m->path_len == 0u) {
            if (!best) {
                best = m;
                best_rest = abs;
            }
            continue;
        }

        if (strncmp(abs, m->path, m->path_len) == 0 &&
            (abs[m->path_len] == '\0' || abs[m->path_len] == '/')) {
            if (!best || m->path_len > best->path_len) {
                best = m;
                best_rest = abs + m->path_len;
                if (best_rest[0] == '\0')
                    best_rest = "/";
            }
        }
    }

    if (rest)
        *rest = best_rest;
    return best;
}

/* ----------------------------------------------------------- path helpers */

/* Builds a normalised absolute path from `in`, interpreting relative paths
 * against the current working directory. Mirrors the old filesystem semantics:
 * "." and ".." are folded away and repeated slashes collapse. */
static void vfs_build_abs(const char* in, char* out)
{
    char temp[VFS_PATH_MAX * 2u + 2u];
    size_t tlen = 0;
    size_t olen = 1;
    size_t i = 0;

    if (in && in[0] == '/') {
        while (in[i] && tlen < sizeof(temp) - 1u)
            temp[tlen++] = in[i++];
    } else {
        i = 0;
        while (s_cwd[i] && tlen < sizeof(temp) - 1u)
            temp[tlen++] = s_cwd[i++];

        if (tlen > 1u && temp[tlen - 1u] != '/' &&
            tlen < sizeof(temp) - 1u)
            temp[tlen++] = '/';

        i = 0;
        if (in) {
            while (in[i] && tlen < sizeof(temp) - 1u)
                temp[tlen++] = in[i++];
        }
    }
    temp[tlen] = '\0';

    out[0] = '/';
    out[1] = '\0';

    i = 0;
    while (temp[i]) {
        char comp[VFS_NAME_MAX + 1];
        size_t cn = 0;

        while (temp[i] == '/')
            i++;
        if (!temp[i])
            break;

        while (temp[i] && temp[i] != '/' && cn < VFS_NAME_MAX)
            comp[cn++] = temp[i++];

        /* A component longer than VFS_NAME_MAX is truncated (and the rest
         * of it discarded) rather than overflowing the output. */
        while (temp[i] && temp[i] != '/')
            i++;
        comp[cn] = '\0';

        if (strcmp(comp, ".") == 0)
            continue;

        if (strcmp(comp, "..") == 0) {
            if (olen > 1u) {
                while (olen > 1u && out[olen - 1u] != '/')
                    olen--;
                if (olen > 1u)
                    olen--;
                out[olen] = '\0';
            }
            continue;
        }

        if (olen > 1u && out[olen - 1u] != '/') {
            if (olen < VFS_PATH_MAX - 2u)
                out[olen++] = '/';
        }

        for (size_t j = 0; j < cn && olen < VFS_PATH_MAX - 1u; j++)
            out[olen++] = comp[j];
        out[olen] = '\0';
    }

    if (olen == 0u) {
        out[0] = '/';
        olen = 1;
    }
    out[olen] = '\0';
}

/* Low-level stat: no permission checks, resolves the owning mount. */
static int vfs_fs_stat(const char* abs, vfs_node_t* out)
{
    const char* rest;
    const vfs_mount_t* m = find_mount(abs, &rest);

    if (!m)
        return VFS_ENOENT;
    if (!out)
        return VFS_EINVAL;
    return m->ops->stat(m->fs, rest, out);
}

/* ------------------------------------------------------------------ perms */

uint16_t vfs_get_uid(void) { return s_uid; }
uint16_t vfs_get_gid(void) { return s_gid; }
void vfs_set_cred(uint16_t uid, uint16_t gid) { s_uid = uid; s_gid = gid; }
uint16_t vfs_get_umask(void) { return s_umask; }
void vfs_set_umask(uint16_t umask) { s_umask = (uint16_t)(umask & 0777u); }

/* Does node `n` grant `want` (a VFS_WANT_* bitmask) to the current user? */
static int perm_ok(const vfs_node_t* n, int want)
{
    const uint16_t uid = vfs_get_uid();
    const uint16_t gid = vfs_get_gid();
    const uint16_t m = n->mode;

    if (uid == 0)
        return 1;                       /* root bypasses everything */

    if (want & VFS_WANT_R) {
        if (uid == n->uid)      { if (!(m & VFS_IRUSR)) return 0; }
        else if (gid == n->gid) { if (!(m & VFS_IRGRP)) return 0; }
        else                    { if (!(m & VFS_IROTH)) return 0; }
    }
    if (want & VFS_WANT_W) {
        if (uid == n->uid)      { if (!(m & VFS_IWUSR)) return 0; }
        else if (gid == n->gid) { if (!(m & VFS_IWGRP)) return 0; }
        else                    { if (!(m & VFS_IWOTH)) return 0; }
    }
    if (want & VFS_WANT_X) {
        if (uid == n->uid)      { if (!(m & VFS_IXUSR)) return 0; }
        else if (gid == n->gid) { if (!(m & VFS_IXGRP)) return 0; }
        else                    { if (!(m & VFS_IXOTH)) return 0; }
    }
    return 1;
}

/*
 * Central access check for a path. Validates execute permission on every
 * directory component, then handles the leaf:
 *
 *   create == 0  the leaf must exist; `want` is tested on it
 *   create == 1  the leaf may exist (open-for-write): existing leaves get
 *                `want`; missing leaves fall back to W+X on the parent
 *   create == 2  mkdir/symlink semantics: an existing leaf passes (the
 *                filesystem reports EEXIST), a missing one needs W+X on
 *                the parent
 */
static int vfs_check_access(const char* abs, int want, int create)
{
    char copy[VFS_PATH_MAX];
    char prefix[VFS_PATH_MAX];
    char parent[VFS_PATH_MAX];
    char comp[VFS_NAME_MAX + 1];
    vfs_node_t node;
    size_t plen = 0;
    int r;
    char* p;

    strlcpy(copy, abs, sizeof(copy));

    if (strcmp(copy, "/") == 0) {
        r = vfs_fs_stat("/", &node);
        if (r != VFS_OK)
            return r;
        if (node.type != VFS_NODE_DIR)
            return VFS_ENOTDIR;
        if ((want & VFS_WANT_X) && !perm_ok(&node, VFS_WANT_X))
            return VFS_EPERM;
        if ((want & VFS_WANT_W) && !perm_ok(&node, VFS_WANT_W))
            return VFS_EPERM;
        return VFS_OK;
    }

    prefix[0] = '\0';
    p = copy;
    while (*p == '/')
        p++;

    while (*p) {
        size_t cn = 0;
        int is_leaf;

        while (*p && *p != '/' && cn < VFS_NAME_MAX)
            comp[cn++] = *p++;
        comp[cn] = '\0';
        while (*p == '/')
            p++;
        is_leaf = (*p == '\0');

        strlcpy(parent, prefix, sizeof(parent));

        if (plen + cn + 2u >= sizeof(prefix))
            return VFS_EINVAL;
        prefix[plen++] = '/';
        memcpy(prefix + plen, comp, cn);
        plen += cn;
        prefix[plen] = '\0';

        if (!is_leaf) {
            r = vfs_fs_stat(prefix, &node);
            if (r != VFS_OK)
                return r;
            if (node.type != VFS_NODE_DIR)
                return VFS_ENOTDIR;
            if (!perm_ok(&node, VFS_WANT_X))
                return VFS_EPERM;
            continue;
        }

        /* ---- leaf ---- */
        r = vfs_fs_stat(prefix, &node);
        if (r != VFS_OK) {
            if (create == 0)
                return r;

            /* Creation: require W+X on the existing parent directory. */
            if (parent[0] == '\0')
                r = vfs_fs_stat("/", &node);
            else
                r = vfs_fs_stat(parent, &node);
            if (r != VFS_OK)
                return r;
            if (node.type != VFS_NODE_DIR)
                return VFS_ENOTDIR;
            if (!perm_ok(&node, VFS_WANT_W | VFS_WANT_X))
                return VFS_EPERM;
            return VFS_OK;
        }

        /* Leaf exists. mkdir()/symlink() shouldn't pre-empt the
         * filesystem's own EEXIST; open-for-write tests `want`. */
        if (create == 2)
            return VFS_OK;
        if (want && !perm_ok(&node, want))
            return VFS_EPERM;
        return VFS_OK;
    }

    return VFS_OK;
}

/* Sticky-bit protection: may `child` be removed or renamed inside `parent`? */
static int sticky_ok(const vfs_node_t* parent, const vfs_node_t* child)
{
    if (!(parent->mode & VFS_ISVTX))
        return 1;
    if (vfs_get_uid() == 0)
        return 1;
    if (vfs_get_uid() == parent->uid)
        return 1;
    if (vfs_get_uid() == child->uid)
        return 1;
    return 0;
}

/* Splits an absolute path into its parent directory and leaf name. */
static int vfs_split_path(const char* abs, char* parent, size_t parent_cap,
                          char* leaf, size_t leaf_cap)
{
    size_t len, slash, plen;

    if (!abs || !abs[0])
        return -1;
    len = strlen(abs);
    if (len >= VFS_PATH_MAX)
        return -1;

    slash = len;
    while (slash > 0 && abs[slash - 1] != '/')
        slash--;
    if (slash >= len || (len - slash) + 1u > leaf_cap)
        return -1;

    memcpy(leaf, abs + slash, len - slash);
    leaf[len - slash] = '\0';

    if (slash <= 1u) {
        if (parent_cap < 2u)
            return -1;
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        plen = slash - 1u;
        if (plen + 1u > parent_cap)
            return -1;
        memcpy(parent, abs, plen);
        parent[plen] = '\0';
    }
    return 0;
}

/* Checks removing `abs`: execute on all prefixes, W+X on the containing
 * directory, and sticky protection. Fills *leaf_out if given. */
static int vfs_check_remove(const char* abs, vfs_node_t* leaf_out)
{
    char parent[VFS_PATH_MAX];
    char leaf[VFS_NAME_MAX + 1];
    vfs_node_t pn, cn;
    int r;

    if (vfs_split_path(abs, parent, sizeof(parent), leaf, sizeof(leaf)) != 0)
        return VFS_EINVAL;

    if (strcmp(parent, "/") == 0)
        r = vfs_fs_stat("/", &pn);
    else {
        r = vfs_check_access(parent, 0, 0);
        if (r == VFS_OK)
            r = vfs_fs_stat(parent, &pn);
    }
    if (r != VFS_OK)
        return r;
    if (pn.type != VFS_NODE_DIR)
        return VFS_ENOTDIR;
    if (!perm_ok(&pn, VFS_WANT_W | VFS_WANT_X))
        return VFS_EPERM;

    r = vfs_fs_stat(abs, &cn);
    if (r != VFS_OK)
        return r;
    if (!sticky_ok(&pn, &cn))
        return VFS_EPERM;

    if (leaf_out)
        *leaf_out = cn;
    return VFS_OK;
}

/* rename(): remove perms on the source, W+X (and sticky) on the dest side. */
static int vfs_check_rename(const char* abs_from, const char* abs_to)
{
    char parent[VFS_PATH_MAX];
    char leaf[VFS_NAME_MAX + 1];
    vfs_node_t pn, cn;
    int r;

    r = vfs_check_remove(abs_from, NULL);
    if (r != VFS_OK)
        return r;

    if (vfs_split_path(abs_to, parent, sizeof(parent), leaf, sizeof(leaf)) != 0)
        return VFS_EINVAL;

    if (strcmp(parent, "/") == 0)
        r = vfs_fs_stat("/", &pn);
    else {
        r = vfs_check_access(parent, 0, 0);
        if (r == VFS_OK)
            r = vfs_fs_stat(parent, &pn);
    }
    if (r != VFS_OK)
        return r;
    if (pn.type != VFS_NODE_DIR)
        return VFS_ENOTDIR;
    if (!perm_ok(&pn, VFS_WANT_W | VFS_WANT_X))
        return VFS_EPERM;

    /* Sticky directories guard the destination entry too. */
    if (vfs_fs_stat(abs_to, &cn) == VFS_OK && !sticky_ok(&pn, &cn))
        return VFS_EPERM;

    return VFS_OK;
}

/* ---------------------------------------------------------------- queries */

int vfs_exists(const char* path)
{
    char abs[VFS_PATH_MAX];
    vfs_node_t n;

    vfs_build_abs(path, abs);
    return vfs_fs_stat(abs, &n) == VFS_OK;
}

int vfs_is_dir(const char* path)
{
    char abs[VFS_PATH_MAX];
    vfs_node_t n;

    vfs_build_abs(path, abs);
    if (vfs_fs_stat(abs, &n) != VFS_OK)
        return 0;
    return n.type == VFS_NODE_DIR;
}

int32_t vfs_file_size(const char* path)
{
    char abs[VFS_PATH_MAX];
    vfs_node_t n;

    vfs_build_abs(path, abs);
    if (vfs_fs_stat(abs, &n) != VFS_OK)
        return -1;
    if (n.type != VFS_NODE_FILE)
        return -1;
    return (int32_t)n.size;
}

/* Full stat, following symlinks. Requires only traversal permission (unix
 * stat() needs no permission on the file itself). */
int vfs_stat(const char* path, vfs_node_t* out)
{
    char abs[VFS_PATH_MAX];
    int r;

    vfs_build_abs(path, abs);
    r = vfs_check_access(abs, 0, 0);
    if (r != VFS_OK)
        return r;
    return vfs_fs_stat(abs, out);
}

/* Check the current user's access to `path` (VFS_WANT_* bitmask). */
int vfs_access(const char* path, int want)
{
    char abs[VFS_PATH_MAX];

    vfs_build_abs(path, abs);
    return vfs_check_access(abs, want, 0);
}

/* ------------------------------------------------------------- directories */

int vfs_cd(const char* path)
{
    char abs[VFS_PATH_MAX];
    const char* rest;
    const vfs_mount_t* m;
    vfs_node_t n;
    int r;

    vfs_build_abs(path, abs);
    r = vfs_check_access(abs, VFS_WANT_X, 0);
    if (r != VFS_OK)
        return r;

    m = find_mount(abs, &rest);
    if (!m)
        return VFS_ENOENT;

    if (m->ops->stat(m->fs, rest, &n) != VFS_OK || n.type != VFS_NODE_DIR)
        return VFS_ENOENT;

    strlcpy(s_cwd, abs, sizeof(s_cwd));
    return VFS_OK;
}

const char* vfs_get_cwd(void)
{
    return s_cwd;
}

int vfs_get_entries(char out[][VFS_NAME_MAX + 1], uint8_t is_dir[],
                    int max_count)
{
    const char* rest;
    const vfs_mount_t* m;
    int count = 0;

    if (!out || max_count <= 0)
        return 0;

    m = find_mount(s_cwd, &rest);
    if (!m)
        return 0;

    while (count < max_count) {
        char name[VFS_NAME_MAX + 1];
        vfs_node_t n;
        int r = m->ops->readdir(m->fs, rest, (uint32_t)count,
                                name, sizeof(name), &n);

        if (r != VFS_OK)
            break;

        strlcpy(out[count], name, VFS_NAME_MAX + 1);
        if (is_dir)
            is_dir[count] = (uint8_t)(n.type == VFS_NODE_DIR);
        count++;
    }

    return count;
}

int vfs_get_entries_info(char out[][VFS_NAME_MAX + 1], vfs_node_t info[],
                         int max_count)
{
    const char* rest;
    const vfs_mount_t* m;
    int count = 0;

    if (!out || !info || max_count <= 0)
        return 0;

    m = find_mount(s_cwd, &rest);
    if (!m)
        return 0;

    while (count < max_count) {
        char name[VFS_NAME_MAX + 1];
        int r = m->ops->readdir(m->fs, rest, (uint32_t)count,
                                name, sizeof(name), &info[count]);

        if (r != VFS_OK)
            break;

        strlcpy(out[count], name, VFS_NAME_MAX + 1);
        count++;
    }

    return count;
}

int vfs_get_names(char out[][VFS_NAME_MAX + 1], int max_count)
{
    return vfs_get_entries(out, NULL, max_count);
}

int vfs_get_names_current(char out[][VFS_NAME_MAX + 1], int max_count)
{
    return vfs_get_entries(out, NULL, max_count);
}

/* ------------------------------------------------------------------ reading */

int vfs_read_all(const char* path, char* buffer, uint32_t buffer_size,
                 uint32_t* out_length)
{
    char abs[VFS_PATH_MAX];
    const char* rest;
    const vfs_mount_t* m;
    vfs_node_t n;
    uint32_t to_read, got = 0;
    int r;

    vfs_build_abs(path, abs);
    r = vfs_check_access(abs, VFS_WANT_R, 0);
    if (r != VFS_OK)
        return r;

    m = find_mount(abs, &rest);
    if (!m)
        return VFS_ENOENT;

    if (m->ops->stat(m->fs, rest, &n) != VFS_OK)
        return VFS_ENOENT;
    if (n.type == VFS_NODE_DIR)
        return VFS_EISDIR;

    if (out_length)
        *out_length = n.size;

    if (n.size == 0)
        return VFS_OK;

    to_read = n.size;
    if (to_read > buffer_size)
        to_read = buffer_size;

    if (m->ops->read(m->fs, rest, 0, buffer, to_read, &got) != VFS_OK)
        return VFS_EIO;

    return VFS_OK;
}

/* General stream read: forwards to the owning driver without looking at
 * the node's size first, so it works on device nodes (which report size
 * 0 but keep producing data) as well as on regular files. */
int vfs_read(const char* path, uint32_t offset, void* buffer, uint32_t length,
             uint32_t* out_read)
{
    char abs[VFS_PATH_MAX];
    const char* rest;
    const vfs_mount_t* m;
    int r;

    if (out_read)
        *out_read = 0;

    vfs_build_abs(path, abs);
    r = vfs_check_access(abs, VFS_WANT_R, 0);
    if (r != VFS_OK)
        return r;

    m = find_mount(abs, &rest);
    if (!m)
        return VFS_ENOENT;

    return m->ops->read(m->fs, rest, offset, buffer, length, out_read);
}

static int s_cat_last = '\n';
int vfs_cat_last_char(void) { return s_cat_last; }

int vfs_cat(const char* path)
{
    char abs[VFS_PATH_MAX];
    const char* rest;
    const vfs_mount_t* m;
    vfs_node_t n;
    static char buf[4096];
    uint32_t off = 0;
    int r;

    vfs_build_abs(path, abs);
    r = vfs_check_access(abs, VFS_WANT_R, 0);
    if (r != VFS_OK)
        return r;

    m = find_mount(abs, &rest);
    if (!m)
        return VFS_ENOENT;

    if (m->ops->stat(m->fs, rest, &n) != VFS_OK)
        return VFS_ENOENT;
    if (n.type == VFS_NODE_DIR)
        return VFS_EISDIR;

    /*
     * Stream until the driver reports end-of-file rather than reading a
     * size fixed up front. Regular files end at their size, but the
     * device nodes in /dev report size 0 and keep giving data
     * (/dev/zero, /dev/urandom) or wait for input (/dev/tty) -- those
     * never end, and Ctrl-C stops them the way ^C stops a unix cat
     * (the keyboard IRQ raises the abort flag while we sit in here).
     */
    for (;;) {
        uint32_t got = 0;

        if (kabort_requested())
            break;

        if (m->ops->read(m->fs, rest, off, buf, sizeof(buf), &got) != VFS_OK)
            return VFS_EIO;
        if (got == 0)
            break;

        for (uint32_t i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\0')
                c = ' ';
            terminal_putchar(c);
            s_cat_last = (unsigned char)c;
        }

        off += got;
        kyield();
    }

    return VFS_OK;           /* exact bytes, like unix cat: no newline is invented */
}

/* ------------------------------------------------------------------ writing */

int vfs_write(const char* path, const char* data, uint32_t length)
{
    char abs[VFS_PATH_MAX];
    const char* rest;
    const vfs_mount_t* m;
    int r;

    vfs_build_abs(path, abs);
    r = vfs_check_access(abs, VFS_WANT_W, 1);
    if (r != VFS_OK)
        return r;

    m = find_mount(abs, &rest);
    if (!m)
        return VFS_ENOENT;

    return m->ops->write(m->fs, rest, data, length);
}

int vfs_delete(const char* path)
{
    char abs[VFS_PATH_MAX];
    const char* rest;
    const vfs_mount_t* m;
    int r;

    vfs_build_abs(path, abs);
    r = vfs_check_remove(abs, NULL);
    if (r != VFS_OK)
        return r;

    m = find_mount(abs, &rest);
    if (!m)
        return VFS_ENOENT;

    return m->ops->unlink(m->fs, rest);
}

int vfs_rename(const char* old_path, const char* new_path)
{
    char abs_old[VFS_PATH_MAX];
    char abs_new[VFS_PATH_MAX];
    const char* rest_old;
    const char* rest_new;
    const vfs_mount_t* m_old;
    const vfs_mount_t* m_new;
    int r;

    vfs_build_abs(old_path, abs_old);
    vfs_build_abs(new_path, abs_new);
    r = vfs_check_rename(abs_old, abs_new);
    if (r != VFS_OK)
        return r;

    m_old = find_mount(abs_old, &rest_old);
    m_new = find_mount(abs_new, &rest_new);
    if (!m_old || m_new != m_old)
        return VFS_EINVAL;

    return m_old->ops->rename(m_old->fs, rest_old, rest_new);
}

int vfs_mkdir(const char* path)
{
    char abs[VFS_PATH_MAX];
    const char* rest;
    const vfs_mount_t* m;
    int r;

    vfs_build_abs(path, abs);
    r = vfs_check_access(abs, 0, 2);
    if (r != VFS_OK)
        return r;

    m = find_mount(abs, &rest);
    if (!m)
        return VFS_ENOENT;

    return m->ops->mkdir(m->fs, rest);
}

int vfs_rmdir(const char* path)
{
    char abs[VFS_PATH_MAX];
    const char* rest;
    const vfs_mount_t* m;
    int r;

    vfs_build_abs(path, abs);
    r = vfs_check_remove(abs, NULL);
    if (r != VFS_OK)
        return r;

    m = find_mount(abs, &rest);
    if (!m)
        return VFS_ENOENT;

    return m->ops->rmdir(m->fs, rest);
}

/* ------------------------------------------------- symlinks / metadata */

int vfs_symlink(const char* path, const char* target)
{
    char abs[VFS_PATH_MAX];
    const char* rest;
    const vfs_mount_t* m;
    int r;

    vfs_build_abs(path, abs);
    r = vfs_check_access(abs, 0, 2);
    if (r != VFS_OK)
        return r;

    m = find_mount(abs, &rest);
    if (!m)
        return VFS_ENOENT;
    if (!m->ops->symlink)
        return VFS_ENOSYS;

    return m->ops->symlink(m->fs, rest, target);
}

int vfs_readlink(const char* path, char* buf, size_t cap)
{
    char abs[VFS_PATH_MAX];
    const char* rest;
    const vfs_mount_t* m;
    int r;

    if (!buf || cap == 0)
        return VFS_EINVAL;

    vfs_build_abs(path, abs);
    r = vfs_check_access(abs, 0, 0);
    if (r != VFS_OK)
        return r;

    m = find_mount(abs, &rest);
    if (!m)
        return VFS_ENOENT;
    if (!m->ops->readlink)
        return VFS_ENOSYS;

    return m->ops->readlink(m->fs, rest, buf, cap);
}

int vfs_chmod(const char* path, uint16_t mode)
{
    char abs[VFS_PATH_MAX];
    const char* rest;
    const vfs_mount_t* m;
    vfs_node_t n;
    int r;

    vfs_build_abs(path, abs);
    r = vfs_check_access(abs, 0, 0);
    if (r != VFS_OK)
        return r;

    /* Only the owner (or root) may chmod. */
    r = vfs_fs_stat(abs, &n);
    if (r != VFS_OK)
        return r;
    if (vfs_get_uid() != 0 && vfs_get_uid() != n.uid)
        return VFS_EPERM;

    m = find_mount(abs, &rest);
    if (!m)
        return VFS_ENOENT;
    if (!m->ops->chmod)
        return VFS_ENOSYS;

    return m->ops->chmod(m->fs, rest, (uint16_t)(mode & 07777u));
}

int vfs_chown(const char* path, uint16_t uid, uint16_t gid)
{
    char abs[VFS_PATH_MAX];
    const char* rest;
    const vfs_mount_t* m;
    vfs_node_t n;
    int r;

    vfs_build_abs(path, abs);
    r = vfs_check_access(abs, 0, 0);
    if (r != VFS_OK)
        return r;

    /* Only the owner (or root) may change ownership. */
    r = vfs_fs_stat(abs, &n);
    if (r != VFS_OK)
        return r;
    if (vfs_get_uid() != 0 && vfs_get_uid() != n.uid)
        return VFS_EPERM;

    m = find_mount(abs, &rest);
    if (!m)
        return VFS_ENOENT;
    if (!m->ops->chown)
        return VFS_ENOSYS;

    return m->ops->chown(m->fs, rest, uid, gid);
}

int vfs_link(const char* from, const char* to)
{
    char abs_from[VFS_PATH_MAX];
    char abs_to[VFS_PATH_MAX];
    const char* rest_from;
    const char* rest_to;
    const vfs_mount_t* m;
    int r;

    vfs_build_abs(from, abs_from);
    vfs_build_abs(to, abs_to);

    /* Source must exist and be traversable; destination parent W+X. */
    r = vfs_check_access(abs_from, 0, 0);
    if (r != VFS_OK)
        return r;
    r = vfs_check_access(abs_to, 0, 2);
    if (r != VFS_OK)
        return r;

    m = find_mount(abs_from, &rest_from);
    if (!m)
        return VFS_ENOENT;
    if (find_mount(abs_to, &rest_to) != m)
        return VFS_EINVAL;
    if (!m->ops->link)
        return VFS_ENOSYS;

    return m->ops->link(m->fs, rest_from, rest_to);
}

/* -------------------------------------------------------------------- space */

uint32_t vfs_total_bytes(void)
{
    const char* rest;
    const vfs_mount_t* m = find_mount(s_cwd, &rest);
    uint32_t total = 0;

    if (m && m->ops->statfs)
        m->ops->statfs(m->fs, &total, NULL);
    return total;
}

uint32_t vfs_free_bytes(void)
{
    const char* rest;
    const vfs_mount_t* m = find_mount(s_cwd, &rest);
    uint32_t free_bytes = 0;

    if (m && m->ops->statfs)
        m->ops->statfs(m->fs, NULL, &free_bytes);
    return free_bytes;
}

/* -------------------------------------------------------------------- misc */

const char* vfs_strerror(int err)
{
    switch (err) {
    case VFS_OK:        return "success";
    case VFS_ENOENT:    return "no such file or directory";
    case VFS_EIO:       return "I/O error";
    case VFS_EEXIST:    return "already exists";
    case VFS_ENOSPC:    return "no space left on device";
    case VFS_EISDIR:    return "is a directory";
    case VFS_ENOTDIR:   return "not a directory";
    case VFS_ENOTEMPTY: return "directory not empty";
    case VFS_EINVAL:    return "invalid argument";
    case VFS_EPERM:     return "operation not permitted";
    case VFS_ENOSYS:    return "not supported";
    case VFS_ELOOP:     return "too many levels of symbolic links";
    default:            return "unknown error";
    }
}

/* Public helpers for realpath/mount/procfs. */
void vfs_abspath(const char* in, char* out) { vfs_build_abs(in, out); }

int vfs_mount_path(int index, char* out, size_t cap)
{
    if (index < 0 || index >= (int)s_mount_n) return VFS_ENOENT;
    strlcpy(out, s_mounts[index].path[0] ? s_mounts[index].path : "/", cap);
    return VFS_OK;
}

int vfs_initialize(void)
{
    int r;

    r = ext2_init(EXT2_BASE_LBA);
    if (r != VFS_OK)
        return r;

    r = vfs_mount("", ext2_get_ops(), ext2_get_fs());
    if (r != VFS_OK)
        return r;

    /* devfs on top of the root volume: /dev/null, /dev/zero, /dev/random,
     * /dev/urandom and /dev/tty, wired to the kernel drivers. */
    r = devfs_init();
    if (r != VFS_OK)
        return r;

    /* procfs on top of /proc: /proc/version, /proc/uptime, ... */
    r = procfs_init();
    if (r != VFS_OK)
        return r;

    s_cwd[0] = '/';
    s_cwd[1] = '\0';

    return VFS_OK;
}