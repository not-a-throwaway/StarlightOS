
.code16
.section .text
.global _start

# ------------------------------------------------------------
# FAT16 boot sector
# Bytes 0-61 = BPB
# Bytes 62-509 = boot code
# Bytes 510-511 = 55 AA
# ------------------------------------------------------------

    jmp _start
    nop

    .ascii "STARFAT "          # 8  OEM
    .word 512                  # 11 bytes/sector
    .byte 1                    # 13 sectors/cluster
    .word 1024                 # 14 reserved sectors
    .byte 2                    # 16 FATs
    .word 256                  # 17 root entries
    .word 0                    # 19 total sectors (use 32-bit field)
    .byte 0xf8                 # 21 media
    .word 128                  # 22 sectors/FAT
    .word 32                   # 24 sectors/track
    .word 64                   # 26 heads
    .long 0                    # 28 hidden sectors
    .long 32768                # 32 total sectors (16 MiB)
    .byte 0                    # 36 drive number
    .byte 0                    # 37 reserved
    .byte 0x29                 # 38 extended boot signature
    .long 0x12345678           # 39 volume ID
    .ascii "STARLIGHT  "       # 43 volume label
    .ascii "FAT16   "          # 54 filesystem type

# ------------------------------------------------------------
# BIOS bootloader starts at byte 62
# ------------------------------------------------------------

_start:
    cli
    cld

    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7c00, %sp

    # BIOS gives boot drive in DL.
    movb %dl, boot_drive

    # Load stage2 from LBA 1 with INT 13h Extensions. CHS geometry
    # comes from the BIOS and is not necessarily the BPB geometry.
    movw $dap, %si
    movb boot_drive, %dl
    movb $0x42, %ah
    int $0x13
    jc disk_error

    # Stage2 successfully loaded.
    ljmp $0x0000, $0x8000

disk_error:
    movw $msg_error, %si

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

boot_drive:
    .byte 0

msg_error:
    .asciz "StarlightOS: disk read failed."

.align 4
dap:
    .byte 0x10, 0
    .word 16
    .word 0x8000
    .word 0
    .long 1
    .long 0

# Pad the boot sector to byte 510.
