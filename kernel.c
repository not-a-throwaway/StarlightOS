#include <stdint.h>
#include <stddef.h>

#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "mouse.h"
#include "shell.h"
#include "disk.h"
#include "vfs.h"
#include "multiboot.h"
#include "gui.h"
#include "vga.h"
#include "font8x8.h"
#include "font.h"
#include "terminal.h"
#include "process.h"
#include "net.h"
#include "poll.h"
#include "ac97.h"
#include "serial.h"
#include "kheap.h"
#include "ktime.h"
#include "ksched.h"
#include "kprintf.h"
#include "kstring.h"
#include "audio.h"

/*
 * The shell used to render into the hardware's own 80x25 text
 * mode (0xB8000) and the GUI lived in a completely separate
 * 320x200 graphics mode, so entering/leaving the GUI meant a
 * full VGA register reprogram each way. Now everything -- shell
 * and GUI alike -- renders into the same framebuffer the whole
 * time, so there's no mode switch at all: the GUI hosts the
 * terminal in an actual window (see gui.c's Terminal app) by
 * moving this grid's origin and calling terminal_redraw(),
 * instead of the two being separate full-screen modes.
 *
 * The trade-off is real and worth naming: at 8x8 pixels per
 * character, this "terminal" is 40 columns x 25 rows instead of
 * 80x25, so more output wraps than it used to.
 */
#define GFX_CHAR_W TERMINAL_CHAR_W
#define GFX_CHAR_H TERMINAL_CHAR_H

/* Backing store is sized to the maximum the Terminal window could
 * ever be resized to; active_cols/active_rows (initially the
 * original fixed 40x25) is the viewport actually in use -- see
 * terminal.h's file header for why. */
static char screen_chars[MAX_TERMINAL_ROWS][MAX_TERMINAL_COLS];
static uint8_t screen_colors[MAX_TERMINAL_ROWS][MAX_TERMINAL_COLS];
static int active_cols = TERMINAL_COLS;
static int active_rows = TERMINAL_ROWS;

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;

/* ---- scrollback: lines that scrolled off the top, viewable with PgUp/PgDn ---- */
#define SB_LINES 1000
static char sb_chars[SB_LINES][MAX_TERMINAL_COLS];
static uint8_t sb_colors[SB_LINES][MAX_TERMINAL_COLS];
static int sb_count = 0;       /* valid lines in the ring */
static int sb_next = 0;        /* ring slot the next pushed line lands in */
static int sb_view = 0;        /* how many lines back the viewport is (0 = live) */
static volatile uint32_t terminal_dirty = 0;
static uint32_t scroll_total = 0;

/* ---- output capture (pipes / redirection) ---- */
static char* capture_buf = NULL;
static size_t capture_cap = 0, capture_len = 0;

/* Where the grid's top-left cell lands in framebuffer pixels.
 * (0,0) at boot, so the pre-GUI boot log still behaves exactly
 * like a plain full-screen terminal; gui.c moves this once the
 * Terminal app becomes a window. */
static int term_origin_x = 0;
static int term_origin_y = 0;

static int cursor_row = -1;
static int cursor_col = -1;

static volatile uint32_t timer_ticks = 0;
volatile uint32_t keyboard_irq_count = 0;
volatile uint32_t mouse_irq_count = 0;

static uint32_t multiboot_mem_lower = 0;
static uint32_t multiboot_mem_upper = 0;
static int multiboot_mem_valid = 0;

static uint8_t make_color(
    uint8_t foreground,
    uint8_t background
)
{
    return foreground | (background << 4);
}

/*
 * Redraws exactly one character cell from the backing store --
 * background fill, then the glyph on top. Used both for normal
 * output and to "erase" the cursor mark by repainting whatever
 * is actually supposed to be there.
 */
/* NULL means "draw straight to real video memory" -- see
 * terminal_set_render_target(). */
static gfx_t* term_render_target = NULL;

static gfx_t* term_fb(void)
{
    return term_render_target ? term_render_target : vga_framebuffer();
}

