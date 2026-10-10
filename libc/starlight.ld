/* Linker script for StarlightOS user programs linked against newlib.
 * Same layout the kernel's ELF loader (process.c) expects: one R+X segment and one RW
 * segment inside the 0x40000000 window. Everything after .bss up to the stack is the
 * brk() heap, handed out by newlib's malloc via _sbrk(). */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
PHDRS
{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}
SECTIONS
{
  . = 0x40000000;
  .text : { *(.text .text.*) *(.rodata .rodata.*) } :text
  . = ALIGN(0x1000);
  .data : {
    *(.data .data.*)
    . = ALIGN(8);
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array))
    PROVIDE_HIDDEN(__preinit_array_end = .);
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    PROVIDE_HIDDEN(__init_array_end = .);
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } :data
  .bss : { *(COMMON) *(.bss .bss.*) } :data
  end = .;
  PROVIDE(_end = .);
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gcc_except_table*) }
}
