/*
 * GDEMO.ELF -- M1 demo: the ring-3 window syscalls (user/slwindow.h).
 *
 *   fbmap(w, h)   opens a window with a w*h 0x00RRGGBB pixel surface and returns
 *                 its address in this process; draw straight into it and call
 *                 present() to composite the desktop.
 *   present()     pumps input, recomposites and shows the frame.
 *   getevents()   reads key/mouse/close events from the queue (timeout in ms;
 *                 0 = poll, -1 = block).
 *
 * It draws a gradient, then waits ~3 s for input, printing what arrives. The
 * serial log doubles as the test's assertion surface (terminal output is
 * mirrored to COM1), and the exit status is checked from the shell.
 */
#include <errno.h>
#include <stdio.h>
#include "slwindow.h"

#define W 320
#define H 200

int main(void)
{
    printf("GDEMO: start\n");

    /* The window syscalls need a surface: present() before fbmap() is EBADF. */
    errno = 0;
    int early = sl_present();
    printf("GDEMO: present before fbmap %s errno=%d (want %d)\n",
           early == -1 ? "fails" : "UNEXPECTED", errno, EBADF);

    void *surf = sl_fbmap(W, H);
    if (!surf) { printf("GDEMO: fbmap FAIL errno=%d\n", errno); return 1; }
    printf("GDEMO: fbmap ok %p\n", surf);

    /* Paint a gradient, then read a pixel back: the surface is mapped read/write. */
    uint32_t *px = (uint32_t *)surf;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            px[y * W + x] = ((uint32_t)(x * 255 / W) << 16) |
                            ((uint32_t)(y * 255 / H) << 8) | 0x40u;
    uint32_t probe = px[0];
    printf("GDEMO: pixels %s (probe=%06x)\n", probe == 0x40u ? "readable" : "BAD", probe);

    if (sl_present() != 0) { printf("GDEMO: present FAIL errno=%d\n", errno); return 1; }
    printf("GDEMO: present ok\n");

    /* Same size again returns the same surface; a different size is E_BUSY (M1). */
    void *again = sl_fbmap(W, H);
    printf("GDEMO: fbmap again %s\n", again == surf ? "same" : "DIFF");
    errno = 0;
    void *other = sl_fbmap(640, 480);
    printf("GDEMO: fbmap resize %s errno=%d (want %d)\n",
           other ? "allowed" : "busy", errno, EBUSY);

    /* Nothing typed yet: a short poll must time out with 0 events. */
    struct sl_event evs[4];
    int n = sl_getevents(evs, 1, 500);
    printf("GDEMO: poll n=%d\n", n);

    /* Wait up to ~3 s for input. 'q', ESC or the window close control quit. */
    printf("GDEMO: waiting\n");
    for (int i = 0; i < 30; i++) {
        n = sl_getevents(evs, 4, 100);
        if (n <= 0) continue;

        for (int k = 0; k < n; k++) {
            struct sl_event ev = evs[k];
            if (ev.type == SL_EV_QUIT) {
                printf("GDEMO: quit event\n");
                i = 30;
                break;
            }
            if (ev.type == SL_EV_KEY) {
                printf("GDEMO: key 0x%x\n", ev.data);
                if (ev.data == 'q' || ev.data == 27) i = 30;
            } else if (ev.type == SL_EV_DOWN) {
                printf("GDEMO: down x=%d y=%d btn=%x\n", ev.x, ev.y, ev.data);
            } else if (ev.type == SL_EV_MOVE) {
                /* draw a red dot under the cursor and show it */
                if (ev.x >= 0 && ev.x < W && ev.y >= 0 && ev.y < H)
                    px[ev.y * W + ev.x] = 0xFF0000u;
                sl_present();
            }
        }
    }

    printf("GDEMO: done\n");
    return 0;
}
