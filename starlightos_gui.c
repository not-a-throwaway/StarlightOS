#include <stddef.h>

#include "starlightos_gui.h"
#include "vga.h"

/* Was 14, sized for the old 8px-tall glyphs (title text + a little
 * padding). Glyphs are 16x16 now (see font.h) -- 14px would clip
 * title-bar/topbar text top and bottom, so this grew to comfortably
 * fit 16px text plus real padding. Everything below that reads this
 * macro (window title bars, the topbar, hit-testing, the close
 * button, corner rounding) scales with it automatically; only the
 * few spots that had their *own* separate hardcoded copy of the old
 * "HEADER_H + 3" relationship (gui.c's window-content-Y math) needed
 * a matching manual fix -- see that file's comments. */
#define HEADER_H 30

static int point_in(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* ---- small drawing helpers shared by the icon set ---- */

static void diag_line(gfx_t* frame, int x0, int y0, int x1, int y1, gfx_t color)
{
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2;
    int x = x0, y = y0;

    for (;;) {
        vga_put_pixel(frame, x, y, color);
        if (x == x1 && y == y1) break;
        int e2 = err;
        if (e2 > -dx) { err -= dy; x += sx; }
        if (e2 < dy)  { err += dx; y += sy; }
    }
}

/* Blends `bg` over the pixel at (px,py) by `outside` (0..255): how much of that pixel lies outside the rounded
 * shape. This is what turns a 1-bit corner notch into a properly anti-aliased edge. */
static void corner_pixel(gfx_t* frame, int px, int py, gfx_t bg, unsigned outside)
{
    if (outside == 0) return;
    vga_blend_pixel(frame, px, py, bg, outside);
}

/* Coverage of a corner pixel by the rounding circle: returns 0..255 = how much of the pixel is OUTSIDE the
 * circle of the given radius whose centre sits at (radius, radius) in corner-box coordinates. Fixed-point
 * (1/16 px), integer sqrt. */
static unsigned corner_outside(int radius, int dx, int dy)
{
    int vx = (2 * radius - 2 * dx - 1) * 8, vy = (2 * radius - 2 * dy - 1) * 8;      /* pixel centre -> circle centre, 1/16 px */
    int d = (int)gfx_isqrt((unsigned)(vx * vx + vy * vy));
    int inside16 = radius * 16 - d + 8;                                              /* 0 = fully out, 16 = fully in */
    if (inside16 >= 16) return 0;
    if (inside16 <= 0) return 255;
    return (unsigned)((16 - inside16) * 255 / 16);
}

/* Rounds the four corners of a w x h surface that has already been painted: each corner pixel outside the
 * circle is blended towards `bg[corner]` (top-left, top-right, bottom-left, bottom-right) -- the colour
 * sampled from behind that corner before painting, so a window on a gradient wallpaper reveals the wallpaper. */
static void round_rect_corners(gfx_t* frame, int x, int y, int w, int h, int radius, const gfx_t bg[4])
{
    if (radius <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (radius <= 0) return;

    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            unsigned out = corner_outside(radius, dx, dy);
            if (!out) continue;
            corner_pixel(frame, x + dx, y + dy, bg[0], out);
            corner_pixel(frame, x + w - 1 - dx, y + dy, bg[1], out);
            corner_pixel(frame, x + dx, y + h - 1 - dy, bg[2], out);
            corner_pixel(frame, x + w - 1 - dx, y + h - 1 - dy, bg[3], out);
        }
    }
}

/* One flat backdrop colour on all four corners (buttons, tooltips, icons: small boxes on a known solid). */
static void round_rect_corners_flat(gfx_t* frame, int x, int y, int w, int h, int radius, gfx_t bg)
{
    gfx_t all[4] = { bg, bg, bg, bg };
    round_rect_corners(frame, x, y, w, h, radius, all);
}

/* Icon tiles: a corner radius that scales with the icon (about 1/5 of its size). */
static void round_corners(gfx_t* frame, int x, int y, int w, int h, gfx_t bg)
{
    int r = (w < h ? w : h) / 5;
    if (r < 2) r = 2;
    round_rect_corners_flat(frame, x, y, w, h, r, bg);
}

