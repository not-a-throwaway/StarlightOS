/* gdt.s
 *
 * Loads our GDT and reloads all segment registers.
 */

.section .text

.global gdt_flush
.type gdt_flush, @function

gdt_flush:
    /*
     * cdecl:
     *
     * [esp]     = return address
     * [esp + 4] = first argument
     *
     * The argument is the address of our GDTR.
     */
    mov 4(%esp), %eax

    /*
     * Load GDTR.
     */
    lgdt (%eax)

    /*
     * CS cannot be changed with MOV.
     *
     * Perform a far jump so the CPU loads
     * the new kernel code selector.
     *
     * 0x08 = GDT entry 1.
     */
    ljmp $0x08, $flush_cs

flush_cs:

    /*
     * 0x10 = GDT entry 2,
     * our kernel data segment.
     */
    mov $0x10, %ax

    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    ret

.size gdt_flush, . - gdt_flush

.section .note.GNU-stack,"",@progbits
