#include "ac97.h"
#include "pci.h"
#include "kstring.h"
#include "ksched.h"
#include "ktime.h"
#include "kprintf.h"
#include <stdint.h>

#define N_BUF        32u
#define FRAMES_BUF   1024u                    /* 21.3 ms per buffer at 48 kHz */
#define BUF_BYTES    (FRAMES_BUF * 4u)
#define LEAD         8u                       /* keep ~170 ms queued */

/* NAM (mixer) registers */
#define NAM_RESET     0x00
#define NAM_MASTER    0x02
#define NAM_PCM_OUT   0x18
#define NAM_EXT_ID    0x28
#define NAM_EXT_CTRL  0x2A
#define NAM_DAC_RATE  0x2C
#define NAM_VENDOR1   0x7C
#define NAM_VENDOR2   0x7E
/* NABM (bus master) PCM-out registers, relative to nabm_base + 0x10 */
#define PO_BDBAR 0x00
#define PO_CIV   0x04
#define PO_LVI   0x05
#define PO_SR    0x06
#define PO_PICB  0x08
#define PO_CR    0x0B
#define GLOB_CNT 0x2C
#define GLOB_STA 0x30

struct bdl_entry { uint32_t addr; uint16_t samples; uint16_t ctl; };

static struct bdl_entry bdl[N_BUF] __attribute__((aligned(8)));
static int16_t pcm[N_BUF][FRAMES_BUF * 2] __attribute__((aligned(4096)));
static struct ac97_info info;
static ac97_producer producer;
static unsigned fill_idx;
static volatile int in_pump;
static int ready;

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t  inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }

static uint16_t po(uint16_t reg) { return (uint16_t)(info.nabm_base + 0x10u + reg); }
static void nam_w(uint16_t r, uint16_t v) { outw((uint16_t)(info.nam_base + r), v); }
static uint16_t nam_r(uint16_t r) { return inw((uint16_t)(info.nam_base + r)); }

int ac97_ready(void) { return ready; }
int ac97_running(void) { return info.running; }
void ac97_set_producer(ac97_producer p) { producer = p; }
void ac97_get_info(struct ac97_info *o) { *o = info; }

const char *ac97_codec_name(void)
{
    /* The top three bytes of the vendor ID are ASCII (e.g. 'ADS' = Analog Devices). */
    static char name[32];
    char a = (char)(info.vendor_id >> 24), b = (char)(info.vendor_id >> 16), c = (char)(info.vendor_id >> 8);
    unsigned dev = info.vendor_id & 0xFF;
    const char *vendor = "unknown vendor";
    if (a == 'A' && b == 'D' && c == 'S') vendor = "Analog Devices";
    else if (a == 'A' && b == 'K' && c == 'M') vendor = "Asahi Kasei";
    else if (a == 'A' && b == 'L' && c == 'C') vendor = "Realtek ALC";
    else if (a == 'C' && b == 'M' && c == 'I') vendor = "C-Media";
    else if (a == 'C' && b == 'R' && c == 'Y') vendor = "Cirrus Logic";
    else if (a == 'S' && b == 'I' && c == 'L') vendor = "Silicon Labs";
    else if (a == 'V' && b == 'I' && c == 'A') vendor = "VIA";
    else if (a == 'T' && b == 'R' && c == 'A') vendor = "TriTech";
    else if (a == 'N' && b == 'S' && c == 'C') vendor = "National Semiconductor";
    if (vendor[0] == 'u') ksnprintf(name, sizeof(name), "codec id %08X", info.vendor_id);
    else ksnprintf(name, sizeof(name), "%s (rev %02X)", vendor, dev);
    return name;
}

void ac97_set_hw_volume(int percent)
{
    if (!ready) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* Attenuation is 5 bits at 1.5 dB per step. Map 100% -> 0, 0% -> mute. */
    unsigned att = percent == 0 ? 0x1F : (unsigned)((100 - percent) * 20 / 100);   /* up to -30 dB */
    uint16_t v = percent == 0 ? 0x8000u : (uint16_t)((att << 8) | att);
    nam_w(NAM_MASTER, v);
    nam_w(NAM_PCM_OUT, percent == 0 ? 0x8000u : 0x0000u);                          /* PCM at 0 dB: the software limiter provides headroom */
    info.master_att = (int)att;
}