void gui_soft_shadow(gfx_t* frame, int x, int y, int w, int h, int dx, int dy, int spread, unsigned max_alpha)
{
    if (spread < 1 || w <= 0 || h <= 0) return;
    int ox = x + dx, oy = y + dy;

    /* Strips the offset exposes to the right of / below the surface (the surface itself covers the rest). */
    if (dx > 0) vga_fill_rect_alpha(frame, x + w, oy, dx, h, COL_BLACK, max_alpha);
    if (dy > 0) vga_fill_rect_alpha(frame, ox, y + h, w, dy, COL_BLACK, max_alpha);

    for (int d = 1; d <= spread; d++) {
        unsigned t = (unsigned)(spread - d + 1);
        unsigned a = max_alpha * t * t / (unsigned)(spread * spread);
        if (!a) continue;
        int rx = ox - d, ry = oy - d, rw = w + 2 * d, rh = h + 2 * d;
        vga_fill_rect_alpha(frame, rx, ry, rw, 1, COL_BLACK, a);                 /* top    */
        vga_fill_rect_alpha(frame, rx, ry + rh - 1, rw, 1, COL_BLACK, a);       /* bottom */
        vga_fill_rect_alpha(frame, rx, ry + 1, 1, rh - 2, COL_BLACK, a);        /* left   */
        vga_fill_rect_alpha(frame, rx + rw - 1, ry + 1, 1, rh - 2, COL_BLACK, a);/* right  */
    }
}

