#include <stdint.h>
#include <stddef.h>

#include "gui.h"
#include "vga.h"
#include "mouse.h"
#include "rtc.h"
#include "keyboard.h"
#include "vfs.h"
#include "starlightos_gui.h"
#include "terminal.h"
#include "shell.h"
#include "ksched.h"
#include "ktime.h"
#include "process.h"
#include "net.h"
#include "audio.h"
#include "kprintf.h"
#include "kstring.h"
#include "kheap.h"
#include "gfx.h"

#define APP_DESKTOP  0
#define APP_ABOUT    1
#define APP_DISK     2
#define APP_FILES    3
#define APP_PAINT    4
#define APP_HELP     5
#define APP_TERMINAL 6
#define APP_MINES    7
#define APP_GUEST    8   /* ring-3 app window (fbmap/present, see slwindow.h) */


#define FILES_MAX_ENTRIES 32
#define FILES_VISIBLE_ROWS 8
#define FILES_WINDOW_W 320 /* was 200 -- names up to VFS_NAME_MAX
                            * chars (63) at 16px/glyph plus the folder
                            * icon's offset need real room now;
                            * 200px fit the old 8.3 names at 8px glyphs. */

/* Reserved band at the very top for the GNOME-style top bar --
 * windows shouldn't be draggable up underneath it. */
#define HEADER_TOP 30

static void render_frame(const struct rtc_time* time, int mx, int my, int lb, int rb);
static void handle_mouse_down(int x, int y);
static void handle_mouse_drag(int x, int y);

