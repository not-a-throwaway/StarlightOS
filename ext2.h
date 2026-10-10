#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>
#include "vfs.h"

/*
 * ext2 driver.
 *
 * The filesystem lives at a fixed byte offset on the ATA disk: the first
 * 1024 sectors are reserved for the BIOS boot sector, stage2 and the
 * kernel (see make_disk.py), and the ext2 volume starts right after, so
 * the conventional superblock-at-offset-1024 lands in sector 1026 and
 * never collides with the boot area.
 */
#define EXT2_BASE_LBA 1024

/* Probe and load the filesystem starting at `base_lba`.
 * Returns VFS_OK on success or a negative VFS_* code. */
int ext2_init(uint32_t base_lba);

/* Operation vector to hand to vfs_mount(). */
const vfs_ops_t* ext2_get_ops(void);

/* Opaque instance pointer (NULL is fine; ext2 keeps its own state). */
void* ext2_get_fs(void);

#endif