void gui_draw_icon(gfx_t* frame, enum gui_icon_kind kind, int x, int y, int size, int hovered)
{
    int s = size;
    gfx_t glow = hovered ? COL_ACCENT_HOVER : COL_BORDER;

    switch (kind) {
        case ICON_FOLDER_UP:
        case ICON_FOLDER: {
            int tab_w = s * 2 / 5;
            int tab_h = s / 5;
            int body_y = y + tab_h;
            int body_h = s - tab_h;

            vga_fill_rect(frame, x, y, tab_w, tab_h + 1, COL_FOLDER_TAB);
            vga_fill_rect(frame, x, body_y, s, body_h, COL_FOLDER_BODY);
            vga_draw_rect(frame, x, body_y, s, body_h, COL_BLACK);
            vga_draw_rect(frame, x, y, tab_w, tab_h + 1, COL_BLACK);

            if (kind == ICON_FOLDER_UP) {
                int cx = x + s / 2;
                int top = body_y + body_h / 4;
                int bot = body_y + body_h - body_h / 4;
                diag_line(frame, cx, top, cx - s / 5, top + s / 4, COL_WHITE);
                diag_line(frame, cx, top, cx + s / 5, top + s / 4, COL_WHITE);
                vga_fill_rect(frame, cx, top, 1, bot - top, COL_WHITE);
            }

            if (hovered) {
                vga_draw_rect(frame, x - 1, y - 1, s + 2, s + 2, glow);
            }
            break;
        }

        case ICON_FILE: {
            int fold = s / 3;
            vga_fill_rect(frame, x, y, s, s, COL_WHITE);
            vga_draw_rect(frame, x, y, s, s, COL_BLACK);
            diag_line(frame, x + s - fold, y, x + s - 1, y + fold, COL_BLACK);
            vga_fill_rect(frame, x + s - fold, y, fold, 1, COL_BORDER);

            for (int i = 0; i < 3; i++) {
                int step = s / 6 > 0 ? s / 6 : 2;
                int ly = y + s / 3 + i * step;
                if (ly < y + s - 2) {
                    vga_fill_rect(frame, x + s / 6, ly, s - s / 3, 1, COL_DIVIDER);
                }
            }
            break;
        }

        case ICON_ABOUT: {
            vga_fill_rect(frame, x, y, s, s, COL_ACCENT);
            round_corners(frame, x, y, s, s, COL_WINDOW_BG);
            int cx = x + s / 2;
            int dot = 1 + (s > 16);
            vga_fill_rect(frame, cx, y + s / 5, dot, dot, COL_WHITE);
            vga_fill_rect(frame, cx, y + s / 5 + s / 6, dot, s / 2, COL_WHITE);
            if (hovered) vga_draw_rect(frame, x - 1, y - 1, s + 2, s + 2, glow);
            break;
        }

        case ICON_DISK: {
            vga_fill_rect(frame, x, y, s, s, COL_DISK_BODY);
            vga_draw_rect(frame, x, y, s, s, COL_BLACK);
            vga_fill_rect(frame, x + s / 6, y + s / 6, s * 2 / 3, s / 4, COL_ACCENT);
            for (int i = 0; i < 2; i++) {
                vga_fill_rect(frame, x + s / 6, y + s / 2 + i * (s / 6 + 1), s * 2 / 3, 1, COL_BORDER);
            }
            if (hovered) vga_draw_rect(frame, x - 1, y - 1, s + 2, s + 2, glow);
            break;
        }

        case ICON_PAINT: {
            vga_fill_rect(frame, x, y, s, s * 4 / 5, COL_PAINT_BODY);
            round_corners(frame, x, y, s, s * 4 / 5, COL_WINDOW_BG);
            static const gfx_t dots[3] = { COL_PAINT_RED, COL_PAINT_GREEN, COL_PAINT_DKGREEN };
            for (int i = 0; i < 3; i++) {
                int dx = x + s / 6 + i * (s / 3);
                int dy = y + s / 3;
                int dsz = s / 6 > 1 ? s / 6 : 2;
                vga_fill_rect(frame, dx, dy, dsz, dsz, dots[i]);
            }
            diag_line(frame, x + s / 2, y + s * 4 / 5, x + s - 1, y + s - 1, COL_BORDER);
            if (hovered) vga_draw_rect(frame, x - 1, y - 1, s + 2, s + 2, glow);
            break;
        }

        case ICON_HELP: {
            vga_fill_rect(frame, x, y, s, s, COL_HIGHLIGHT);
            round_corners(frame, x, y, s, s, COL_WINDOW_BG);
            int cx = x + s / 2;
            int hook_y = y + s / 4;
            diag_line(frame, cx - s / 6, hook_y, cx + s / 6, hook_y, COL_BLACK);
            diag_line(frame, cx + s / 6, hook_y, cx + s / 6, y + s / 2, COL_BLACK);
            diag_line(frame, cx + s / 6, y + s / 2, cx, y + s / 2 + s / 8, COL_BLACK);
            int dot = 1 + (s > 16);
            vga_fill_rect(frame, cx - dot / 2, y + s * 3 / 4, dot, dot, COL_BLACK);
            if (hovered) vga_draw_rect(frame, x - 1, y - 1, s + 2, s + 2, glow);
            break;
        }

        case ICON_TERMINAL: {
            vga_fill_rect(frame, x, y, s, s, COL_HEADERBAR);
            round_corners(frame, x, y, s, s, COL_WINDOW_BG);
            int px0 = x + s / 5, py0 = y + s / 3;
            int px1 = x + s / 2, py1 = y + s / 2;
            int px2 = x + s / 5, py2 = y + s * 2 / 3;
            diag_line(frame, px0, py0, px1, py1, COL_SUCCESS);
            diag_line(frame, px1, py1, px2, py2, COL_SUCCESS);
            vga_fill_rect(frame, x + s / 2, y + s * 2 / 3, s / 3, 1, COL_SUCCESS);
            if (hovered) vga_draw_rect(frame, x - 1, y - 1, s + 2, s + 2, glow);
            break;
        }

        case ICON_MINES: {
            int pad = s / 6;
            int d = s - 2 * pad;
            int cx = x + s / 2, cy = y + s / 2;
            vga_fill_rect(frame, x, y, s, s, COL_ACCENT);
            round_corners(frame, x, y, s, s, COL_WINDOW_BG);
            vga_fill_rect(frame, cx - 1, y + pad / 2, 2, d, COL_HEADERBAR);
            vga_fill_rect(frame, x + pad / 2, cy - 1, d, 2, COL_HEADERBAR);
            vga_fill_rect(frame, cx - d / 3, cy - d / 3, 2 * d / 3, 2 * d / 3, COL_CLOSE);
            vga_draw_rect(frame, cx - d / 3, cy - d / 3, 2 * d / 3, 2 * d / 3, COL_BLACK);
            vga_fill_rect(frame, cx - d / 6, cy - d / 6, 2, 2, COL_WHITE);
            if (hovered) vga_draw_rect(frame, x - 1, y - 1, s + 2, s + 2, glow);
            break;
        }
    }
}

