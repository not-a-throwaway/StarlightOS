/*
 * krand.c -- entropy pool + SHA-256 based generator (see krand.h).
 */
#include "krand.h"
#include "crypto.h"
#include "ktime.h"
#include "net.h"
#include "kstring.h"

static uint8_t pool[32];
static uint32_t out_counter;

static inline uint64_t rdtsc64(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int cpu_has_rdrand(void)
{
    uint32_t a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return (int)((c >> 30) & 1u);
}

static int rdrand32(uint32_t *v)
{
    for (int i = 0; i < 10; i++) {
        uint32_t x; unsigned char ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(x), "=qm"(ok) : : "cc");
        if (ok) { *v = x; return 1; }
    }
    return 0;
}

static void mix(const void *data, size_t n)
{
    struct sha256_ctx c;
    sha256_init(&c); sha256_update(&c, pool, sizeof(pool)); sha256_update(&c, data, n); sha256_final(&c, pool);
}

static void gather(void)
{
    uint64_t jitter[64];
    for (int i = 0; i < 64; i++) {                    /* TSC deltas around short loops: interrupt/cache/emulator jitter */
        uint64_t t0 = rdtsc64();
        for (volatile int k = 0; k < 40 + (i & 7); k++) { }
        jitter[i] = (rdtsc64() - t0) ^ (t0 << 13);
    }
    mix(jitter, sizeof(jitter));

    uint64_t t[5];
    t[0] = rdtsc64(); t[1] = ktime_us(); t[2] = ktime_unix(); t[3] = out_counter; t[4] = (uint64_t)(uintptr_t)&t;
    mix(t, sizeof(t));
    mix(net_get_config()->mac, 6);

    static int rd = -1;
    if (rd < 0) rd = cpu_has_rdrand();
    if (rd) {
        uint32_t r[8]; int got = 0;
        for (int i = 0; i < 8; i++) got += rdrand32(&r[i]);
        if (got) mix(r, sizeof(r));
    }
}

void krand_bytes(void *buf, size_t n)
{
    uint8_t *o = (uint8_t *)buf;
    gather();
    while (n) {
        uint8_t blk[32], in[36];
        memcpy(in, pool, 32);
        in[32] = (uint8_t)(out_counter >> 24); in[33] = (uint8_t)(out_counter >> 16);
        in[34] = (uint8_t)(out_counter >> 8);  in[35] = (uint8_t)out_counter;
        out_counter++;
        sha256(in, sizeof(in), blk);
        size_t k = n < 32 ? n : 32;
        memcpy(o, blk, k); o += k; n -= k;
    }
    uint8_t next[33]; memcpy(next, pool, 32); next[32] = 0xA5;     /* ratchet so past output can't be recomputed from the pool */
    sha256(next, sizeof(next), pool);
}
