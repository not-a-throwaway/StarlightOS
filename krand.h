#ifndef KRAND_H
#define KRAND_H
#include <stddef.h>
/* Random bytes for TLS (key shares, client random). Pool seeded from RDRAND (when the CPU has it),
 * TSC jitter, the microsecond clock, the wall clock and the NIC MAC, whitened with SHA-256.
 * Good enough for a hobby OS; not a certified CSPRNG (QEMU's default CPU has no RDRAND). */
void krand_bytes(void *buf, size_t n);
#endif