int gui_window_chrome(gfx_t* frame, const char* title, int x, int y, int w, int h, int dragging, int focused, int resizable)
{
    /* Sample the real pixels behind each outer corner before
     * drawing over them, so rounding reveals whatever's actually
     * back there (the desktop's gradient, dock, etc.) instead of
     * guessing one flat fill color that would show up as an
     * obviously-wrong colored square notch. */
    /* Soft shadow first (real alpha over the wallpaper), then sample what is behind each corner -- shadow
     * included -- so the anti-aliased rounding blends towards the true backdrop. */
    gui_soft_shadow(frame, x, y, w, h, 0, 6, 14, 90);

    gfx_t corner_bg[4] = {
        vga_get_pixel(frame, x, y),
        vga_get_pixel(frame, x + w - 1, y),
        vga_get_pixel(frame, x, y + h - 1),
        vga_get_pixel(frame, x + w - 1, y + h - 1),
    };

    vga_fill_rect(frame, x, y, w, h, COL_WINDOW_BG);
    vga_draw_rect(frame, x, y, w, h, COL_BORDER);
    round_rect_corners(frame, x, y, w, h, WINDOW_CORNER_RADIUS, corner_bg);

    gfx_t header_col = dragging ? COL_ACCENT : (focused ? COL_HEADERBAR : COL_TITLEBAR_IDLE);
    /* Subtle top-lit gradient on the header bar (a flat fill would be all 8-bit palettes could do). */
    vga_fill_gradient_v(frame, x + 1, y + 1, w - 2, HEADER_H - 1, gfx_shade(header_col, 14), header_col);
    vga_fill_rect(frame, x + 1, y + HEADER_H, w - 2, 1, COL_DIVIDER);

    /* Header bar shares the window's rounded top corners -- same sampled colours, since it's flush against
     * the same edge. */
    gfx_t header_corner_bg[4] = { corner_bg[0], corner_bg[1], header_col, header_col };
    round_rect_corners(frame, x, y, w, HEADER_H + 1, WINDOW_CORNER_RADIUS, header_corner_bg);

    vga_draw_string(frame, x + 6, y + 5, title, COL_WHITE);

    /* Single round close control, GNOME-style (one button, no
     * minimize/maximize clutter), top-right of the header bar.
     * Was a fixed 8x8 box (matching the old 8x8 glyph exactly);
     * glyphs are 16x16 now, so this grew to 18x18 to actually
     * contain the 'x' with a little breathing room instead of
     * spilling out of the button. */
    int cbx = x + w - 22, cby = y + 4;
    vga_fill_rect(frame, cbx, cby, 18, 18, COL_CLOSE);
    round_rect_corners_flat(frame, cbx, cby, 18, 18, 7, header_col);
    vga_draw_char(frame, cbx + 1, cby + 1, 'x', COL_WHITE);

    if (resizable) {
        /* The "mouse square" resize grip -- a small diagonal-notch
         * handle bottom-right, the standard spot every resizable
         * window (GNOME, KDE, Windows, macOS) puts one. Three
         * descending bars rather than a plain solid square reads
         * as "grab and drag" at a glance rather than as a random
         * colored block. */
        int gx = x + w - GUI_RESIZE_GRIP;
        int gy = y + h - GUI_RESIZE_GRIP;

        for (int i = 0; i < 3; i++) {
            int len = 2 + i * 2;
            int bx = gx + GUI_RESIZE_GRIP - len;
            int by = gy + 2 + i * 3;
            vga_fill_rect(frame, bx, by, len, 2, COL_BORDER);
        }
    }

    return y + HEADER_H + 3;
}

