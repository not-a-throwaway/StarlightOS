/*
 * telnet.c -- telnet protocol engine (see telnet.h).
 */
#include "telnet.h"
#include "keyboard.h"
#include "kstring.h"

enum { TN_SE = 240, TN_SB = 250, TN_WILL = 251, TN_WONT = 252, TN_DO = 253, TN_DONT = 254, TN_IAC = 255 };
enum { TO_ECHO = 1, TO_SGA = 3, TO_TTYPE = 24, TO_NAWS = 31 };
enum { TS_DATA, TS_IAC, TS_WILL, TS_WONT, TS_DO, TS_DONT, TS_SB, TS_SB_IAC };

void telnet_init(struct telnet *t, const struct telnet_io *io)
{
    memset(t, 0, sizeof(*t));
    t->io = io;
}

static void send_bytes(struct telnet *t, const uint8_t *b, size_t n) { t->io->send(t->io->ctx, b, n); }
static void send3(struct telnet *t, uint8_t cmd, uint8_t opt) { uint8_t b[3] = { TN_IAC, cmd, opt }; send_bytes(t, b, 3); }

static void send_naws(struct telnet *t)
{
    int cols = 80, rows = 24;
    t->io->size(t->io->ctx, &cols, &rows);
    uint8_t raw[4] = { (uint8_t)(cols >> 8), (uint8_t)cols, (uint8_t)(rows >> 8), (uint8_t)rows };
    uint8_t b[16]; int n = 0;
    b[n++] = TN_IAC; b[n++] = TN_SB; b[n++] = TO_NAWS;
    for (int i = 0; i < 4; i++) { b[n++] = raw[i]; if (raw[i] == TN_IAC) b[n++] = TN_IAC; }
    b[n++] = TN_IAC; b[n++] = TN_SE;
    send_bytes(t, b, (size_t)n);
}

static int we_want(int opt)  { return opt == TO_ECHO || opt == TO_SGA; }      /* options we let the server enable */
static int we_offer(int opt) { return opt == TO_TTYPE || opt == TO_NAWS; }    /* options we enable on request */

static void subneg(struct telnet *t)
{
    if (t->sb_len >= 2 && t->sb[0] == TO_TTYPE && t->sb[1] == 1) {            /* SB TTYPE SEND -> IS "DUMB" */
        static const uint8_t reply[] = { TN_IAC, TN_SB, TO_TTYPE, 0, 'D', 'U', 'M', 'B', TN_IAC, TN_SE };
        send_bytes(t, reply, sizeof(reply));
    }
}

static void put_data(struct telnet *t, uint8_t b)
{
    void *c = t->io->ctx;
    /* swallow ANSI escape sequences: ESC [ params final | ESC ( B | ESC c ... */
    if (t->esc == 1) { t->esc = (b == '[') ? 2 : (b == '(' || b == ')' || b == '*' || b == '+') ? 3 : 0; return; }
    if (t->esc == 2) { if (b >= 0x40 && b <= 0x7E) t->esc = 0; return; }
    if (t->esc == 3) { t->esc = 0; return; }
    if (b == 27) { t->esc = 1; return; }
    if (t->cr && (b == 0 || b == '\n')) { t->cr = 0; if (b == '\n') t->io->put(c, '\n'); return; }   /* CR NUL / CR LF */
    t->cr = (b == '\r');
    if (b == '\r' || b == '\n' || b == '\b') { t->io->put(c, (char)b); return; }
    if (b == '\t') { int n = 8 - (t->io->column(c) & 7); while (n--) t->io->put(c, ' '); return; }
    if (b >= 32 && b < 127) { t->io->put(c, (char)b); return; }
    if (b >= 0xC0) t->io->put(c, '?');                                          /* UTF-8 lead byte: the font is ASCII only */
}

static void input_byte(struct telnet *t, uint8_t b)
{
    switch (t->state) {
    case TS_DATA:
        if (b == TN_IAC) t->state = TS_IAC; else put_data(t, b);
        break;
    case TS_IAC:
        t->state = TS_DATA;
        if (b == TN_IAC) put_data(t, '?');                                      /* escaped 0xFF */
        else if (b == TN_WILL) t->state = TS_WILL; else if (b == TN_WONT) t->state = TS_WONT;
        else if (b == TN_DO) t->state = TS_DO;     else if (b == TN_DONT) t->state = TS_DONT;
        else if (b == TN_SB) { t->state = TS_SB; t->sb_len = 0; }
        break;                                                                   /* NOP, GA, ... ignored */
    case TS_WILL:
        t->state = TS_DATA;
        if (we_want(b)) { if (!t->his[b]) { t->his[b] = 1; send3(t, TN_DO, b); } if (b == TO_ECHO) t->remote_echo = 1; }
        else send3(t, TN_DONT, b);
        break;
    case TS_WONT:
        t->state = TS_DATA;
        if (t->his[b]) { t->his[b] = 0; send3(t, TN_DONT, b); }
        if (b == TO_ECHO) t->remote_echo = 0;
        break;
    case TS_DO:
        t->state = TS_DATA;
        if (we_offer(b)) { if (!t->mine[b]) { t->mine[b] = 1; send3(t, TN_WILL, b); if (b == TO_NAWS) send_naws(t); } }
        else send3(t, TN_WONT, b);
        break;
    case TS_DONT:
        t->state = TS_DATA;
        if (t->mine[b]) { t->mine[b] = 0; send3(t, TN_WONT, b); }
        break;
    case TS_SB:
        if (b == TN_IAC) t->state = TS_SB_IAC;
        else if (t->sb_len < (int)sizeof(t->sb)) t->sb[t->sb_len++] = b;
        break;
    case TS_SB_IAC:
        if (b == TN_SE) { subneg(t); t->state = TS_DATA; }
        else if (b == TN_IAC) { if (t->sb_len < (int)sizeof(t->sb)) t->sb[t->sb_len++] = b; t->state = TS_SB; }
        else t->state = TS_DATA;
        break;
    }
}

void telnet_input(struct telnet *t, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) input_byte(t, data[i]);
}

size_t telnet_encode_key(int c, uint8_t o[8])
{
    switch (c) {
    case '\n':       o[0] = '\r'; o[1] = 0; return 2;                            /* bare CR is sent as CR NUL (RFC 854) */
    case '\b':       o[0] = 0x7f; return 1;
    case KEY_UP:     o[0] = 27; o[1] = '['; o[2] = 'A'; return 3;
    case KEY_DOWN:   o[0] = 27; o[1] = '['; o[2] = 'B'; return 3;
    case KEY_RIGHT:  o[0] = 27; o[1] = '['; o[2] = 'C'; return 3;
    case KEY_LEFT:   o[0] = 27; o[1] = '['; o[2] = 'D'; return 3;
    case KEY_HOME:   o[0] = 27; o[1] = '['; o[2] = 'H'; return 3;
    case KEY_END:    o[0] = 27; o[1] = '['; o[2] = 'F'; return 3;
    case KEY_DEL:    o[0] = 27; o[1] = '['; o[2] = '3'; o[3] = '~'; return 4;
    default:
        if (c >= 0 && c < 0x80) { o[0] = (uint8_t)c; return 1; }          /* ASCII only, so never an IAC byte */
        return 0;                                                                /* other special keys: nothing sensible to send */
    }
}
