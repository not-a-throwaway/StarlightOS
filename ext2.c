/*
 * ext2.c -- a small, read/write ext2 filesystem driver for StarlightOS.
 *
 * This is the only filesystem driver compiled in today. It implements the
 * vfs_ops_t vector (see vfs.h) and is mounted at the VFS root by
 * vfs_initialize(). The on-disk layout it expects matches make_disk.py:
 * 1024 reserved sectors at the front of the ATA disk for the boot sector,
 * stage2 and the kernel, then the ext2 volume. 4 KiB blocks, one block
 * group, 128-byte inodes.
 *
 * Design notes:
 *  - All I/O goes through disk_read()/disk_write() (raw ATA PIO).
 *  - Everything is parsed/serialised by hand (little-endian helpers
 *    below); no packed structs, so it stays portable and warning-clean.
 *  - The superblock, group descriptor table, block/inode bitmaps and the
 *    inode currently being edited are cached in static buffers. Public
 *    mutating operations call ext2_fs_flush() at the end so the metadata
 *    on disk stays consistent across reboots.
 *  - Block mapping supports direct, single and double indirect blocks;
 *    that covers every file the kernel can currently create.
 */

#include <stdint.h>
#include <stddef.h>

#include "ext2.h"
#include "disk.h"
#include "kstring.h"

#define EXT2_SUPER_MAGIC      0xEF53u
#define EXT2_ROOT_INO         2u

#define EXT2_NDIR_BLOCKS      12u
#define EXT2_IND_BLOCK        12u
#define EXT2_DIND_BLOCK       13u
#define EXT2_TIND_BLOCK       14u
#define EXT2_N_BLOCKS         15u

#define EXT2_S_IFMT           0xF000u
#define EXT2_S_IFREG          0x8000u
#define EXT2_S_IFDIR          0x4000u
#define EXT2_S_IFLNK          0xA000u

#define EXT2_FT_REG_FILE      1u
#define EXT2_FT_DIR           2u
#define EXT2_FT_SYMLINK       7u

#define EXT2_MAX_BLOCK_SIZE   4096u
#define EXT2_MAX_GROUPS       128u
#define EXT2_INODE_MAX        256u

#define EXT2_INVALID_BLOCK    0xFFFFFFFFu

/* ------------------------------------------------------------------ state */

static uint32_t s_base_lba;

static uint8_t  s_sb[1024];
static uint8_t  s_gdt[EXT2_MAX_GROUPS * 32];
static uint8_t  s_bb[EXT2_MAX_BLOCK_SIZE];       /* cached block bitmap   */
static uint8_t  s_ib[EXT2_MAX_BLOCK_SIZE];       /* cached inode bitmap   */
static uint8_t  s_inode[EXT2_INODE_MAX];
static uint8_t  s_iobuf[EXT2_MAX_BLOCK_SIZE];    /* inode-table block     */
static uint8_t  s_dir[EXT2_MAX_BLOCK_SIZE];      /* directory data block  */
static uint8_t  s_data[EXT2_MAX_BLOCK_SIZE];     /* file data block       */
static uint8_t  s_ind[EXT2_MAX_BLOCK_SIZE];      /* indirect scratch      */
static uint8_t  s_ind2[EXT2_MAX_BLOCK_SIZE];

static uint32_t s_block_size;
static uint32_t s_sectors_per_block;
static uint32_t s_blocks_count;
static uint32_t s_inodes_count;
static uint32_t s_blocks_per_group;
static uint32_t s_inodes_per_group;
static uint32_t s_first_data_block;
static uint32_t s_inode_size;
static uint32_t s_first_ino;
static uint32_t s_groups;
static uint32_t s_free_blocks;
static uint32_t s_free_inodes;

static uint32_t s_bb_group = EXT2_INVALID_BLOCK;  /* group cached in s_bb  */
static uint32_t s_ib_group = EXT2_INVALID_BLOCK;
static int      s_bb_dirty;
static int      s_ib_dirty;
static int      s_gdt_dirty;

/* --------------------------------------------------------- little-endian */

static uint16_t rd16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wr32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t align4(uint32_t n)
{
    return (n + 3u) & ~3u;
}

/* ------------------------------------------------------------- block I/O */

static int blk_read(uint32_t block, void* buf)
{
    return disk_read(s_base_lba + block * s_sectors_per_block,
                     (uint8_t)s_sectors_per_block, buf);
}

static int blk_write(uint32_t block, const void* buf)
{
    return disk_write(s_base_lba + block * s_sectors_per_block,
                      (uint8_t)s_sectors_per_block, buf);
}

/* ------------------------------------------------------ superblock / GDT */

static int sb_write(void)
{
    /* The superblock is always at byte offset 1024 of the volume. */
    return disk_write(s_base_lba + 2, 2, s_sb);
}

static int gdt_flush(void)
{
    uint32_t bytes = s_groups * 32u;
    uint32_t sectors = (bytes + 511u) / 512u;
    return disk_write(s_base_lba +
                          (s_first_data_block + 1u) * s_sectors_per_block,
                      (uint8_t)sectors, s_gdt);
}

static int gdt_load(void)
{
    uint32_t bytes = s_groups * 32u;
    uint32_t sectors = (bytes + 511u) / 512u;
    return disk_read(s_base_lba +
                         (s_first_data_block + 1u) * s_sectors_per_block,
                     (uint8_t)sectors, s_gdt);
}

/* ------------------------------------------------------------- bitmaps */

static uint32_t grp_block_bitmap(uint32_t g)
{
    return rd32(s_gdt + g * 32u + 0u);
}

static uint32_t grp_inode_bitmap(uint32_t g)
{
    return rd32(s_gdt + g * 32u + 4u);
}

static int bbitmap_flush(void)
{
    if (!s_bb_dirty || s_bb_group == EXT2_INVALID_BLOCK)
        return 0;
    if (blk_write(grp_block_bitmap(s_bb_group), s_bb) != 0)
        return -1;
    s_bb_dirty = 0;
    return 0;
}

static int ibitmap_flush(void)
{
    if (!s_ib_dirty || s_ib_group == EXT2_INVALID_BLOCK)
        return 0;
    if (blk_write(grp_inode_bitmap(s_ib_group), s_ib) != 0)
        return -1;
    s_ib_dirty = 0;
    return 0;
}

static int bbitmap_load(uint32_t g)
{
    if (s_bb_group == g)
        return 0;
    if (bbitmap_flush() != 0)
        return -1;
    if (blk_read(grp_block_bitmap(g), s_bb) != 0) {
        s_bb_group = EXT2_INVALID_BLOCK;
        return -1;
    }
    s_bb_group = g;
    s_bb_dirty = 0;
    return 0;
}

static int ibitmap_load(uint32_t g)
{
    if (s_ib_group == g)
        return 0;
    if (ibitmap_flush() != 0)
        return -1;
    if (blk_read(grp_inode_bitmap(g), s_ib) != 0) {
        s_ib_group = EXT2_INVALID_BLOCK;
        return -1;
    }
    s_ib_group = g;
    s_ib_dirty = 0;
    return 0;
}