int gui_resize_hit(int x, int y, int w, int h, int mx, int my)
{
    return point_in(mx, my, x + w - GUI_RESIZE_GRIP - 2, y + h - GUI_RESIZE_GRIP - 2,
                     GUI_RESIZE_GRIP + 2, GUI_RESIZE_GRIP + 2);
}

int gui_close_hit(int x, int y, int w, int mx, int my)
{
    /* Kept a couple pixels bigger than the visible 18x18 button
     * (drawn at x+w-22,y+4 in gui_window_chrome) -- a slightly
     * generous click target, same margin philosophy as the old
     * 8x8-button/10x10-hitbox version had. */
    return point_in(mx, my, x + w - 23, y + 3, 20, 20);
}

int gui_titlebar_hit(int x, int y, int w, int mx, int my)
{
    if (!point_in(mx, my, x, y, w, HEADER_H + 1)) return 0;
    if (gui_close_hit(x, y, w, mx, my)) return 0;
    return 1;
}

void gui_label(gfx_t* frame, const char* text, int x, int y, gfx_t color)
{
    vga_draw_string(frame, x, y, text, color);
}

void gui_draw_button(gfx_t* frame, const char* text, int x, int y, int w, int h, int pressed)
{
    vga_fill_rect(frame, x, y, w, h, pressed ? COL_CONTROL_PRESS : COL_ACCENT);
    vga_draw_rect(frame, x, y, w, h, COL_BORDER);
    round_rect_corners_flat(frame, x, y, w, h, 5, COL_WINDOW_BG);
    /* Vertically centers the label glyph (16px tall now, was 8) --
     * a button shorter than that just left-aligns to the top rather
     * than going negative. */
    vga_draw_string(frame, x + 4, y + (h > 17 ? (h - 16) / 2 : 1), text, COL_WHITE);
}

int gui_rect_hit(int x, int y, int w, int h, int mx, int my)
{
    return point_in(mx, my, x, y, w, h);
}

void gui_draw_list(
    gfx_t* frame,
    int x, int y, int w,
    const char* const* items,
    const uint8_t* flags,
    int count,
    int visible_rows,
    int scroll_offset,
    int selected
) {
    int need_scrollbar = count > visible_rows;
    int list_w = need_scrollbar ? w - 6 : w;

    for (int row = 0; row < visible_rows; row++) {
        int idx = scroll_offset + row;
        int ry = y + row * GUI_LIST_ROW_H;

        if (idx >= count) break;

        if (idx == selected) {
            vga_fill_rect(frame, x, ry, list_w, GUI_LIST_ROW_H, COL_ACCENT_HOVER);
        }

        int is_dir = flags && (flags[idx] & GUI_LIST_FLAG_DIR);
        int text_x;

        if (is_dir) {
            /* Icon size/offset grew alongside GUI_LIST_ROW_H (was
             * 10px icon / x+15 text offset when rows were 12px). */
            gui_draw_icon(frame, ICON_FOLDER, x + 3, ry + 3, 16, 0);
            text_x = x + 22;
        } else {
            text_x = x + 6;
        }

        vga_draw_string(frame, text_x, ry + 3, items[idx], COL_TEXT_DARK);
    }

    if (need_scrollbar) {
        int track_x = x + w - 5;
        int track_h = visible_rows * GUI_LIST_ROW_H;

        vga_fill_rect(frame, track_x, y, 5, track_h, COL_DOCK_WELL);

        int thumb_h = track_h * visible_rows / count;
        if (thumb_h < 4) thumb_h = 4;

        int max_scroll = count - visible_rows;
        int thumb_y = (max_scroll > 0)
            ? y + (track_h - thumb_h) * scroll_offset / max_scroll
            : y;

        vga_fill_rect(frame, track_x + 1, thumb_y, 3, thumb_h, COL_ACCENT);
    }
}

