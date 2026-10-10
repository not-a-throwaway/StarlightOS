/* StarlightOS x86_64 linker script: BIOS-loaded kernel at 1 MiB. */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .multiboot : ALIGN(4K)
    {
        *(.multiboot)
    }

    .text : ALIGN(4K)
    {
        *(.text)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata)
    }

    .data : ALIGN(4K)
    {
        *(.data)
    }

    .bss : ALIGN(4K)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        __bss_end = .;
    }

    /* Kernel heap + large framebuffers: NOLOAD, so it is neither stored in
     * the boot image nor zeroed by the loader. */
    .heap (NOLOAD) : ALIGN(4K)
    {
        *(.heap)
    }

    __kernel_end = .;
}
