.code16
.section .text
.global _start

_start:
    cli
    cld

    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %ss
    movw $0x9000, %sp

    # Preserve BIOS boot drive.
    movb %dl, boot_drive

    # Initial kernel load address: 0x1000:0000 = 0x10000.
    movw $0x1000, kernel_segment

load_kernel:
    movw kernel_sectors, %cx
    testw %cx, %cx
    jz kernel_loaded

    # Read at most 64 sectors per BIOS request.
    cmpw $64, %cx
    jbe chunk_ok
    movw $64, %cx

chunk_ok:
    movw %cx, dap_count

    # BIOS buffer = kernel_segment:0000
    movw kernel_segment, %ax
    movw %ax, dap_segment
    movw $0, dap_offset

    # Current LBA.
    movl kernel_lba, %eax
    movl %eax, dap_lba
    movl $0, dap_lba+4

    # INT 13h Extensions, AH=42h.
    movw $dap, %si
    movb $0x42, %ah
    movb boot_drive, %dl
    int $0x13
    jc disk_error

    # Subtract sectors successfully read.
    movw dap_count, %ax
    subw %ax, kernel_sectors

    # Advance memory buffer by sectors * 512 bytes.
    # Segment increment = sectors * 32.
    shlw $5, %ax
    addw %ax, kernel_segment

    # Advance LBA.
    movzwl dap_count, %eax
    addl %eax, kernel_lba

    jmp load_kernel


kernel_loaded:
    # Enable A20 using the fast gate.
    inb $0x92, %al
    orb $0x02, %al
    andb $0xfe, %al
    outb %al, $0x92

    # Load GDT.
    lgdt gdt_desc

    # Enter protected mode.
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0

    ljmp $0x08, $protected_mode


disk_error:
    movw $msg_disk, %si

print:
    lodsb
    testb %al, %al
    jz halt

    movb $0x0e, %ah
    xorw %bx, %bx
    int $0x10
    jmp print


halt:
    cli
1:
    hlt
    jmp 1b


.code32
protected_mode:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movl $0x90000, %esp

    # Copy kernel from 0x10000 to linked address 0x100000.
    movl $0x10000, %esi
    movl $0x100000, %edi
    movl kernel_bytes, %ecx
    rep movsb

    # Clear .bss.
    movl bss_start, %edi
    movl bss_end, %ecx
    subl %edi, %ecx
    xorl %eax, %eax
    rep stosb

    # Multiboot-compatible handoff.
    #
    # boot64.s expects:
    #   EAX = 0x2BADB002
    #   EBX = pointer to Multiboot info
    #
    # Do NOT overwrite EAX with the jump address.
    movl $0x2BADB002, %eax
    movl $boot_info, %ebx

    # _start is 0x101000 with the current linker layout.
    # Use EDX so EAX survives the jump.
    movl $0x101000, %edx
    jmp *%edx


.align 4
boot_info:
    .long 0
    .long 0
    .long 0
    .long 0
    .long 0
    .long 0
    .long 0
    .long 0
    .long 0
    .long 0
    .long 0
    .long 0
    .long 0


.align 8
gdt:
    .quad 0
    .quad 0x00cf9a000000ffff
    .quad 0x00cf92000000ffff
gdt_end:

gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt


boot_drive:
    .byte 0

.align 2
kernel_segment:
    .word 0


# BIOS INT 13h Extensions Disk Address Packet.
#
# Offset  Size  Meaning
# 00      1     Packet size = 0x10
# 01      1     Reserved
# 02      2     Sector count
# 04      2     Buffer offset
# 06      2     Buffer segment
# 08      4     LBA low
# 0C      4     LBA high
#
# Exactly 16 bytes.
.align 2
dap:
    .byte 0x10
    .byte 0x00
dap_count:
    .word 0
dap_offset:
    .word 0
dap_segment:
    .word 0
dap_lba:
    .long 0
    .long 0


# Patched by make_disk.py.

.ascii "KSEC"
kernel_sectors:
    .word 0

.ascii "KLBA"
kernel_lba:
    .long 0

.ascii "KBYT"
kernel_bytes:
    .long 0

.ascii "BSSS"
bss_start:
    .long 0

.ascii "BSSE"
bss_end:
    .long 0


msg_disk:
    .asciz "StarlightOS: kernel disk read failed.\r\n"


.section .note.GNU-stack,"",@progbits