int gui_list_hit_row(
    int x, int y, int w,
    int count,
    int visible_rows,
    int scroll_offset,
    int mx, int my
) {
    if (mx < x || mx >= x + w) return -1;
    if (my < y) return -1;

    int row = (my - y) / GUI_LIST_ROW_H;

    if (row < 0 || row >= visible_rows) return -1;

    int idx = scroll_offset + row;

    if (idx < 0 || idx >= count) return -1;

    return idx;
}

void gui_draw_topbar(gfx_t* frame, int screen_w, const char* app_title, const char* clock_str)
{
    vga_fill_rect(frame, 0, 0, screen_w, HEADER_H - 1, COL_HEADERBAR);
    vga_fill_rect(frame, 0, HEADER_H - 1, screen_w, 1, COL_DIVIDER);

    /* Text y-offset (4) and the *16 char-width math below both
     * follow the 16x16 glyph size in font.h -- were y=2 and *8 back
     * when glyphs were 8x8. */
    const char* left = (app_title && app_title[0]) ? app_title : "ACTIVITIES";
    vga_draw_string(frame, 6, 4, left, COL_WHITE);

    int clock_x = screen_w / 2 - vga_string_width(clock_str) / 2;
    vga_draw_string(frame, clock_x, 4, clock_str, COL_WHITE);

    vga_draw_string(frame, screen_w - vga_string_width("STARLIGHTOS") - 6, 4, "STARLIGHTOS", COL_ACCENT_HOVER);
}

void gui_draw_topbar_status(gfx_t* frame, int screen_w, const char* app_title, const char* clock_str,
                            const char* net_text, gfx_t net_color, const char* snd_text, int level_l, int level_r)
{
    gui_draw_topbar(frame, screen_w, app_title, clock_str);
    /* Right-to-left: [net] | [vol + meter] | STARLIGHTOS */
    int x = screen_w - vga_string_width("STARLIGHTOS") - 6 - 18;
    int meter_w = 40;
    x -= meter_w;
    int l = level_l * meter_w / 100, r = level_r * meter_w / 100;
    vga_fill_rect(frame, x, 6, meter_w, 4, COL_DOCK_WELL);
    vga_fill_rect(frame, x, 12, meter_w, 4, COL_DOCK_WELL);
    if (l > 0) vga_fill_rect(frame, x, 6, l, 4, level_l > 80 ? COL_CLOSE : COL_SUCCESS);
    if (r > 0) vga_fill_rect(frame, x, 12, r, 4, level_r > 80 ? COL_CLOSE : COL_SUCCESS);
    x -= 8 + vga_string_width(snd_text);
    vga_draw_string(frame, x, 4, snd_text, COL_WHITE);
    x -= 16;
    vga_fill_rect(frame, x + 6, 4, 1, 16, COL_DIVIDER);
    x -= 8 + vga_string_width(net_text);
    vga_draw_string(frame, x, 4, net_text, net_color);
}

void gui_draw_toast(gfx_t* frame, int screen_w, const char* text)
{
    int tw = vga_string_width(text);
    int w = tw + 32, h = 34;
    int x = screen_w - w - 14, y = HEADER_H + 10;
    gui_soft_shadow(frame, x, y, w, h, 0, 4, 10, 80);
    gfx_t corners[4] = {
        vga_get_pixel(frame, x, y), vga_get_pixel(frame, x + w - 1, y),
        vga_get_pixel(frame, x, y + h - 1), vga_get_pixel(frame, x + w - 1, y + h - 1),
    };
    vga_fill_rect(frame, x, y, w, h, COL_HEADERBAR);
    vga_draw_rect(frame, x, y, w, h, COL_BORDER);
    vga_fill_rect(frame, x, y, 5, h, COL_ACCENT);
    round_rect_corners(frame, x, y, w, h, 8, corners);
    vga_draw_string(frame, x + 18, y + 9, text, COL_WHITE);
}

