#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/*
 * Just enough PCI config-space access to find one device by
 * vendor/device ID and read its BAR0 -- used to locate the
 * QEMU/Bochs "std VGA" display device so vbe.c can talk to it
 * without any BIOS help. Not a general PCI driver.
 */

struct pci_addr {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

/* Scans bus 0 only (QEMU's std VGA is always there) for a
 * device matching vendor_id/device_id. Returns 1 and fills
 * *out on success, 0 if nothing matched. */
int pci_find_device(uint16_t vendor_id, uint16_t device_id, struct pci_addr* out);

/* Reads BAR0 and masks off the low flag bits, returning the
 * physical base address of the memory region it describes.
 * Only meaningful for a 32-bit, memory-space BAR (which is what
 * QEMU's std VGA framebuffer BAR0 is). */
uint32_t pci_read_bar0(struct pci_addr addr);
uint32_t pci_read_bar(struct pci_addr addr, uint8_t index);
void pci_enable_io_busmaster(struct pci_addr addr);

/* Raw config-space read of any 32-bit-aligned register. */
uint32_t pci_config_read32_public(struct pci_addr addr, uint8_t offset);

/* Enumeration for `lspci`: calls cb for every present function on bus 0..255. */
struct pci_info {
    struct pci_addr addr;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if, irq_line;
};
int pci_enumerate(struct pci_info *out, int max);
const char *pci_class_name(uint8_t class_code, uint8_t subclass);
const char *pci_vendor_name(uint16_t vendor);
const char *pci_device_name(uint16_t vendor, uint16_t device);

#endif
