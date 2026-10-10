#include "serial.h"
#include <stdint.h>

#define COM1 0x3F8
static int present;

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }

void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);       /* DLAB on */
    outb(COM1 + 0, 0x01);       /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);       /* 8N1 */
    outb(COM1 + 2, 0xC7);       /* enable + clear FIFOs */
    outb(COM1 + 4, 0x1E);       /* loopback self-test */
    outb(COM1 + 0, 0xAE);
    if (inb(COM1 + 0) != 0xAE) { present = 0; return; }
    outb(COM1 + 4, 0x0F);       /* normal operation, OUT1/OUT2 set */
    present = 1;
}

int serial_present(void) { return present; }

void serial_putc(char c)
{
    if (!present) return;
    for (int i = 0; i < 20000; i++)
        if (inb(COM1 + 5) & 0x20) break;
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) { while (*s) { if (*s == '\n') serial_putc('\r'); serial_putc(*s++); } }

int serial_getc(void)
{
    if (!present || !(inb(COM1 + 5) & 1)) return -1;
    return inb(COM1);
}
