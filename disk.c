#include "disk.h"

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_CNT  0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE       0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30
#define ATA_CMD_IDENTIFY 0xEC

#define ATA_STATUS_ERR  0x01
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_DF   0x20
#define ATA_STATUS_RDY  0x40
#define ATA_STATUS_BSY  0x80

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile (
        "outw %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;

    __asm__ volatile (
        "inw %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

static int ata_wait_busy(void)
{
    uint8_t status;

    do {
        status = inb(ATA_STATUS);
    } while (status & ATA_STATUS_BSY);

    if (status & ATA_STATUS_ERR) {
        return -1;
    }

    if (status & ATA_STATUS_DF) {
        return -1;
    }

    return 0;
}

static int ata_wait_drq(void)
{
    uint8_t status;

    do {
        status = inb(ATA_STATUS);
    } while (status & ATA_STATUS_BSY);

    if (status & ATA_STATUS_ERR) {
        return -1;
    }

    if (status & ATA_STATUS_DF) {
        return -1;
    }

    if (!(status & ATA_STATUS_DRQ)) {
        return -1;
    }

    return 0;
}

static int ata_read_sector(uint32_t lba, uint16_t* buffer)
{
    if (ata_wait_busy() != 0) {
        return -1;
    }

    /*
     * LBA28, primary master.
     */
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));

    outb(ATA_SECTOR_CNT, 1);
    outb(ATA_LBA_LOW,  lba & 0xFF);
    outb(ATA_LBA_MID,  (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);

    outb(ATA_COMMAND, ATA_CMD_READ);

    if (ata_wait_drq() != 0) {
        return -1;
    }

    for (int i = 0; i < 256; i++) {
        buffer[i] = inw(ATA_DATA);
    }

    return 0;
}

static int ata_write_sector(uint32_t lba, const uint16_t* buffer)
{
    if (ata_wait_busy() != 0) {
        return -1;
    }

    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));

    outb(ATA_SECTOR_CNT, 1);
    outb(ATA_LBA_LOW,  lba & 0xFF);
    outb(ATA_LBA_MID,  (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);

    outb(ATA_COMMAND, ATA_CMD_WRITE);

    if (ata_wait_drq() != 0) {
        return -1;
    }

    for (int i = 0; i < 256; i++) {
        outw(ATA_DATA, buffer[i]);
    }

    return ata_wait_busy();
}

int disk_initialize(void)
{
    /*
     * Select primary master.
     */
    outb(ATA_DRIVE, 0xE0);

    /*
     * Clear sector count/LBA registers.
     */
    outb(ATA_SECTOR_CNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);

    /*
     * IDENTIFY.
     */
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(ATA_STATUS);

    /*
     * No drive.
     */
    if (status == 0) {
        return -1;
    }

    if (ata_wait_busy() != 0) {
        return -1;
    }

    /*
     * Wait for DRQ.
     */
    if (ata_wait_drq() != 0) {
        return -1;
    }

    /*
     * Consume IDENTIFY data.
     */
    for (int i = 0; i < 256; i++) {
        (void)inw(ATA_DATA);
    }

    return 0;
}

int disk_read(uint32_t lba, uint8_t count, void* buffer)
{
    uint8_t* destination = (uint8_t*)buffer;

    for (uint8_t sector = 0; sector < count; sector++) {
        if (ata_read_sector(
                lba + sector,
                (uint16_t*)(destination + sector * 512)) != 0) {
            return -1;
        }
    }

    return 0;
}

int disk_write(uint32_t lba, uint8_t count, const void* buffer)
{
    const uint8_t* source = (const uint8_t*)buffer;

    for (uint8_t sector = 0; sector < count; sector++) {
        if (ata_write_sector(
                lba + sector,
                (const uint16_t*)(source + sector * 512)) != 0) {
            return -1;
        }
    }

    return 0;
}
