.set MAGIC,    0x1BADB002
.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot, "a"
.align 4
    .long MAGIC
    .long FLAGS
    .long CHECKSUM

.section .bss
.align 16

multiboot_magic_saved:
    .skip 4

multiboot_info_saved:
    .skip 4

stack_bottom:
    .skip 65536

.align 16
stack_top:

.align 4096
p4_table:
    .global p4_table
    .skip 4096

p3_table:
    .global p3_table
    .skip 4096

p2_table:
    .skip 4096

p2_table1:
    .global p2_table1
    .skip 4096

p2_table2:
    .skip 4096

p2_table3:
    .skip 4096


.section .text
.code32

.global _start
.type _start, @function

_start:
    cli

    # Save Multiboot-style arguments from stage2.
    movl %eax, multiboot_magic_saved
    movl %ebx, multiboot_info_saved

    # 32-bit stack.
    movl $stack_top, %esp

    # CHECKPOINT 1: reached boot64 _start
    movl $0xb8000, %edi
    movw $0x1f31, (%edi)

    call check_cpuid

    # CHECKPOINT 2: CPUID works
    movl $0xb8000, %edi
    movw $0x1f32, 2(%edi)

    call check_long_mode

    # CHECKPOINT 3: long mode supported
    movl $0xb8000, %edi
    movw $0x1f33, 4(%edi)

    call set_up_page_tables

    # CHECKPOINT 4: page tables constructed
    movl $0xb8000, %edi
    movw $0x1f34, 6(%edi)

    call enable_paging

    # CHECKPOINT 5: paging enabled
    movl $0xb8000, %edi
    movw $0x1f35, 8(%edi)

    # Load 64-bit GDT.
    movl $gdt64_pointer, %eax
    lgdt (%eax)

    # CHECKPOINT 6: LGDT completed
    movl $0xb8000, %edi
    movw $0x1f36, 10(%edi)

    # Enter long mode with a 32-bit far jump.
    ljmp $0x08, $long_mode_start


check_cpuid:
    pushfl
    popl %eax

    movl %eax, %ecx

    xorl $(1 << 21), %eax

    pushl %eax
    popfl

    pushfl
    popl %eax

    pushl %ecx
    popfl

    cmpl %ecx, %eax
    je no_cpuid

    ret

no_cpuid:
    movb $'1', %al
    jmp boot_error


check_long_mode:
    movl $0x80000000, %eax
    cpuid

    cmpl $0x80000001, %eax
    jb no_long_mode

    movl $0x80000001, %eax
    cpuid

    testl $(1 << 29), %edx
    jz no_long_mode

    testl $(1 << 20), %edx
    jz no_nx

    ret

no_long_mode:
    movb $'2', %al
    jmp boot_error

no_nx:
    movb $'3', %al
    jmp boot_error


set_up_page_tables:
    # P4[0] -> P3
    movl $p3_table, %eax
    orl $0x03, %eax

    movl $p4_table, %edx
    movl %eax, (%edx)

    # P3[0] -> P2
    movl $p2_table, %eax
    orl $0x03, %eax

    movl $p3_table, %edx
    movl %eax, (%edx)

    # Connect four page directories for a 4 GiB identity map.
    movl $p2_table1, %eax
    orl $0x03, %eax
    movl %eax, 8(%edx)
    movl $p2_table2, %eax
    orl $0x03, %eax
    movl %eax, 16(%edx)
    movl $p2_table3, %eax
    orl $0x03, %eax
    movl %eax, 24(%edx)

    # Map the first 4 GiB with 2 MiB pages. QEMU's VBE LFB is
    # commonly at 0xfd000000, beyond the old 1 GiB identity map.
    xorl %ecx, %ecx

map_p2_entry:
    movl %ecx, %eax

    # eax = entry index * 2 MiB
    shll $21, %eax

    # Present | writable | huge page
    orl $0x83, %eax

    movl %ecx, %ebx
    shrl $9, %ebx
    shll $12, %ebx
    addl $p2_table, %ebx
    movl %ecx, %edi
    andl $511, %edi
    movl %eax, (%ebx,%edi,8)

    incl %ecx

    cmpl $2048, %ecx
    jne map_p2_entry

    ret


enable_paging:
    # CR3 = P4
    movl $p4_table, %eax
    movl %eax, %cr3

    # CR4.PAE = 1
    movl %cr4, %eax
    # PAE | OSFXSR | OSXMMEXCPT. The freestanding C build may use
    # SSE for ordinary structure copies/clears, so enabling long mode
    # without advertising OS support causes #UD on the first XMM op.
    orl $((1 << 5) | (1 << 9) | (1 << 10)), %eax
    movl %eax, %cr4

    # EFER.LME = 1
    movl $0xC0000080, %ecx
    rdmsr

    # EFER.LME | EFER.NXE: enable 64-bit mode and no-execute PTEs.
    orl $((1 << 8) | (1 << 11)), %eax

    wrmsr

    # CR0.PG = 1
    movl %cr0, %eax
    # CR0.PG | CR0.WP: supervisor writes also respect read-only pages.
    orl $((1 << 31) | (1 << 16)), %eax
    movl %eax, %cr0

    ret


boot_error:
    movl $0xb8000, %edi

    movl $0x4f524f45, (%edi)
    movl $0x4f3a4f52, 4(%edi)
    movl $0x4f204f20, 8(%edi)

    movb %al, 10(%edi)

    cli

1:
    hlt
    jmp 1b


.size _start, . - _start


.section .data

.align 8

gdt64:
    .quad 0x0000000000000000

    # 64-bit code:
    # Present
    # DPL 0
    # Code
    # Executable
    # Long mode
    .quad 0x00209A0000000000

    # 64-bit data:
    # Present
    # DPL 0
    # Writable
    .quad 0x0000920000000000

    # Ring 3 code/data descriptors. Selectors are 0x1b and 0x23.
    .quad 0x00affa000000ffff
    .quad 0x00cff2000000ffff

    # 64-bit TSS descriptor, filled by process_initialize().
    .global gdt64_tss_descriptor
gdt64_tss_descriptor:
    .quad 0
    .quad 0

gdt64_end:

gdt64_pointer:
    .word gdt64_end - gdt64 - 1
    .long gdt64


.section .text
.code64

.type long_mode_start, @function

long_mode_start:

    # CHECKPOINT 7: successfully entered 64-bit mode
    mov $0xb8000, %rdi
    movw $0x1f37, (%rdi)

    # Load valid data selector.
    mov $0x10, %eax

    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs

    # 64-bit stack.
    mov $stack_top, %rsp
    xor %ebp, %ebp

    # CHECKPOINT 8: segments + stack okay
    mov $0xb8000, %rdi
    movw $0x1f38, 2(%rdi)

    # Restore the arguments expected by kernel_main.
    mov multiboot_magic_saved, %edi
    mov multiboot_info_saved, %esi

    # CHECKPOINT 9: immediately before kernel_main
    mov $0xb8000, %rdi
    movw $0x1f39, 4(%rdi)

    call kernel_main

    # If kernel_main ever returns:
    mov $0xb8000, %rdi
    movw $0x1f52, 6(%rdi)

    cli

1:
    hlt
    jmp 1b


.size long_mode_start, . - long_mode_start


.section .note.GNU-stack,"",@progbits