/* ------------------------------------------------------- block allocation */

static uint32_t ext2_alloc_block(void)
{
    for (uint32_t g = 0; g < s_groups; g++) {
        if (bbitmap_load(g) != 0)
            return 0;

        uint32_t first = s_first_data_block + g * s_blocks_per_group;
        uint32_t count = s_blocks_per_group;

        if (first >= s_blocks_count)
            continue;
        if (first + count > s_blocks_count)
            count = s_blocks_count - first;

        for (uint32_t i = 0; i < count; i++) {
            uint32_t byte = i >> 3;
            uint8_t  mask = (uint8_t)(1u << (i & 7u));

            if (!(s_bb[byte] & mask)) {
                s_bb[byte] |= mask;
                s_bb_dirty = 1;
                if (s_free_blocks > 0)
                    s_free_blocks--;
                {
                    uint16_t f = rd16(s_gdt + g * 32u + 12u);
                    wr16(s_gdt + g * 32u + 12u, (uint16_t)(f - 1u));
                    s_gdt_dirty = 1;
                }
                return first + i;
            }
        }
    }
    return 0;
}

static void ext2_free_block(uint32_t b)
{
    if (b < s_first_data_block || b >= s_blocks_count)
        return;

    uint32_t rel = b - s_first_data_block;
    uint32_t g = rel / s_blocks_per_group;
    uint32_t i = rel % s_blocks_per_group;

    if (bbitmap_load(g) != 0)
        return;

    s_bb[i >> 3] &= (uint8_t)~(1u << (i & 7u));
    s_bb_dirty = 1;
    s_free_blocks++;
    {
        uint16_t f = rd16(s_gdt + g * 32u + 12u);
        wr16(s_gdt + g * 32u + 12u, (uint16_t)(f + 1u));
        s_gdt_dirty = 1;
    }
}

/* ------------------------------------------------------- inode allocation */

static int inode_read(uint32_t ino, uint8_t* out)
{
    if (ino == 0 || ino > s_inodes_count)
        return -1;

    uint32_t g = (ino - 1u) / s_inodes_per_group;
    uint32_t idx = (ino - 1u) % s_inodes_per_group;
    uint32_t table = rd32(s_gdt + g * 32u + 8u);
    uint32_t byte = table * s_block_size + idx * s_inode_size;
    uint32_t blk = byte / s_block_size;
    uint32_t off = byte % s_block_size;

    if (blk_read(blk, s_iobuf) != 0)
        return -1;
    memcpy(out, s_iobuf + off, s_inode_size);
    return 0;
}

static int inode_write(uint32_t ino, const uint8_t* in)
{
    if (ino == 0 || ino > s_inodes_count)
        return -1;

    uint32_t g = (ino - 1u) / s_inodes_per_group;
    uint32_t idx = (ino - 1u) % s_inodes_per_group;
    uint32_t table = rd32(s_gdt + g * 32u + 8u);
    uint32_t byte = table * s_block_size + idx * s_inode_size;
    uint32_t blk = byte / s_block_size;
    uint32_t off = byte % s_block_size;

    if (blk_read(blk, s_iobuf) != 0)
        return -1;
    memcpy(s_iobuf + off, in, s_inode_size);
    return blk_write(blk, s_iobuf);
}

static uint32_t ext2_alloc_inode(void)
{
    for (uint32_t g = 0; g < s_groups; g++) {
        if (ibitmap_load(g) != 0)
            return 0;

        uint32_t base = g * s_inodes_per_group;
        uint32_t start = 0;

        if (g == 0 && s_first_ino > 1u)
            start = s_first_ino - 1u;

        for (uint32_t i = start; i < s_inodes_per_group; i++) {
            uint32_t ino = base + i + 1u;
            uint32_t byte = i >> 3;
            uint8_t  mask = (uint8_t)(1u << (i & 7u));

            if (ino > s_inodes_count)
                break;

            if (!(s_ib[byte] & mask)) {
                s_ib[byte] |= mask;
                s_ib_dirty = 1;
                if (s_free_inodes > 0)
                    s_free_inodes--;
                {
                    uint16_t f = rd16(s_gdt + g * 32u + 14u);
                    wr16(s_gdt + g * 32u + 14u, (uint16_t)(f - 1u));
                    s_gdt_dirty = 1;
                }

                memset(s_inode, 0, s_inode_size);
                inode_write(ino, s_inode);
                return ino;
            }
        }
    }
    return 0;
}

static void ext2_free_inode(uint32_t ino)
{
    if (ino == 0 || ino > s_inodes_count)
        return;

    uint32_t g = (ino - 1u) / s_inodes_per_group;
    uint32_t i = (ino - 1u) % s_inodes_per_group;

    if (ibitmap_load(g) != 0)
        return;

    s_ib[i >> 3] &= (uint8_t)~(1u << (i & 7u));
    s_ib_dirty = 1;
    s_free_inodes++;
    {
        uint16_t f = rd16(s_gdt + g * 32u + 14u);
        wr16(s_gdt + g * 32u + 14u, (uint16_t)(f + 1u));
        s_gdt_dirty = 1;
    }
}

/* ------------------------------------------------------- block mapping */

/* Returns the physical block holding file block `idx`, or 0 for a hole. */
static uint32_t bmap(const uint8_t* inode, uint32_t idx)
{
    uint32_t per = s_block_size / 4u;

    if (idx < EXT2_NDIR_BLOCKS)
        return rd32(inode + 40u + idx * 4u);

    idx -= EXT2_NDIR_BLOCKS;

    if (idx < per) {
        uint32_t ind = rd32(inode + 40u + EXT2_IND_BLOCK * 4u);
        if (ind == 0)
            return 0;
        if (blk_read(ind, s_ind) != 0)
            return 0;
        return rd32(s_ind + idx * 4u);
    }

    idx -= per;

    if (idx < per * per) {
        uint32_t dind = rd32(inode + 40u + EXT2_DIND_BLOCK * 4u);
        if (dind == 0)
            return 0;
        if (blk_read(dind, s_ind) != 0)
            return 0;
        {
            uint32_t l1 = rd32(s_ind + (idx / per) * 4u);
            if (l1 == 0)
                return 0;
            if (blk_read(l1, s_ind2) != 0)
                return 0;
            return rd32(s_ind2 + (idx % per) * 4u);
        }
    }

    return 0;
}

static void inode_add_block_sector(uint8_t* inode, uint32_t count)
{
    wr32(inode + 28u, rd32(inode + 28u) + count * s_sectors_per_block);
}

