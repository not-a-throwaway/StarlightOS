#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include "user/slwindow.h"

/*
 * Runs the desktop -- top bar, dock, and whichever app window is
 * open (including the Terminal, which hosts the shell rather
 * than being a separate full-screen mode). kernel_main() calls
 * this once, straight after boot, and it isn't expected to
 * return during normal operation.
 */
void gui_run(void);
/* Shows a short notification in the top-right corner (3.5 s). Safe to call before the GUI starts. */
void gui_toast(const char* text);

/*
 * Guest windows -- the ring-3 app window (see user/slwindow.h).
 * process.c's window syscalls call these; the desktop loop is
 * suspended while a process runs, so the guest functions pump
 * the background themselves (kyield, keyboard/mouse, redraw) and
 * render the composited frame with render_frame().
 */
int   gui_guest_open(int w, int h);        /* 0 ok, -1 window does not fit the screen, -2 out of memory */
void  gui_guest_close(void);               /* frees the surface and restores the previous app */
int   gui_guest_is_open(void);
void  gui_guest_dims(int* w, int* h);      /* current client-area size */
void* gui_guest_surface(void);             /* the pixel surface mapped into the process */
int   gui_guest_present(void);             /* pump input, composite, present; returns 0 */
int   gui_guest_wait(struct sl_event* buf, int count, int timeout_ms); /* 0..count events, -1 = aborted */

#endif