static gfx_t desktop[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];
static gfx_t paint_canvas[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

/* Everything for one frame -- background, top bar/dock, the open
 * window, the cursor -- gets composited into this off-screen
 * buffer, then vga_present()'d to real video memory exactly once.
 * Previously every one of those pieces was drawn with its own
 * separate call straight onto vga_framebuffer() (live video
 * memory, single-buffered, no vsync), so each redraw was visibly
 * assembled on screen piece by piece -- that incremental
 * "background, *then* dock, *then* window, *then* cursor" paint-in
 * is the flicker. Compositing off-screen first and presenting once
 * makes every redraw appear as a single atomic update instead. */
static gfx_t frame_buffer[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];
static int paint_canvas_ready = 0;

static const gfx_t paint_palette[8] = { 0xFFFFFF, 0x62A0EA, 0x57E389, 0xFF7B63, 0xF8E45C, 0xDC8ADD, 0xFFA348, 0x9A9996 };
static int paint_color_index = 7; /* white */

/* Where the last paint stroke's line segment ended, so a new
 * mouse packet can draw a connecting line to it instead of just
 * a dot -- fast drags otherwise leave visible gaps. */
static int paint_last_x = -1;
static int paint_last_y = -1;

static volatile int gui_running;
static int need_redraw_hint;
static volatile int current_app;
static int last_left_button;
static int last_right_button;
static int hovered_dock_icon = -1;

/* Window position -- draggable by its title bar. window_w/window_h
 * is the window's *actual* current size -- window_size_for() below
 * gives each app's natural/default/minimum size, but the window can
 * be dragged bigger (or, for Files/Terminal, smaller down to a
 * usable floor) via the resize grip gui_window_chrome draws at the
 * bottom-right corner. Reset to the natural size every time open_app()
 * switches apps. */
static int window_x = 40;
static int window_y = 30;
static int window_w = 0;
static int window_h = 0;
static int dragging_window = 0;
static int drag_offset_x = 0;
static int drag_offset_y = 0;
static int resizing_window = 0;
static int resize_start_w = 0;
static int resize_start_h = 0;
static int resize_start_mx = 0;
static int resize_start_my = 0;

/* ---- guest (ring-3 app) window state ----
 * guest_surface is the client-area pixel buffer (w*h gfx_t, kheap) that fbmap
 * maps into the process; the app draws straight into it. The window chrome is
 * 3px of side border and 33px of titlebar+header above the content (matches
 * gui_window_chrome()'s return value and the other apps' content geometry), so
 * the window is 6x39 pixels bigger than the surface. Input is queued here and
 * drained by getevents(); the desktop loop is suspended while the process runs,
 * so guest_input() pumps everything the loop normally would.
 */
#define GUEST_CONTENT_X 3
#define GUEST_CONTENT_Y 33
#define GUEST_EVQ 128
#define GUEST_PAGE 4096u   /* fbmap() maps whole pages, so the surface must be page-aligned */

static gfx_t* guest_surface = 0;       /* page-aligned client-area pixels (mapped into the app) */
static gfx_t* guest_surface_raw = 0;   /* original kcalloc() pointer, for kfree() */
static int guest_w = 0;
static int guest_h = 0;
static int guest_prev_app = APP_DESKTOP;
static int guest_lb_sent = 0;   /* a content-area left DOWN is outstanding: release must send UP */
static int guest_rb_sent = 0;
static int guest_last_mx = 0, guest_last_my = 0;
static int guest_last_lb = 0, guest_last_rb = 0;
static int guest_last_second = -1;
static struct sl_event guest_evq[GUEST_EVQ];
static int guest_ev_head = 0, guest_ev_len = 0;
static int guest_ticking = 0;           /* re-entrancy guard for the kyield() background pump */

/* Cached disk usage so opening the DISK window doesn't rescan
 * the whole FAT table on every redraw -- only when it's freshly
 * opened or once a second while it stays open. */
static uint32_t disk_total_cache = 0;
static uint32_t disk_free_cache = 0;
static int disk_cache_valid = 0;

/* ---- Files app state ----
 * Real navigation this time: vfs_get_entries() gives us actual
 * is_dir flags (the previous window used vfs_get_names(), which
 * doesn't -- that's why folders looked exactly like plain files
 * and clicking one did nothing). A synthetic ".." row is
 * prepended whenever we're not at the VFS root so there's a way
 * back up without a keyboard-only `cd ..`.
 */
static char files_names[FILES_MAX_ENTRIES][VFS_NAME_MAX + 1];
static uint8_t files_is_dir[FILES_MAX_ENTRIES];
static int files_count = 0;
static int files_has_up = 0;
static int files_selected = -1;
static int files_scroll = 0;

static const int dock_app_ids[] = {
    APP_ABOUT, APP_DISK, APP_FILES, APP_PAINT, APP_HELP, APP_TERMINAL, APP_MINES
};

static void clamp_window_to_screen(void);

static const struct gui_dock_icon dock_icons[] = {
    { ICON_ABOUT,    "About"    },
    { ICON_DISK,     "Disk"     },
    { ICON_FOLDER,   "Files"    },
    { ICON_PAINT,    "Paint"    },
    { ICON_HELP,     "Help"     },
    { ICON_TERMINAL, "Terminal" },
    { ICON_MINES,    "Mines"    },
};

#define DOCK_COUNT (int)(sizeof(dock_icons) / sizeof(dock_icons[0]))

static const char* app_name(int app)
{
    switch (app) {
        case APP_ABOUT:    return "ABOUT";
        case APP_DISK:     return "DISK";
        case APP_FILES:    return "FILES";
        case APP_HELP:     return "HELP";
        case APP_PAINT:    return "PAINT";
        case APP_TERMINAL: return "TERMINAL";
        case APP_MINES:    return "MINESWEEPER";
        case APP_GUEST:    return "APPLICATION";
        default:           return "";
    }
}

static void format_uint(uint32_t value, char* out)
{
    char buffer[11];
    int index = 0;

    if (value == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }

    while (value > 0 && index < 10) {
        buffer[index++] = (char)('0' + value % 10);
        value /= 10;
    }

    int pos = 0;

    while (index > 0) {
        out[pos++] = buffer[--index];
    }

    out[pos] = '\0';
}

static void format_two_digit(uint8_t value, char* out)
{
    out[0] = (char)('0' + (value / 10) % 10);
    out[1] = (char)('0' + value % 10);
    out[2] = '\0';
}

static void append(char* dest, int* pos, const char* src)
{
    while (*src) {
        dest[(*pos)++] = *src++;
    }
}

/* ---- per-app window geometry ----
 * window_size_for() gives each app's natural/default size (used as
 * the initial size on open, and doubles as the minimum size for
 * About/Disk/Help, whose fixed short text can't usefully shrink
 * further). window_min_size_for()/window_max_size_for() give the
 * actual drag-resize bounds -- Files and Terminal both have real
 * content that scales with size (more/fewer list rows; a bigger/
 * smaller character grid), so they get a wider range than the fixed-
 * text windows.
 */

static void window_size_for(int app, int* w, int* h)
{
    switch (app) {
        case APP_FILES:
            *w = FILES_WINDOW_W;
            *h = 33 + FILES_VISIBLE_ROWS * GUI_LIST_ROW_H + 6;
            break;

        case APP_TERMINAL:
            *w = TERMINAL_COLS * TERMINAL_CHAR_W + 2;
            *h = 33 + TERMINAL_ROWS * TERMINAL_CHAR_H + 2;
            break;

        case APP_ABOUT:
            /* Widths grew alongside the 8px->16px glyph bump -- same
             * text, twice the pixels per character. */
            *w = 440; *h = 33 + 4 * GUI_TEXT_LINE_H + 8;
            break;

        case APP_DISK:
            *w = 440; *h = 33 + 3 * GUI_TEXT_LINE_H + 8;
            break;

        case APP_GUEST:
            /* Fixed to the app's client-area size (open_app() is never used for
             * a guest window, but this doubles as the fallback geometry). */
            *w = guest_w + GUEST_CONTENT_X * 2;
            *h = guest_h + GUEST_CONTENT_Y + 6;
            break;

        case APP_HELP:
        default:
            *w = 560; *h = 33 + 7 * GUI_TEXT_LINE_H + 8;
            break;
    }
}

/* How many list rows the Files window's current height actually
 * has room for -- inverse of window_size_for's APP_FILES formula.
 * At least 1 so a heavily-shrunk window still shows something
 * rather than dividing content into nothing. */
static int files_visible_rows(void)
{
    int rows = (window_h - 33 - 6) / GUI_LIST_ROW_H;
    return rows < 1 ? 1 : rows;
}

/* How many terminal columns/rows the current window size has room
 * for -- inverse of window_size_for's APP_TERMINAL formula. */
static void terminal_cols_rows_for_size(int w, int h, int* cols, int* rows)
{
    *cols = (w - 2) / TERMINAL_CHAR_W;
    *rows = (h - 33 - 2) / TERMINAL_CHAR_H;
    if (*cols < 1) *cols = 1;
    if (*rows < 1) *rows = 1;
}

static void window_min_size_for(int app, int* w, int* h)
{
    switch (app) {
        case APP_FILES:
            *w = FILES_WINDOW_W;
            *h = 33 + 3 * GUI_LIST_ROW_H + 6; /* at least 3 rows visible */
            break;

        case APP_TERMINAL:
            /* At least a genuinely usable 20x8 grid -- smaller than
             * that and most shell output/prompts stop fitting. */
            *w = 20 * TERMINAL_CHAR_W + 2;
            *h = 33 + 8 * TERMINAL_CHAR_H + 2;
            break;

        case APP_GUEST:
            /* Fixed size: a guest window has no resize grip -- its surface is
             * mapped once at fbmap() time and never grows. */
            window_size_for(app, w, h);
            break;

        default:
            /* About/Disk/Help: fixed short text, can't usefully
             * shrink below its natural size. */
            window_size_for(app, w, h);
            break;
    }
}

static void window_max_size_for(int app, int* w, int* h)
{
    switch (app) {
        case APP_FILES:
            *w = FILES_WINDOW_W + 200;
            *h = 33 + 24 * GUI_LIST_ROW_H + 6; /* comfortably under FILES_MAX_ENTRIES */
            break;

        case APP_TERMINAL:
            *w = MAX_TERMINAL_COLS * TERMINAL_CHAR_W + 2;
            *h = 33 + MAX_TERMINAL_ROWS * TERMINAL_CHAR_H + 2;
            break;

        case APP_GUEST:
            window_size_for(app, w, h);   /* same as the minimum: fixed size */
            break;

        default: {
            /* About/Disk/Help: allow growing well past the natural
             * text size -- the extra becomes padding, not reflowed
             * content, but a fixed-can't-grow-at-all window would
             * make the resize grip on those feel broken/dead. */
            int nat_w, nat_h;
            window_size_for(app, &nat_w, &nat_h);
            *w = nat_w + 160;
            *h = nat_h + 160;
            break;
        }
    }
}

static void clamp_window_size(void)
{
    int min_w, min_h, max_w, max_h;
    window_min_size_for(current_app, &min_w, &min_h);
    window_max_size_for(current_app, &max_w, &max_h);

    if (max_w > vga_width())  max_w = vga_width();
    if (max_h > vga_height() - HEADER_TOP) max_h = vga_height() - HEADER_TOP;

    if (window_w < min_w) window_w = min_w;
    if (window_h < min_h) window_h = min_h;
    if (window_w > max_w) window_w = max_w;
    if (window_h > max_h) window_h = max_h;
}

static void clamp_window_to_screen(void)
{
    int max_x = vga_width() - window_w;
    int max_y = vga_height() - window_h;

    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;

    if (window_x < 0) window_x = 0;
    if (window_y < HEADER_TOP) window_y = HEADER_TOP;
    if (window_x > max_x) window_x = max_x;
    if (window_y > max_y) window_y = max_y;
}

static void clear_paint_canvas(void)
{
    for (int i = 0; i < vga_width() * vga_height(); i++) {
        paint_canvas[i] = COL_CANVAS;
    }

    paint_canvas_ready = 1;
    paint_last_x = -1;
    paint_last_y = -1;
}

static void refresh_disk_cache(void)
{
    disk_total_cache = vfs_total_bytes();
    disk_free_cache = vfs_free_bytes();
    disk_cache_valid = 1;
}

static int at_vfs_root(void)
{
    const char* cwd = vfs_get_cwd();
    return cwd[0] == '/' && cwd[1] == '\0';
}

static void refresh_files_listing(void)
{
    files_count = vfs_get_entries(files_names, files_is_dir, FILES_MAX_ENTRIES);
    files_has_up = !at_vfs_root();
    files_selected = -1;
    files_scroll = 0;
}

static void files_total_rows(int* out)
{
    *out = files_count + (files_has_up ? 1 : 0);
}

/* Turns a display-list row (0 = the synthetic ".." row, if
 * present) back into "go up" or a real entry, and performs
 * the navigation. */
static void files_activate(int row)
{
    if (row < 0) return;

    if (files_has_up) {
        if (row == 0) {
            vfs_cd("..");
            refresh_files_listing();
            return;
        }
        row--;
    }

    if (row < 0 || row >= files_count) return;

    if (files_is_dir[row]) {
        vfs_cd(files_names[row]);
        refresh_files_listing();
    } else {
        files_selected = row + (files_has_up ? 1 : 0);
    }
}

static void open_app(int app_id)
{
    current_app = app_id;

    /* Windows always (re)open at a predictable spot and size rather
     * than wherever a previous drag/resize left them -- there's no
     * taskbar list of hidden windows to find a lost one again.
     * Centered on the screen (rather than pinned near the top-left
     * corner) so a window doesn't look lost in the corner of a
     * 1920x1080 desktop -- size has to be known first to center it. */
    window_size_for(app_id, &window_w, &window_h);
    window_x = (vga_width() - window_w) / 2;
    window_y = HEADER_TOP + (vga_height() - HEADER_TOP - window_h) / 3;
    if (window_x < 0) window_x = 0;
    if (window_y < HEADER_TOP) window_y = HEADER_TOP;
    resizing_window = 0;

    if (app_id == APP_PAINT && !paint_canvas_ready) {
        clear_paint_canvas();
    }

    if (app_id == APP_DISK) {
        refresh_disk_cache();
    }

    if (app_id == APP_FILES) {
        refresh_files_listing();
    }

    if (app_id == APP_TERMINAL) {
        terminal_set_size(TERMINAL_COLS, TERMINAL_ROWS);
        terminal_set_origin(window_x + 1, window_y + window_h - TERMINAL_ROWS * TERMINAL_CHAR_H - 1);
        terminal_redraw();
    }
}

static void launch_minesweeper_user(void)
{
    open_app(APP_TERMINAL);
#if defined(__x86_64__)
    struct rtc_time now;
    int mx, my, lb, rb;
    rtc_read(&now);
    mouse_get_state(&mx, &my, &lb, &rb);
    render_frame(&now, mx, my, lb, rb);
    if (process_execute("MINES.ELF") != 0)
        terminal_writestring("Could not load MINES.ELF.\n");
    terminal_writestring("\nStarlightOS> ");
#else
    terminal_writestring("Minesweeper user apps require the x86_64 build.\n");
#endif
}

/* ==========================================================================================
 * Guest (ring-3 app) window -- see user/slwindow.h for the ABI and gui.h for the API.
 * The desktop loop is suspended while a process runs, so everything the loop normally
 * does (kyield, keyboard, mouse edges, click routing, clock redraw) happens here, driven
 * by the app's present()/getevents() syscalls.
 * ======================================================================================= */

static void guest_push(uint32_t type, int32_t x, int32_t y, uint32_t data)
{
    /* Coalesce consecutive moves so a moving mouse cannot fill the queue. */
    if (type == SL_EV_MOVE && guest_ev_len) {
        int last = (guest_ev_head + guest_ev_len - 1) % GUEST_EVQ;
        if (guest_evq[last].type == SL_EV_MOVE) {
            guest_evq[last].x = x;
            guest_evq[last].y = y;
            return;
        }
    }

    if (guest_ev_len >= GUEST_EVQ) {                 /* full: drop the oldest event */
        guest_ev_head = (guest_ev_head + 1) % GUEST_EVQ;
        guest_ev_len--;
    }

    int slot = (guest_ev_head + guest_ev_len) % GUEST_EVQ;
    guest_evq[slot].type = type;
    guest_evq[slot].x = x;
    guest_evq[slot].y = y;
    guest_evq[slot].data = data;
    guest_ev_len++;
}

static int guest_drain(struct sl_event* buf, int count)
{
    int n = 0;
    while (n < count && guest_ev_len) {
        buf[n++] = guest_evq[guest_ev_head];
        guest_ev_head = (guest_ev_head + 1) % GUEST_EVQ;
        guest_ev_len--;
    }
    return n;
}

static int guest_in_content(int x, int y)
{
    return x >= window_x + GUEST_CONTENT_X && x < window_x + GUEST_CONTENT_X + guest_w &&
           y >= window_y + GUEST_CONTENT_Y && y < window_y + GUEST_CONTENT_Y + guest_h;
}

static void guest_render_now(void)
{
    struct rtc_time time;
    int mx, my, lb, rb;
    rtc_read(&time);
    mouse_get_state(&mx, &my, &lb, &rb);
    render_frame(&time, mx, my, lb, rb);
}

static void guest_input(void)
{
    kyield();   /* network + audio background work while an app owns the foreground */

    int c;
    while ((c = keyboard_getchar_nonblocking()) >= 0) guest_push(SL_EV_KEY, 0, 0, (uint32_t)c);

    int mx, my, lb, rb;
    mouse_get_state(&mx, &my, &lb, &rb);
    int dirty = mouse_take_dirty();
    int moved = (mx != guest_last_mx || my != guest_last_my);

    if (lb && !guest_last_lb) {
        handle_mouse_down(mx, my);                   /* close -> QUIT, titlebar -> drag, content -> DOWN */
        need_redraw_hint = 1;
    }

    if (rb && !guest_last_rb && guest_in_content(mx, my)) {
        guest_push(SL_EV_DOWN, mx - (window_x + GUEST_CONTENT_X), my - (window_y + GUEST_CONTENT_Y), SL_BTN_RIGHT);
        guest_rb_sent = 1;
    }

    if (!lb) dragging_window = 0;
    else if (dragging_window) {
        handle_mouse_drag(mx, my);
        need_redraw_hint = 1;
    }

    /* A release belongs to the app whenever the matching press did. */
    if (!lb && guest_lb_sent) {
        guest_push(SL_EV_UP, mx - (window_x + GUEST_CONTENT_X), my - (window_y + GUEST_CONTENT_Y), SL_BTN_LEFT);
        guest_lb_sent = 0;
    }
    if (!rb && guest_rb_sent) {
        guest_push(SL_EV_UP, mx - (window_x + GUEST_CONTENT_X), my - (window_y + GUEST_CONTENT_Y), SL_BTN_RIGHT);
        guest_rb_sent = 0;
    }

    if (moved && !dragging_window)
        guest_push(SL_EV_MOVE, mx - (window_x + GUEST_CONTENT_X), my - (window_y + GUEST_CONTENT_Y), 0);

    guest_last_mx = mx; guest_last_my = my;
    guest_last_lb = lb; guest_last_rb = rb;

    if (moved || dirty) need_redraw_hint = 1;

    struct rtc_time now;                             /* the top-bar clock keeps ticking */
    rtc_read(&now);
    if (now.second != guest_last_second) { guest_last_second = now.second; need_redraw_hint = 1; }
}

static void draw_guest_window(gfx_t* frame)
{
    int cy = gui_window_chrome(frame, "Application", window_x, window_y, window_w, window_h,
                               dragging_window, 1, 0 /* no resize grip */);
    int cx = window_x + GUEST_CONTENT_X;

    for (int row = 0; row < guest_h; row++) {
        int y = cy + row;
        if (y < 0 || y >= vga_height()) continue;
        int x0 = cx < 0 ? 0 : cx;
        int x1 = cx + guest_w > vga_width() ? vga_width() : cx + guest_w;
        if (x1 <= x0) continue;
        memcpy(&frame[(size_t)y * (size_t)vga_width() + (size_t)x0],
               &guest_surface[(size_t)row * (size_t)guest_w + (size_t)(x0 - cx)],
               (size_t)(x1 - x0) * sizeof(gfx_t));
    }
}

/* kyield() background hook: while a guest window is open, pump input and redraw
 * from the desktop's cooperative-yield path too. Network waits (connect/recv/poll)
 * call kyield(), so a blocked app's window stays live. guest_ticking stops the
 * guest_input()->kyield()->guest_background() recursion. */
static void guest_background(void)
{
    if (!guest_surface || guest_ticking) return;
    guest_ticking = 1;
    guest_input();
    if (need_redraw_hint) { need_redraw_hint = 0; guest_render_now(); }
    guest_ticking = 0;
}

int gui_guest_open(int w, int h)
{
    if (guest_surface) return -2;
    if (w < 1 || h < 1 || w + GUEST_CONTENT_X * 2 > vga_width() ||
        h + GUEST_CONTENT_Y + 6 + HEADER_TOP > vga_height()) return -1;

    ksched_register(guest_background);      /* keep the window alive during network waits */

    size_t bytes = (size_t)w * (size_t)h * sizeof(gfx_t);
    gfx_t* raw = (gfx_t*)kcalloc(1, bytes + GUEST_PAGE);
    if (!raw) return -2;
    gfx_t* surface = (gfx_t*)(((uintptr_t)raw + GUEST_PAGE - 1) & ~(uintptr_t)(GUEST_PAGE - 1));

    guest_surface = surface;
    guest_surface_raw = raw;
    guest_w = w;
    guest_h = h;
    guest_prev_app = current_app;
    current_app = APP_GUEST;

    window_w = w + GUEST_CONTENT_X * 2;
    window_h = h + GUEST_CONTENT_Y + 6;
    window_x = (vga_width() - window_w) / 2;
    window_y = HEADER_TOP + (vga_height() - HEADER_TOP - window_h) / 3;
    if (window_x < 0) window_x = 0;
    if (window_y < HEADER_TOP) window_y = HEADER_TOP;

    dragging_window = 0;
    resizing_window = 0;
    guest_ev_head = guest_ev_len = 0;
    guest_lb_sent = guest_rb_sent = 0;
    guest_last_second = -1;
    mouse_get_state(&guest_last_mx, &guest_last_my, &guest_last_lb, &guest_last_rb);
    need_redraw_hint = 1;
    return 0;
}

void gui_guest_close(void)
{
    if (!guest_surface) return;
    kfree(guest_surface_raw);
    guest_surface = 0;
    guest_surface_raw = 0;
    guest_w = guest_h = 0;
    guest_ev_head = guest_ev_len = 0;
    current_app = guest_prev_app;
    need_redraw_hint = 1;
}

int gui_guest_is_open(void) { return guest_surface != 0; }

void gui_guest_dims(int* w, int* h) { if (w) *w = guest_w; if (h) *h = guest_h; }

void* gui_guest_surface(void) { return guest_surface; }

int gui_guest_present(void)
{
    guest_input();
    need_redraw_hint = 0;
    guest_render_now();
    return 0;
}

int gui_guest_wait(struct sl_event* buf, int count, int timeout_ms)
{
    uint32_t start = ktime_ms();

    for (;;) {
        guest_input();

        if (need_redraw_hint) {
            need_redraw_hint = 0;
            guest_render_now();
        }

        if (guest_ev_len) return guest_drain(buf, count);

        if (timeout_ms == 0) return 0;
        if (timeout_ms > 0 && (uint32_t)(ktime_ms() - start) >= (uint32_t)timeout_ms) return 0;
        if (kabort_requested()) return -1;

        __asm__ volatile("sti; hlt; cli" ::: "memory");
    }
}

static void gui_key_handler(char c)
{
    if (current_app == APP_PAINT) {
        if (c == (char)KEY_ESC) {
            current_app = APP_DESKTOP;
            return;
        }

        if (c == 'c' || c == 'C') {
            clear_paint_canvas();
            return;
        }

        if (c >= '1' && c <= '8') {
            paint_color_index = c - '1';
            return;
        }

        return;
    }

    if (current_app == APP_TERMINAL) {
        /* ESC leaves the Terminal app and goes back to the
         * desktop; every other key -- including ones that would
         * otherwise be a GUI shortcut, like digits -- goes
         * straight to the shell, because you're typing into a
         * terminal, not driving the window manager. */
        if (c == (char)KEY_ESC) {
            current_app = APP_DESKTOP;
            return;
        }

        shell_handle_character(c);
        return;
    }

    if (c == (char)KEY_ESC || c == 'q' || c == 'Q') {
        if (current_app != APP_DESKTOP) {
            current_app = APP_DESKTOP;
        }
        return;
    }

    if (current_app == APP_FILES) {
        int total;
        files_total_rows(&total);

        if (c == (char)KEY_UP) {
            if (files_selected < 0) files_selected = 0;
            else if (files_selected > 0) files_selected--;

            if (files_selected < files_scroll) files_scroll = files_selected;
            return;
        }

        if (c == (char)KEY_DOWN) {
            if (files_selected < 0) files_selected = 0;
            else if (files_selected < total - 1) files_selected++;

            int visible = files_visible_rows();
            if (files_selected >= files_scroll + visible) {
                files_scroll = files_selected - visible + 1;
            }
            return;
        }

        if (c == '\n' || c == '\r') {
            files_activate(files_selected);
            return;
        }

        if (c == '\b') {
            if (files_has_up) {
                vfs_cd("..");
                refresh_files_listing();
            }
            return;
        }
    }

    if (c >= '1' && c <= '0' + DOCK_COUNT) {
        int which = dock_app_ids[c - '1'];

        if (which == APP_MINES) {
            launch_minesweeper_user();
            return;
        }

        if (current_app == which) {
            current_app = APP_DESKTOP;
        } else {
            open_app(which);
        }
        return;
    }
}

static void handle_mouse_down(int x, int y)
{
    if (current_app == APP_DESKTOP) {
        int hit = gui_dock_hit(vga_width(), vga_height(), DOCK_COUNT, x, y);

        if (hit >= 0) {
            if (dock_app_ids[hit] == APP_MINES) launch_minesweeper_user();
            else open_app(dock_app_ids[hit]);
        }

        return;
    }

    if (current_app == APP_PAINT) {
        return;
    }

    /* The guest (ring-3 app) window: close sends SL_EV_QUIT, the titlebar
     * drags like any other window, and clicks in the client area become
     * button events for the app. No resize grip -- the surface is fixed. */
    if (current_app == APP_GUEST) {
        if (gui_close_hit(window_x, window_y, window_w, x, y)) {
            guest_push(SL_EV_QUIT, 0, 0, 0);
            need_redraw_hint = 1;
            return;
        }

        if (gui_titlebar_hit(window_x, window_y, window_w, x, y)) {
            dragging_window = 1;
            drag_offset_x = x - window_x;
            drag_offset_y = y - window_y;
            return;
        }

        int cx = x - (window_x + GUEST_CONTENT_X);
        int cy = y - (window_y + GUEST_CONTENT_Y);

        if (cx >= 0 && cx < guest_w && cy >= 0 && cy < guest_h) {
            guest_push(SL_EV_DOWN, cx, cy, SL_BTN_LEFT);
            guest_lb_sent = 1;
        }

        return;
    }

    /* Every windowed app (Files, Terminal, About, Disk, Help) is
     * resizable -- see window_min_size_for/window_max_size_for for
     * how much each one actually allows. */
    if (gui_resize_hit(window_x, window_y, window_w, window_h, x, y)) {
        resizing_window = 1;
        resize_start_w = window_w;
        resize_start_h = window_h;
        resize_start_mx = x;
        resize_start_my = y;
        return;
    }

    if (gui_close_hit(window_x, window_y, window_w, x, y)) {
        current_app = APP_DESKTOP;
        return;
    }

    if (gui_titlebar_hit(window_x, window_y, window_w, x, y)) {
        dragging_window = 1;
        drag_offset_x = x - window_x;
        drag_offset_y = y - window_y;
        return;
    }

    if (current_app == APP_FILES) {
        int content_x = window_x + 3;
        int content_y = window_y + 33;

        int row = gui_list_hit_row(
            content_x, content_y, window_w - 6,
            files_count + (files_has_up ? 1 : 0),
            files_visible_rows(), files_scroll,
            x, y
        );

        if (row >= 0) {
            files_activate(row);
        }
    }
}

static void handle_mouse_drag(int x, int y)
{
    if (resizing_window) {
        window_w = resize_start_w + (x - resize_start_mx);
        window_h = resize_start_h + (y - resize_start_my);
        clamp_window_size();

        if (current_app == APP_FILES) {
            int visible = files_visible_rows();
            int max_scroll = files_count + (files_has_up ? 1 : 0) - visible;
            if (max_scroll < 0) max_scroll = 0;
            if (files_scroll > max_scroll) files_scroll = max_scroll;
        }

        if (current_app == APP_TERMINAL) {
            int cols, rows;
            terminal_cols_rows_for_size(window_w, window_h, &cols, &rows);
            terminal_set_size(cols, rows);
            terminal_set_origin(window_x + 1, window_y + window_h - rows * TERMINAL_CHAR_H - 1);
            /* Deliberately not calling terminal_redraw() here: it
             * would draw straight to real video memory (the render
             * target is only pointed at the off-screen frame buffer
             * from inside render_frame's own per-frame redraw), which
             * is exactly the direct-to-live-framebuffer pattern that
             * used to cause the flicker this whole compositing setup
             * exists to avoid. The main loop already treats an
             * in-progress resize as needing a redraw every frame, so
             * render_frame() picks up this new size and origin on its
             * own via the normal composited path -- nothing here
             * needs to force an extra one. */
        }

        return;
    }

    if (!dragging_window) {
        return;
    }

    window_x = x - drag_offset_x;
    window_y = y - drag_offset_y;

    clamp_window_to_screen();

    if (current_app == APP_TERMINAL) {
        int cols, rows;
        terminal_get_size(&cols, &rows);
        terminal_set_origin(window_x + 1, window_y + window_h - rows * TERMINAL_CHAR_H - 1);
        /* No terminal_redraw() here either -- same reasoning as the
         * resize path just above: it would bypass the off-screen
         * compositing buffer and draw straight to real video memory
         * on every mouse-move packet during a drag. The main loop's
         * own render_frame() call picks up the new origin already. */
    }
}

/* Cheap deterministic scatter hash -- no rand() in a freestanding
 * kernel, so this stands in for one. Same (x,y) always gives the
 * same value, which is fine (even desirable: the wallpaper looks
 * identical every boot instead of "randomly" different, and this
 * only ever runs once in build_desktop() anyway). */
static uint32_t scatter_hash(int x, int y)
{
    uint32_t h32 = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h32 = (h32 ^ (h32 >> 13)) * 1274126177u;
    return h32 ^ (h32 >> 16);
}

/* Piecewise-linear colour ramp through `n` stops at position p (0..(n-1)*256, 8 fractional bits). The
 * result keeps 8 extra bits per channel (value*256) so the caller can dither them away. */
static void ramp16(const gfx_t* stops, int n, int p, unsigned out[3])
{
    int seg = p >> 8, frac = p & 255;
    if (seg >= n - 1) { seg = n - 2; frac = 256; }
    gfx_t a = stops[seg], b = stops[seg + 1];
    out[0] = GFX_R(a) * (unsigned)(256 - frac) + GFX_R(b) * (unsigned)frac;
    out[1] = GFX_G(a) * (unsigned)(256 - frac) + GFX_G(b) * (unsigned)frac;
    out[2] = GFX_B(a) * (unsigned)(256 - frac) + GFX_B(b) * (unsigned)frac;
}

static inline unsigned clampu(int v, int lo, int hi) { return (unsigned)(v < lo ? lo : v > hi ? hi : v); }

/* Paints one layer of a Fuji-like mountain into `desktop`: a broad cone with an anti-aliased silhouette
 * (sub-pixel edge coverage), atmospheric fog towards its base, sun-side warm light / shade-side darkening,
 * and (near layer only) a ragged snowcap. */
static void paint_mountain(int w, int cx, int peak_y, int base_y, int half_base, int near)
{
    int span = base_y - peak_y;
    if (span <= 0) return;

    gfx_t body = near ? COL_MTN_NEAR : COL_MTN_FAR;
    int snow_span = span / 5;

    for (int y = peak_y; y < base_y; y++) {
        int rise = y - peak_y;
        int half256 = rise * half_base * 256 / span;                  /* half width, 1/256 px */
        int el = cx * 256 - half256, er = cx * 256 + half256;          /* silhouette edges, 1/256 px */
        int x0 = el >> 8, x1 = er >> 8;
        if (x0 < 0) x0 = 0;
        if (x1 >= w) x1 = w - 1;

        /* Fog: the lower part of each ridge dissolves into the haze colour. */
        unsigned fog = clampu((rise * 255 / span) - (near ? 130 : 60), 0, 255) * (near ? 150u : 190u) / 255u;
        gfx_t row = gfx_mix(body, COL_MTN_HAZE, fog);

        for (int x = x0; x <= x1; x++) {
            int cov_l = (x + 1) * 256 - el, cov_r = er - x * 256;
            int cov = cov_l < cov_r ? cov_l : cov_r;
            if (cov <= 0) continue;
            if (cov > 256) cov = 256;

            gfx_t c = row;
            if (near && rise < snow_span + 3) {
                int jitter = (int)(scatter_hash(x, y) % 5) - 2;
                if (rise + jitter < snow_span) c = COL_MTN_SNOW;
            }
            /* Lighting: sun sits to the right, so that slope catches warm light and the left side falls dark. */
            int side = half256 > 0 ? (x * 256 - cx * 256) * 255 / half256 : 0;         /* -255 .. 255 */
            if (side > 0) c = gfx_mix(c, COL_SUN_HALO, clampu(side * (near ? 70 : 40) / 255, 0, 255));
            else          c = gfx_mix(c, COL_BLACK, clampu(-side * (near ? 90 : 40) / 255, 0, 255));

            unsigned a = (unsigned)cov >= 256u ? 255u : (unsigned)cov;
            if (!near) a = a * clampu((base_y - y) * 3 * 255 / span, 0, 255) / 255u;   /* far ridge dissolves into the mist at its foot */
            vga_blend_pixel(desktop, x, y, c, a);
        }
    }
}

/* Scatters small cherry-blossom petals across a band of the desktop, alpha-blended so they sit softly on
 * whatever is behind them. `density` is roughly "1 in density pixels checked becomes a candidate". */
static void paint_blossoms(int w, int y0, int y1, int density)
{
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t hv = scatter_hash(x, y);
            if ((hv % (uint32_t)density) != 0)
                continue;

            gfx_t c;
            uint32_t pick = (hv >> 8) % 5;
            if (pick == 0) c = COL_BLOSSOM_LIGHT;
            else if (pick <= 2) c = COL_BLOSSOM_SOFT;
            else c = COL_BLOSSOM_DEEP;
            unsigned a = 150u + ((hv >> 20) & 63u);

            vga_blend_pixel(desktop, x, y, c, a);
            if (((hv >> 16) & 3) == 0) vga_blend_pixel(desktop, x + 1, y, c, a / 2);        /* tiny soft cluster */
            if (((hv >> 18) & 3) == 0) vga_blend_pixel(desktop, x, y + 1, c, a / 3);
        }
    }
}

