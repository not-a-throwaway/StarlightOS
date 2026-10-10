#include "kprintf.h"
#include "kstring.h"

extern void terminal_writestring(const char *s);

static void (*sink_fn)(const char *) = terminal_writestring;
void kprintf_set_sink(void (*sink)(const char *text)) { sink_fn = sink ? sink : terminal_writestring; }
void kprintf_reset_sink(void) { sink_fn = terminal_writestring; }

struct out { char *buf; size_t size, pos; };

static void put(struct out *o, char c)
{
    if (o->pos + 1 < o->size) o->buf[o->pos] = c;
    o->pos++;
}

static void pad(struct out *o, int n, char c) { while (n-- > 0) put(o, c); }

static void emit_number(struct out *o, uint64_t v, int base, int upper, int neg,
                        int width, int left, int zero, int plus, int space)
{
    char tmp[24];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[n++] = '0';
    while (v) {
        uint32_t d;
        if (base == 10) { d = (uint32_t)(v % 10u); v /= 10u; }
        else if (base == 16) { d = (uint32_t)(v & 15u); v >>= 4; }
        else { d = (uint32_t)(v & 7u); v >>= 3; }
        tmp[n++] = digits[d];
    }
    char sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
    int len = n + (sign ? 1 : 0);
    if (!left && !zero) pad(o, width - len, ' ');
    if (sign) put(o, sign);
    if (!left && zero) pad(o, width - len, '0');
    while (n) put(o, tmp[--n]);
    if (left) pad(o, width - len, ' ');
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        int left = 0, zero = 0, plus = 0, space = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
        else while (kisdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (kisdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;
        for (;; fmt++) {
            if (*fmt == 'l') lng++;
            else if (*fmt == 'z') lng = 2;
            else if (*fmt == 'h') lng = -1;
            else break;
        }
        char c = *fmt;
        if (!c) break;
        switch (c) {
        case '%': put(&o, '%'); break;
        case 'c': {
            char ch = (char)va_arg(ap, int);
            if (!left) pad(&o, width - 1, ' ');
            put(&o, ch);
            if (left) pad(&o, width - 1, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)(prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s));
            if (!left) pad(&o, width - len, ' ');
            for (int i = 0; i < len; i++) put(&o, s[i]);
            if (left) pad(&o, width - len, ' ');
            break;
        }
        case 'd': case 'i': {
            int64_t v = lng >= 2 ? va_arg(ap, long long) : lng == 1 ? (int64_t)va_arg(ap, long) : (int64_t)va_arg(ap, int);
            int neg = v < 0;
            emit_number(&o, neg ? (uint64_t)(-v) : (uint64_t)v, 10, 0, neg, width, left, zero && !left, plus, space);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = lng >= 2 ? va_arg(ap, unsigned long long) : lng == 1 ? (uint64_t)va_arg(ap, unsigned long)
                       : (uint64_t)va_arg(ap, unsigned int);
            if (lng == -1) v &= 0xFFFFu;
            emit_number(&o, v, c == 'u' ? 10 : c == 'o' ? 8 : 16, c == 'X', 0, width, left, zero && !left, 0, 0);
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            put(&o, '0'); put(&o, 'x');
            emit_number(&o, v, 16, 0, 0, width, left, zero, 0, 0);
            break;
        }
        default: put(&o, '%'); put(&o, c); break;
        }
    }
    if (size) o.buf[o.pos < size ? o.pos : size - 1] = 0;
    return (int)o.pos;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int kvprintf(const char *fmt, va_list ap)
{
    char line[512];
    int n = kvsnprintf(line, sizeof(line), fmt, ap);
    sink_fn(line);
    return n;
}

/* Implemented by the shell (shell_stderr_write); weak so that host tests
 * and early boot code that don't link the shell still resolve. */
extern void shell_stderr_write(const char *text) __attribute__((weak));

int kvprintf_err(const char *fmt, va_list ap)
{
    char line[512];
    int n = kvsnprintf(line, sizeof(line), fmt, ap);
    if (shell_stderr_write) shell_stderr_write(line);
    else sink_fn(line);
    return n;
}

int kprintf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}
