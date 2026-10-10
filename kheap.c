#include "kheap.h"
#include "kstring.h"

/*
 * First-fit allocator with block splitting and forward coalescing.
 * The arena lives in a NOLOAD section (see the linker scripts) so it
 * costs nothing in the boot image and is not zeroed by the loader.
 */
#define HEAP_SIZE (40u * 1024u * 1024u)
#define ALIGN 16u
#define MAGIC_USED 0xA110C8EDu
#define MAGIC_FREE 0xF4EEB10Cu

static uint8_t arena[HEAP_SIZE] __attribute__((section(".heap"), aligned(4096)));

struct block {
    uint32_t magic;
    uint32_t free;
    size_t   size;          /* payload bytes */
    struct block *next;
    struct block *prev;
};
#define HDR ((sizeof(struct block) + ALIGN - 1) & ~(size_t)(ALIGN - 1))

static struct block *head;

void kheap_init(void)
{
    head = (struct block *)arena;
    head->magic = MAGIC_FREE;
    head->free = 1;
    head->size = HEAP_SIZE - HDR;
    head->next = head->prev = NULL;
}

static void coalesce(struct block *b)
{
    while (b->next && b->next->free) {
        b->size += HDR + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
}

void *kmalloc(size_t n)
{
    if (!head) kheap_init();
    if (n == 0) n = 1;
    n = (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    for (struct block *b = head; b; b = b->next) {
        if (!b->free || b->size < n) continue;
        if (b->size >= n + HDR + ALIGN) {
            struct block *r = (struct block *)((uint8_t *)b + HDR + n);
            r->magic = MAGIC_FREE; r->free = 1;
            r->size = b->size - n - HDR;
            r->next = b->next; r->prev = b;
            if (b->next) b->next->prev = r;
            b->next = r; b->size = n;
        }
        b->free = 0; b->magic = MAGIC_USED;
        return (uint8_t *)b + HDR;
    }
    return NULL;
}

void *kcalloc(size_t count, size_t size)
{
    size_t total = count * size;
    if (size && total / size != count) return NULL;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void kfree(void *p)
{
    if (!p) return;
    struct block *b = (struct block *)((uint8_t *)p - HDR);
    if (b->magic != MAGIC_USED) return;              /* double free / bad pointer: ignore */
    b->free = 1; b->magic = MAGIC_FREE;
    coalesce(b);
    if (b->prev && b->prev->free) coalesce(b->prev);
}

void *krealloc(void *p, size_t n)
{
    if (!p) return kmalloc(n);
    struct block *b = (struct block *)((uint8_t *)p - HDR);
    if (b->magic != MAGIC_USED) return NULL;
    if (b->size >= n) return p;
    void *q = kmalloc(n);
    if (!q) return NULL;
    memcpy(q, p, b->size);
    kfree(p);
    return q;
}

void kheap_stats(size_t *total, size_t *used, size_t *largest, size_t *blocks)
{
    size_t u = 0, l = 0, c = 0;
    for (struct block *b = head; b; b = b->next) {
        c++;
        if (b->free) { if (b->size > l) l = b->size; } else u += b->size + HDR;
    }
    if (total) *total = HEAP_SIZE;
    if (used) *used = u;
    if (largest) *largest = l;
    if (blocks) *blocks = c;
}