/* Ensures file block `idx` is allocated and returns it (0 on failure). */
static uint32_t bmap_alloc(uint32_t ino, uint8_t* inode, uint32_t idx)
{
    uint32_t per = s_block_size / 4u;

    (void)ino;

    if (idx < EXT2_NDIR_BLOCKS) {
        uint32_t b = rd32(inode + 40u + idx * 4u);
        if (b != 0)
            return b;
        b = ext2_alloc_block();
        if (b == 0)
            return 0;
        wr32(inode + 40u + idx * 4u, b);
        inode_add_block_sector(inode, 1);
        return b;
    }

    idx -= EXT2_NDIR_BLOCKS;

    if (idx < per) {
        uint32_t ind = rd32(inode + 40u + EXT2_IND_BLOCK * 4u);
        uint32_t b;

        if (ind == 0) {
            ind = ext2_alloc_block();
            if (ind == 0)
                return 0;
            memset(s_ind, 0, s_block_size);
            blk_write(ind, s_ind);
            wr32(inode + 40u + EXT2_IND_BLOCK * 4u, ind);
            inode_add_block_sector(inode, 1);
        }

        if (blk_read(ind, s_ind) != 0)
            return 0;
        b = rd32(s_ind + idx * 4u);
        if (b != 0)
            return b;
        b = ext2_alloc_block();
        if (b == 0)
            return 0;
        wr32(s_ind + idx * 4u, b);
        if (blk_write(ind, s_ind) != 0)
            return 0;
        inode_add_block_sector(inode, 1);
        return b;
    }

    idx -= per;

    if (idx < per * per) {
        uint32_t dind = rd32(inode + 40u + EXT2_DIND_BLOCK * 4u);
        uint32_t l1, b;

        if (dind == 0) {
            dind = ext2_alloc_block();
            if (dind == 0)
                return 0;
            memset(s_ind, 0, s_block_size);
            blk_write(dind, s_ind);
            wr32(inode + 40u + EXT2_DIND_BLOCK * 4u, dind);
            inode_add_block_sector(inode, 1);
        }

        if (blk_read(dind, s_ind) != 0)
            return 0;
        l1 = rd32(s_ind + (idx / per) * 4u);
        if (l1 == 0) {
            l1 = ext2_alloc_block();
            if (l1 == 0)
                return 0;
            memset(s_ind2, 0, s_block_size);
            blk_write(l1, s_ind2);
            wr32(s_ind + (idx / per) * 4u, l1);
            if (blk_write(dind, s_ind) != 0)
                return 0;
            inode_add_block_sector(inode, 1);
        }

        if (blk_read(l1, s_ind2) != 0)
            return 0;
        b = rd32(s_ind2 + (idx % per) * 4u);
        if (b != 0)
            return b;
        b = ext2_alloc_block();
        if (b == 0)
            return 0;
        wr32(s_ind2 + (idx % per) * 4u, b);
        if (blk_write(l1, s_ind2) != 0)
            return 0;
        inode_add_block_sector(inode, 1);
        return b;
    }

    return 0;
}

/* Frees every block referenced by `inode` and clears its block pointers. */
static void bmap_free_all(uint8_t* inode)
{
    uint32_t per = s_block_size / 4u;

    for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        uint32_t b = rd32(inode + 40u + i * 4u);
        if (b != 0) {
            ext2_free_block(b);
            wr32(inode + 40u + i * 4u, 0);
        }
    }

    {
        uint32_t ind = rd32(inode + 40u + EXT2_IND_BLOCK * 4u);
        if (ind != 0) {
            if (blk_read(ind, s_ind) == 0) {
                for (uint32_t i = 0; i < per; i++) {
                    uint32_t b = rd32(s_ind + i * 4u);
                    if (b != 0)
                        ext2_free_block(b);
                }
            }
            ext2_free_block(ind);
            wr32(inode + 40u + EXT2_IND_BLOCK * 4u, 0);
        }
    }

    {
        uint32_t dind = rd32(inode + 40u + EXT2_DIND_BLOCK * 4u);
        if (dind != 0) {
            if (blk_read(dind, s_ind) == 0) {
                for (uint32_t j = 0; j < per; j++) {
                    uint32_t l1 = rd32(s_ind + j * 4u);
                    if (l1 == 0)
                        continue;
                    if (blk_read(l1, s_ind2) == 0) {
                        for (uint32_t i = 0; i < per; i++) {
                            uint32_t b = rd32(s_ind2 + i * 4u);
                            if (b != 0)
                                ext2_free_block(b);
                        }
                    }
                    ext2_free_block(l1);
                }
            }
            ext2_free_block(dind);
            wr32(inode + 40u + EXT2_DIND_BLOCK * 4u, 0);
        }
    }

    wr32(inode + 40u + EXT2_TIND_BLOCK * 4u, 0);
    wr32(inode + 28u, 0);
}

/* ---------------------------------------------------------------- directory */

static void make_dirent(uint8_t* e, uint32_t ino, uint16_t rec_len,
                        uint8_t name_len, uint8_t ftype, const char* name)
{
    wr32(e, ino);
    wr16(e + 4u, rec_len);
    e[6] = name_len;
    e[7] = ftype;
    if (name_len)
        memcpy(e + 8u, name, name_len);
}

static int is_dot_or_dotdot(const uint8_t* e)
{
    if (e[6] == 1 && e[8] == '.')
        return 1;
    if (e[6] == 2 && e[8] == '.' && e[9] == '.')
        return 1;
    return 0;
}

/* Fills the VFS node description from a raw ext2 inode. */
static void inode_fill_node(uint32_t ino, const uint8_t* inode, vfs_node_t* out)
{
    uint16_t mode = rd16(inode);

    out->mode = mode;
    out->uid = rd16(inode + 2u);
    out->gid = rd16(inode + 24u);
    out->nlink = rd16(inode + 26u);
    out->size = rd32(inode + 4u);
    out->ino = ino;

    switch (mode & EXT2_S_IFMT) {
    case EXT2_S_IFDIR: out->type = VFS_NODE_DIR; break;
    case EXT2_S_IFLNK: out->type = VFS_NODE_LNK; break;
    default:           out->type = VFS_NODE_FILE; break;
    }
}

static int dir_find(uint32_t dir_ino, const char* name, uint32_t nlen,
                    uint32_t* out_ino, uint8_t* out_type)
{
    uint8_t inode[EXT2_INODE_MAX];
    uint32_t size, nblocks;

    if (inode_read(dir_ino, inode) != 0)
        return VFS_EIO;

    size = rd32(inode + 4u);
    nblocks = (size + s_block_size - 1u) / s_block_size;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = bmap(inode, b);
        uint32_t off = 0;

        if (blk == 0)
            continue;
        if (blk_read(blk, s_dir) != 0)
            return VFS_EIO;

        while (off + 8u <= s_block_size) {
            uint8_t* e = s_dir + off;
            uint32_t eino = rd32(e);
            uint16_t rec = rd16(e + 4u);

            if (rec < 8u || off + rec > s_block_size)
                break;

            if (eino != 0 && e[6] == nlen && memcmp(e + 8u, name, nlen) == 0) {
                if (out_ino)
                    *out_ino = eino;
                if (out_type)
                    *out_type = e[7];
                return VFS_OK;
            }

            off += rec;
        }
    }

    return VFS_ENOENT;
}