static void build_desktop(void)
{
    int w = vga_width();
    int h = vga_height();

    /* Sky: a true 24-bit vertical gradient through the eight dusk stops. Each channel keeps 8 fractional
     * bits and is ordered-dithered down to 8, so there is no banding even on the long, slow stretches. */
    static const gfx_t sky[COL_SKY_STOPS] = { COL_SKY_0, COL_SKY_1, COL_SKY_2, COL_SKY_3, COL_SKY_4, COL_SKY_5, COL_SKY_6, COL_SKY_7 };
    static const uint8_t bayer4[4][4] = { {0, 136, 34, 170}, {204, 68, 238, 102}, {51, 187, 17, 153}, {255, 119, 221, 85} };
    for (int y = 0; y < h; y++) {
        unsigned v[3];
        ramp16(sky, COL_SKY_STOPS, h > 1 ? y * (COL_SKY_STOPS - 1) * 256 / (h - 1) : 0, v);
        gfx_t* row = desktop + y * w;
        for (int x = 0; x < w; x++) {
            unsigned t = bayer4[y & 3][x & 3];
            unsigned r = (v[0] >> 8) + ((v[0] & 255u) > t ? 1u : 0u);
            unsigned g = (v[1] >> 8) + ((v[1] & 255u) > t ? 1u : 0u);
            unsigned b = (v[2] >> 8) + ((v[2] & 255u) > t ? 1u : 0u);
            row[x] = RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
        }
    }

    /* Sun: a bright core with an anti-aliased edge and a smooth quadratic glow fading out to 3x its radius. */
    int sun_cx = (w * 3) / 5;
    int sun_cy = h / 3;
    int sun_r = h / 11;
    if (sun_r < 6) sun_r = 6;

    for (int y = sun_cy - sun_r * 3; y <= sun_cy + sun_r * 3; y++) {
        if (y < 0 || y >= h) continue;
        for (int x = sun_cx - sun_r * 3; x <= sun_cx + sun_r * 3; x++) {
            if (x < 0 || x >= w) continue;
            int dx = x - sun_cx, dy = y - sun_cy;
            int d = (int)gfx_isqrt((unsigned)(dx * dx + dy * dy) * 256u);            /* distance in 1/16 px */
            int R = sun_r * 16, G = sun_r * 16 * 3;
            if (d >= G) continue;
            if (d <= R + 8) {
                unsigned core = d <= R - 8 ? 255u : clampu((R + 8 - d) * 255 / 16, 0, 255);
                gfx_t c = gfx_mix(COL_SUN_CORE, COL_SUN_HALO, clampu((d * 255 / (R ? R : 1)) / 3, 0, 255));
                vga_blend_pixel(desktop, x, y, c, core);
                if (core == 255u) continue;
            }
            int g = (G - d) * 255 / (G - R);                                         /* 255 at the core edge .. 0 outside */
            unsigned a = clampu(g * g / 255 * 150 / 255, 0, 150);
            vga_blend_pixel(desktop, x, y, gfx_mix(COL_SUN_HALO, COL_SUN_FAR_HALO, 255u - clampu(g, 0, 255)), a);
        }
    }

    /* Far ridge first (hazy), then the main near peak on top of it -- layered mountains with parallax. */
    paint_mountain(w, (w * 2) / 5, (h * 34) / 100, (h * 62) / 100, (w * 22) / 100, 0);
    paint_mountain(w, w / 2, (h * 40) / 100, (h * 78) / 100, (w * 30) / 100, 1);

    /* Mist where the mountain base meets the foreground: a soft triangular alpha band, not a hard stripe. */
    int haze_y = (h * 76) / 100, haze_h = h / 14;
    for (int i = 0; i < haze_h * 2; i++) {
        int dist = i < haze_h ? i : haze_h * 2 - 1 - i;
        vga_fill_rect_alpha(desktop, 0, haze_y - haze_h + i, w, 1, COL_MTN_HAZE, (unsigned)(dist * 150 / haze_h));
    }

    /* Foreground: a gradient wash that darkens towards the bottom edge, with a soft mist line at its top. */
    int fy = (h * 77) / 100;
    vga_fill_gradient_v(desktop, 0, fy, w, h - fy, COL_WASH_1, COL_WASH_2);
    for (int i = 0; i < 24; i++)
        vga_fill_rect_alpha(desktop, 0, fy + i, w, 1, COL_MTN_HAZE, (unsigned)((24 - i) * 90 / 24));
    paint_blossoms(w, fy, h, 110);
    paint_blossoms(w, (h * 20) / 100, (h * 76) / 100, 900);

    /* Small, understated wordmark bottom-left. */
    vga_draw_string(desktop, 10, h - GUI_DOCK_H - 20, "STARLIGHTOS", gfx_mix(COL_BLOSSOM_LIGHT, COL_WASH_1, 90));
}

