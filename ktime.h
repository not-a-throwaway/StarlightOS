#ifndef KTIME_H
#define KTIME_H
#include <stdint.h>

/* Monotonic time since boot. Microsecond resolution once the TSC has been
 * calibrated against the PIT (ktime_init); 10 ms resolution before that. */
void     ktime_init(void);
uint64_t ktime_us(void);
uint32_t ktime_ms(void);

/* Wall-clock helpers (CMOS RTC based). */
struct rtc_time;
uint32_t ktime_unix(void);                 /* seconds since 1970-01-01 UTC (RTC is treated as UTC + tz offset) */
void     ktime_set_unix(uint32_t secs);    /* writes the RTC (used by ntpdate) */
int      ktime_tz_offset_minutes(void);
void     ktime_set_tz_offset_minutes(int m);
void     ktime_unix_to_fields(uint32_t unix_secs, int *y, int *mo, int *d, int *h, int *mi, int *s, int *wday);
uint32_t ktime_fields_to_unix(int y, int mo, int d, int h, int mi, int s);

/* Memory size from CMOS (KiB), 0 if unknown. */
uint32_t ktime_cmos_ram_kb(void);

#endif