static int dir_readdir(uint32_t dir_ino, uint32_t index,
                       char* name, size_t name_cap, vfs_node_t* out)
{
    uint8_t inode[EXT2_INODE_MAX];
    uint32_t size, nblocks;
    uint32_t seen = 0;

    if (inode_read(dir_ino, inode) != 0)
        return VFS_EIO;

    if ((rd16(inode) & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return VFS_ENOTDIR;

    size = rd32(inode + 4u);
    nblocks = (size + s_block_size - 1u) / s_block_size;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = bmap(inode, b);
        uint32_t off = 0;

        if (blk == 0)
            continue;
        if (blk_read(blk, s_dir) != 0)
            return VFS_EIO;

        while (off + 8u <= s_block_size) {
            uint8_t* e = s_dir + off;
            uint32_t eino = rd32(e);
            uint16_t rec = rd16(e + 4u);

            if (rec < 8u || off + rec > s_block_size)
                break;

            if (eino != 0 && !is_dot_or_dotdot(e)) {
                if (seen == index) {
                    size_t n = e[6] < name_cap - 1u ? e[6] : name_cap - 1u;
                    memcpy(name, e + 8u, n);
                    name[n] = '\0';
                    if (out) {
                        /* Report the entry's own inode (no symlink
                         * following): `ls -l` needs the link itself to
                         * show its target, mode and owner. */
                        uint8_t cinode[EXT2_INODE_MAX];
                        if (inode_read(eino, cinode) != 0)
                            return VFS_EIO;
                        inode_fill_node(eino, cinode, out);
                    }
                    return VFS_OK;
                }
                seen++;
            }

            off += rec;
        }
    }

    return VFS_ENOENT;
}

static int dir_empty(uint32_t dir_ino)
{
    uint8_t inode[EXT2_INODE_MAX];
    uint32_t size, nblocks;

    if (inode_read(dir_ino, inode) != 0)
        return 0;

    size = rd32(inode + 4u);
    nblocks = (size + s_block_size - 1u) / s_block_size;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = bmap(inode, b);
        uint32_t off = 0;

        if (blk == 0)
            continue;
        if (blk_read(blk, s_dir) != 0)
            return 0;

        while (off + 8u <= s_block_size) {
            uint8_t* e = s_dir + off;
            uint32_t eino = rd32(e);
            uint16_t rec = rd16(e + 4u);

            if (rec < 8u || off + rec > s_block_size)
                break;

            if (eino != 0 && !is_dot_or_dotdot(e))
                return 0;

            off += rec;
        }
    }

    return 1;
}

static int dir_add(uint32_t dir_ino, const char* name, uint32_t nlen,
                   uint32_t target_ino, uint8_t ftype)
{
    uint8_t inode[EXT2_INODE_MAX];
    uint32_t need;
    uint32_t size, nblocks;

    if (nlen == 0 || nlen > 255u)
        return VFS_EINVAL;
    if (inode_read(dir_ino, inode) != 0)
        return VFS_EIO;

    need = align4(8u + nlen);
    size = rd32(inode + 4u);
    nblocks = (size + s_block_size - 1u) / s_block_size;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = bmap(inode, b);
        uint32_t off = 0;

        if (blk == 0)
            continue;
        if (blk_read(blk, s_dir) != 0)
            return VFS_EIO;

        while (off + 8u <= s_block_size) {
            uint8_t* e = s_dir + off;
            uint32_t eino = rd32(e);
            uint16_t rec = rd16(e + 4u);

            if (rec < 8u || off + rec > s_block_size)
                break;

            if (eino == 0) {
                if (rec >= need) {
                    make_dirent(e, target_ino, rec, (uint8_t)nlen, ftype, name);
                    return blk_write(blk, s_dir) == 0 ? VFS_OK : VFS_EIO;
                }
            } else {
                uint32_t actual = align4(8u + e[6]);
                if (rec >= actual + need) {
                    wr16(e + 4u, (uint16_t)actual);
                    make_dirent(e + actual, target_ino,
                                (uint16_t)(rec - actual),
                                (uint8_t)nlen, ftype, name);
                    return blk_write(blk, s_dir) == 0 ? VFS_OK : VFS_EIO;
                }
            }

            off += rec;
        }
    }

    /* No room in the existing blocks: append a fresh one. */
    {
        uint32_t nb = bmap_alloc(dir_ino, inode, nblocks);

        if (nb == 0)
            return VFS_ENOSPC;

        wr32(inode + 4u, (nblocks + 1u) * s_block_size);
        if (inode_write(dir_ino, inode) != 0)
            return VFS_EIO;

        memset(s_dir, 0, s_block_size);
        make_dirent(s_dir, target_ino, (uint16_t)s_block_size,
                    (uint8_t)nlen, ftype, name);
        return blk_write(nb, s_dir) == 0 ? VFS_OK : VFS_EIO;
    }
}

static int dir_remove(uint32_t dir_ino, const char* name, uint32_t nlen)
{
    uint8_t inode[EXT2_INODE_MAX];
    uint32_t size, nblocks;

    if (inode_read(dir_ino, inode) != 0)
        return VFS_EIO;

    size = rd32(inode + 4u);
    nblocks = (size + s_block_size - 1u) / s_block_size;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = bmap(inode, b);
        uint32_t off = 0;
        uint32_t prev = EXT2_INVALID_BLOCK;

        if (blk == 0)
            continue;
        if (blk_read(blk, s_dir) != 0)
            return VFS_EIO;

        while (off + 8u <= s_block_size) {
            uint8_t* e = s_dir + off;
            uint32_t eino = rd32(e);
            uint16_t rec = rd16(e + 4u);

            if (rec < 8u || off + rec > s_block_size)
                break;

            if (eino != 0 && e[6] == nlen && memcmp(e + 8u, name, nlen) == 0) {
                if (prev != EXT2_INVALID_BLOCK) {
                    uint16_t prec = rd16(s_dir + prev + 4u);
                    wr16(s_dir + prev + 4u, (uint16_t)(prec + rec));
                } else {
                    wr32(e, 0);
                }
                return blk_write(blk, s_dir) == 0 ? VFS_OK : VFS_EIO;
            }

            prev = off;
            off += rec;
        }
    }

    return VFS_ENOENT;
}

static int dir_set_parent(uint32_t child_ino, uint32_t new_parent)
{
    uint8_t inode[EXT2_INODE_MAX];
    uint32_t size, nblocks;

    if (inode_read(child_ino, inode) != 0)
        return VFS_EIO;

    size = rd32(inode + 4u);
    nblocks = (size + s_block_size - 1u) / s_block_size;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = bmap(inode, b);
        uint32_t off = 0;

        if (blk == 0)
            continue;
        if (blk_read(blk, s_dir) != 0)
            return VFS_EIO;

        while (off + 8u <= s_block_size) {
            uint8_t* e = s_dir + off;
            uint32_t eino = rd32(e);
            uint16_t rec = rd16(e + 4u);

            if (rec < 8u || off + rec > s_block_size)
                break;

            if (eino != 0 && e[6] == 2 && e[8] == '.' && e[9] == '.') {
                wr32(e, new_parent);
                return blk_write(blk, s_dir) == 0 ? VFS_OK : VFS_EIO;
            }

            off += rec;
        }
    }

    return VFS_ENOENT;
}

/* ------------------------------------------------------------ path resolution */

