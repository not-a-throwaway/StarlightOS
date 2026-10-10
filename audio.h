#ifndef AUDIO_H
#define AUDIO_H
#include <stdint.h>

/*
 * StarlightOS audio: software mixer + sequencer on top of the AC'97 driver
 * (PC speaker fallback when there is no codec).
 *
 *   voices     8 simultaneous synth/PCM voices with ADSR
 *   sequencer  up to 3 music tracks + 1 sound-effect track, MML text
 *   wav        8/16-bit PCM, mono/stereo, any rate (resampled)
 */
enum audio_wave { WAVE_SINE = 0, WAVE_SQUARE, WAVE_SAW, WAVE_TRI, WAVE_NOISE, WAVE_COUNT };

enum audio_sfx { SFX_CLICK = 0, SFX_OPEN, SFX_CLOSE, SFX_NOTIFY, SFX_ERROR, SFX_STARTUP, SFX_SUCCESS, SFX_COUNT };

struct audio_status {
    int  backend;               /* 0 none, 1 PC speaker, 2 AC'97 */
    int  volume, muted;
    int  voices_active;
    int  music_playing, music_loop;
    int  wav_playing;
    int  level_left, level_right;   /* 0..100 peak meters */
    uint32_t underruns, samples_rendered;
    int  ui_sounds;
};

int  audio_init(void);
int  audio_available(void);
void audio_tick(void);                                  /* from the 100 Hz timer IRQ */

void audio_set_volume(int percent);
int  audio_get_volume(void);
void audio_set_muted(int muted);
void audio_set_ui_sounds(int on);
int  audio_ui_sounds(void);

/* One-shot tone. Returns 0 on success. Non-blocking. */
int  audio_tone(int freq_hz, int ms, int wave, int volume_pct);
void audio_sfx(enum audio_sfx which);

/* MML text, tracks separated by ','. Returns number of tracks started or -1 on syntax error;
 * *err_pos (optional) receives the offending offset. */
int  audio_play_mml(const char *mml, int wave, int loop, int *err_pos);
int  audio_mml_check(const char *mml, int *err_pos);
const char *audio_song_lookup(const char *name);        /* built-in songs */
const char *audio_song_name(int index);
int  audio_song_count(void);

/* WAV playback from the disk. */
struct wav_info { uint32_t rate; uint16_t channels, bits; uint32_t frames; uint32_t ms; };
int  audio_play_wav(const char *filename, int loop, struct wav_info *info);   /* 0 ok, <0 error */
const char *audio_wav_error(int code);

/* Offline: render MML to a mono 16-bit 24 kHz WAV file on disk. */
int  audio_render_wav(const char *mml, int wave, const char *filename, uint32_t *ms_out);

void audio_stop(void);                                  /* stop everything */
void audio_pause(int pause);
int  audio_busy(void);                                  /* anything still sounding? */
void audio_get_status(struct audio_status *s);

#endif