/* ---- toast notifications ---- */
static char toast_text[72];
static uint32_t toast_until_ms;

void gui_toast(const char* text)
{
    int i = 0;
    while (text[i] && i < (int)sizeof(toast_text) - 1) { toast_text[i] = text[i] >= 'a' && text[i] <= 'z' ? (char)(text[i] - 32) : text[i]; i++; }
    toast_text[i] = 0;
    toast_until_ms = ktime_ms() + 3500;
}

static void draw_top_and_dock(gfx_t* frame, const struct rtc_time* time)
{
    char h[3], m[3], s[3];

    format_two_digit(time->hour, h);
    format_two_digit(time->minute, m);
    format_two_digit(time->second, s);

    char clock_str[9];
    int i = 0;
    clock_str[i++] = h[0]; clock_str[i++] = h[1]; clock_str[i++] = ':';
    clock_str[i++] = m[0]; clock_str[i++] = m[1]; clock_str[i++] = ':';
    clock_str[i++] = s[0]; clock_str[i++] = s[1];
    clock_str[i] = '\0';

    char net_text[32];
    gfx_t net_color;
    if (net_configured()) {
        char ip[16]; net_fmt_ip(ip, net_get_config()->ip);
        ksnprintf(net_text, sizeof(net_text), "NET %s", ip); net_color = COL_SUCCESS;
    } else if (net_ready()) { ksnprintf(net_text, sizeof(net_text), "NET ..."); net_color = COL_HIGHLIGHT; }
    else { ksnprintf(net_text, sizeof(net_text), "NO NET"); net_color = COL_CLOSE; }
    struct audio_status as; audio_get_status(&as);
    char snd_text[24];
    if (!audio_available()) ksnprintf(snd_text, sizeof(snd_text), "NO AUDIO");
    else if (as.muted) ksnprintf(snd_text, sizeof(snd_text), "MUTE");
    else ksnprintf(snd_text, sizeof(snd_text), "VOL %d", as.volume);

    gui_draw_topbar_status(frame, vga_width(), app_name(current_app), clock_str, net_text, net_color, snd_text,
                           as.level_left, as.level_right);
    if (toast_text[0] && (int32_t)(toast_until_ms - ktime_ms()) > 0) gui_draw_toast(frame, vga_width(), toast_text);
    gui_draw_dock(frame, vga_width(), vga_height(), dock_icons, DOCK_COUNT, hovered_dock_icon);
}