void terminal_set_render_target(gfx_t* target)
{
    term_render_target = target;
}

gfx_t* terminal_get_render_target(void)
{
    return term_render_target;
}

static void cell_source(size_t row, size_t col, char* ch, uint8_t* color)
{
    int line = (int)row - sb_view;                       /* <0 => scrollback */
    if (line >= 0) { *ch = screen_chars[line][col]; *color = screen_colors[line][col]; return; }
    int back = -line;                                    /* 1 = newest scrollback line */
    if (back > sb_count) { *ch = ' '; *color = terminal_color; return; }
    int slot = (sb_next - back + SB_LINES * 2) % SB_LINES;
    *ch = sb_chars[slot][col]; *color = sb_colors[slot][col];
}

static void gfx_draw_cell(size_t row, size_t col)
{
    gfx_t* fb = term_fb();

    char cell_ch;
    uint8_t color;
    cell_source(row, col, &cell_ch, &color);
    uint8_t fg = color & 0x0F;
    uint8_t bg = (color >> 4) & 0x0F;

    int px = term_origin_x + (int)col * GFX_CHAR_W;
    int py = term_origin_y + (int)row * GFX_CHAR_H;

    vga_fill_rect(fb, px, py, GFX_CHAR_W, GFX_CHAR_H, gfx_vga16(bg));
    vga_draw_char_clipped(fb, px, py, cell_ch, gfx_vga16(fg), GFX_CHAR_W);
    if (!term_render_target) vga_fb_update(px, py, GFX_CHAR_W, GFX_CHAR_H);   /* drew straight to "video memory" */
}

static void gfx_draw_cursor(size_t row, size_t col)
{
    gfx_t* fb = term_fb();

    int px = term_origin_x + (int)col * GFX_CHAR_W;
    int py = term_origin_y + (int)row * GFX_CHAR_H;

    uint8_t fg = terminal_color & 0x0F;

    /* An underline rather than a solid block, so it doesn't
     * hide whatever character is already sitting under it. */
    vga_fill_rect(fb, px, py + GFX_CHAR_H - 2, GFX_CHAR_W, 2, gfx_vga16(fg));
    if (!term_render_target) vga_fb_update(px, py + GFX_CHAR_H - 2, GFX_CHAR_W, 2);
}

void terminal_set_origin(int x, int y)
{
    term_origin_x = x;
    term_origin_y = y;
}

void terminal_get_origin(int* out_x, int* out_y)
{
    *out_x = term_origin_x;
    *out_y = term_origin_y;
}

void terminal_move_cursor(size_t row, size_t col)
{
    if (cursor_row >= 0) {
        gfx_draw_cell((size_t)cursor_row, (size_t)cursor_col);
    }

    if (sb_view == 0 && row < (size_t)active_rows && col < (size_t)active_cols) {
        gfx_draw_cursor(row, col);
    }

    cursor_row = (int)row;
    cursor_col = (int)col;
}

/*
 * Repaints the whole screen from the backing store. Called once
 * at boot (nothing to repaint yet, but harmless) and, more
 * importantly, right after the GUI hands control back -- it
 * drew over this same framebuffer, so the shell's screen needs
 * to be redrawn on top of whatever it left behind.
 */
void terminal_redraw(void)
{
    int saved_cursor_row = cursor_row;
    int saved_cursor_col = cursor_col;

    cursor_row = -1;
    cursor_col = -1;

    for (size_t row = 0; row < (size_t)active_rows; row++) {
        for (size_t col = 0; col < (size_t)active_cols; col++) {
            gfx_draw_cell(row, col);
        }
    }

    if (saved_cursor_row >= 0) {
        terminal_move_cursor((size_t)saved_cursor_row, (size_t)saved_cursor_col);
    }
}

