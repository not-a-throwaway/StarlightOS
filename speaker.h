#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdint.h>

void speaker_on(uint32_t frequency);
void speaker_off(void);

/*
 * Blocking beep. duration_ticks is measured in PIT ticks
 * (100 ticks/sec by default -- see pit_initialize).
 */
void speaker_beep(uint32_t frequency, uint32_t duration_ticks);

#endif