static void draw_about_window(gfx_t* frame)
{
    int cy = gui_window_chrome(frame, "About StarlightOS", window_x, window_y, window_w, window_h, dragging_window, 1, 1);

    const char* lines[] = {
        "STARLIGHTOS 4.0 - AURORA",
        "X86_64, RING-3 ELF APPS, TRUECOLOR",
        "TCP/IP: DHCP DNS HTTP(S) NTP TELNET",
        "AC97 MIXER + MML SEQUENCER",
    };

    for (int i = 0; i < 4; i++) {
        gui_label(frame, lines[i], window_x + 6, cy + i * GUI_TEXT_LINE_H, COL_TEXT_DARK);
    }
}

static void draw_disk_window(gfx_t* frame)
{
    int cy = gui_window_chrome(frame, "Disk", window_x, window_y, window_w, window_h, dragging_window, 1, 1);

    char total_buf[12], free_buf[12];
    char total_line[40], free_line[40];
    int pos;

    format_uint(disk_total_cache, total_buf);
    format_uint(disk_free_cache, free_buf);

    pos = 0;
    append(total_line, &pos, "Total: ");
    append(total_line, &pos, total_buf);
    append(total_line, &pos, " bytes");
    total_line[pos] = '\0';

    pos = 0;
    append(free_line, &pos, "Free:  ");
    append(free_line, &pos, free_buf);
    append(free_line, &pos, " bytes");
    free_line[pos] = '\0';

    const char* lines[] = { total_line, free_line, "See df/ls in the Terminal" };

    for (int i = 0; i < 3; i++) {
        gui_label(frame, lines[i], window_x + 6, cy + i * GUI_TEXT_LINE_H, COL_TEXT_DARK);
    }
}

