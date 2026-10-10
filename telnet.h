#ifndef TELNET_H
#define TELNET_H
/*
 * telnet.h -- RFC 854/855 protocol engine for the `telnet` shell command (I/O via callbacks, so it can be
 * unit-tested on the host).
 *
 * Starts in line mode (local echo); a server that sends WILL ECHO switches it to character mode.
 * Options: ECHO and SUPPRESS-GO-AHEAD are accepted from the server; TERMINAL-TYPE ("DUMB", the terminal
 * has no ANSI) and NAWS (window size) are offered when asked; everything else is refused.
 * Incoming ANSI escape sequences are swallowed, tabs are expanded, non-ASCII is shown as '?'.
 */
#include <stdint.h>
#include <stddef.h>

struct telnet_io {
    void (*send)(void *ctx, const uint8_t *data, size_t n);   /* bytes to the server */
    void (*put)(void *ctx, char c);                           /* character to the terminal */
    void (*size)(void *ctx, int *cols, int *rows);            /* terminal size, for NAWS */
    int  (*column)(void *ctx);                                /* current cursor column, for tab expansion */
    void *ctx;
};

struct telnet {
    const struct telnet_io *io;
    uint8_t his[256], mine[256];       /* options currently enabled by the server / by us */
    int remote_echo;                   /* server echoes: character mode */
    int state, sb_len, esc, cr;
    uint8_t sb[16];
};

void   telnet_init(struct telnet *t, const struct telnet_io *io);
void   telnet_input(struct telnet *t, const uint8_t *data, size_t n);         /* feed bytes received from the server */
/* Translates a key (ASCII or KEY_* from keyboard.h) typed in character mode into the bytes to send.
 * out must hold 8 bytes; returns the count (0 = nothing to send). */
size_t telnet_encode_key(int key, uint8_t out[8]);
#endif
