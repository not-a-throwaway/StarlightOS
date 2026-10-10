#ifndef KPRINTF_H
#define KPRINTF_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/*
 * printf-style formatting for the kernel.
 * Supports: %d %i %u %x %X %o %c %s %p %%  with flags '-' '0' '+' ' ',
 * width, .precision (strings), '*' width/precision, and the length
 * modifiers h, hh, l, ll, z.
 */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int kvprintf(const char *fmt, va_list ap);

/* Like kvprintf() but for standard error: the shell decides where it goes
 * (the screen even inside a pipe, a file for 2>file, or stdout for 2>&1). */
int kvprintf_err(const char *fmt, va_list ap);

/* Where kprintf() output goes. Defaults to terminal_writestring(). */
void kprintf_set_sink(void (*sink)(const char *text));
void kprintf_reset_sink(void);

#endif
