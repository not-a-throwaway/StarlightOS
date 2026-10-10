#ifndef AC97_H
#define AC97_H
#include <stdint.h>

/*
 * Intel AC'97 (ICH) PCM-out driver: a ring of 32 DMA buffers that a
 * producer callback keeps topped up.
 *
 *   ac97_initialize()   probe + codec reset + unmute
 *   ac97_set_producer() who fills buffers (the software mixer in audio.c)
 *   ac97_pump()         refill free buffers; call from the timer tick
 *
 * Output format is fixed at 48 kHz, 16-bit, stereo.
 */
#define AC97_RATE 48000u

struct ac97_info {
    int      ready, running;
    uint16_t nam_base, nabm_base;
    uint32_t vendor_id;            /* codec vendor id (regs 0x7C/0x7E) */
    uint32_t buffers_played, underruns;
    int      master_att, pcm_att;  /* hardware attenuation steps (0 = loudest) */
    uint8_t  irq_line;
};

typedef void (*ac97_producer)(int16_t *stereo_frames, unsigned frame_count);

int  ac97_initialize(void);
int  ac97_ready(void);
void ac97_set_producer(ac97_producer p);
void ac97_start(void);
void ac97_stop(void);
int  ac97_running(void);
void ac97_pump(void);
void ac97_set_hw_volume(int percent);              /* 0..100 -> codec master+pcm attenuation */
void ac97_get_info(struct ac97_info *out);
const char *ac97_codec_name(void);
int  ac97_play_test_tone(void);                    /* blocking 1 s 440 Hz tone (diagnostic) */
#endif
