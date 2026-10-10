.section .text

.extern isr_handler
.extern irq_handler

.global idt_load
idt_load:
    movl 4(%esp), %eax
    lidt (%eax)
    ret


.macro ISR_NOERR num
.global isr\num
isr\num:
    pushl $0
    pushl $\num
    jmp isr_common
.endm


.macro ISR_ERR num
.global isr\num
isr\num:
    pushl $\num
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


.macro IRQ num
.global irq\num
irq\num:
    pushl $0
    pushl $\num
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


isr_common:
    pusha

    pushl %esp
    call isr_handler
    addl $4, %esp

    popa

    addl $8, %esp
    iret


irq_common:
    pusha

    pushl %esp
    call irq_handler
    addl $4, %esp

    popa

    addl $8, %esp
    iret


.section .note.GNU-stack,"",@progbits
