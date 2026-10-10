#include "keyboard.h"
#include "ksched.h"
#include "poll.h"

#define KEYBOARD_DATA_PORT 0x60

static int shift_left = 0;
static int shift_right = 0;
static int caps_lock = 0;
static int extended_prefix = 0;
static int ctrl_down = 0;
static int alt_down = 0;

extern void shell_handle_character(char character);

static void (*dispatch_target)(char character) = 0;

#define KEY_QUEUE_SIZE 256
static volatile char key_queue[KEY_QUEUE_SIZE];
static volatile uint8_t key_head, key_tail;

static void queue_character(char c)
{
    uint8_t next = (uint8_t)((key_head + 1) % KEY_QUEUE_SIZE);
    if (next != key_tail) {
        key_queue[key_head] = c;
        key_head = next;
    }
    /* Wake anything sleeping in kpoll() on the keyboard (IRQ-safe: sets flags only). */
    kwq_wake_all(kfd_tty_waitq());
}

int keyboard_has_char(void)
{
    return key_tail != key_head;
}

int keyboard_getchar_nonblocking(void)
{
    int c = -1;
    __asm__ volatile("pushf; cli" ::: "memory");
    if (key_tail != key_head) {
        c = (unsigned char)key_queue[key_tail];
        key_tail = (uint8_t)((key_tail + 1) % KEY_QUEUE_SIZE);
    }
    __asm__ volatile("popf" ::: "memory", "cc");
    return c;
}

void keyboard_flush_queue(void)
{
    __asm__ volatile("pushf; cli" ::: "memory");
    key_head = key_tail = 0;
    __asm__ volatile("popf" ::: "memory", "cc");
}

/*
 * Set 1 scancode -> normal character.
 *
 * This is deliberately explicit so the scancode positions
 * are easy to verify.
 */
static char scancode_to_char(uint8_t scancode)
{
    switch (scancode) {
        case 0x01: return (char)KEY_ESC;

        case 0x02: return '1';
        case 0x03: return '2';
        case 0x04: return '3';
        case 0x05: return '4';
        case 0x06: return '5';
        case 0x07: return '6';
        case 0x08: return '7';
        case 0x09: return '8';
        case 0x0A: return '9';
        case 0x0B: return '0';

        case 0x0C: return '-';
        case 0x0D: return '=';
        case 0x0E: return '\b';
        case 0x0F: return '\t';

        case 0x10: return 'q';
        case 0x11: return 'w';
        case 0x12: return 'e';
        case 0x13: return 'r';
        case 0x14: return 't';
        case 0x15: return 'y';
        case 0x16: return 'u';
        case 0x17: return 'i';
        case 0x18: return 'o';
        case 0x19: return 'p';

        case 0x1A: return '[';
        case 0x1B: return ']';
        case 0x1C: return '\n';

        case 0x1E: return 'a';
        case 0x1F: return 's';
        case 0x20: return 'd';
        case 0x21: return 'f';
        case 0x22: return 'g';
        case 0x23: return 'h';
        case 0x24: return 'j';
        case 0x25: return 'k';
        case 0x26: return 'l';

        case 0x27: return ';';
        case 0x28: return '\'';
        case 0x29: return '`';

        case 0x2B: return '\\';

        case 0x2C: return 'z';
        case 0x2D: return 'x';
        case 0x2E: return 'c';
        case 0x2F: return 'v';
        case 0x30: return 'b';
        case 0x31: return 'n';
        case 0x32: return 'm';

        case 0x33: return ',';
        case 0x34: return '.';
        case 0x35: return '/';

        case 0x39: return ' ';

        default:
            return 0;
    }
}

static char scancode_to_shifted_char(uint8_t scancode)
{
    switch (scancode) {
        case 0x02: return '!';
        case 0x03: return '@';
        case 0x04: return '#';
        case 0x05: return '$';
        case 0x06: return '%';
        case 0x07: return '^';
        case 0x08: return '&';
        case 0x09: return '*';
        case 0x0A: return '(';
        case 0x0B: return ')';

        case 0x0C: return '_';
        case 0x0D: return '+';
        case 0x0E: return '\b';
        case 0x0F: return '\t';

        case 0x10: return 'Q';
        case 0x11: return 'W';
        case 0x12: return 'E';
        case 0x13: return 'R';
        case 0x14: return 'T';
        case 0x15: return 'Y';
        case 0x16: return 'U';
        case 0x17: return 'I';
        case 0x18: return 'O';
        case 0x19: return 'P';

        case 0x1A: return '{';
        case 0x1B: return '}';
        case 0x1C: return '\n';

        case 0x1E: return 'A';
        case 0x1F: return 'S';
        case 0x20: return 'D';
        case 0x21: return 'F';
        case 0x22: return 'G';
        case 0x23: return 'H';
        case 0x24: return 'J';
        case 0x25: return 'K';
        case 0x26: return 'L';

        case 0x27: return ':';
        case 0x28: return '"';
        case 0x29: return '~';

        case 0x2B: return '|';

        case 0x2C: return 'Z';
        case 0x2D: return 'X';
        case 0x2E: return 'C';
        case 0x2F: return 'V';
        case 0x30: return 'B';
        case 0x31: return 'N';
        case 0x32: return 'M';

        case 0x33: return '<';
        case 0x34: return '>';
        case 0x35: return '?';

        case 0x39: return ' ';

        default:
            return 0;
    }
}

