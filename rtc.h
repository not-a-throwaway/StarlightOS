#ifndef RTC_H
#define RTC_H

#include <stdint.h>

struct rtc_time {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;

    uint8_t day;
    uint8_t month;
    uint16_t year;
};

/*
 * Reads the current date/time from the CMOS RTC. Handles BCD
 * conversion and re-reads until two consecutive samples agree,
 * so callers don't have to worry about tearing mid-update.
 */
void rtc_read(struct rtc_time* out);

#endif
