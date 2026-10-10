#ifndef DISK_H
#define DISK_H

#include <stdint.h>

int disk_initialize(void);

int disk_read(uint32_t lba, uint8_t count, void* buffer);
int disk_write(uint32_t lba, uint8_t count, const void* buffer);

#endif
