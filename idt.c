#include "idt.h"

#define IDT_ENTRIES 256

#if defined(__x86_64__)

/* 64-bit interrupt gate descriptor -- 16 bytes, not 8. The extra
 * room over the 32-bit format is base_high (bits 32-63 of the
 * handler address -- a 32-bit-only base can't reach a 64-bit
 * address) plus a reserved dword. `ist` selects an alternate stack
 * from the Interrupt Stack Table in the TSS for critical handlers
 * (double fault, NMI) that can't safely run on a stack that might
 * itself be the reason they fired -- unused here (0) since this
 * kernel doesn't set up a TSS/IST yet; every handler runs on
 * whatever stack was active, same as the 32-bit build always did. */
struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t base_mid;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idt_descriptor;

static void idt_set_gate(uint8_t num, uint64_t base)
{
    idt[num].base_low = base & 0xFFFF;
    idt[num].base_mid = (base >> 16) & 0xFFFF;
    idt[num].base_high = (base >> 32) & 0xFFFFFFFF;

    idt[num].selector = 0x08;
    idt[num].ist = 0;
    idt[num].flags = 0x8E;
    idt[num].reserved = 0;
}

static void idt_zero_gate(uint8_t num)
{
    idt[num].base_low = 0;
    idt[num].base_mid = 0;
    idt[num].base_high = 0;
    idt[num].selector = 0;
    idt[num].ist = 0;
    idt[num].flags = 0;
    idt[num].reserved = 0;
}

#else

struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idt_descriptor;

static void idt_set_gate(uint8_t num, uint32_t base)
{
    idt[num].base_low = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;

    idt[num].selector = 0x08;
    idt[num].zero = 0;
    idt[num].flags = 0x8E;
}

static void idt_zero_gate(uint8_t num)
{
    idt[num].base_low = 0;
    idt[num].base_high = 0;
    idt[num].selector = 0;
    idt[num].zero = 0;
    idt[num].flags = 0;
}

#endif

extern void idt_load(struct idt_ptr *ptr);

extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);
#if defined(__x86_64__)
extern void isr128(void);
#endif

void idt_initialize(void)
{
    idt_descriptor.limit = sizeof(idt) - 1;
    idt_descriptor.base = (uintptr_t)&idt;

    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_zero_gate((uint8_t)i);
    }

    /* CPU exceptions */
    idt_set_gate(0,  (uintptr_t)isr0);
    idt_set_gate(1,  (uintptr_t)isr1);
    idt_set_gate(2,  (uintptr_t)isr2);
    idt_set_gate(3,  (uintptr_t)isr3);
    idt_set_gate(4,  (uintptr_t)isr4);
    idt_set_gate(5,  (uintptr_t)isr5);
    idt_set_gate(6,  (uintptr_t)isr6);
    idt_set_gate(7,  (uintptr_t)isr7);
    idt_set_gate(8,  (uintptr_t)isr8);
    idt_set_gate(9,  (uintptr_t)isr9);
    idt_set_gate(10, (uintptr_t)isr10);
    idt_set_gate(11, (uintptr_t)isr11);
    idt_set_gate(12, (uintptr_t)isr12);
    idt_set_gate(13, (uintptr_t)isr13);
    idt_set_gate(14, (uintptr_t)isr14);
    idt_set_gate(15, (uintptr_t)isr15);
    idt_set_gate(16, (uintptr_t)isr16);
    idt_set_gate(17, (uintptr_t)isr17);
    idt_set_gate(18, (uintptr_t)isr18);
    idt_set_gate(19, (uintptr_t)isr19);
    idt_set_gate(20, (uintptr_t)isr20);
    idt_set_gate(21, (uintptr_t)isr21);
    idt_set_gate(22, (uintptr_t)isr22);
    idt_set_gate(23, (uintptr_t)isr23);
    idt_set_gate(24, (uintptr_t)isr24);
    idt_set_gate(25, (uintptr_t)isr25);
    idt_set_gate(26, (uintptr_t)isr26);
    idt_set_gate(27, (uintptr_t)isr27);
    idt_set_gate(28, (uintptr_t)isr28);
    idt_set_gate(29, (uintptr_t)isr29);
    idt_set_gate(30, (uintptr_t)isr30);
    idt_set_gate(31, (uintptr_t)isr31);

    /* Hardware IRQs */
    idt_set_gate(32, (uintptr_t)irq0);
    idt_set_gate(33, (uintptr_t)irq1);
    idt_set_gate(34, (uintptr_t)irq2);
    idt_set_gate(35, (uintptr_t)irq3);
    idt_set_gate(36, (uintptr_t)irq4);
    idt_set_gate(37, (uintptr_t)irq5);
    idt_set_gate(38, (uintptr_t)irq6);
    idt_set_gate(39, (uintptr_t)irq7);
    idt_set_gate(40, (uintptr_t)irq8);
    idt_set_gate(41, (uintptr_t)irq9);
    idt_set_gate(42, (uintptr_t)irq10);
    idt_set_gate(43, (uintptr_t)irq11);
    idt_set_gate(44, (uintptr_t)irq12);
    idt_set_gate(45, (uintptr_t)irq13);
    idt_set_gate(46, (uintptr_t)irq14);
    idt_set_gate(47, (uintptr_t)irq15);

#if defined(__x86_64__)
    /* User-callable syscall trap (DPL 3, present, 64-bit interrupt gate). */
    idt_set_gate(128, (uintptr_t)isr128);
    idt[128].flags = 0xEE;
#endif

    idt_load(&idt_descriptor);
}
