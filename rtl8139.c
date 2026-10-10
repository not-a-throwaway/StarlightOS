#include "rtl8139.h"
#include "pci.h"
#include "kstring.h"
#include "ktime.h"

/*
 * Realtek RTL8139 Fast Ethernet driver (polled).
 *
 * Register map used here:
 *   0x00 IDR0-5   MAC           0x30 RBSTART  RX ring physical address
 *   0x10 TSD0-3   TX status     0x37 CR       command (RST/RE/TE/BUFE)
 *   0x20 TSAD0-3  TX address    0x38 CAPR     RX read pointer (minus 16)
 *   0x3C IMR / 0x3E ISR         0x40 TCR      transmit config
 *   0x44 RCR      receive config  0x52 CONFIG1 power on   0x58 MSR media status
 */
#define REG_IDR0     0x00
#define REG_TSD0     0x10
#define REG_TSAD0    0x20
#define REG_RBSTART  0x30
#define REG_CR       0x37
#define REG_CAPR     0x38
#define REG_CBR      0x3A
#define REG_IMR      0x3C
#define REG_ISR      0x3E
#define REG_TCR      0x40
#define REG_RCR      0x44
#define REG_MPC      0x4C
#define REG_CONFIG1  0x52
#define REG_MSR      0x58

#define CR_BUFE 0x01
#define CR_TE   0x04
#define CR_RE   0x08
#define CR_RST  0x10

#define TSD_OWN  0x2000u   /* set by NIC when DMA finished */
#define TSD_TOK  0x8000u
#define TSD_TUN  0x4000u
#define TSD_TABT 0x40000000u

#define RX_BYTES 32768u
#define RX_MASK  (RX_BYTES - 1u)
#define TX_BYTES 1792u

static uint8_t rx_ring[RX_BYTES + 16 + 2048] __attribute__((aligned(256)));
static uint8_t tx_buffers[4][TX_BYTES] __attribute__((aligned(16)));
static uint8_t mac[6];
static uint16_t io;
static uint32_t rx_offset;
static uint8_t tx_slot;
static uint8_t tx_used[4];
static int ready, irq_line = -1;
static struct rtl8139_stats stats;

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t  inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }

int rtl8139_initialize(void)
{
    struct pci_addr dev;
    if (!pci_find_device(0x10EC, 0x8139, &dev)) return -1;
    pci_enable_io_busmaster(dev);

    uint32_t bar = pci_read_bar(dev, 0);
    if (!(bar & 1u)) bar = pci_read_bar(dev, 1);
    if (!(bar & 1u)) return -1;
    io = (uint16_t)(bar & 0xFFFCu);
    if (!io) return -1;
    irq_line = (int)(pci_config_read32_public(dev, 0x3C) & 0xFF);

    outb(io + REG_CONFIG1, 0x00);                       /* power on / wake */
    outb(io + REG_CR, CR_RST);                           /* software reset */
    uint32_t t0 = 0;
    while ((inb(io + REG_CR) & CR_RST) && t0++ < 1000000u) { }
    if (inb(io + REG_CR) & CR_RST) return -1;

    for (int i = 0; i < 6; i++) mac[i] = inb(io + REG_IDR0 + (uint16_t)i);

    memset(rx_ring, 0, sizeof(rx_ring));
    memset(&stats, 0, sizeof(stats));
    outl(io + REG_RBSTART, (uint32_t)(uintptr_t)rx_ring);
    for (int i = 0; i < 4; i++) outl(io + REG_TSAD0 + (uint16_t)i * 4u, (uint32_t)(uintptr_t)tx_buffers[i]);
    outw(io + REG_IMR, 0x0000);                          /* polled: no interrupts */
    outw(io + REG_ISR, 0xFFFF);
    outl(io + REG_TCR, 0x03000700u);                     /* IFG normal, max DMA burst */
    /* AB | AM | APM  (broadcast, multicast, physical match); WRAP=0 so the
     * NIC wraps frames at the ring end itself; RBLEN=10b -> 32 KiB ring. */
    outl(io + REG_RCR, 0x0000070Fu | (2u << 11));
    outb(io + REG_CR, CR_RE | CR_TE);

    rx_offset = 0; tx_slot = 0;
    memset(tx_used, 0, sizeof(tx_used));
    ready = 1;
    return 0;
}

