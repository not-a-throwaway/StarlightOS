#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>
#include "gfx.h"

/*
 * The fixed-pitch character terminal kernel.c implements. It
 * always rendered into the framebuffer from a full char/color
 * backing store (screen_chars/screen_colors) -- terminal_set_origin()
 * just adds an (x,y) pixel offset to where that grid gets drawn, so
 * gui.c can host it inside a window instead of pinned at (0,0).
 *
 * The backing store is sized to MAX_TERMINAL_COLS x MAX_TERMINAL_ROWS
 * (big enough to cover the whole screen at 8x8/cell), but the grid
 * actually in use -- what gets scrolled, wrapped into, and redrawn
 * -- is a smaller "active" cols x rows window into that, set with
 * terminal_set_size(). This is what makes the Terminal window
 * resizable: growing/shrinking it just changes the active size, no
 * reflow of already-written lines (same as a real terminal emulator
 * resize -- existing rows don't rewrap, only future writes see the
 * new width). Starts at TERMINAL_COLS x TERMINAL_ROWS -- bumped from
 * the original 40x25 (sized for the old 320x200 default mode) to a
 * roomier 80x32 default at a tight 9x16 pixel character pitch
 * (JetBrains Mono ink is at most 8 px wide, so 9 leaves a 1 px gutter;
 * it was 12, which looked loosely spaced) --
 * until something calls terminal_set_size(). MAX_TERMINAL_COLS/ROWS
 * is the hard backing-store ceiling (the resize grip's max size and
 * the boot-log scrollback capacity); left as-is since it's a window
 * inside a much bigger desktop now, not the whole screen.
 */
#define MAX_TERMINAL_COLS 80
#define MAX_TERMINAL_ROWS 56
#define TERMINAL_COLS   80
#define TERMINAL_ROWS   32
#define TERMINAL_CHAR_W 9
#define TERMINAL_CHAR_H 16

void terminal_initialize(void);
void terminal_clear(void);
void terminal_putchar(char character);
void terminal_writestring(const char* string);
uint8_t terminal_get_color(void);
int terminal_set_color(uint8_t fg, uint8_t bg);

/* Repaints every cell from the backing store at the current
 * origin. Called after the origin changes, and after anything
 * else (like the desktop) has drawn over the framebuffer. */
void terminal_redraw(void);

/* Moves where the terminal grid draws itself, in pixels. Doesn't
 * redraw by itself -- call terminal_redraw() after. */
void terminal_set_origin(int x, int y);
void terminal_get_origin(int* out_x, int* out_y);

/* Changes the active grid size (clamped to
 * 1..MAX_TERMINAL_COLS/ROWS). The cursor and any on-screen content
 * outside the new bounds is clamped/clipped, not reflowed -- same
 * behavior a real terminal emulator resize has. Doesn't redraw by
 * itself -- call terminal_redraw() after. */
void terminal_set_size(int cols, int rows);
void terminal_get_size(int* out_cols, int* out_rows);

/* Where the terminal grid draws itself. Pass NULL (the default)
 * to draw straight to real video memory -- what the full-screen
 * boot terminal wants. The GUI passes its off-screen composition
 * buffer here for the duration of one Terminal-window redraw, so
 * the grid ends up in the same single vga_present() as everything
 * else instead of being visible mid-draw on real hardware memory. */
void terminal_set_render_target(gfx_t* target);

/* Output capture: while active, terminal_putchar() appends to the buffer
 * instead of drawing (used by shell pipes and > redirection). */
#include <stddef.h>
void   terminal_capture_begin(char* buffer, size_t capacity);
size_t terminal_capture_end(void);
int    terminal_capturing(void);
/* Nested capture + direct (stderr) output; see kernel.c. */
struct terminal_capture_state { char* buf; size_t cap; size_t len; };
void   terminal_capture_save(struct terminal_capture_state* st);
void   terminal_capture_restore(const struct terminal_capture_state* st);
void   terminal_write_direct(const char* string);

/* Scrollback viewing (PgUp/PgDn). delta > 0 scrolls back in history. */
void terminal_scroll_view(int delta);
int  terminal_scroll_offset(void);
int  terminal_scrollback_lines(void);
/* Increments whenever the terminal's contents change (for redraw throttling). */
uint32_t terminal_dirty_count(void);
/* Cursor positioning for line editing; terminal_scroll_total() counts how
 * many times the screen has scrolled (so callers can track a moving row). */
void terminal_get_cursor(int* row, int* col);
void terminal_set_cursor(int row, int col);
uint32_t terminal_scroll_total(void);

#endif