/* Reads the target string of symlink `ino` into buf (NUL-terminated).
 * Fast symlinks (<= 60 chars) store the target in the i_block area;
 * longer ones keep it in the first data block. */
static int read_link_data(uint32_t ino, char* buf, size_t cap)
{
    uint8_t inode[EXT2_INODE_MAX];
    uint32_t size, blocks, blk, chunk;

    if (buf == NULL || cap == 0)
        return VFS_EINVAL;
    if (inode_read(ino, inode) != 0)
        return VFS_EIO;

    size = rd32(inode + 4u);
    if (size >= cap)
        size = (uint32_t)cap - 1u;

    blocks = rd32(inode + 28u);
    if (blocks == 0u) {
        memcpy(buf, inode + 40u, size);
    } else {
        blk = rd32(inode + 40u);
        if (blk == 0)
            return VFS_EIO;
        if (blk_read(blk, s_data) != 0)
            return VFS_EIO;
        chunk = size;
        if (chunk > s_block_size)
            chunk = s_block_size;
        memcpy(buf, s_data, chunk);
    }
    buf[size] = '\0';
    return VFS_OK;
}

/*
 * Walks `path` from directory `start_ino`, following symbolic links. With
 * follow_final set the last component is followed too (stat/open); with it
 * clear the walk stops at a final link (readlink). `*hops` bounds the total
 * number of links followed (see VFS_ELOOP). Recursion depth is bounded by
 * that same limit.
 */
static int resolve_impl(const char* path, int follow_final, uint32_t start_ino,
                        int* hops, uint32_t* out_ino)
{
    uint32_t cur = start_ino;
    const char* p = path;

    if (p == NULL || *p == '\0')
        return VFS_EINVAL;

    if (*p == '/')
        p++;

    while (*p) {
        const char* start = p;
        uint32_t next;
        uint8_t  ftype;

        while (*p && *p != '/')
            p++;

        {
            size_t len = (size_t)(p - start);

            if (len == 0) {
                /* skip repeated slashes */
            } else if (len == 1u && start[0] == '.') {
                /* current directory */
            } else if (len == 2u && start[0] == '.' && start[1] == '.') {
                uint32_t parent;
                if (dir_find(cur, "..", 2u, &parent, NULL) != VFS_OK)
                    return VFS_ENOENT;
                cur = parent;
            } else {
                if (dir_find(cur, start, (uint32_t)len, &next, &ftype) != VFS_OK)
                    return VFS_ENOENT;

                if (ftype == EXT2_FT_SYMLINK &&
                    (*p != '\0' || follow_final)) {
                    char target[VFS_PATH_MAX];
                    char joined[VFS_PATH_MAX];
                    int r;

                    if (*hops >= 8)
                        return VFS_ELOOP;
                    (*hops)++;

                    r = read_link_data(next, target, sizeof(target));
                    if (r != VFS_OK)
                        return r;

                    /* Rebuild the walk as "target / remaining" and resolve
                     * the link's destination. Relative targets are taken
                     * against the directory containing the link; absolute
                     * ones restart from the filesystem root. */
                    if (*p) {
                        if (strlen(target) + 2u + strlen(p) >= sizeof(joined))
                            return VFS_EINVAL;
                        strlcpy(joined, target, sizeof(joined));
                        strlcat(joined, "/", sizeof(joined));
                        strlcat(joined, p, sizeof(joined));
                    } else {
                        strlcpy(joined, target, sizeof(joined));
                    }

                    if (joined[0] == '/')
                        return resolve_impl(joined, follow_final,
                                            EXT2_ROOT_INO, hops, out_ino);
                    return resolve_impl(joined, follow_final, cur, hops,
                                        out_ino);
                }

                if (ftype != EXT2_FT_DIR && *p != '\0')
                    return VFS_ENOTDIR;

                cur = next;
            }
        }

        while (*p == '/')
            p++;
    }

    *out_ino = cur;
    return VFS_OK;
}

/* resolve_path follows intermediate and final symlinks (stat/open). */
static int resolve_path(const char* path, uint32_t* out_ino)
{
    int hops = 0;
    return resolve_impl(path, 1, EXT2_ROOT_INO, &hops, out_ino);
}

/* resolve_path_nofollow stops at a final symlink (readlink). */
static int resolve_path_nofollow(const char* path, uint32_t* out_ino)
{
    int hops = 0;
    return resolve_impl(path, 0, EXT2_ROOT_INO, &hops, out_ino);
}

/* Splits an absolute path into a parent directory path and the leaf name. */
static int split_path(const char* path, char* parent, size_t parent_cap,
                      char* leaf, size_t leaf_cap)
{
    size_t len, slash, leaf_len, plen;

    if (path == NULL)
        return -1;

    len = strlen(path);
    if (len == 0 || len >= VFS_PATH_MAX)
        return -1;

    slash = len;
    while (slash > 0 && path[slash - 1] != '/')
        slash--;

    if (slash >= len || path[slash] == '\0')
        return -1;

    leaf_len = len - slash;
    if (leaf_len + 1u > leaf_cap)
        return -1;
    memcpy(leaf, path + slash, leaf_len);
    leaf[leaf_len] = '\0';

    if (slash <= 1u) {
        if (parent_cap < 2u)
            return -1;
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        plen = slash - 1u;
        if (plen + 1u > parent_cap)
            return -1;
        memcpy(parent, path, plen);
        parent[plen] = '\0';
    }

    return 0;
}

/* --------------------------------------------------------------- metadata flush */

static int ext2_fs_flush(void)
{
    if (bbitmap_flush() != 0)
        return VFS_EIO;
    if (ibitmap_flush() != 0)
        return VFS_EIO;

    wr32(s_sb + 12u, s_free_blocks);
    wr32(s_sb + 16u, s_free_inodes);

    if (sb_write() != 0)
        return VFS_EIO;
    if (s_gdt_dirty && gdt_flush() != 0)
        return VFS_EIO;
    s_gdt_dirty = 0;

    return VFS_OK;
}

/* ------------------------------------------------------------- vfs_ops_t */

static int ext2_op_stat(void* fs, const char* path, vfs_node_t* out)
{
    uint32_t ino;
    uint8_t inode[EXT2_INODE_MAX];
    int r;

    (void)fs;

    r = resolve_path(path, &ino);
    if (r != VFS_OK)
        return r;

    if (inode_read(ino, inode) != 0)
        return VFS_EIO;

    inode_fill_node(ino, inode, out);
    return VFS_OK;
}

static int ext2_op_readdir(void* fs, const char* path, uint32_t index,
                           char* name, size_t name_cap, vfs_node_t* out)
{
    uint32_t ino;
    int r;

    (void)fs;

    r = resolve_path(path, &ino);
    if (r != VFS_OK)
        return r;

    return dir_readdir(ino, index, name, name_cap, out);
}

