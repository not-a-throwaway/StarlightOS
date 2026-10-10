#include "speaker.h"
#include "ksched.h"

#define PIT_CHANNEL2  0x42
#define PIT_COMMAND   0x43
#define SPEAKER_PORT  0x61

#define PIT_BASE_FREQUENCY 1193182

extern uint32_t timer_get_ticks(void);

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

void speaker_on(uint32_t frequency)
{
    if (frequency == 0) {
        frequency = 440;
    }

    uint32_t divisor = PIT_BASE_FREQUENCY / frequency;

    if (divisor < 1) {
        divisor = 1;
    }

    if (divisor > 65535) {
        divisor = 65535;
    }

    /*
     * Channel 2, low/high byte, mode 3 (square wave), binary.
     */
    outb(PIT_COMMAND, 0xB6);

    outb(PIT_CHANNEL2, divisor & 0xFF);
    outb(PIT_CHANNEL2, (divisor >> 8) & 0xFF);

    /*
     * Bit 0: enable the PIT channel 2 gate.
     * Bit 1: connect it to the speaker.
     */
    uint8_t state = inb(SPEAKER_PORT);

    if (!(state & 0x03)) {
        outb(SPEAKER_PORT, state | 0x03);
    }
}

void speaker_off(void)
{
    uint8_t state = inb(SPEAKER_PORT);

    outb(SPEAKER_PORT, state & 0xFC);
}

void speaker_beep(uint32_t frequency, uint32_t duration_ticks)
{
    speaker_on(frequency);

    /* Yield-aware wait: the shell now runs in normal context with
     * interrupts enabled, so this no longer deadlocks, and Ctrl-C works. */
    ksleep_ms(duration_ticks * 10u);

    speaker_off();
}
