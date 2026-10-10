#include "gdt.h"

/*
 * One GDT entry is 8 bytes.
 *
 * Layout:
 *
 * 31                24 23       20 19                  16
 * +------------------+-----------+----------------------+
 * |    base 31:24    | flags     |    limit 19:16      |
 * +------------------+-----------+----------------------+
 * |      access      |       base 23:16                |
 * +------------------+----------------------------------+
 * |              base 15:0                            |
 * +----------------------------------------------------+
 * |              limit 15:0                            |
 * +----------------------------------------------------+
 */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;

    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

/*
 * GDTR:
 *
 * limit = size of GDT - 1
 * base  = address of GDT
 */
struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/*
 * Three descriptors:
 *
 * 0x00 = null descriptor
 * 0x08 = kernel code
 * 0x10 = kernel data
 */
static struct gdt_entry gdt[3];
static struct gdt_ptr gdt_descriptor;

/*
 * Implemented in gdt.s.
 */
extern void gdt_flush(uint32_t gdt_address);

/*
 * Fill one GDT descriptor.
 */
static void gdt_set_entry(
    int index,
    uint32_t base,
    uint32_t limit,
    uint8_t access,
    uint8_t granularity
) {
    gdt[index].base_low =
        (uint16_t)(base & 0xFFFF);

    gdt[index].base_middle =
        (uint8_t)((base >> 16) & 0xFF);

    gdt[index].base_high =
        (uint8_t)((base >> 24) & 0xFF);

    gdt[index].limit_low =
        (uint16_t)(limit & 0xFFFF);

    gdt[index].granularity =
        (uint8_t)((limit >> 16) & 0x0F);

    gdt[index].granularity |=
        granularity & 0xF0;

    gdt[index].access = access;
}

void gdt_initialize(void) {
    /*
     * Tell the CPU where our GDT is.
     */
    gdt_descriptor.limit =
        sizeof(gdt) - 1;

    gdt_descriptor.base =
        (uint32_t)&gdt;

    /*
     * Entry 0:
     *
     * Required null descriptor.
     */
    gdt_set_entry(
        0,
        0,
        0,
        0,
        0
    );

    /*
     * Entry 1:
     *
     * Kernel code segment.
     *
     * Selector = 0x08
     *
     * 0x9A:
     *   Present
     *   Ring 0
     *   Code
     *   Readable
     *
     * 0xCF:
     *   4 KiB granularity
     *   32-bit protected mode
     */
    gdt_set_entry(
        1,
        0,
        0xFFFFFFFF,
        0x9A,
        0xCF
    );

    /*
     * Entry 2:
     *
     * Kernel data segment.
     *
     * Selector = 0x10
     *
     * 0x92:
     *   Present
     *   Ring 0
     *   Data
     *   Writable
     */
    gdt_set_entry(
        2,
        0,
        0xFFFFFFFF,
        0x92,
        0xCF
    );

    /*
     * Actually load the GDT and reload
     * all segment registers.
     */
    gdt_flush(
        (uint32_t)&gdt_descriptor
    );
}
