#ifndef STARLIGHTOS_GUI_H
#define STARLIGHTOS_GUI_H

#include <stdint.h>
#include "gfx.h"

/*
 * StarlightOS desktop SDK
 * ================
 * This is the one real widget layer for StarlightOS apps -- window
 * chrome, lists, buttons, icons, the top bar and the dock all
 * live here so every app (and the desktop shell itself) gets the
 * same GNOME/KDE-flavoured look for free instead of hand-rolling
 * vga_fill_rect() calls per app.
 *
 * Older versions of this header declared a `gui_window` /
 * `gui_label` / `gui_button` / `app_init/update/draw/close` API
 * that was never actually implemented (every function in
 * starlightos_gui.c was a `(void)arg;` no-op) and a matching set of
 * apps/ directory's .c files, whose init/update/draw/close hooks were compiled
 * and linked but never called by anything -- gui.c had its own,
 * separate, ad hoc drawing code for every window instead. Both
 * have been replaced by the real widgets below, which gui.c now
 * actually uses.
 */

/* ---- Theme colors (TrueColor 0xRRGGBB) ----
 * The GNOME/Adwaita-inspired chrome palette plus the "dusk over the mountain" wallpaper stops. Everything
 * that draws pulls its colours from these names, so the whole look can be re-themed here. These are real
 * 24-bit values -- the old 6-bit DAC approximations (e.g. 0x3586E7 for Adwaita blue) are now exact hex.
 */
#define COL_WHITE          0xFFFFFF
#define COL_BLACK          0x000000
#define COL_HEADERBAR      0x241F31   /* dark charcoal titlebar/topbar   */
#define COL_BORDER         0x3D3846   /* window border charcoal          */
#define COL_WINDOW_BG      0xF6F5F4   /* warm light content background   */
#define COL_DOCK_BG        0xC0BFBC   /* dock/panel medium gray          */
#define COL_ACCENT         0x3584E4   /* Adwaita blue                    */
#define COL_ACCENT_HOVER   0x62A0EA   /* Adwaita blue, lighter (hover)   */
#define COL_SUCCESS        0x26A269   /* green accent                    */
#define COL_CLOSE          0xE01B24   /* red accent (close control)      */
#define COL_HIGHLIGHT      0xF5C211   /* yellow accent (selection)       */
#define COL_BEVEL_LIGHT    0x514D5D
#define COL_BEVEL_DARK     0x181420
#define COL_DOCK_WELL      0xB6B2AE
#define COL_TITLEBAR_IDLE  0x6E6A86   /* desaturated titlebar (unfocused) */
#define COL_CONTROL_PRESS  0x2A6AB8   /* pressed button                   */
#define COL_DIVIDER        0x9A9996   /* subtle divider                   */
#define COL_TEXT_DARK      0x1E1E24   /* near-black text on light bg      */
#define COL_CANVAS         0x24222B   /* paint canvas background          */

/* Icon fills */
#define COL_FOLDER_TAB     0xE5A50A
#define COL_FOLDER_BODY    0xF6D32D
#define COL_DISK_BODY      0xC0BFBC
#define COL_PAINT_BODY     0xB5835A
#define COL_PAINT_RED      0xE01B24
#define COL_PAINT_GREEN    0x57E389
#define COL_PAINT_DKGREEN  0x26A269

/* ---- Wallpaper theme ----
 * "Dusk over the mountain": sky stops (top to horizon), mountain/blossom/sun tones. build_desktop() in gui.c
 * interpolates between the sky stops per scanline in full 24-bit colour. */
#define COL_SKY_0          0x141041   /* top of sky: deep night-indigo    */
#define COL_SKY_1          0x201455
#define COL_SKY_2          0x351C6D
#define COL_SKY_3          0x552879
#define COL_SKY_4          0x7D3982
#define COL_SKY_5          0xAA4D79
#define COL_SKY_6          0xDB6D69
#define COL_SKY_7          0xFF9A61   /* bottom of sky: warm coral        */
#define COL_SKY_STOPS      8          /* how many COL_SKY_* stops exist   */
#define COL_SKY_HIGHLIGHT  0xF3BAA2   /* soft glow band near the sun      */
#define COL_SKY_SHADOW     0x281851   /* deep band just above the peaks   */
#define COL_MTN_FAR        0x4D3561   /* hazy/distant mountain silhouette */
#define COL_MTN_NEAR       0x1C1435   /* dark near mountain silhouette    */
#define COL_MTN_SNOW       0xE7CEDB   /* snowcap                          */
#define COL_MTN_HAZE       0x865569   /* base haze blending into the sky  */
#define COL_BLOSSOM_SOFT   0xE382A2   /* soft petal pink                  */
#define COL_BLOSSOM_DEEP   0xB64575   /* deeper magenta-pink              */
#define COL_BLOSSOM_LIGHT  0xFFC6D7   /* near-white highlight petal       */
#define COL_BLOSSOM_BRANCH 0x352024   /* dark branch                      */
#define COL_SUN_CORE       0xFFD28A   /* warm sun/glow core               */
#define COL_SUN_HALO       0xDF9679   /* soft outer halo                  */
#define COL_SUN_FAR_HALO   0xA26171   /* faint far halo into the sky      */
#define COL_WASH_1         0x3A2442   /* foreground wash                  */
#define COL_WASH_2         0x452D51
#define COL_WASH_3         0x313959
#define COL_WASH_4         0x71618A
#define COL_WASH_5         0x9A7992

