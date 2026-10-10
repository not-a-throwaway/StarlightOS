/* linker.ld
 *
 * Kernel memory layout.
 */

ENTRY(_start)

SECTIONS
{
    /*
     * Load the kernel at 1 MiB.
     */
    . = 1M;

    /*
     * Multiboot header must be near the beginning
     * of the kernel image.
     */
    .multiboot : ALIGN(4K)
    {
        *(.multiboot)
    }

    /*
     * Executable code.
     */
    .text : ALIGN(4K)
    {
        *(.text)
    }

    /*
     * Read-only data.
     */
    .rodata : ALIGN(4K)
    {
        *(.rodata)
    }

    /*
     * Initialized writable data.
     */
    .data : ALIGN(4K)
    {
        *(.data)
    }

    /*
     * Uninitialized data / stack.
     */
    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    /* Kernel heap (NOLOAD, not part of the image). */
    .heap (NOLOAD) : ALIGN(4K)
    {
        *(.heap)
    }
}
