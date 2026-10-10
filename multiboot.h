#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/*
 * Only the fields we actually use are named here. The real
 * structure has more (boot device, cmdline, module list, memory
 * map, framebuffer info, ...) but this project only needs the
 * "flags" word and the low/high memory sizes to answer `mem`.
 */
struct multiboot_info {
    uint32_t flags;

    uint32_t mem_lower;
    uint32_t mem_upper;

    uint32_t boot_device;
    uint32_t cmdline;

    uint32_t mods_count;
    uint32_t mods_addr;

    uint32_t syms[4];

    uint32_t mmap_length;
    uint32_t mmap_addr;
} __attribute__((packed));

#define MULTIBOOT_FLAG_MEM (1u << 0)

#endif