/* ---- Icon glyphs ----
 * Small flat vector icons drawn with plain rects/lines (no
 * bitmaps) so they scale with whatever icon box size the dock or
 * desktop asks for.
 */
enum gui_icon_kind {
    ICON_ABOUT = 0,
    ICON_DISK,
    ICON_FOLDER,
    ICON_PAINT,
    ICON_HELP,
    ICON_TERMINAL,
    ICON_FILE,
    ICON_FOLDER_UP,
    ICON_MINES,
};

void gui_draw_icon(gfx_t* frame, enum gui_icon_kind kind, int x, int y, int size, int hovered);

/* ---- Window chrome ----
 * Draws a GNOME-style header bar (dark, title left, single round
 * close control right) plus a dithered drop shadow and a bevel,
 * then fills the content area. Returns the y coordinate where
 * app content should start drawing (just below the header bar).
 * When `resizable` is set, also draws the bottom-right resize grip
 * ("mouse square") -- see gui_resize_hit for its hit test.
 */
#define WINDOW_CORNER_RADIUS 7
#define GUI_RESIZE_GRIP 10

/* Soft drop shadow for a (x,y,w,h) surface, drawn BEFORE the surface itself: a falloff of alpha-blended
 * 1px frames fanning out `spread` pixels from the surface rect shifted by (dx,dy) (both >= 0), plus the
 * strips the shift exposes. Real alpha blending, so it darkens whatever wallpaper is underneath. */
void gui_soft_shadow(gfx_t* frame, int x, int y, int w, int h, int dx, int dy, int spread, unsigned max_alpha);

int gui_window_chrome(gfx_t* frame, const char* title, int x, int y, int w, int h, int dragging, int focused, int resizable);

/* Hit test for the close control drawn by gui_window_chrome. */
int gui_close_hit(int x, int y, int w, int mx, int my);

/* Hit test for the draggable titlebar region (excludes the close
 * control) drawn by gui_window_chrome. */
int gui_titlebar_hit(int x, int y, int w, int mx, int my);

/* Hit test for the resize grip drawn by gui_window_chrome when
 * `resizable` is set (bottom-right corner). */
int gui_resize_hit(int x, int y, int w, int h, int mx, int my);

/* ---- Basic controls ---- */
void gui_label(gfx_t* frame, const char* text, int x, int y, gfx_t color);
void gui_draw_button(gfx_t* frame, const char* text, int x, int y, int w, int h, int pressed);
int gui_rect_hit(int x, int y, int w, int h, int mx, int my);

/* ---- Scrollable list (used by Files and any future list app) ----
 * `flags` is a per-item bitmask: bit 0 set means "draw the
 * folder marker for this row". `selected` highlights one row
 * (-1 for none). Returns nothing; use gui_list_hit_row to turn a
 * click back into a row index.
 */
/* Was 12, sized for 8px-tall glyphs; the font pipeline now renders
 * 16x16 glyphs (see font.h), so a 12px-tall row would clip text.
 * 16px text + ~6px of padding. */
#define GUI_LIST_ROW_H 22
/* One line of body text (About/Disk/Help windows): 16px glyph +
 * ~6px line gap. Was an unnamed literal "12" scattered across
 * gui.c; named here once the font size stopped being safe to
 * hardcode in three different places. */
#define GUI_TEXT_LINE_H 22
#define GUI_LIST_FLAG_DIR (1u << 0)

void gui_draw_list(
    gfx_t* frame,
    int x, int y, int w,
    const char* const* items,
    const uint8_t* flags,
    int count,
    int visible_rows,
    int scroll_offset,
    int selected
);

/* Returns the row index (0-based, already offset by scroll) a
 * click at (mx,my) landed on inside a list drawn with the same
 * geometry, or -1 if the click missed the list entirely. */
int gui_list_hit_row(
    int x, int y, int w,
    int count,
    int visible_rows,
    int scroll_offset,
    int mx, int my
);

/* ---- Top bar + dock (GNOME topbar, KDE-ish bottom dock) ---- */
/* Top bar with live status indicators on the right (network text/colour, volume text, L/R level 0-100). */
void gui_draw_topbar_status(gfx_t* frame, int screen_w, const char* app_title, const char* clock_str,
                            const char* net_text, gfx_t net_color, const char* snd_text, int level_l, int level_r);
/* Rounded toast notification below the top bar, right-aligned. */
void gui_draw_toast(gfx_t* frame, int screen_w, const char* text);
void gui_draw_topbar(gfx_t* frame, int screen_w, const char* app_title, const char* clock_str);

struct gui_dock_icon {
    enum gui_icon_kind kind;
    const char* label;
};

/* Sized for the 1920x1080 HD desktop -- proportionate to a real
 * dock (GNOME dash / KDE Latte-style) instead of the old 640x480-era
 * 28px icons, which would look lost on a full HD canvas. Every
 * dock layout/hit-test function reads these macros, not a literal
 * number, so this is a safe, self-contained bump. */
#define GUI_DOCK_ICON_SIZE 44
#define GUI_DOCK_H 64

void gui_draw_dock(
    gfx_t* frame, int screen_w, int screen_h,
    const struct gui_dock_icon* icons, int count,
    int hovered_index
);

/* Returns the dock icon index under (mx,my), or -1. */
int gui_dock_hit(int screen_w, int screen_h, int count, int mx, int my);

#endif