static void draw_files_window(gfx_t* frame)
{
    char title[32];
    int pos = 0;
    append(title, &pos, "Files: ");
    append(title, &pos, vfs_get_cwd());
    title[pos] = '\0';

    int cy = gui_window_chrome(frame, title, window_x, window_y, window_w, window_h, dragging_window, 1, 1);

    int total = files_count + (files_has_up ? 1 : 0);

    if (total == 0) {
        gui_label(frame, "(empty)", window_x + 6, cy, COL_TEXT_DARK);
        return;
    }

    static const char* items[FILES_MAX_ENTRIES + 1];
    static uint8_t flags[FILES_MAX_ENTRIES + 1];
    int n = 0;

    if (files_has_up) {
        items[n] = "..";
        flags[n] = GUI_LIST_FLAG_DIR;
        n++;
    }

    for (int i = 0; i < files_count; i++) {
        items[n] = files_names[i];
        flags[n] = files_is_dir[i] ? GUI_LIST_FLAG_DIR : 0;
        n++;
    }

    gui_draw_list(
        frame, window_x + 3, cy, window_w - 6,
        items, flags, n,
        files_visible_rows(), files_scroll, files_selected
    );
}

static void draw_help_window(gfx_t* frame)
{
    int cy = gui_window_chrome(frame, "Help", window_x, window_y, window_w, window_h, dragging_window, 1, 1);

    const char* lines[] = {
        "Click a dock icon, or press 1-7",
        "to open About/Disk/Files/Paint/",
        "Help/Terminal. Minesweeper runs as",
        "a separate user-mode program.",
        "Use r x y / f x y, n for new, q quit.",
        "Drag a window's title bar to",
        "move it. Files: click a folder",
        "to open it, .. to go up.",
    };

    for (int i = 0; i < 8; i++) {
        gui_label(frame, lines[i], window_x + 6, cy + i * GUI_TEXT_LINE_H, COL_TEXT_DARK);
    }
}

