#include "mouse.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );
    return value;
}

static void wait_input_clear(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & 0x02))
            return;
    }
}

static int wait_output_full(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & 0x01)
            return 1;
    }
    return 0;
}

static void flush_output(void)
{
    for (int i = 0; i < 64; i++) {
        if (!(inb(PS2_STATUS) & 0x01))
            break;
        (void)inb(PS2_DATA);
    }
}

static void mouse_write(uint8_t value)
{
    wait_input_clear();
    outb(PS2_COMMAND, 0xD4);

    wait_input_clear();
    outb(PS2_DATA, value);
}

static int mouse_read(uint8_t* value)
{
    if (!wait_output_full())
        return 0;

    *value = inb(PS2_DATA);
    return 1;
}

static int bounds_width = 320;
static int bounds_height = 200;

static volatile int mouse_x = 160;
static volatile int mouse_y = 100;

static volatile int button_left = 0;
static volatile int button_right = 0;

static volatile int dirty = 0;
static volatile unsigned int packet_count = 0;
static volatile unsigned int left_click_count = 0;
static volatile unsigned int right_click_count = 0;

static uint8_t packet[3];
static int packet_index = 0;

void mouse_initialize(void)
{
    mouse_x = bounds_width / 2;
    mouse_y = bounds_height / 2;

    button_left = 0;
    button_right = 0;

    packet_index = 0;
    packet_count = 0;
    dirty = 0;

    /*
     * Enable the auxiliary PS/2 device.
     */
    wait_input_clear();
    outb(PS2_COMMAND, 0xA8);

    /*
     * Read controller configuration byte.
     */
    wait_input_clear();
    outb(PS2_COMMAND, 0x20);

    uint8_t config = 0;

    if (!mouse_read(&config))
        return;

    /*
     * Enable IRQ12 (bit 1).
     * Enable mouse clock (clear bit 5).
     */
    config |= 0x02;
    config &= (uint8_t)~0x20;

    /*
     * Write controller configuration byte.
     */
    wait_input_clear();
    outb(PS2_COMMAND, 0x60);

    wait_input_clear();
    outb(PS2_DATA, config);

    /*
     * Reset mouse to defaults.
     */
    mouse_write(0xF6);

    uint8_t ack;
    mouse_read(&ack);

    /*
     * Enable mouse data reporting.
     */
    mouse_write(0xF4);

    mouse_read(&ack);

    /*
     * Clear anything left in the controller output buffer.
     * Do this BEFORE accepting normal packets.
     */
    flush_output();

    packet_index = 0;
}

void mouse_set_bounds(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;

    bounds_width = width;
    bounds_height = height;
    /* The display mode can jump from 320x200 to HD at desktop start.
     * Recenter the pointer in the new coordinate space instead of
     * stranding it at the old mode's 160x100 origin. */
    mouse_x = bounds_width / 2;
    mouse_y = bounds_height / 2;

    dirty = 1;
}

void mouse_handle_byte(uint8_t byte)
{
    /*
     * First byte must have bit 3 set.
     */
    if (packet_index == 0 && !(byte & 0x08))
        return;

    packet[packet_index++] = byte;

    if (packet_index < 3)
        return;

    packet_index = 0;
    packet_count++;

    uint8_t flags = packet[0];

    int dx = (int8_t)packet[1];
    int dy = (int8_t)packet[2];

    /*
     * Ignore overflow packets.
     */
    if (flags & 0x40)
        dx = 0;

    if (flags & 0x80)
        dy = 0;

    mouse_x += dx;
    mouse_y -= dy;

    if (mouse_x < 0)
        mouse_x = 0;

    if (mouse_y < 0)
        mouse_y = 0;

    if (mouse_x >= bounds_width)
        mouse_x = bounds_width - 1;

    if (mouse_y >= bounds_height)
        mouse_y = bounds_height - 1;

    int new_left = (flags & 0x01) ? 1 : 0;
    int new_right = (flags & 0x02) ? 1 : 0;

    if (new_left && !button_left)
        left_click_count++;

    if (new_right && !button_right)
        right_click_count++;

    button_left = new_left;
    button_right = new_right;

    dirty = 1;
}

void mouse_get_state(
    int* x,
    int* y,
    int* left_button,
    int* right_button
)
{
    if (x)
        *x = mouse_x;

    if (y)
        *y = mouse_y;

    if (left_button)
        *left_button = button_left;

    if (right_button)
        *right_button = button_right;
}

unsigned int mouse_get_packet_count(void)
{
    return packet_count;
}

int mouse_take_dirty(void)
{
    int was_dirty = dirty;
    dirty = 0;
    return was_dirty;
}



unsigned int mouse_get_left_click_count(void)
{
    return left_click_count;
}

unsigned int mouse_get_right_click_count(void)
{
    return right_click_count;
}
