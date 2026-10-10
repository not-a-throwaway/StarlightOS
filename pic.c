#include "pic.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21

#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define PIC_EOI      0x20

#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01

#define ICW4_8086    0x01

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

static void io_wait(void)
{
    outb(0x80, 0);
}

void pic_remap(void)
{
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /*
     * Begin initialization.
     */
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /*
     * Master PIC:
     * IRQ0-7 -> interrupt vectors 32-39
     */
    outb(PIC1_DATA, 0x20);
    io_wait();

    /*
     * Slave PIC:
     * IRQ8-15 -> interrupt vectors 40-47
     */
    outb(PIC2_DATA, 0x28);
    io_wait();

    /*
     * Master IRQ2 is connected to the slave.
     */
    outb(PIC1_DATA, 0x04);
    io_wait();

    /*
     * Tell the slave it is connected to IRQ2.
     */
    outb(PIC2_DATA, 0x02);
    io_wait();

    /*
     * 8086/88 mode.
     */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();

    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /*
     * Restore previous interrupt masks.
     */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }

    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_clear_mask(uint8_t irq)
{
    uint16_t port;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        irq -= 8;
        port = PIC2_DATA;
    }

    uint8_t mask = inb(port);

    mask &= ~(1u << irq);

    outb(port, mask);
}

void pic_set_mask(uint8_t irq)
{
    uint16_t port;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        irq -= 8;
        port = PIC2_DATA;
    }

    uint8_t mask = inb(port);

    mask |= (1u << irq);

    outb(port, mask);
}
