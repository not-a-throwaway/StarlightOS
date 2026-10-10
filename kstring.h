#ifndef KSTRING_H
#define KSTRING_H
/*
 * Minimal freestanding C string/memory library.
 *
 * GCC is allowed to emit calls to memcpy/memset/memmove/memcmp even in
 * -ffreestanding code (struct copies, zero-initialisers), so these are
 * real symbols, not just helpers.
 */
#include <stddef.h>
#include <stdint.h>

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int value, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strcasestr(const char *haystack, const char *needle);

int    kisdigit(int c);
int    kisalpha(int c);
int    kisalnum(int c);
int    kisspace(int c);
int    kisprint(int c);
int    kisxdigit(int c);
int    ktolower(int c);
int    ktoupper(int c);

/* Parses an optionally-signed decimal (or 0x-prefixed hex) integer.
 * Returns 0 on success; *end (if non-NULL) receives the first unparsed char. */
int    kparse_int(const char *s, int32_t *out);
int    kparse_uint(const char *s, uint32_t *out);
uint32_t kstrtoul(const char *s, const char **end, int base);

#endif