static int ext2_op_read(void* fs, const char* path, uint32_t offset,
                        void* buf, uint32_t len, uint32_t* out_len)
{
    uint32_t ino, size;
    uint8_t inode[EXT2_INODE_MAX];
    uint8_t* dst = (uint8_t*)buf;
    uint32_t done = 0;
    int r;

    (void)fs;

    r = resolve_path(path, &ino);
    if (r != VFS_OK)
        return r;

    if (inode_read(ino, inode) != 0)
        return VFS_EIO;

    if ((rd16(inode) & EXT2_S_IFMT) == EXT2_S_IFDIR)
        return VFS_EISDIR;

    size = rd32(inode + 4u);

    if (out_len)
        *out_len = 0;
    if (offset >= size || len == 0)
        return VFS_OK;

    if (len > size - offset)
        len = size - offset;

    while (done < len) {
        uint32_t blk_idx = (offset + done) / s_block_size;
        uint32_t blk_off = (offset + done) % s_block_size;
        uint32_t chunk = s_block_size - blk_off;
        uint32_t blk = bmap(inode, blk_idx);

        if (chunk > len - done)
            chunk = len - done;

        if (blk == 0) {
            memset(dst + done, 0, chunk);
        } else {
            if (blk_read(blk, s_data) != 0)
                return VFS_EIO;
            memcpy(dst + done, s_data + blk_off, chunk);
        }

        done += chunk;
    }

    if (out_len)
        *out_len = done;
    return VFS_OK;
}

