/* idt64.s
 *
 * 64-bit counterpart to idt.s. Same job (ISR/IRQ entry stubs that
 * save state, call into the C handler, restore state, return), but
 * two real architectural differences from the 32-bit version mean
 * it can't just be reassembled with wider mnemonics:
 *
 * 1. No pusha/popa. Those single-byte opcodes were sacrificed in
 *    long mode to make room for REX prefixes (same story as the
 *    single-byte INC/DEC r32 removal boot64.s's comments cover) --
 *    every register has to be pushed/popped individually.
 *
 * 2. Calling convention. 32-bit cdecl (idt.s) passes the handler's
 *    argument on the stack: pushl %esp; call; addl $4,%esp. SysV
 *    x86-64 passes the first argument in a register (rdi) instead --
 *    there is no stack push/cleanup dance for it at all.
 *
 * struct registers (kernel.c) only has two fields on this target --
 * int_no and err_code, both uint64_t -- since nothing in this
 * codebase ever reads the individual saved GPRs back out of it, only
 * int_no. They're still saved/restored below regardless: the
 * *interrupted* code needs them back exactly as they were, whether
 * or not the C handler happens to look at them.
 */

.section .text

.global idt_load
idt_load:
    /* First (only) argument arrives in rdi under SysV x86-64 --
     * no stack access needed at all, unlike idt.s's 4(%esp). */
    lidt (%rdi)
    ret


.macro ISR_NOERR num
.global isr\num
isr\num:
    pushq $0
    pushq $\num
    jmp isr_common
.endm


.macro ISR_ERR num
.global isr\num
isr\num:
    pushq $\num
    jmp isr_common
.endm


ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7

ISR_ERR 8

ISR_NOERR 9

ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14

ISR_NOERR 15
ISR_NOERR 16

ISR_ERR 17

ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

.global isr128
isr128:
    pushq $0
    pushq $128
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, %rdi
    call process_syscall
    mov %rax, 112(%rsp)

    cmpl $0, process_exit_pending(%rip)
    jne process_return_to_kernel

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    add $16, %rsp
    iretq


.macro IRQ num
.global irq\num
irq\num:
    pushq $0
    pushq $\num
    jmp irq_common
.endm


IRQ 0
IRQ 1
IRQ 2
IRQ 3
IRQ 4
IRQ 5
IRQ 6
IRQ 7
IRQ 8
IRQ 9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15


/* Pushes all 15 general-purpose registers other than rsp itself
 * (which the CPU already preserved in the interrupt frame beneath
 * all of this). 15 * 8 = 120 bytes -- struct registers {int_no,
 * err_code} sits right above this save area on the stack (pushed
 * by the isrN/irqN stub before jumping here), so lea 120(%rsp) is
 * exactly its address. */
isr_common:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    lea 120(%rsp), %rdi
    call isr_handler

    cmpl $0, process_exit_pending(%rip)
    jne process_return_to_kernel

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    add $16, %rsp   /* discard int_no + err_code (8 bytes each) */
    iretq


irq_common:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    lea 120(%rsp), %rdi
    call irq_handler

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    add $16, %rsp
    iretq


.section .note.GNU-stack,"",@progbits
