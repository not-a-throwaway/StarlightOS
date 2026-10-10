#ifndef STARLIGHTOS_USER_SLWINDOW_H
#define STARLIGHTOS_USER_SLWINDOW_H

/*
 * StarlightOS window ABI -- shared verbatim by the kernel (gui.c/process.c),
 * the newlib syscall layer (libc/syscalls.c) and freestanding ring-3 apps
 * (user/syscall.h), so there is exactly one definition of the event struct
 * and the syscall numbers.
 *
 * A ring-3 app opens a window by calling fbmap(w, h): the kernel allocates a
 * w*h*4-byte pixel surface (0x00RRGGBB, same gfx_t as the desktop), maps it
 * into the process and returns its address. The app draws into it and calls
 * present() to composite the desktop; it reads keyboard/mouse input from a
 * queue with getevents(). Closing the window (the titlebar close control)
 * delivers SL_EV_QUIT; the window disappears when the process exits.
 *
 * This file is deliberately freestanding: <stdint.h> only, no libc, no kernel
 * headers -- it is included by every build (kernel, host test, newlib app).
 */
#include <stdint.h>

/* ---- syscall numbers (int $0x80, process.c dispatcher) ---- */
#define SL_SYS_FBMAP     110   /* long fbmap(int w, int h) -> surface VA (or -errno) */
#define SL_SYS_PRESENT   111   /* int present(void) -> 0 (or -errno) */
#define SL_SYS_GETEVENTS 112   /* int getevents(struct sl_event *buf, int count, int timeout_ms) */

/* ---- input events ----
 * KEY:   data = the character code keyboard.c queues (ASCII, plus the KEY_*
 *        specials >= 0x80), delivery order preserved.
 * MOVE:  x,y = cursor position in client-area pixels (may be outside
 *        [0,w)x[0,h) when the cursor leaves the window).
 * DOWN/UP: x,y = client-area position; data = SL_BTN_* button mask.
 * QUIT:  the window's close control was clicked; the app should exit.
 */
struct sl_event {
    uint32_t type;
    int32_t  x;
    int32_t  y;
    uint32_t data;
};

enum {
    SL_EV_NONE = 0,
    SL_EV_KEY  = 1,
    SL_EV_MOVE = 2,
    SL_EV_DOWN = 3,
    SL_EV_UP   = 4,
    SL_EV_QUIT = 5
};

#define SL_BTN_LEFT  1u
#define SL_BTN_RIGHT 2u

/* ---- newlib wrappers (libc/syscalls.c) ----
 * Freestanding apps get raw `starlightos_*` inlines from user/syscall.h
 * instead, so these names never clash with them in the same translation unit.
 */
#ifdef __cplusplus
extern "C" {
#endif
void *sl_fbmap(int w, int h);                                  /* NULL + errno on failure */
int   sl_present(void);                                        /* 0, or -1 + errno */
int   sl_getevents(struct sl_event *buf, int count, int timeout_ms); /* events, 0 on timeout, -1 + errno */
#ifdef __cplusplus
}
#endif

#endif
