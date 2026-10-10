#include "kstring.h"

/* Loop-to-libcall rewriting would turn these bodies into calls to
 * themselves, so it is disabled for the whole file. */
#pragma GCC optimize("no-tree-loop-distribute-patterns")

void *memcpy(void *dst, const void *src, size_t n)
{
    void *ret = dst;
    __asm__ volatile("cld; rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) {
        __asm__ volatile("cld; rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    } else {
        d += n - 1; s += n - 1;
        __asm__ volatile("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    void *ret = dst;
    __asm__ volatile("cld; rep stosb" : "+D"(dst), "+c"(n) : "a"((uint8_t)value) : "memory");
    return ret;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
size_t strnlen(const char *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

int ktolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int ktoupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int kisdigit(int c) { return c >= '0' && c <= '9'; }
int kisalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int kisalnum(int c) { return kisalpha(c) || kisdigit(c); }
int kisspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int kisprint(int c) { return c >= 32 && c < 127; }
int kisxdigit(int c) { return kisdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

int strcasecmp(const char *a, const char *b)
{
    while (*a && ktolower(*a) == ktolower(*b)) { a++; b++; }
    return ktolower((uint8_t)*a) - ktolower((uint8_t)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int x = ktolower((uint8_t)a[i]), y = ktolower((uint8_t)b[i]);
        if (x != y) return x - y;
        if (!a[i]) return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *r = dst;
    while ((*dst++ = *src++)) { }
    return r;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t c = len < size - 1 ? len : size - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dl = strnlen(dst, size);
    if (dl == size) return size + strlen(src);
    return dl + strlcpy(dst + dl, src, size - dl);
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

char *strstr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) if (*h == *n && strncmp(h, n, nl) == 0) return (char *)h;
    return NULL;
}

char *strcasestr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) if (strncasecmp(h, n, nl) == 0) return (char *)h;
    return NULL;
}

uint32_t kstrtoul(const char *s, const char **end, int base)
{
    uint32_t v = 0;
    while (kisspace(*s)) s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0) base = 10;
    for (;; s++) {
        int d;
        if (kisdigit(*s)) d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (uint32_t)base + (uint32_t)d;
    }
    if (end) *end = s;
    return v;
}

int kparse_uint(const char *s, uint32_t *out)
{
    const char *e;
    if (!*s) return -1;
    uint32_t v = kstrtoul(s, &e, 0);
    if (*e) return -1;
    *out = v;
    return 0;
}

int kparse_int(const char *s, int32_t *out)
{
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    uint32_t v;
    if (kparse_uint(s, &v) != 0) return -1;
    *out = neg ? -(int32_t)v : (int32_t)v;
    return 0;
}