static void dock_icon_pos(int screen_w, int screen_h, int count, int index, int* out_x, int* out_y)
{
    int total_w = count * (GUI_DOCK_ICON_SIZE + 8);
    int start_x = (screen_w - total_w) / 2;

    *out_x = start_x + index * (GUI_DOCK_ICON_SIZE + 8) + 4;
    *out_y = screen_h - GUI_DOCK_H + (GUI_DOCK_H - GUI_DOCK_ICON_SIZE) / 2;
}

void gui_draw_dock(
    gfx_t* frame, int screen_w, int screen_h,
    const struct gui_dock_icon* icons, int count,
    int hovered_index
) {
    int dock_y = screen_h - GUI_DOCK_H;
    int panel_w = count * (GUI_DOCK_ICON_SIZE + 8) + 24;
    int panel_h = GUI_DOCK_H - 8;
    int panel_x = (screen_w - panel_w) / 2;
    int panel_y = dock_y + 4;
    gui_soft_shadow(frame, panel_x, panel_y, panel_w, panel_h, 0, 3, 10, 70);
    gfx_t panel_corners[4] = {
        vga_get_pixel(frame, panel_x, panel_y),
        vga_get_pixel(frame, panel_x + panel_w - 1, panel_y),
        vga_get_pixel(frame, panel_x, panel_y + panel_h - 1),
        vga_get_pixel(frame, panel_x + panel_w - 1, panel_y + panel_h - 1),
    };
    /* Frosted-glass look: the dock is translucent, the wallpaper shows through it. */
    vga_fill_rect_alpha(frame, panel_x, panel_y, panel_w, panel_h, COL_HEADERBAR, 215);
    vga_draw_rect(frame, panel_x, panel_y, panel_w, panel_h, gfx_mix(COL_BORDER, COL_ACCENT_HOVER, 40));
    round_rect_corners(frame, panel_x, panel_y, panel_w, panel_h, 12, panel_corners);

    for (int i = 0; i < count; i++) {
        int ix, iy;
        dock_icon_pos(screen_w, screen_h, count, i, &ix, &iy);

        int hovered = (i == hovered_index);

        if (hovered) {
            vga_fill_rect(frame, ix - 3, iy - 3, GUI_DOCK_ICON_SIZE + 6, GUI_DOCK_ICON_SIZE + 6, COL_DOCK_WELL);
        }

        gui_draw_icon(frame, icons[i].kind, ix, iy, GUI_DOCK_ICON_SIZE, hovered);

        if (hovered && icons[i].label) {
            /* Tooltip width/height and its y-offset above the dock
             * all grew alongside the 8px->16px glyph bump -- an
             * 11px-tall bubble (the old height) would clip label
             * text now. */
            int tip_w = vga_string_width(icons[i].label) + 8;
            int tip_h = 20;
            int tip_x = ix + GUI_DOCK_ICON_SIZE / 2 - tip_w / 2;
            int tip_y = dock_y - tip_h - 4;

            {
                gfx_t tip_bg[4] = {
                    vga_get_pixel(frame, tip_x, tip_y),
                    vga_get_pixel(frame, tip_x + tip_w - 1, tip_y),
                    vga_get_pixel(frame, tip_x, tip_y + tip_h - 1),
                    vga_get_pixel(frame, tip_x + tip_w - 1, tip_y + tip_h - 1),
                };
                vga_fill_rect(frame, tip_x, tip_y, tip_w, tip_h, COL_HEADERBAR);
                vga_draw_rect(frame, tip_x, tip_y, tip_w, tip_h, COL_BORDER);
                round_rect_corners(frame, tip_x, tip_y, tip_w, tip_h, 3, tip_bg);
            }
            vga_draw_string(frame, tip_x + 4, tip_y + 2, icons[i].label, COL_WHITE);
        }
    }
}

int gui_dock_hit(int screen_w, int screen_h, int count, int mx, int my)
{
    for (int i = 0; i < count; i++) {
        int ix, iy;
        dock_icon_pos(screen_w, screen_h, count, i, &ix, &iy);

        if (point_in(mx, my, ix - 3, iy - 3, GUI_DOCK_ICON_SIZE + 6, GUI_DOCK_ICON_SIZE + 6)) {
            return i;
        }
    }

    return -1;
}