void terminal_set_size(int cols, int rows)
{
    if (cols < 1) cols = 1;
    if (cols > MAX_TERMINAL_COLS) cols = MAX_TERMINAL_COLS;
    if (rows < 1) rows = 1;
    if (rows > MAX_TERMINAL_ROWS) rows = MAX_TERMINAL_ROWS;

    int old_cols = active_cols;
    int old_rows = active_rows;

    /* Growing reveals cells that may never have been drawn into --
     * clear them to blank instead of showing whatever's sitting in
     * the backing store out past anything that's ever been written.
     * Shrinking deliberately does NOT clear anything: the hidden
     * cells stay exactly as they were, so growing back later
     * restores them -- same "resize doesn't lose scrollback"
     * behavior a real terminal emulator has. */
    if (cols > old_cols) {
        for (int row = 0; row < old_rows; row++) {
            for (int col = old_cols; col < cols; col++) {
                screen_chars[row][col] = ' ';
                screen_colors[row][col] = terminal_color;
            }
        }
    }

    if (rows > old_rows) {
        for (int row = old_rows; row < rows; row++) {
            for (int col = 0; col < cols; col++) {
                screen_chars[row][col] = ' ';
                screen_colors[row][col] = terminal_color;
            }
        }
    }

    active_cols = cols;
    active_rows = rows;

    /* Clamp the write cursor and the drawn cursor mark into the
     * new bounds so a shrink can't leave either pointing outside
     * the now-smaller active grid. */
    if (terminal_column >= (size_t)active_cols) terminal_column = (size_t)(active_cols - 1);
    if (terminal_row >= (size_t)active_rows) terminal_row = (size_t)(active_rows - 1);
    if (cursor_row >= active_rows) cursor_row = -1;
    if (cursor_col >= active_cols) cursor_col = -1;
}

void terminal_get_size(int* out_cols, int* out_rows)
{
    *out_cols = active_cols;
    *out_rows = active_rows;
}

void terminal_clear(void)
{
    for (size_t row = 0; row < (size_t)active_rows; row++) {
        for (size_t col = 0; col < (size_t)active_cols; col++) {
            screen_chars[row][col] = ' ';
            screen_colors[row][col] = terminal_color;
        }
    }

    terminal_row = 0;
    terminal_column = 0;

    cursor_row = -1;
    cursor_col = -1;

    terminal_redraw();
}

void terminal_initialize(void)
{
    terminal_row = 0;
    terminal_column = 0;

    terminal_color =
        make_color(0x0F, 0x00);

    terminal_clear();
}

uint8_t terminal_get_color(void)
{
    return terminal_color;
}

/*
 * fg/bg are standard VGA text-mode color numbers, 0-15 --
 * these index the same 16-color palette vga_init_default_palette()
 * sets up, so existing color choices still look the same.
 */
int terminal_set_color(uint8_t fg, uint8_t bg)
{
    if (fg > 15 || bg > 15) {
        return -1;
    }

    terminal_color = make_color(fg, bg);

    return 0;
}

static void terminal_scroll(void)
{
    for (size_t col = 0; col < MAX_TERMINAL_COLS; col++) {
        sb_chars[sb_next][col] = screen_chars[0][col];
        sb_colors[sb_next][col] = screen_colors[0][col];
    }
    sb_next = (sb_next + 1) % SB_LINES;
    if (sb_count < SB_LINES) sb_count++;
    scroll_total++;

    for (size_t row = 1; row < (size_t)active_rows; row++) {
        for (size_t col = 0; col < (size_t)active_cols; col++) {
            screen_chars[row - 1][col] = screen_chars[row][col];
            screen_colors[row - 1][col] = screen_colors[row][col];
        }
    }

    for (size_t col = 0; col < (size_t)active_cols; col++) {
        screen_chars[active_rows - 1][col] = ' ';
        screen_colors[active_rows - 1][col] = terminal_color;
    }

    terminal_row = (size_t)(active_rows - 1);

    cursor_row = -1;
    cursor_col = -1;

    terminal_redraw();
}

void terminal_get_cursor(int* row, int* col) { *row = (int)terminal_row; *col = (int)terminal_column; }

