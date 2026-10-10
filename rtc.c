#include "rtc.h"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

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

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

static int rtc_update_in_progress(void)
{
    outb(CMOS_ADDRESS, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t value)
{
    return (uint8_t)((value & 0x0F) + ((value / 16) * 10));
}

static void rtc_read_raw(struct rtc_time* out)
{
    while (rtc_update_in_progress()) {
        /* spin */
    }

    out->second = cmos_read(0x00);
    out->minute = cmos_read(0x02);
    out->hour   = cmos_read(0x04);
    out->day    = cmos_read(0x07);
    out->month  = cmos_read(0x08);
    out->year   = cmos_read(0x09);
}

void rtc_read(struct rtc_time* out)
{
    struct rtc_time first;
    struct rtc_time second;

    /*
     * Read twice and compare, discarding a read that lands on
     * a tick boundary. Two matching reads in a row means we
     * caught a stable value.
     */
    do {
        rtc_read_raw(&first);
        rtc_read_raw(&second);
    } while (
        first.second != second.second ||
        first.minute != second.minute ||
        first.hour   != second.hour   ||
        first.day    != second.day    ||
        first.month  != second.month  ||
        first.year   != second.year
    );

    uint8_t status_b = cmos_read(0x0B);

    int is_binary = status_b & 0x04;
    int is_24hour = status_b & 0x02;

    uint8_t hour_raw = second.hour;
    int is_pm = 0;

    if (!is_24hour) {
        is_pm = hour_raw & 0x80;
        hour_raw &= 0x7F;
    }

    if (!is_binary) {
        out->second = bcd_to_bin(second.second);
        out->minute = bcd_to_bin(second.minute);
        out->hour   = bcd_to_bin(hour_raw);
        out->day    = bcd_to_bin(second.day);
        out->month  = bcd_to_bin(second.month);
        out->year   = (uint16_t)(2000 + bcd_to_bin((uint8_t)second.year));
    } else {
        out->second = second.second;
        out->minute = second.minute;
        out->hour   = hour_raw;
        out->day    = second.day;
        out->month  = second.month;
        out->year   = (uint16_t)(2000 + second.year);
    }

    if (!is_24hour) {
        if (is_pm && out->hour != 12) {
            out->hour = (uint8_t)(out->hour + 12);
        } else if (!is_pm && out->hour == 12) {
            out->hour = 0;
        }
    }
}
