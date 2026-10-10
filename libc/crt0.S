/* StarlightOS user-mode C runtime entry (for newlib programs).
 *
 * The kernel enters here with rsp -> argc, then argv[0..argc-1], NULL, envp (NULL),
 * exactly like the SysV x86-64 process entry (rsp is 8 mod 16 rather than 16-aligned,
 * which we fix below). */
    .section .text
    .global _start
    .type _start, @function
_start:
    xor   %ebp, %ebp
    mov   (%rsp), %rdi              /* argc            */
    lea   8(%rsp), %rsi             /* argv            */
    lea   16(%rsp,%rdi,8), %rdx     /* envp (= NULL-terminated, empty) */
    and   $-16, %rsp
    mov   %rdx, environ(%rip)
    call  __starlight_start         /* C: init libc, run main, exit */
    ud2

/* newlib's __libc_init_array calls these around the .init_array entries. */
    .global _init
    .type _init, @function
_init:
    ret
    .global _fini
    .type _fini, @function
_fini:
    ret

    .section .note.GNU-stack,"",@progbits