int rtl8139_ready(void) { return ready; }
int rtl8139_irq_line(void) { return irq_line; }
const uint8_t *rtl8139_mac_address(void) { return mac; }
const struct rtl8139_stats *rtl8139_get_stats(void) { return &stats; }
int rtl8139_link_up(void) { return ready && !(inb(io + REG_MSR) & 0x04); }

int rtl8139_send(const uint8_t *frame, uint16_t length)
{
    if (!ready || !frame || length < 14 || length > 1514) return -1;
    uint8_t s = tx_slot;

    /* A slot we've used before is free again once the NIC sets OWN. */
    if (tx_used[s]) {
        uint32_t spin = 0;
        while (!(inl(io + REG_TSD0 + (uint16_t)s * 4u) & TSD_OWN)) {
            if (++spin > 2000000u) { stats.tx_timeouts++; return -1; }
        }
        uint32_t st = inl(io + REG_TSD0 + (uint16_t)s * 4u);
        if (st & (TSD_TUN | TSD_TABT)) stats.tx_errors++;
    }

    memcpy(tx_buffers[s], frame, length);
    uint16_t padded = length < 60 ? 60 : length;         /* Ethernet minimum (no FCS) */
    if (padded > length) memset(tx_buffers[s] + length, 0, (size_t)(padded - length));
    outl(io + REG_TSD0 + (uint16_t)s * 4u, padded);      /* OWN=0 -> NIC starts DMA */
    tx_used[s] = 1;
    tx_slot = (uint8_t)((s + 1) & 3);
    stats.tx_frames++; stats.tx_bytes += length;
    return 0;
}

static uint8_t  rb(uint32_t at)  { return rx_ring[at & RX_MASK]; }

int rtl8139_receive(uint8_t *frame, uint16_t capacity, uint16_t *length)
{
    if (!ready || !frame || !length) return 0;
    if (inb(io + REG_CR) & CR_BUFE) return 0;

    uint16_t status = (uint16_t)(rb(rx_offset) | (rb(rx_offset + 1) << 8));
    uint16_t plen   = (uint16_t)(rb(rx_offset + 2) | (rb(rx_offset + 3) << 8));

    if (!(status & 1u) || plen < 8 || plen > 1792) {
        /* Ring out of sync: reset the receiver and resync from scratch. */
        stats.rx_errors++;
        outb(io + REG_CR, CR_TE);
        outb(io + REG_CR, CR_RE | CR_TE);
        outl(io + REG_RCR, 0x0000070Fu | (2u << 11));
        outl(io + REG_RBSTART, (uint32_t)(uintptr_t)rx_ring);
        rx_offset = 0;
        outw(io + REG_CAPR, (uint16_t)(0u - 16u));
        outw(io + REG_ISR, 0xFFFF);
        return 0;
    }

    uint32_t start = rx_offset + 4;
    uint16_t data_len = (uint16_t)(plen - 4u);            /* strip FCS */
    int good = (status & 0x000Eu) == 0;                   /* FAE|CRC|LONG|RUNT clear */
    if (good) {
        uint16_t n = data_len > capacity ? capacity : data_len;
        for (uint16_t i = 0; i < n; i++) frame[i] = rb(start + i);
        *length = n;
        stats.rx_frames++; stats.rx_bytes += n;
    } else stats.rx_errors++;

    rx_offset = (rx_offset + 4u + plen + 3u) & ~3u;
    rx_offset &= RX_MASK;
    outw(io + REG_CAPR, (uint16_t)(rx_offset - 16u));
    outw(io + REG_ISR, 0x0005);                           /* ack ROK + overflow bits */
    return good ? 1 : 0;
}
