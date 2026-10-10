#ifndef PROCFS_H
#define PROCFS_H

#include "vfs.h"

/*
 * procfs.h -- the StarlightOS /proc filesystem.
 *
 * Like devfs, a synthetic filesystem with no backing store. It is
 * mounted over the /proc directory on the root volume and its files are
 * generated on read from kernel state:
 *
 *   /proc/version     kernel version + build strings
 *   /proc/uptime      seconds since boot
 *   /proc/meminfo     system and kernel-heap memory
 *   /proc/hostname    the machine's hostname
 */

const vfs_ops_t* procfs_get_ops(void);
void*            procfs_get_fs(void);

/* Make sure the /proc mount point exists on the backing filesystem,
 * then mount procfs there. Returns a VFS_* code. */
int procfs_init(void);

#endif