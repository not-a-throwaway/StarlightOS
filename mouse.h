#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

/* Call once at boot, after the IDT/PIC are set up. */
void mouse_initialize(void);

/* Call from irq_handler when int_no == 12. */
void mouse_handle_byte(uint8_t byte);

void mouse_get_state(int* x, int* y, int* left_button, int* right_button);

/* Clamps the tracked position to a screen of this size (used
 * when entering/leaving the graphical mode). */
void mouse_set_bounds(int width, int height);

/* Non-zero if the position or buttons changed since the last
 * call (and clears the flag). */
int mouse_take_dirty(void);

unsigned int mouse_get_left_click_count(void);
unsigned int mouse_get_right_click_count(void);

#endif

unsigned int mouse_get_packet_count(void);