void terminal_set_cursor(int row, int col)
{
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (row >= active_rows) row = active_rows - 1;
    if (col >= active_cols) col = active_cols - 1;
    terminal_row = (size_t)row;
    terminal_column = (size_t)col;
    terminal_dirty++;
    terminal_move_cursor(terminal_row, terminal_column);
}

uint32_t terminal_scroll_total(void) { return scroll_total; }

void terminal_capture_begin(char* buffer, size_t capacity)
{
    capture_buf = buffer; capture_cap = capacity; capture_len = 0;
    if (capture_cap) capture_buf[0] = '\0';
}

size_t terminal_capture_end(void)
{
    size_t n = capture_len;
    capture_buf = NULL; capture_cap = 0; capture_len = 0;
    return n;
}

int terminal_capturing(void) { return capture_buf != NULL; }

/* Nested capture support ($(...) inside a pipeline stage that is itself being
 * captured): save the current capture state, run something with its own
 * capture, then restore. */
void terminal_capture_save(struct terminal_capture_state* st)
{
    st->buf = capture_buf; st->cap = capture_cap; st->len = capture_len;
}

void terminal_capture_restore(const struct terminal_capture_state* st)
{
    capture_buf = st->buf; capture_cap = st->cap; capture_len = st->len;
}

/* stderr: written straight to the screen even while stdout is captured by a
 * pipe or a > redirection, so `cmd | wc` never counts error messages. */
void terminal_write_direct(const char* string)
{
    struct terminal_capture_state st;
    terminal_capture_save(&st);
    capture_buf = NULL; capture_cap = 0; capture_len = 0;
    terminal_writestring(string);
    terminal_capture_restore(&st);
}

uint32_t terminal_dirty_count(void) { return terminal_dirty; }

void terminal_scroll_view(int delta)
{
    int old = sb_view;
    sb_view += delta;
    if (sb_view < 0) sb_view = 0;
    if (sb_view > sb_count) sb_view = sb_count;
    if (sb_view != old) { terminal_dirty++; terminal_redraw(); }
}

int terminal_scroll_offset(void) { return sb_view; }
int terminal_scrollback_lines(void) { return sb_count; }

void terminal_putchar(char character)
{
    if (capture_buf) {
        if (character != '\r' && capture_len + 1 < capture_cap) {
            capture_buf[capture_len++] = character;
            capture_buf[capture_len] = '\0';
        } else if (character != '\r') {
            /* The intermediate pipe buffer is full: a producer like `yes`
             * would spin forever writing into an already-full pipe. Raise
             * the abort flag so it stops (a tiny "SIGPIPE" analogue). */
            kabort_raise();
        }
        return;
    }
    terminal_dirty++;
    if (sb_view != 0) { sb_view = 0; terminal_redraw(); }

    /* Mirror everything to COM1: gives a scrollback-free log for headless
     * runs and automated tests (qemu -serial file:...). */
    if (character == '\n') serial_putc('\r');
    if (character != '\r' && character != '\b') serial_putc(character);
    else if (character == '\b') { serial_putc('\b'); serial_putc(' '); serial_putc('\b'); }

    if (character == '\n') {
        terminal_column = 0;
        terminal_row++;

        if (terminal_row >= (size_t)active_rows) {
            terminal_scroll();
        }

        terminal_move_cursor(terminal_row, terminal_column);
        return;
    }

    if (character == '\r') {
        terminal_column = 0;
        terminal_move_cursor(terminal_row, terminal_column);
        return;
    }

    if (character == '\b') {
        if (terminal_column > 0) {
            terminal_column--;

            screen_chars[terminal_row][terminal_column] = ' ';
            screen_colors[terminal_row][terminal_column] = terminal_color;

            gfx_draw_cell(terminal_row, terminal_column);
        }

        terminal_move_cursor(terminal_row, terminal_column);
        return;
    }

    screen_chars[terminal_row][terminal_column] = character;
    screen_colors[terminal_row][terminal_column] = terminal_color;

    gfx_draw_cell(terminal_row, terminal_column);

    terminal_column++;

    if (terminal_column >= (size_t)active_cols) {
        terminal_column = 0;
        terminal_row++;

        if (terminal_row >= (size_t)active_rows) {
            terminal_scroll();
        }
    }

    terminal_move_cursor(terminal_row, terminal_column);
}

