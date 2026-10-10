#include "vbe.h"
#include "pci.h"

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID          0
#define VBE_DISPI_INDEX_XRES        1
#define VBE_DISPI_INDEX_YRES        2
#define VBE_DISPI_INDEX_BPP         3
#define VBE_DISPI_INDEX_ENABLE      4
#define VBE_DISPI_INDEX_BANK        5
#define VBE_DISPI_INDEX_VIRT_WIDTH  6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7
#define VBE_DISPI_INDEX_X_OFFSET    8
#define VBE_DISPI_INDEX_Y_OFFSET    9

#define VBE_DISPI_DISABLED 0x00
#define VBE_DISPI_ENABLED  0x01
#define VBE_DISPI_LFB_ENABLED 0x40
#define VBE_DISPI_NOCLEARMEM  0x80

#define VBE_DISPI_ID0 0xB0C0
#define VBE_DISPI_ID5 0xB0C5

/* QEMU's default "-vga std" device (also used by Bochs). */
#define QEMU_STDVGA_VENDOR 0x1234
#define QEMU_STDVGA_DEVICE 0x1111

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void dispi_write(uint16_t index, uint16_t value)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16_t dispi_read(uint16_t index)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

int vbe_set_mode(int want_w, int want_h, int bpp, uint32_t* out_fb)
{
    /*
     * Confirm the DISPI interface is actually there before
     * touching anything else -- these ports don't exist on real
     * VGA hardware or on emulators that don't implement Bochs
     * VBE, and probing unclaimed I/O ports is harmless (reads as
     * 0xFFFF) but writing a mode to registers that don't exist
     * would silently do nothing useful, so bail out cleanly
     * instead of pretending it worked.
     */
    dispi_write(VBE_DISPI_INDEX_ID, VBE_DISPI_ID5);
    uint16_t id = dispi_read(VBE_DISPI_INDEX_ID);

    if (id < VBE_DISPI_ID0) {
        return 0;
    }

    struct pci_addr addr;

    if (!pci_find_device(QEMU_STDVGA_VENDOR, QEMU_STDVGA_DEVICE, &addr)) {
        return 0;
    }

    uint32_t fb = pci_read_bar0(addr);

    if (fb == 0) {
        return 0;
    }

    /* Standard Bochs VBE mode-set sequence: disable first, program
     * the geometry, then enable with the linear-framebuffer bit
     * set. NOCLEARMEM is deliberately *not* set -- we want a
     * known-blank framebuffer, and callers redraw a full frame
     * before presenting anyway. */
    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    dispi_write(VBE_DISPI_INDEX_XRES, (uint16_t)want_w);
    dispi_write(VBE_DISPI_INDEX_YRES, (uint16_t)want_h);
    dispi_write(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);
    dispi_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)want_w);
    dispi_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)want_h);
    dispi_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    dispi_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    dispi_write(VBE_DISPI_INDEX_BANK, 0);
    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    /* Read XRES back -- if the device really took the mode, this
     * matches what we asked for. A mismatch means something about
     * the request wasn't accepted (e.g. an unsupported size), so
     * treat that as failure rather than trusting a mode we didn't
     * actually get. */
    if (dispi_read(VBE_DISPI_INDEX_XRES) != (uint16_t)want_w ||
        dispi_read(VBE_DISPI_INDEX_BPP) != (uint16_t)bpp) {
        dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        return 0;
    }

    *out_fb = fb;
    return 1;
}
