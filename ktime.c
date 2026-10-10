#include "ktime.h"
#include "rtc.h"

extern uint32_t timer_get_ticks(void);

static uint64_t tsc_per_us;       /* 0 = uncalibrated */
static uint64_t tsc_base;
static int tz_minutes;

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }

void ktime_init(void)
{
    /* Needs interrupts enabled and the PIT running at 100 Hz. Measure
     * 5 full ticks (50 ms) edge-to-edge. */
    uint32_t t = timer_get_ticks();
    while (timer_get_ticks() == t) __asm__ volatile("pause");
    uint32_t start_tick = timer_get_ticks();
    uint64_t a = rdtsc();
    while (timer_get_ticks() - start_tick < 5) __asm__ volatile("pause");
    uint64_t b = rdtsc();
    uint64_t per_us = (b - a) / 50000u;
    if (per_us >= 50 && per_us <= 20000) {        /* 50 MHz .. 20 GHz sanity window */
        tsc_per_us = per_us;
        tsc_base = rdtsc() - (uint64_t)timer_get_ticks() * 10000u * per_us;
    }
}

uint64_t ktime_us(void)
{
    if (!tsc_per_us) return (uint64_t)timer_get_ticks() * 10000u;
    return (rdtsc() - tsc_base) / tsc_per_us;
}

uint32_t ktime_ms(void) { return (uint32_t)(ktime_us() / 1000u); }

/* ---- calendar ---- */

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const uint8_t mdays[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };

uint32_t ktime_fields_to_unix(int y, int mo, int d, int h, int mi, int s)
{
    uint32_t days = 0;
    for (int yy = 1970; yy < y; yy++) days += is_leap(yy) ? 366u : 365u;
    for (int m = 1; m < mo; m++) days += mdays[m - 1] + (m == 2 && is_leap(y) ? 1u : 0u);
    days += (uint32_t)(d - 1);
    return days * 86400u + (uint32_t)h * 3600u + (uint32_t)mi * 60u + (uint32_t)s;
}

void ktime_unix_to_fields(uint32_t t, int *y, int *mo, int *d, int *h, int *mi, int *s, int *wday)
{
    uint32_t days = t / 86400u, rem = t % 86400u;
    if (h) *h = (int)(rem / 3600u);
    if (mi) *mi = (int)((rem % 3600u) / 60u);
    if (s) *s = (int)(rem % 60u);
    if (wday) *wday = (int)((days + 4u) % 7u);     /* 1970-01-01 was a Thursday */
    int yy = 1970;
    for (;;) {
        uint32_t n = is_leap(yy) ? 366u : 365u;
        if (days < n) break;
        days -= n; yy++;
    }
    int m = 0;
    for (; m < 12; m++) {
        uint32_t n = mdays[m] + (m == 1 && is_leap(yy) ? 1u : 0u);
        if (days < n) break;
        days -= n;
    }
    if (y) *y = yy;
    if (mo) *mo = m + 1;
    if (d) *d = (int)days + 1;
}

int ktime_tz_offset_minutes(void) { return tz_minutes; }
void ktime_set_tz_offset_minutes(int m) { tz_minutes = m; }

uint32_t ktime_unix(void)
{
    struct rtc_time t;
    rtc_read(&t);
    uint32_t local = ktime_fields_to_unix(t.year, t.month, t.day, t.hour, t.minute, t.second);
    return local - (uint32_t)(tz_minutes * 60);
}

static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static uint8_t cmos_read(uint8_t reg) { outb(0x70, reg); return inb(0x71); }
static void cmos_write(uint8_t reg, uint8_t v) { outb(0x70, reg); outb(0x71, v); }

void ktime_set_unix(uint32_t secs)
{
    int y, mo, d, h, mi, s;
    ktime_unix_to_fields(secs + (uint32_t)(tz_minutes * 60), &y, &mo, &d, &h, &mi, &s, 0);
    uint8_t regb = cmos_read(0x0B);
    int bin = regb & 0x04;
    while (cmos_read(0x0A) & 0x80) { }
    cmos_write(0x0B, (uint8_t)(regb | 0x80));          /* halt updates */
    cmos_write(0x00, bin ? (uint8_t)s  : to_bcd(s));
    cmos_write(0x02, bin ? (uint8_t)mi : to_bcd(mi));
    cmos_write(0x04, bin ? (uint8_t)h  : to_bcd(h));   /* 24-hour mode assumed (QEMU default) */
    cmos_write(0x07, bin ? (uint8_t)d  : to_bcd(d));
    cmos_write(0x08, bin ? (uint8_t)mo : to_bcd(mo));
    cmos_write(0x09, bin ? (uint8_t)(y % 100) : to_bcd(y % 100));
    cmos_write(0x32, bin ? (uint8_t)(y / 100) : to_bcd(y / 100));
    cmos_write(0x0B, regb);                            /* resume */
}

uint32_t ktime_cmos_ram_kb(void)
{
    uint32_t low = (uint32_t)cmos_read(0x30) | ((uint32_t)cmos_read(0x31) << 8);   /* KiB above 1 MiB (BIOS caps it) */
    uint32_t high = (uint32_t)cmos_read(0x34) | ((uint32_t)cmos_read(0x35) << 8);  /* 64 KiB units above 16 MiB */
    if (high) return 16u * 1024u + high * 64u;      /* memory above 16 MiB is authoritative; below it is all present */
    if (!low) return 0;
    return 1024u + low;
}
