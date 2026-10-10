#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/*
 * Special, non-printable character codes shell_handle_character
 * (or any other dispatch target) may receive in addition to
 * normal ASCII. None of these collide with real ASCII control
 * codes already in use ('\b' = 8, '\t' = 9, '\n' = 10) or with
 * printable characters (32-126).
 */
#define KEY_ESC   27
/* Special keys live above 0x80 so they can never collide with the
 * Ctrl+letter control codes (Ctrl-A = 1 ... Ctrl-Z = 26). */
#define KEY_UP    0x81
#define KEY_DOWN  0x82
#define KEY_LEFT  0x83
#define KEY_RIGHT 0x84
#define KEY_HOME  0x85
#define KEY_END   0x86
#define KEY_DEL   0x87
#define KEY_PGUP  0x88
#define KEY_PGDN  0x89
#define KEY_INS   0x8A
#define KEY_F1    0x90   /* F1..F12 = 0x90..0x9B */
#define KEY_CTRL_C 3

void keyboard_initialize(void);
void keyboard_handle_scancode(uint8_t scancode);
int keyboard_getchar_nonblocking(void);
int keyboard_has_char(void);          /* non-destructive: is a key queued? (poll.c) */
void keyboard_flush_queue(void);

/* The IRQ handler only *queues* keystrokes. The main loop calls this to
 * deliver them to the dispatch target (the shell by default) in normal
 * context with interrupts enabled, so commands may block, sleep and
 * use the network without freezing the machine. Returns keys delivered. */
int keyboard_pump(void);
int keyboard_modifiers(void);   /* bit0 shift, bit1 ctrl, bit2 alt */

/*
 * Lets a full-screen mode (like the GUI) steal keystrokes away
 * from the shell temporarily. Pass NULL to fall back to the
 * default target, shell_handle_character.
 */
void keyboard_set_dispatch(void (*handler)(char character));
void (*keyboard_get_dispatch(void))(char character);

#endif