static void draw_terminal_window(gfx_t* frame)
{
    gui_window_chrome(frame, "Terminal", window_x, window_y, window_w, window_h, dragging_window, 1, 1);

    /* The shell itself renders through terminal_redraw() -- see
     * render_frame() -- this just draws the chrome around
     * wherever its origin currently is. */
}

static void draw_cursor(gfx_t* frame, int x, int y)
{
    static const char* const arrow[] = {
        "X           ", "XW          ", "XWW         ", "XWWW        ",
        "XWWWW       ", "XWWWWW      ", "XWWWWWW     ", "XWWWWWWW    ",
        "XWWWWWWWW   ", "XWWXXXXWWX  ", "XXWX  XWWX  ", "  XX   XWWX ",
        "       XWWX ", "       XX   "
    };
    for (int row = 0; row < 14; row++) {
        for (int col = 0; arrow[row][col]; col++) {
            if (arrow[row][col] == 'X') vga_put_pixel(frame, x + col, y + row, COL_BLACK);
            else if (arrow[row][col] == 'W') vga_put_pixel(frame, x + col, y + row, COL_WHITE);
        }
    }
}

static void plot_brush(int cx, int cy, gfx_t color)
{
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            vga_put_pixel(paint_canvas, cx + dx, cy + dy, color);
        }
    }
}