void terminal_writestring(const char* string)
{
    for (size_t i = 0;
         string[i] != '\0';
         i++) {
        terminal_putchar(string[i]);
    }
}

void terminal_write_uint(uint32_t value)
{
    char buffer[11];
    size_t index = 0;

    if (value == 0) {
        terminal_putchar('0');
        return;
    }

    while (value > 0) {
        buffer[index++] =
            (char)('0' + value % 10);

        value /= 10;
    }

    while (index > 0) {
        index--;

        terminal_putchar(buffer[index]);
    }
}

void terminal_write_hex(uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    terminal_writestring("0x");

    for (int shift = 28; shift >= 0; shift -= 4) {
        terminal_putchar(digits[(value >> shift) & 0xF]);
    }
}

uint32_t timer_get_ticks(void)
{
    return timer_ticks;
}

int multiboot_get_memory(uint32_t* lower_kb, uint32_t* upper_kb)
{
    if (!multiboot_mem_valid) {
        return -1;
    }

    if (lower_kb) { *lower_kb = multiboot_mem_lower; }
    if (upper_kb) { *upper_kb = multiboot_mem_upper; }

    return 0;
}

#if defined(__x86_64__)
/* idt64.s passes the two qwords pushed by the per-vector stub and
 * CPU exception frame: vector number, then error code. */
struct registers {
    uint64_t int_no;
    uint64_t err_code;
    uint64_t rip;
    uint64_t cs;
};
#else
struct registers {
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;

    uint32_t int_no;
    uint32_t err_code;
};
#endif

