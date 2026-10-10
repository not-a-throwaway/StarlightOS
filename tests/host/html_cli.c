/* Host driver: html_to_text(stdin) -> stdout, width 78. Used by html_test.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include "../../net.h"
int kisspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
uint32_t kstrtoul(const char *s, const char **e, int base) { char *end; unsigned long v = strtoul(s, &end, base); if (e) *e = end; return (uint32_t)v; }
int ksnprintf(char *b, size_t n, const char *f, ...) { va_list ap; va_start(ap, f); int r = vsnprintf(b, n, f, ap); va_end(ap); return r; }
int main(void)
{
    static char in[1 << 22], out[1 << 23];
    size_t n = fread(in, 1, sizeof in - 1, stdin);
    size_t r = html_to_text(in, n, out, sizeof out, 78, 1);
    fwrite(out, 1, r, stdout);
    return 0;
}
