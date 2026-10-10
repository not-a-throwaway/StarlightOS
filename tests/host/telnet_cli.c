/* Host driver for telnet.c: reads "CHUNK <n> <hex stream>" lines; feeds the stream in chunks of n bytes;
 * prints OUT:<text with \xNN escapes for control chars>, SENT:<hex of everything replied>, ECHO:<0|1>.
 * Also "KEY <code>" lines to test telnet_encode_key. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../telnet.h"

static char out[1 << 16]; static size_t on; static uint8_t sent[1 << 16]; static size_t sn; static int col;
static void io_send(void *c, const uint8_t *d, size_t n) { (void)c; memcpy(sent + sn, d, n); sn += n; }
static void io_put(void *c, char ch) { (void)c; if (ch == '\n' || ch == '\r') col = 0; else if (ch == '\b') { if (col) col--; } else col++;
    if (ch >= 32 && ch < 127) out[on++] = ch; else on += (size_t)sprintf(out + on, "\\x%02x", (unsigned char)ch); }
static void io_size(void *c, int *cols, int *rows) { (void)c; *cols = 80; *rows = 32; }
static int io_col(void *c) { (void)c; return col; }

int main(void)
{
    static char line[1 << 17];
    static const struct telnet_io io = { io_send, io_put, io_size, io_col, NULL };
    while (fgets(line, sizeof line, stdin)) {
        if (!strncmp(line, "KEY ", 4)) { uint8_t o[8]; size_t n = telnet_encode_key(atoi(line + 4), o); for (size_t i = 0; i < n; i++) printf("%02x", o[i]); printf("\n"); continue; }
        if (strncmp(line, "CHUNK ", 6)) continue;
        char *p = line + 6; int chunk = (int)strtol(p, &p, 10); while (*p == ' ') p++;
        static uint8_t in[1 << 16]; size_t n = 0;
        for (; p[0] && p[1] && p[0] != '\n'; p += 2) { unsigned v; sscanf(p, "%2x", &v); in[n++] = (uint8_t)v; }
        static struct telnet t; telnet_init(&t, &io); on = sn = 0; col = 0;
        for (size_t i = 0; i < n; i += (size_t)chunk) telnet_input(&t, in + i, n - i < (size_t)chunk ? n - i : (size_t)chunk);
        out[on] = 0; printf("OUT:%s\nSENT:", out); for (size_t i = 0; i < sn; i++) printf("%02x", sent[i]); printf("\nECHO:%d\n", t.remote_echo); fflush(stdout);
    }
    return 0;
}