void isr_handler(struct registers* regs)
{
#if defined(__x86_64__)
    if ((regs->cs & 3u) == 3u) {
        process_handle_user_fault(regs->int_no, regs->err_code, regs->rip);
        return;
    }
#endif
    terminal_writestring("EXCEPTION: ");
    terminal_write_hex((uint32_t)regs->int_no);
    terminal_writestring(" error ");
    terminal_write_hex((uint32_t)regs->err_code);
#if defined(__x86_64__)
    terminal_writestring(" at ");
    terminal_write_hex((uint32_t)regs->rip);
#endif
    terminal_writestring(" ");

    switch (regs->int_no) {
        case 0:
            terminal_writestring("Divide by zero");
            break;
        case 1:
            terminal_writestring("Debug");
            break;
        case 2:
            terminal_writestring("Non-maskable interrupt");
            break;
        case 3:
            terminal_writestring("Breakpoint");
            break;
        case 4:
            terminal_writestring("Overflow");
            break;
        case 5:
            terminal_writestring("Bound range exceeded");
            break;
        case 6:
            terminal_writestring("Invalid opcode");
            break;
        case 7:
            terminal_writestring("Device not available");
            break;
        case 8:
            terminal_writestring("Double fault");
            break;
        case 9:
            terminal_writestring("Coprocessor segment overrun");
            break;
        case 10:
            terminal_writestring("Invalid TSS");
            break;
        case 11:
            terminal_writestring("Segment not present");
            break;
        case 12:
            terminal_writestring("Stack-segment fault");
            break;
        case 13:
            terminal_writestring("General protection fault");
            break;
        case 14:
            terminal_writestring("Page fault");
            break;
        case 15:
            terminal_writestring("Reserved");
            break;
        case 16:
            terminal_writestring(
                "x87 floating-point exception"
            );
            break;
        case 17:
            terminal_writestring(
                "Alignment check"
            );
            break;
        case 18:
            terminal_writestring(
                "Machine check"
            );
            break;
        case 19:
            terminal_writestring(
                "SIMD floating-point exception"
            );
            break;
        case 20:
            terminal_writestring(
                "Virtualization exception"
            );
            break;
        case 21:
            terminal_writestring(
                "Control protection exception"
            );
            break;
        default:
            terminal_writestring(
                "Unknown exception"
            );
            break;
    }

    terminal_writestring(
        "\nSystem halted.\n"
    );

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void irq_handler(struct registers* regs)
{
    int eoi_sent = 0;
    if (regs->int_no == 0) {
        timer_ticks++;
        audio_tick();
    }
    else if (regs->int_no == 1) {
        keyboard_irq_count++;
        uint8_t scancode;

        __asm__ volatile (
            "inb %1, %0"
            : "=a"(scancode)
            : "Nd"((uint16_t)0x60)
        );

        /* A shell command can synchronously enter ring 3 from this
         * keyboard interrupt. Acknowledge it before dispatch so the
         * user process can receive its next keystroke. */
        pic_send_eoi(1);
        eoi_sent = 1;
        keyboard_handle_scancode(scancode);
    }
    else if (regs->int_no == 12) {
        mouse_irq_count++;

        /*
         * PS/2 status bit 0 = output buffer full.
         * PS/2 status bit 5 = byte came from auxiliary device.
         */
        uint8_t status;

        __asm__ volatile (
            "inb %1, %0"
            : "=a"(status)
            : "Nd"((uint16_t)0x64)
        );

        if ((status & 0x01) && (status & 0x20)) {
            uint8_t data;

            __asm__ volatile (
                "inb %1, %0"
                : "=a"(data)
                : "Nd"((uint16_t)0x60)
            );

            mouse_handle_byte(data);
        }
    }

    if (!eoi_sent)
        pic_send_eoi((uint8_t)regs->int_no);
}

void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info_addr)
{
    /*
     * Mode 13h first, always -- it's guaranteed to work everywhere
     * and gives us a screen to print the boot log to. Then try to
     * upgrade to a bigger linear-framebuffer mode via the Bochs/
     * QEMU VBE registers (see vga_try_enable_vbe()); if that PCI
     * probe doesn't find a matching device, we're simply still in
     * mode 13h and nothing about boot changes. Either way we never
     * leave graphics mode again -- the shell and the GUI both
     * render into this same framebuffer.
     */
    serial_init();
    kheap_init();

    vga_enter_mode13h();
    vga_init_default_palette();

    terminal_initialize();

    terminal_writestring(
        "StarlightOS kernel starting...\n"
    );

    if (vga_try_enable_vbe(1920, 1080)) {
        terminal_writestring("Display: 1920x1080, 32-bit TrueColor (VBE linear framebuffer).\n");
    } else {
        terminal_writestring("Display: 320x200, 8-bit (mode 13h, no VBE device found; colours are approximated).\n");
    }

    if (multiboot_magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        /* multiboot_info_addr is always a real 32-bit physical
         * address (GRUB hands it over in a 32-bit register even
         * when this is the x86_64 build -- multiboot1 itself never
         * changes), so it fits in a pointer on both targets. Routing
         * through uintptr_t instead of casting the uint32_t straight
         * to a pointer keeps that explicit rather than relying on an
         * implicit widen-then-truncate-looking conversion, which is
         * what triggers -Wint-to-pointer-cast on the 64-bit build. */
        struct multiboot_info* mbi =
            (struct multiboot_info*)(uintptr_t)multiboot_info_addr;

        if (mbi->flags & MULTIBOOT_FLAG_MEM) {
            multiboot_mem_lower = mbi->mem_lower;
            multiboot_mem_upper = mbi->mem_upper;
            multiboot_mem_valid = 1;
        }
    }

#if defined(__x86_64__)
    /* No gdt_initialize() call on x86_64: boot64.s already loaded a
     * flat GDT64 (null + code64 + data64) before jumping into long
     * mode, and gdt.c/gdt.s themselves are 32-bit-only (a 64-bit
     * code/data descriptor is a different bit layout, and this
     * kernel doesn't need a second GDT switch once one is already
     * loaded and working). See x86_64_port/boot64.s. */
    terminal_writestring(
        "GDT: using boot64.s's long-mode GDT.\n"
    );
#else
    gdt_initialize();

    terminal_writestring(
        "GDT: initialized.\n"
    );
#endif

    idt_initialize();

#if defined(__x86_64__)
    process_initialize();
#endif

    terminal_writestring(
        "IDT: initialized.\n"
    );

    pic_remap();

    terminal_writestring(
        "PIC: remapped.\n"
    );

    pit_initialize(100);

    terminal_writestring(
        "PIT: initialized at 100 Hz.\n"
    );

    keyboard_initialize();

    terminal_writestring(
        "Keyboard: initialized.\n"
    );

    mouse_initialize();

    terminal_writestring(
        "Mouse: initialized.\n"
    );

    /*
     * Initialize ATA disk.
     */
    if (disk_initialize() == 0) {
        terminal_writestring(
            "ATA: disk initialized.\n"
        );
    } else {
        terminal_writestring(
            "ATA: disk initialization failed.\n"
        );
    }

    /*
     * Initialize the VFS (mounts the ext2 volume).
     */
    if (vfs_initialize() == 0) {
        terminal_writestring(
            "ext2: filesystem mounted.\n"
        );
    } else {
        terminal_writestring(
            "VFS: filesystem initialization failed.\n"
        );
    }

    /*
     * Load the real outline font off disk, if it's there, instead
     * of staying on the builtin 8x8 bitmap font. FONT.TTF is
     * a compact JetBrains Mono Nerd Font ASCII subset -- see
     * make_disk.py and font_ttf.h.
     *
     * This is silent-fail by design, same as font_load()/font_load_ttf()
     * always have been: an old disk image built before this existed,
     * a disk image built without the bundled JetBrains Mono font
     * present, or `font default` typed later at the shell should all
     * degrade to the builtin font rather than the boot log spelling
     * out a scary-looking error for what's an entirely optional nicety.
     */
    if (font_load_ttf("FONT.TTF") == 0) {
        terminal_writestring(
            "Font: loaded FONT.TTF.\n"
        );
    }

    pic_clear_mask(0);
    pic_clear_mask(1);
    pic_clear_mask(2);  /* cascade, needed for IRQ8-15 */
    pic_clear_mask(12); /* PS/2 mouse */

    terminal_writestring(
        "Timer IRQ: enabled.\n"
    );

    terminal_writestring(
        "Keyboard/mouse IRQs: enabled.\n"
    );

    __asm__ volatile ("sti");

    terminal_writestring(
        "Interrupts: enabled.\n"
    );

    ktime_init();
    terminal_writestring("Clock: TSC calibrated.\n");

    kfd_init();
    if (net_initialize() == 0) {
        char mac[18];
        net_fmt_mac(mac, net_get_config()->mac);
        kprintf("Network: RTL8139 %s, link %s, DHCP started.\n", mac, net_link_up() ? "up" : "down");
    } else
        terminal_writestring("Network: RTL8139 not found.\n");

    if (ac97_initialize() == 0) {
        kprintf("Audio: AC'97 %s, 48 kHz stereo.\n", ac97_codec_name());
    } else
        terminal_writestring("Audio: no AC'97 codec; using PC speaker.\n");
    audio_init();
    audio_sfx(SFX_STARTUP);

    terminal_writestring(
        "StarlightOS is running.\n\n"
    );

    shell_initialize();

    /*
     * Straight into the desktop -- the terminal (shell) is one of
     * its apps now rather than a separate full-screen mode to
     * drop back to, so there's no more "wait for the user to type
     * `gui`" step. Everything printed above is already sitting in
     * the terminal's scrollback for whenever the Terminal app gets
     * opened.
     *
     * gui_run() isn't expected to return during normal operation
     * (there's no outer shell loop left to fall back to), but if
     * it ever does, the idle loop below just keeps the machine
     * alive instead of running off the end of kernel_main.
     */
    gui_run();

    for (;;) {
        if (gui_requested) {
            gui_requested = 0;
            gui_run();
            terminal_redraw();
        }

        __asm__ volatile ("hlt");
    }
}
