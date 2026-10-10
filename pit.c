#include "pit.h"

#define PIT_COMMAND   0x43
#define PIT_CHANNEL0  0x40

#define PIT_BASE_FREQUENCY 1193182

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

void pit_initialize(uint32_t frequency)
{
    if (frequency == 0) {
        frequency = 100;
    }

    uint32_t divisor = PIT_BASE_FREQUENCY / frequency;

    if (divisor < 1) {
        divisor = 1;
    }

    if (divisor > 65535) {
        divisor = 65535;
    }

    /*
     * Channel 0
     * Low byte / high byte
     * Mode 3: square wave
     * Binary mode
     */
    outb(PIT_COMMAND, 0x36);

    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);
}
