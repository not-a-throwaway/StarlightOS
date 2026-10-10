#include "pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset)
{
    uint32_t address =
        (1u << 31) |
        ((uint32_t)bus << 16) |
        ((uint32_t)(dev & 0x1F) << 11) |
        ((uint32_t)(fn & 0x07) << 8) |
        (offset & 0xFC);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_config_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset, uint16_t value)
{
    uint32_t address = (1u << 31) | ((uint32_t)bus << 16) |
        ((uint32_t)(dev & 0x1F) << 11) | ((uint32_t)(fn & 7) << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    outw((uint16_t)(PCI_CONFIG_DATA + (offset & 2)), value);
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id, struct pci_addr* out)
{
    /*
     * A full scan would walk all 256 buses; QEMU's std VGA is
     * always bus 0, so that's all this project needs -- keeps
     * boot fast and the code small.
     */
    for (uint16_t dev = 0; dev < 32; dev++) {
        for (uint8_t fn = 0; fn < 8; fn++) {
            uint32_t id = pci_config_read32(0, (uint8_t)dev, fn, 0x00);

            uint16_t vendor = (uint16_t)(id & 0xFFFF);

            if (vendor == 0xFFFF) {
                /* Nothing at this slot. Function 0 not present
                 * means the whole device slot is empty. */
                if (fn == 0) {
                    break;
                }
                continue;
            }

            uint16_t device = (uint16_t)(id >> 16);

            if (vendor == vendor_id && device == device_id) {
                out->bus = 0;
                out->device = (uint8_t)dev;
                out->function = fn;
                return 1;
            }

            /* Only probe functions 1-7 if this is a multi-function
             * device (bit 7 of the header-type byte). */
            uint32_t header = pci_config_read32(0, (uint8_t)dev, fn, 0x0C);
            uint8_t header_type = (uint8_t)((header >> 16) & 0xFF);

            if (fn == 0 && !(header_type & 0x80)) {
                break;
            }
        }
    }

    return 0;
}

uint32_t pci_read_bar0(struct pci_addr addr)
{
    return pci_read_bar(addr, 0) & 0xFFFFFFF0u;
}

uint32_t pci_read_bar(struct pci_addr addr, uint8_t index)
{
    return pci_config_read32(addr.bus, addr.device, addr.function,
        (uint8_t)(0x10 + (index % 6) * 4));
}

void pci_enable_io_busmaster(struct pci_addr addr)
{
    uint32_t command = pci_config_read32(addr.bus, addr.device, addr.function, 0x04);
    pci_config_write16(addr.bus, addr.device, addr.function, 0x04,
        (uint16_t)((command & 0xFFFFu) | 0x0005u));
}

uint32_t pci_config_read32_public(struct pci_addr addr, uint8_t offset)
{
    return pci_config_read32(addr.bus, addr.device, addr.function, offset);
}

int pci_enumerate(struct pci_info *out, int max)
{
    int n = 0;
    for (int bus = 0; bus < 8 && n < max; bus++) {
        for (int dev = 0; dev < 32 && n < max; dev++) {
            for (int fn = 0; fn < 8 && n < max; fn++) {
                uint32_t id = pci_config_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; continue; }
                uint32_t cls = pci_config_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 8);
                uint32_t irq = pci_config_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x3C);
                out[n].addr.bus = (uint8_t)bus; out[n].addr.device = (uint8_t)dev; out[n].addr.function = (uint8_t)fn;
                out[n].vendor = (uint16_t)(id & 0xFFFF); out[n].device = (uint16_t)(id >> 16);
                out[n].class_code = (uint8_t)(cls >> 24); out[n].subclass = (uint8_t)(cls >> 16);
                out[n].prog_if = (uint8_t)(cls >> 8); out[n].irq_line = (uint8_t)irq;
                n++;
                if (fn == 0) {
                    uint32_t hdr = pci_config_read32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
                    if (!((hdr >> 16) & 0x80)) break;
                }
            }
        }
    }
    return n;
}

const char *pci_class_name(uint8_t c, uint8_t s)
{
    switch (c) {
    case 0x00: return "Unclassified";
    case 0x01: return s == 1 ? "IDE controller" : s == 6 ? "SATA controller" : "Storage controller";
    case 0x02: return s == 0 ? "Ethernet controller" : "Network controller";
    case 0x03: return s == 0 ? "VGA controller" : "Display controller";
    case 0x04: return s == 1 ? "Audio device" : s == 3 ? "HD Audio" : "Multimedia controller";
    case 0x05: return "Memory controller";
    case 0x06: return s == 0 ? "Host bridge" : s == 1 ? "ISA bridge" : s == 4 ? "PCI bridge" : "Bridge";
    case 0x07: return "Communication controller";
    case 0x08: return "System peripheral";
    case 0x09: return "Input device";
    case 0x0C: return s == 3 ? "USB controller" : "Serial bus controller";
    default:   return "Other device";
    }
}

const char *pci_vendor_name(uint16_t v)
{
    switch (v) {
    case 0x8086: return "Intel";
    case 0x10EC: return "Realtek";
    case 0x1234: return "QEMU/Bochs";
    case 0x1AF4: return "Red Hat (virtio)";
    case 0x1022: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x1B36: return "Red Hat (QEMU)";
    default:     return "Unknown vendor";
    }
}

const char *pci_device_name(uint16_t v, uint16_t d)
{
    if (v == 0x8086) {
        switch (d) {
        case 0x1237: return "440FX Host Bridge";
        case 0x7000: return "PIIX3 ISA Bridge";
        case 0x7010: return "PIIX3 IDE";
        case 0x7113: return "PIIX4 ACPI";
        case 0x2415: return "82801AA AC'97 Audio";
        case 0x2918: return "ICH9 LPC";
        case 0x29C0: return "Q35 Host Bridge";
        case 0x100E: return "82540EM Gigabit Ethernet";
        }
    }
    if (v == 0x10EC && d == 0x8139) return "RTL8139 Fast Ethernet";
    if (v == 0x1234 && d == 0x1111) return "Standard VGA (Bochs VBE)";
    return "";
}
