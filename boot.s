/* boot.s
 *
 * This is the very first code that runs. GRUB loads this binary into
 * memory and jumps to _start. Its two jobs are:
 *
 *   1. Contain a "Multiboot header" so GRUB recognizes this file as a
 *      loadable kernel (this must live in the first 8KB of the file).
 *   2. Set up a stack, then call into our C kernel_main().
 *
 * The CPU has no working C runtime yet -- no stack, no globals
 * guaranteed valid -- so this part has to be hand-written assembly.
 */

/* ---- Multiboot header constants (Multiboot 1 spec) ---- */
.set ALIGN,    1<<0             /* align loaded modules on page boundaries */
.set MEMINFO,  1<<1             /* provide memory map to the kernel */
.set FLAGS,    ALIGN | MEMINFO  /* Multiboot 'flag' field */
.set MAGIC,    0x1BADB002       /* magic number GRUB looks for */
.set CHECKSUM, -(MAGIC + FLAGS) /* must make (magic+flags+checksum) == 0 */

/* The Multiboot header itself. It must be 4-byte aligned and within
 * the first 8KB of the kernel file, so we put it in its own section
 * and place that section first in the linker script. */
.section .multiboot, "a"
.align 4
    .long MAGIC
    .long FLAGS
    .long CHECKSUM

/* ---- Stack ----
 * x86 requires a valid stack before you can call any C function.
 * We reserve 16KB of uninitialized memory in .bss and point the
 * stack pointer at the top of it (stacks grow downward). */
.section .bss
.align 16
stack_bottom:
    .skip 16384   /* 16 KiB */
stack_top:

/* ---- Entry point ---- */
.section .text
.global _start
.type _start, @function
_start:
    /* GRUB has placed us in 32-bit protected mode already, with
     * paging disabled and interrupts disabled. We still must set
     * up our own stack before touching any C code.
     *
     * GRUB also leaves two values sitting in registers when it
     * jumps here:
     *   EAX = the Multiboot magic number (0x2BADB002)
     *   EBX = physical address of the Multiboot info structure
     * We stash both before ESP moves and pass them straight
     * through to kernel_main so the kernel can read the real
     * memory map GRUB detected instead of guessing. */
    mov %eax, %edi
    mov %ebx, %esi

    mov $stack_top, %esp

    /* (Optional but good practice) reset EFLAGS. */
    pushl $0
    popf

    /* cdecl: push arguments right-to-left. */
    pushl %esi   /* mbi_addr (2nd arg) */
    pushl %edi   /* magic    (1st arg) */

    /* Hand off to the C kernel. */
    call kernel_main

    /* kernel_main should never return, but if it somehow does,
     * halt the CPU forever rather than executing garbage. */
    cli
1:  hlt
    jmp 1b

.size _start, . - _start

/* Tell the linker we don't need an executable stack (pure convention/
 * silences a linker warning; has no effect on boot behavior here). */
.section .note.GNU-stack,"",@progbits