static inline int ktolower_c(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int shift_is_down(void)
{
    return shift_left || shift_right;
}

/* IRQ context: only queue. keyboard_pump() delivers in normal context. */
static void dispatch(char character)
{
    if (character == (char)KEY_CTRL_C) kabort_raise();
    queue_character(character);
}

int keyboard_modifiers(void)
{
    return (shift_left || shift_right ? 1 : 0) | (ctrl_down ? 2 : 0) | (alt_down ? 4 : 0);
}

int keyboard_pump(void)
{
    int n = 0, c;
    while ((c = keyboard_getchar_nonblocking()) >= 0) {
        char ch = (char)c;
        if (dispatch_target) dispatch_target(ch);
        else shell_handle_character(ch);
        n++;
    }
    return n;
}

void keyboard_initialize(void)
{
    shift_left = 0;
    shift_right = 0;
    caps_lock = 0;
    extended_prefix = 0;
    dispatch_target = 0;
    key_head = key_tail = 0;
}

void keyboard_set_dispatch(void (*handler)(char character))
{
    dispatch_target = handler;
}

void (*keyboard_get_dispatch(void))(char character)
{
    return dispatch_target;
}

void keyboard_handle_scancode(uint8_t scancode)
{
    /*
     * 0xE0 prefixes a two-byte "extended" scancode (arrow keys,
     * Insert/Delete/Home/End, the right-hand Ctrl/Alt, etc).
     * Remember it and process the next byte specially.
     */
    if (scancode == 0xE0) {
        extended_prefix = 1;
        return;
    }

    if (extended_prefix) {
        extended_prefix = 0;

        /*
         * Ignore key releases (top bit set).
         */
        if (scancode & 0x80) {
            if (scancode == 0x9D) ctrl_down = 0;
            if (scancode == 0xB8) alt_down = 0;
            return;
        }

        switch (scancode) {
            case 0x48: dispatch((char)KEY_UP);    return;
            case 0x50: dispatch((char)KEY_DOWN);  return;
            case 0x4B: dispatch((char)KEY_LEFT);  return;
            case 0x4D: dispatch((char)KEY_RIGHT); return;
            case 0x47: dispatch((char)KEY_HOME);  return;
            case 0x4F: dispatch((char)KEY_END);   return;
            case 0x53: dispatch((char)KEY_DEL);   return;
            case 0x49: dispatch((char)KEY_PGUP);  return;
            case 0x51: dispatch((char)KEY_PGDN);  return;
            case 0x52: dispatch((char)KEY_INS);   return;
            case 0x1D: ctrl_down = 1; return;     /* right ctrl press */
            case 0x38: alt_down = 1; return;      /* right alt press  */
            default: return;
        }
    }

    if (scancode == 0x1D) { ctrl_down = 1; return; }
    if (scancode == 0x9D) { ctrl_down = 0; return; }
    if (scancode == 0x38) { alt_down = 1; return; }
    if (scancode == 0xB8) { alt_down = 0; return; }

    if (scancode >= 0x3B && scancode <= 0x44) { dispatch((char)(KEY_F1 + (scancode - 0x3B))); return; }
    if (scancode == 0x57) { dispatch((char)(KEY_F1 + 10)); return; }
    if (scancode == 0x58) { dispatch((char)(KEY_F1 + 11)); return; }

    /*
     * Left Shift press/release.
     *
     * Press  = 0x2A
     * Release = 0xAA
     */
    if (scancode == 0x2A) {
        shift_left = 1;
        return;
    }

    if (scancode == 0xAA) {
        shift_left = 0;
        return;
    }

    /*
     * Right Shift press/release.
     *
     * Press  = 0x36
     * Release = 0xB6
     */
    if (scancode == 0x36) {
        shift_right = 1;
        return;
    }

    if (scancode == 0xB6) {
        shift_right = 0;
        return;
    }

    /*
     * Caps Lock.
     *
     * Press = 0x3A
     * Release = 0xBA
     */
    if (scancode == 0x3A) {
        caps_lock = !caps_lock;
        return;
    }

    /*
     * Ignore all other key releases.
     */
    if (scancode & 0x80) {
        return;
    }

    /*
     * Ignore extended scancodes for now.
     */
    if (scancode >= 0x80) {
        return;
    }

    char character = scancode_to_char(scancode);

    if (character == 0) {
        return;
    }

    /*
     * Letters:
     *
     * Caps     -> uppercase
     * Shift    -> uppercase
     * Both     -> lowercase
     */
    int is_letter =
        character >= 'a' &&
        character <= 'z';

    if (is_letter) {
        if (caps_lock ^ shift_is_down()) {
            character =
                (char)(character - 'a' + 'A');
        }
    }
    else if (shift_is_down()) {
        char shifted = scancode_to_shifted_char(scancode);

        if (shifted != 0) {
            character = shifted;
        }
    }

    if (ctrl_down) {
        char lower = (char)ktolower_c(character);
        if (lower >= 'a' && lower <= 'z') character = (char)(lower - 'a' + 1);
        else if (lower == '[') character = 27;
        else return;
    }

    dispatch(character);
}
