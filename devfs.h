#ifndef DEVFS_H
#define DEVFS_H

#include "vfs.h"

/*
 * devfs.h -- the StarlightOS device filesystem.
 *
 * devfs is a tiny synthetic filesystem the VFS mounts at /dev on top of
 * the root ext2 volume (see vfs_initialize()). It stores nothing on
 * disk: every entry is a device node wired straight into a kernel
 * driver, the same way unix wires /dev/null and friends to the kernel:
 *
 *   /dev/null     read() = end of file, write() = discarded
 *   /dev/zero     read() = endless zeroes, write() = discarded
 *   /dev/random   read() = bytes from the kernel entropy pool
 *   /dev/urandom  read() = bytes from the kernel entropy pool
 *   /dev/tty      read() = keyboard input, write() = the console
 *
 * The mount point itself is a real ext2 directory (make_disk.py creates
 * it on fresh images; devfs_init() creates it on older ones, before the
 * mount takes the path over), so `ls /` shows /dev like any other
 * directory. Device nodes are kernel-owned: creating, deleting or
 * renaming inside /dev is refused with VFS_EPERM.
 */

/* Operation vector for vfs_mount("/dev", ...). */
const vfs_ops_t* devfs_get_ops(void);

/* Opaque instance pointer (NULL; devfs keeps no per-mount state). */
void* devfs_get_fs(void);

/* Make sure the /dev mount point exists on the backing filesystem,
 * then mount devfs there. Returns a VFS_* code. */
int devfs_init(void);

#endif