static int ext2_op_write(void* fs, const char* path,
                         const char* data, uint32_t len)
{
    char parent[VFS_PATH_MAX];
    char leaf[VFS_NAME_MAX + 1];
    uint32_t pino, ino = 0, nblocks;
    uint8_t  ftype = 0;
    uint8_t  inode[EXT2_INODE_MAX];
    uint8_t  pinode[EXT2_INODE_MAX];
    int r;

    (void)fs;

    if (split_path(path, parent, sizeof(parent), leaf, sizeof(leaf)) != 0)
        return VFS_EINVAL;

    r = resolve_path(parent, &pino);
    if (r != VFS_OK)
        return r;

    if (inode_read(pino, pinode) != 0)
        return VFS_EIO;
    if ((rd16(pinode) & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return VFS_ENOTDIR;

    if (dir_find(pino, leaf, (uint32_t)strlen(leaf), &ino, &ftype) == VFS_OK) {
        if (ftype == EXT2_FT_SYMLINK) {
            /* Write through the link: resolve the full path (which follows
             * the final component) and operate on the target. Dangling
             * links stay dangling. */
            r = resolve_path(path, &ino);
            if (r != VFS_OK)
                return r;
            if (inode_read(ino, inode) != 0)
                return VFS_EIO;
            if ((rd16(inode) & EXT2_S_IFMT) == EXT2_S_IFDIR)
                return VFS_EISDIR;
        } else {
            if (ftype == EXT2_FT_DIR)
                return VFS_EISDIR;
            if (inode_read(ino, inode) != 0)
                return VFS_EIO;
        }
        bmap_free_all(inode);
    } else {
        ino = ext2_alloc_inode();
        if (ino == 0)
            return VFS_ENOSPC;
        if (inode_read(ino, inode) != 0)
            return VFS_EIO;
        wr16(inode + 0u, (uint16_t)(EXT2_S_IFREG | (0666u & ~vfs_get_umask())));
        wr16(inode + 2u, vfs_get_uid());
        wr16(inode + 24u, vfs_get_gid());
        wr16(inode + 26u, 1u);
        r = dir_add(pino, leaf, (uint32_t)strlen(leaf), ino, EXT2_FT_REG_FILE);
        if (r != VFS_OK) {
            ext2_free_inode(ino);
            return r;
        }
    }

    nblocks = (len + s_block_size - 1u) / s_block_size;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = bmap_alloc(ino, inode, b);
        uint32_t chunk = len - b * s_block_size;

        if (blk == 0) {
            bmap_free_all(inode);
            wr32(inode + 4u, 0);
            inode_write(ino, inode);
            ext2_fs_flush();
            return VFS_ENOSPC;
        }

        if (chunk > s_block_size)
            chunk = s_block_size;

        memset(s_data, 0, s_block_size);
        memcpy(s_data, data + (size_t)b * s_block_size, chunk);

        if (blk_write(blk, s_data) != 0)
            return VFS_EIO;
    }

    wr32(inode + 4u, len);
    if (inode_write(ino, inode) != 0)
        return VFS_EIO;

    return ext2_fs_flush();
}

static int ext2_op_mkdir(void* fs, const char* path)
{
    char parent[VFS_PATH_MAX];
    char leaf[VFS_NAME_MAX + 1];
    uint32_t pino, ino;
    uint8_t  ftype = 0;
    uint8_t  inode[EXT2_INODE_MAX];
    uint8_t  pinode[EXT2_INODE_MAX];
    uint32_t blk;
    int r;

    (void)fs;

    if (split_path(path, parent, sizeof(parent), leaf, sizeof(leaf)) != 0)
        return VFS_EINVAL;

    r = resolve_path(parent, &pino);
    if (r != VFS_OK)
        return r;

    if (dir_find(pino, leaf, (uint32_t)strlen(leaf), NULL, &ftype) == VFS_OK)
        return VFS_EEXIST;

    ino = ext2_alloc_inode();
    if (ino == 0)
        return VFS_ENOSPC;

    if (inode_read(ino, inode) != 0)
        return VFS_EIO;

    blk = ext2_alloc_block();
    if (blk == 0) {
        ext2_free_inode(ino);
        return VFS_ENOSPC;
    }

    wr16(inode + 0u, (uint16_t)(EXT2_S_IFDIR | (0777u & ~vfs_get_umask())));
    wr16(inode + 2u, vfs_get_uid());
    wr16(inode + 24u, vfs_get_gid());
    wr16(inode + 26u, 2u);
    wr32(inode + 4u, s_block_size);
    wr32(inode + 40u, blk);
    inode_add_block_sector(inode, 1);

    memset(s_dir, 0, s_block_size);
    make_dirent(s_dir, ino, 12u, 1u, EXT2_FT_DIR, ".");
    make_dirent(s_dir + 12u, pino, (uint16_t)(s_block_size - 12u),
                2u, EXT2_FT_DIR, "..");
    blk_write(blk, s_dir);

    if (inode_write(ino, inode) != 0)
        return VFS_EIO;

    r = dir_add(pino, leaf, (uint32_t)strlen(leaf), ino, EXT2_FT_DIR);
    if (r != VFS_OK) {
        bmap_free_all(inode);
        ext2_free_inode(ino);
        return r;
    }

    if (inode_read(pino, pinode) == 0) {
        wr16(pinode + 26u, (uint16_t)(rd16(pinode + 26u) + 1u));
        inode_write(pino, pinode);
    }

    {
        uint32_t g = (ino - 1u) / s_inodes_per_group;
        uint16_t d = rd16(s_gdt + g * 32u + 16u);
        wr16(s_gdt + g * 32u + 16u, (uint16_t)(d + 1u));
        s_gdt_dirty = 1;
    }

    return ext2_fs_flush();
}

static int ext2_op_unlink(void* fs, const char* path)
{
    char parent[VFS_PATH_MAX];
    char leaf[VFS_NAME_MAX + 1];
    uint32_t pino, ino;
    uint8_t  ftype = 0;
    uint8_t  inode[EXT2_INODE_MAX];
    int r;

    (void)fs;

    if (split_path(path, parent, sizeof(parent), leaf, sizeof(leaf)) != 0)
        return VFS_EINVAL;

    r = resolve_path(parent, &pino);
    if (r != VFS_OK)
        return r;

    r = dir_find(pino, leaf, (uint32_t)strlen(leaf), &ino, &ftype);
    if (r != VFS_OK)
        return r;
    if (ftype == EXT2_FT_DIR)
        return VFS_EISDIR;

    if (inode_read(ino, inode) != 0)
        return VFS_EIO;

    /* Remove the dirent first so a failure leaves everything intact. */
    r = dir_remove(pino, leaf, (uint32_t)strlen(leaf));
    if (r != VFS_OK)
        return r;

    /* A hard-linked file just loses one name; the inode survives while
     * other links (or the owner) still reference it. */
    if (rd16(inode + 26u) > 1u) {
        wr16(inode + 26u, (uint16_t)(rd16(inode + 26u) - 1u));
        if (inode_write(ino, inode) != 0)
            return VFS_EIO;
    } else {
        bmap_free_all(inode);
        ext2_free_inode(ino);
    }

    return ext2_fs_flush();
}

static int ext2_op_rmdir(void* fs, const char* path)
{
    char parent[VFS_PATH_MAX];
    char leaf[VFS_NAME_MAX + 1];
    uint32_t pino, ino;
    uint8_t  ftype = 0;
    uint8_t  inode[EXT2_INODE_MAX];
    uint8_t  pinode[EXT2_INODE_MAX];
    int r;

    (void)fs;

    if (split_path(path, parent, sizeof(parent), leaf, sizeof(leaf)) != 0)
        return VFS_EINVAL;

    if (strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0)
        return VFS_EINVAL;

    r = resolve_path(parent, &pino);
    if (r != VFS_OK)
        return r;

    r = dir_find(pino, leaf, (uint32_t)strlen(leaf), &ino, &ftype);
    if (r != VFS_OK)
        return r;
    if (ftype != EXT2_FT_DIR)
        return VFS_ENOTDIR;
    if (ino == EXT2_ROOT_INO)
        return VFS_EINVAL;
    if (!dir_empty(ino))
        return VFS_ENOTEMPTY;

    if (inode_read(ino, inode) != 0)
        return VFS_EIO;
    bmap_free_all(inode);
    ext2_free_inode(ino);

    r = dir_remove(pino, leaf, (uint32_t)strlen(leaf));
    if (r != VFS_OK)
        return r;

    if (inode_read(pino, pinode) == 0 && rd16(pinode + 26u) > 0) {
        wr16(pinode + 26u, (uint16_t)(rd16(pinode + 26u) - 1u));
        inode_write(pino, pinode);
    }

    return ext2_fs_flush();
}

static int ext2_op_rename(void* fs, const char* from, const char* to)
{
    char fparent[VFS_PATH_MAX], fleaf[VFS_NAME_MAX + 1];
    char tparent[VFS_PATH_MAX], tleaf[VFS_NAME_MAX + 1];
    uint32_t fpino, tpino, ino;
    uint8_t  ftype = 0;
    int r;

    (void)fs;

    if (split_path(from, fparent, sizeof(fparent), fleaf, sizeof(fleaf)) != 0)
        return VFS_EINVAL;
    if (split_path(to, tparent, sizeof(tparent), tleaf, sizeof(tleaf)) != 0)
        return VFS_EINVAL;

    r = resolve_path(fparent, &fpino);
    if (r != VFS_OK)
        return r;
    r = resolve_path(tparent, &tpino);
    if (r != VFS_OK)
        return r;

    r = dir_find(fpino, fleaf, (uint32_t)strlen(fleaf), &ino, &ftype);
    if (r != VFS_OK)
        return r;

    if (dir_find(tpino, tleaf, (uint32_t)strlen(tleaf), NULL, NULL) == VFS_OK)
        return VFS_EEXIST;

    r = dir_add(tpino, tleaf, (uint32_t)strlen(tleaf), ino, ftype);
    if (r != VFS_OK)
        return r;

    r = dir_remove(fpino, fleaf, (uint32_t)strlen(fleaf));
    if (r != VFS_OK)
        return r;

    if (ftype == EXT2_FT_DIR && fpino != tpino) {
        uint8_t fi[EXT2_INODE_MAX], ti[EXT2_INODE_MAX];

        dir_set_parent(ino, tpino);

        if (inode_read(fpino, fi) == 0 && rd16(fi + 26u) > 0) {
            wr16(fi + 26u, (uint16_t)(rd16(fi + 26u) - 1u));
            inode_write(fpino, fi);
        }
        if (inode_read(tpino, ti) == 0) {
            wr16(ti + 26u, (uint16_t)(rd16(ti + 26u) + 1u));
            inode_write(tpino, ti);
        }
    }

    return ext2_fs_flush();
}

static int ext2_op_statfs(void* fs, uint32_t* total_bytes, uint32_t* free_bytes)
{
    (void)fs;

    if (total_bytes)
        *total_bytes = s_blocks_count * s_block_size;
    if (free_bytes)
        *free_bytes = s_free_blocks * s_block_size;
    return VFS_OK;
}

static int ext2_op_symlink(void* fs, const char* path, const char* target)
{
    char parent[VFS_PATH_MAX];
    char leaf[VFS_NAME_MAX + 1];
    uint32_t pino, ino;
    uint8_t  ftype = 0;
    uint8_t  inode[EXT2_INODE_MAX];
    size_t tlen;
    int r;

    (void)fs;

    if (split_path(path, parent, sizeof(parent), leaf, sizeof(leaf)) != 0)
        return VFS_EINVAL;

    tlen = strlen(target);
    if (tlen >= VFS_PATH_MAX)
        return VFS_EINVAL;
    if (tlen == 0 || strlen(leaf) == 0)
        return VFS_EINVAL;

    r = resolve_path(parent, &pino);
    if (r != VFS_OK)
        return r;

    if (dir_find(pino, leaf, (uint32_t)strlen(leaf), NULL, &ftype) == VFS_OK)
        return VFS_EEXIST;

    ino = ext2_alloc_inode();
    if (ino == 0)
        return VFS_ENOSPC;
    if (inode_read(ino, inode) != 0)
        return VFS_EIO;

    wr16(inode + 0u, (uint16_t)(EXT2_S_IFLNK | 0777u));
    wr16(inode + 2u, vfs_get_uid());
    wr16(inode + 24u, vfs_get_gid());
    wr16(inode + 26u, 1u);
    wr32(inode + 4u, (uint32_t)tlen);

    if (tlen <= 60u) {
        /* Fast symlink: the target lives in the i_block area. */
        memcpy(inode + 40u, target, tlen);
    } else {
        uint32_t blk = ext2_alloc_block();
        if (blk == 0) {
            ext2_free_inode(ino);
            return VFS_ENOSPC;
        }
        memset(s_data, 0, s_block_size);
        memcpy(s_data, target, tlen);
        blk_write(blk, s_data);
        wr32(inode + 40u, blk);
        inode_add_block_sector(inode, 1);
    }

    if (inode_write(ino, inode) != 0)
        return VFS_EIO;

    r = dir_add(pino, leaf, (uint32_t)strlen(leaf), ino, EXT2_FT_SYMLINK);
    if (r != VFS_OK) {
        bmap_free_all(inode);
        ext2_free_inode(ino);
        return r;
    }

    return ext2_fs_flush();
}

static int ext2_op_readlink(void* fs, const char* path, char* buf, size_t cap)
{
    uint32_t ino;
    uint8_t inode[EXT2_INODE_MAX];
    int r;

    (void)fs;

    r = resolve_path_nofollow(path, &ino);
    if (r != VFS_OK)
        return r;
    if (inode_read(ino, inode) != 0)
        return VFS_EIO;
    if ((rd16(inode) & EXT2_S_IFMT) != EXT2_S_IFLNK)
        return VFS_EINVAL;

    r = read_link_data(ino, buf, cap);
    if (r != VFS_OK)
        return r;
    return (int)strlen(buf);   /* the vfs readlink contract wants the length */
}

static int ext2_op_chmod(void* fs, const char* path, uint16_t mode)
{
    uint32_t ino;
    uint8_t inode[EXT2_INODE_MAX];
    int r;

    (void)fs;

    r = resolve_path(path, &ino);
    if (r != VFS_OK)
        return r;
    if (inode_read(ino, inode) != 0)
        return VFS_EIO;

    wr16(inode + 0u, (uint16_t)((rd16(inode) & EXT2_S_IFMT) | (mode & 07777u)));
    if (inode_write(ino, inode) != 0)
        return VFS_EIO;

    return ext2_fs_flush();
}

static int ext2_op_chown(void* fs, const char* path, uint16_t uid, uint16_t gid)
{
    uint32_t ino;
    uint8_t inode[EXT2_INODE_MAX];
    int r;

    (void)fs;

    r = resolve_path(path, &ino);
    if (r != VFS_OK)
        return r;
    if (inode_read(ino, inode) != 0)
        return VFS_EIO;

    wr16(inode + 2u, uid);
    wr16(inode + 24u, gid);
    if (inode_write(ino, inode) != 0)
        return VFS_EIO;

    return ext2_fs_flush();
}

/* VFS contract: ops->link(fs, from, to) -- `src` is the existing
     * file, `dst` the new name to add to the directory. */
static int ext2_op_link(void* fs, const char* src, const char* dst)
{
    char tparent[VFS_PATH_MAX];
    char tleaf[VFS_NAME_MAX + 1];
    uint32_t tpino, ino;
    uint8_t  ftype = 0, dirtype;
    uint8_t  inode[EXT2_INODE_MAX];
    int r;

    (void)fs;

    r = resolve_path(src, &ino);
    if (r != VFS_OK)
        return r;
    if (inode_read(ino, inode) != 0)
        return VFS_EIO;
    if ((rd16(inode) & EXT2_S_IFMT) == EXT2_S_IFDIR)
        return VFS_EPERM;

    if (split_path(dst, tparent, sizeof(tparent), tleaf, sizeof(tleaf)) != 0)
        return VFS_EINVAL;

    r = resolve_path(tparent, &tpino);
    if (r != VFS_OK)
        return r;

    if (dir_find(tpino, tleaf, (uint32_t)strlen(tleaf), NULL, &ftype) == VFS_OK)
        return VFS_EEXIST;

    dirtype = ((rd16(inode) & EXT2_S_IFMT) == EXT2_S_IFLNK)
                  ? EXT2_FT_SYMLINK : EXT2_FT_REG_FILE;

    r = dir_add(tpino, tleaf, (uint32_t)strlen(tleaf), ino, dirtype);
    if (r != VFS_OK)
        return r;

    wr16(inode + 26u, (uint16_t)(rd16(inode + 26u) + 1u));
    if (inode_write(ino, inode) != 0)
        return VFS_EIO;

    return ext2_fs_flush();
}

static const vfs_ops_t s_ext2_ops = {
    ext2_op_stat,
    ext2_op_readdir,
    ext2_op_read,
    ext2_op_write,
    ext2_op_mkdir,
    ext2_op_unlink,
    ext2_op_rmdir,
    ext2_op_rename,
    ext2_op_statfs,
    ext2_op_symlink,
    ext2_op_readlink,
    ext2_op_chmod,
    ext2_op_chown,
    ext2_op_link,
};

/* -------------------------------------------------------------- public API */

const vfs_ops_t* ext2_get_ops(void)
{
    return &s_ext2_ops;
}

void* ext2_get_fs(void)
{
    return NULL;
}

int ext2_init(uint32_t base_lba)
{
    uint32_t magic, log_block_size, rev_level;

    s_base_lba = base_lba;

    if (disk_read(s_base_lba + 2u, 2u, s_sb) != 0)
        return VFS_EIO;

    magic = rd16(s_sb + 56u);
    if (magic != EXT2_SUPER_MAGIC)
        return VFS_EIO;

    log_block_size = rd32(s_sb + 24u);
    if (log_block_size > 2u)
        return VFS_EIO;

    s_block_size = 1024u << log_block_size;
    s_sectors_per_block = s_block_size / 512u;

    s_inodes_count = rd32(s_sb + 0u);
    s_blocks_count = rd32(s_sb + 4u);
    s_free_blocks = rd32(s_sb + 12u);
    s_free_inodes = rd32(s_sb + 16u);
    s_first_data_block = rd32(s_sb + 20u);
    s_blocks_per_group = rd32(s_sb + 32u);
    s_inodes_per_group = rd32(s_sb + 40u);

    rev_level = rd32(s_sb + 76u);

    if (rev_level >= 1u) {
        s_first_ino = rd32(s_sb + 84u);
        s_inode_size = rd16(s_sb + 88u);
    } else {
        s_first_ino = 11u;
        s_inode_size = 128u;
    }

    if (s_inode_size < 128u || s_inode_size > EXT2_INODE_MAX)
        s_inode_size = 128u;
    if (s_first_ino < 11u || s_first_ino > s_inodes_count)
        s_first_ino = 11u;
    if (s_blocks_per_group == 0u || s_inodes_per_group == 0u)
        return VFS_EIO;

    if (s_blocks_count > s_blocks_per_group)
        s_groups = (s_blocks_count + s_blocks_per_group - 1u) / s_blocks_per_group;
    else
        s_groups = 1u;

    {
        uint32_t ig = (s_inodes_count + s_inodes_per_group - 1u) /
                      s_inodes_per_group;
        if (ig > s_groups)
            s_groups = ig;
    }

    if (s_groups == 0u || s_groups > EXT2_MAX_GROUPS)
        return VFS_EIO;

    if (gdt_load() != 0)
        return VFS_EIO;

    s_bb_group = EXT2_INVALID_BLOCK;
    s_ib_group = EXT2_INVALID_BLOCK;
    s_bb_dirty = 0;
    s_ib_dirty = 0;
    s_gdt_dirty = 0;

    return VFS_OK;
}