/*
 * Bresenham's line algorithm, brushing every point along the
 * way. Without this, dragging the mouse quickly across the
 * canvas only paints the handful of points where a PS/2 packet
 * happened to land, leaving gaps -- exactly the "choppy" look.
 */
static void paint_stroke(int x0, int y0, int x1, int y1, gfx_t color)
{
    int dx = x1 - x0;
    if (dx < 0) { dx = -dx; }

    int dy = y1 - y0;
    if (dy < 0) { dy = -dy; }

    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;

    int err = (dx > dy ? dx : -dy) / 2;

    int x = x0;
    int y = y0;

    for (;;) {
        plot_brush(x, y, color);

        if (x == x1 && y == y1) {
            break;
        }

        int prev_err = err;

        if (prev_err > -dx) {
            err -= dy;
            x += sx;
        }

        if (prev_err < dy) {
            err += dx;
            y += sy;
        }
    }
}

static void paint_update(int mx, int my, int left, int right)
{
    if (!left && !right) {
        paint_last_x = -1;
        paint_last_y = -1;
        return;
    }

    gfx_t color = left ? paint_palette[paint_color_index] : COL_CANVAS;

    if (paint_last_x < 0) {
        plot_brush(mx, my, color);
    } else {
        paint_stroke(paint_last_x, paint_last_y, mx, my, color);
    }

    paint_last_x = mx;
    paint_last_y = my;
}

static void draw_paint_toolbar(gfx_t* frame)
{
    /* Bar height, swatch size/pitch, and the label's position all
     * grew alongside the 8px->16px glyph bump -- a 10px bar (the
     * old height) would clip the label's text top and bottom now. */
    vga_fill_rect(frame, 0, 0, vga_width(), 20, COL_HEADERBAR);

    for (int i = 0; i < 8; i++) {
        int x = 4 + i * 18;
        vga_fill_rect(frame, x, 2, 14, 14, paint_palette[i]);

        if (i == paint_color_index) {
            vga_draw_rect(frame, x - 1, 1, 16, 16, COL_WHITE);
        }
    }

    vga_draw_string(frame, 160, 2, "1-8:COLOR C:CLEAR ESC:BACK", COL_WHITE);
}

static void render_frame(const struct rtc_time* time, int mx, int my, int lb, int rb)
{
    /* Compose entirely into the off-screen frame_buffer -- nothing
     * in this function touches real video memory except the single
     * vga_present() at the end. */
    gfx_t* frame = frame_buffer;

    if (current_app == APP_PAINT) {
        paint_update(mx, my, lb, rb);

        memcpy(frame, paint_canvas, (size_t)vga_width() * (size_t)vga_height() * sizeof(gfx_t));

        draw_paint_toolbar(frame);
    } else {
        memcpy(frame, desktop, (size_t)vga_width() * (size_t)vga_height() * sizeof(gfx_t));

        draw_top_and_dock(frame, time);

        switch (current_app) {
            case APP_ABOUT:    draw_about_window(frame); break;
            case APP_DISK:     draw_disk_window(frame); break;
            case APP_FILES:    draw_files_window(frame); break;
            case APP_HELP:     draw_help_window(frame); break;
            case APP_TERMINAL:
                draw_terminal_window(frame);
                terminal_set_render_target(frame);
                terminal_redraw();
                terminal_set_render_target(NULL);
                break;
            case APP_GUEST:    draw_guest_window(frame); break;
            default: break;
        }
    }

    draw_cursor(frame, mx, my);

    vga_present(frame_buffer);
}

void gui_run(void)
{
    gui_running = 1;
    current_app = APP_DESKTOP;
    paint_canvas_ready = 0;
    last_left_button = 0;
    last_right_button = 0;
    dragging_window = 0;
    disk_cache_valid = 0;
    window_x = 40;
    window_y = 30; /* overwritten by open_app()'s centering the moment any window opens */

    mouse_set_bounds(vga_width(), vga_height());
    keyboard_set_dispatch(gui_key_handler);

    build_desktop();

    struct rtc_time time;
    rtc_read(&time);

    int last_second = -1;

    uint32_t last_term_dirty = 0;
    uint32_t last_blink_phase = 0;
    while (gui_running) {
        kyield();          /* network + audio background work */
        {
            static int was_configured, toast_primed;
            int now_cfg = net_configured();
            if (now_cfg && !was_configured && toast_primed) {
                char ip[16], msg[48]; net_fmt_ip(ip, net_get_config()->ip);
                ksnprintf(msg, sizeof(msg), "Network connected %s", ip);
                gui_toast(msg); audio_sfx(SFX_NOTIFY); need_redraw_hint = 1;
            }
            was_configured = now_cfg; toast_primed = 1;
            static uint32_t last_sec_draw; uint32_t sec = ktime_ms() / 250u;
            if (toast_text[0] && (int32_t)(toast_until_ms - ktime_ms()) > -300) { if (sec != last_sec_draw) { last_sec_draw = sec; need_redraw_hint = 1; } }
            else if (audio_busy() && sec != last_sec_draw) { last_sec_draw = sec; need_redraw_hint = 1; }
        }
        keyboard_pump();   /* keys are queued by the IRQ; run them here */
        int mx, my, lb, rb;
        mouse_get_state(&mx, &my, &lb, &rb);

        int need_redraw = mouse_take_dirty() | need_redraw_hint;
        need_redraw_hint = 0;

        int new_hover = (current_app == APP_DESKTOP)
            ? gui_dock_hit(vga_width(), vga_height(), DOCK_COUNT, mx, my)
            : -1;

        if (new_hover != hovered_dock_icon) {
            hovered_dock_icon = new_hover;
            need_redraw = 1;
        }

        if (lb && !last_left_button) {
            handle_mouse_down(mx, my);
            need_redraw = 1;
        }


        if (!lb) {
            dragging_window = 0;
        } else if (dragging_window) {
            handle_mouse_drag(mx, my);
            need_redraw = 1;
        }

        last_left_button = lb;
        last_right_button = rb;

        if (current_app == APP_PAINT && (lb || rb)) {
            need_redraw = 1;
        }

        if (current_app == APP_TERMINAL) {
            /* Redraw only when the character grid changed or the
             * cursor blink phase flipped: a full 1080p compose is
             * the most expensive thing this loop does. */
            uint32_t d = terminal_dirty_count();
            uint32_t phase = ktime_ms() / 500u;
            if (d != last_term_dirty || phase != last_blink_phase) {
                last_term_dirty = d;
                last_blink_phase = phase;
                need_redraw = 1;
            }
        }

        rtc_read(&time);

        if (time.second != last_second) {
            last_second = time.second;
            need_redraw = 1;

            /* Keep disk stats fresh-ish while the window is open,
             * without rescanning the whole FAT table every frame. */
            if (current_app == APP_DISK) {
                refresh_disk_cache();
            }
        }

        if (current_app == APP_DISK && !disk_cache_valid) {
            refresh_disk_cache();
            need_redraw = 1;
        }

        if (need_redraw) {
            render_frame(&time, mx, my, lb, rb);
        }

        __asm__ volatile ("hlt");
    }

    keyboard_set_dispatch(NULL);
}
