.section .text
.global process_enter_user
.type process_enter_user,@function
process_enter_user:
    # Save the kernel caller's context and interrupt state for synchronous exit.
    pushfq
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, process_saved_rsp(%rip)

    # Arguments were copied to globals before this routine is entered.
    # process_user_stack already points at argc/argv (built by process.c, rsp = 8 mod 16).
    mov process_user_stack(%rip), %rax
    pushq $0x23                 # user SS
    push %rax                   # user RSP
    pushq $0x202                # user RFLAGS: reserved bit + interrupts enabled
    pushq $0x1b                 # user CS
    push process_user_entry(%rip)
    iretq

.global process_return_to_kernel
.type process_return_to_kernel,@function
process_return_to_kernel:
    cli
    mov process_saved_rsp(%rip), %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    popfq
    ret

.section .note.GNU-stack,"",@progbits
