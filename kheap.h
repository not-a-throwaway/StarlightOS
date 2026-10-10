#ifndef KHEAP_H
#define KHEAP_H
#include <stddef.h>
#include <stdint.h>

void  kheap_init(void);
void *kmalloc(size_t n);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *p, size_t n);
void  kfree(void *p);
void  kheap_stats(size_t *total, size_t *used, size_t *largest_free, size_t *blocks);

#endif