int ac97_initialize(void)
{
    struct pci_addr dev;
    if (!pci_find_device(0x8086, 0x2415, &dev) &&           /* 82801AA  (QEMU "AC97") */
        !pci_find_device(0x8086, 0x2425, &dev) &&           /* 82801AB */
        !pci_find_device(0x8086, 0x2445, &dev) &&           /* 82801BA */
        !pci_find_device(0x8086, 0x24C5, &dev) &&           /* 82801DB */
        !pci_find_device(0x8086, 0x266E, &dev))             /* ICH6 */
        return -1;
    pci_enable_io_busmaster(dev);
    uint32_t mixer = pci_read_bar(dev, 0), bus = pci_read_bar(dev, 1);
    if (!(mixer & 1u) || !(bus & 1u)) return -1;
    info.nam_base = (uint16_t)(mixer & 0xFFFCu);
    info.nabm_base = (uint16_t)(bus & 0xFFFCu);
    if (!info.nam_base || !info.nabm_base) return -1;
    info.irq_line = (uint8_t)(pci_config_read32_public(dev, 0x3C) & 0xFF);

    /* Cold reset, then wait for the primary codec to report ready. */
    outl((uint16_t)(info.nabm_base + GLOB_CNT), 0x00000002u);
    for (volatile uint32_t i = 0; i < 200000; i++) { }
    int codec_ok = 0;
    for (uint32_t i = 0; i < 2000000u; i++) {
        if (inl((uint16_t)(info.nabm_base + GLOB_STA)) & 0x100u) { codec_ok = 1; break; }
    }
    if (!codec_ok) return -1;

    nam_w(NAM_RESET, 0x0000);
    for (volatile uint32_t i = 0; i < 100000; i++) { }
    uint16_t ext = nam_r(NAM_EXT_ID);
    if (ext & 1u) nam_w(NAM_EXT_CTRL, (uint16_t)(nam_r(NAM_EXT_CTRL) & ~1u));    /* VRA off: fixed 48 kHz */
    info.vendor_id = ((uint32_t)nam_r(NAM_VENDOR1) << 16) | nam_r(NAM_VENDOR2);

    /* Build the descriptor ring once: every entry points at its own buffer. */
    memset(pcm, 0, sizeof(pcm));
    for (unsigned i = 0; i < N_BUF; i++) {
        bdl[i].addr = (uint32_t)(uintptr_t)pcm[i];
        bdl[i].samples = (uint16_t)(FRAMES_BUF * 2);
        bdl[i].ctl = 0;
    }
    ready = 1;
    ac97_set_hw_volume(100);
    return 0;
}

static void fill_buffer(unsigned idx)
{
    if (producer) producer(pcm[idx], FRAMES_BUF);
    else memset(pcm[idx], 0, BUF_BYTES);
}

void ac97_start(void)
{
    if (!ready || info.running) return;
    outb(po(PO_CR), 0x02);                                  /* reset PCM-out channel */
    for (uint32_t i = 0; i < 100000u && (inb(po(PO_CR)) & 0x02); i++) { }
    outl(po(PO_BDBAR), (uint32_t)(uintptr_t)bdl);
    outw(po(PO_SR), 0x001C);
    in_pump = 1;                                            /* keep the timer-tick pump out while we prime */
    info.running = 1;                                       /* set first: priming may call back into us */
    fill_idx = 0;
    for (unsigned i = 0; i < LEAD; i++) fill_buffer(fill_idx++ % N_BUF);
    fill_idx %= N_BUF;
    outb(po(PO_LVI), (uint8_t)((fill_idx + N_BUF - 1) % N_BUF));
    outb(po(PO_CR), 0x01);                                  /* run */
    in_pump = 0;
}

void ac97_stop(void)
{
    if (!ready || !info.running) return;
    outb(po(PO_CR), 0x00);
    outw(po(PO_SR), 0x001C);
    info.running = 0;
}

void ac97_pump(void)
{
    if (!ready || !info.running || in_pump) return;
    in_pump = 1;
    uint16_t sr = inw(po(PO_SR));
    unsigned civ = inb(po(PO_CIV)) % N_BUF;
    if (sr & 0x0001u) {                                     /* DMA halted: we starved it */
        info.underruns++;
        outb(po(PO_CR), 0x00);
        outw(po(PO_SR), 0x001C);
        in_pump = 0;
        info.running = 0;
        ac97_start();
        return;
    }
    unsigned pending = (fill_idx + N_BUF - civ) % N_BUF;     /* buffers queued ahead of the playing one */
    unsigned filled = 0;
    while (pending < LEAD && filled < N_BUF - 2) {
        fill_buffer(fill_idx);
        fill_idx = (fill_idx + 1) % N_BUF;
        pending++; filled++;
        info.buffers_played++;
    }
    if (filled) outb(po(PO_LVI), (uint8_t)((fill_idx + N_BUF - 1) % N_BUF));
    if (sr & 0x1C) outw(po(PO_SR), (uint16_t)(sr & 0x1C));
    in_pump = 0;
}

/* Blocking diagnostic: used by `sound test` when the mixer is bypassed. */
static unsigned tone_phase;
static void tone_producer(int16_t *f, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        unsigned p = tone_phase++ % (AC97_RATE / 440u);
        int32_t s = (int32_t)p * 2 * 12000 / (int32_t)(AC97_RATE / 440u) - 12000;     /* saw */
        f[i * 2] = f[i * 2 + 1] = (int16_t)s;
    }
}

int ac97_play_test_tone(void)
{
    if (!ready) return -1;
    ac97_producer saved = producer;
    ac97_stop();
    tone_phase = 0;
    producer = tone_producer;
    ac97_start();
    ksleep_ms(1000);
    ac97_stop();
    producer = saved;
    return 0;
}
