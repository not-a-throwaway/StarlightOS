/*
 * audio.c -- software mixer, MML sequencer, WAV player.
 *
 * Everything the mixer touches is shared with the timer IRQ (which pumps
 * the codec), so main-context mutators wrap their changes in
 * irq_save()/irq_restore().
 */
#include "audio.h"
#include "audio_tables.h"
#include "ac97.h"
#include "speaker.h"
#include "vfs.h"
#include "kstring.h"
#include "kprintf.h"
#include "kheap.h"
#include "ksched.h"

#define RATE        AC97_RATE
#define NVOICES     8
#define NTRACKS     4            /* 0..2 music, 3 = sfx */
#define SFX_TRACK   3
#define BLOCK       64           /* sequencer granularity in frames (1.3 ms) */

/* ------------------------------------------------------------- IRQ lock */
static inline unsigned long irq_save(void)
{
    unsigned long f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(unsigned long f) { __asm__ volatile("push %0; popf" :: "r"(f) : "memory", "cc"); }

/* --------------------------------------------------------------- state */
struct voice {
    uint8_t  active, is_pcm, wave, track;      /* track: owning sequencer track or 0xFF */
    uint32_t phase, inc;
    uint16_t vol;                              /* 0..256 */
    uint32_t pos, total, attack, release;      /* envelope, in frames */
    /* PCM */
    const int16_t *pcm; uint32_t pcm_frames; uint32_t pcm_pos, pcm_step; int pcm_loop; int pcm_owned;
};

struct track {
    const char *p;            /* current MML position, NULL if idle */
    const char *start;
    uint32_t next_at;         /* frames until the next event */
    int octave, len, vol, wave, tempo;
    int tie;
};

static struct voice voices[NVOICES];
static struct track tracks[NTRACKS];
static int music_loop;
static char music_text[1024];
static char sfx_text[128];
static int master_volume = 80, muted, ui_sounds = 1, paused;
static int backend;                 /* 0 none, 1 speaker, 2 ac97 */
static uint32_t lfsr = 0xACE1u;
static int peak_l, peak_r;
static uint32_t idle_ticks;
static uint32_t rendered;
static volatile int offline;
static volatile int in_render;      /* set while render() runs: start_note must not (re)start the DMA */
static uint32_t speaker_off_in;     /* ticks until the PC speaker is silenced */
static int speaker_active;
static uint32_t tick_counter;

/* ----------------------------------------------------------- note maths */
static const uint16_t base_hz_x100[12] = { 1635, 1732, 1835, 1945, 2060, 2183, 2312, 2450, 2596, 2750, 2914, 3087 };

static uint32_t note_freq_x100(int semitone, int octave)
{
    /* semitone 0..11 (C..B); base table is octave 0 */
    int oct = octave;
    while (semitone < 0) { semitone += 12; oct--; }
    while (semitone > 11) { semitone -= 12; oct++; }
    if (oct < 0) oct = 0;
    if (oct > 8) oct = 8;
    return (uint32_t)base_hz_x100[semitone] << oct;
}

static uint32_t freq_to_inc(uint32_t freq_x100)
{
    return (uint32_t)(((uint64_t)freq_x100 * 4294967296ULL) / ((uint64_t)RATE * 100ULL));
}

/* -------------------------------------------------------------- voices */
static struct voice *voice_alloc(void)
{
    for (int i = 0; i < NVOICES; i++) if (!voices[i].active) return &voices[i];
    /* steal the voice that is furthest through its envelope */
    int best = 0; uint32_t best_left = 0xFFFFFFFFu;
    for (int i = 0; i < NVOICES; i++) {
        if (voices[i].is_pcm) continue;
        uint32_t left = voices[i].total - voices[i].pos;
        if (left < best_left) { best_left = left; best = i; }
    }
    return &voices[best];
}

static void audio_wake(void)
{
    idle_ticks = 0;
    if (in_render) return;                       /* the stream is being (re)primed right now */
    if (backend == 2 && !ac97_running()) ac97_start();
}

static void start_note(int track, uint32_t freq_x100, uint32_t dur_frames, int vol, int wave)
{
    if (!freq_x100 || !dur_frames) return;
    if (backend == 1) {
        if (track == 0 || track == SFX_TRACK) {
            speaker_on(freq_x100 / 100u ? freq_x100 / 100u : 1u);
            speaker_active = 1;
            speaker_off_in = (dur_frames * 9u / 10u) / (RATE / 100u) + 1u;
        }
        return;
    }
    unsigned long f = irq_save();
    struct voice *v = voice_alloc();
    memset(v, 0, sizeof(*v));
    v->active = 1; v->wave = (uint8_t)wave; v->track = (uint8_t)track;
    v->inc = freq_to_inc(freq_x100);
    v->vol = (uint16_t)(vol * 256 / 100);
    v->total = dur_frames;
    v->attack = RATE / 400u;                        /* 2.5 ms */
    v->release = dur_frames > RATE / 12u ? RATE / 40u : dur_frames / 3u + 1u;
    if (v->release >= v->total) v->release = v->total / 2u + 1u;
    irq_restore(f);
    audio_wake();
}

/* ----------------------------------------------------------------- MML */
struct mml_note { int rest; int semitone; int octave; int len; int dots; };

static const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '|') p++; return p; }

static int read_num(const char **pp, int *out)
{
    const char *p = *pp;
    if (*p < '0' || *p > '9') return 0;
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); if (v > 100000) v = 100000; p++; }
    *out = v; *pp = p;
    return 1;
}

static uint32_t note_frames(int len, int dots, int tempo)
{
    if (len < 1) len = 4;
    uint32_t whole = (uint32_t)((uint64_t)RATE * 60u * 4u / (uint32_t)tempo);     /* whole-note frames */
    uint32_t f = whole / (uint32_t)len, add = f;
    for (int i = 0; i < dots; i++) { add /= 2; f += add; }
    return f;
}

/* Advances one track until it emits a note/rest or ends. Returns 0 at end, -1 on syntax error. */
static int track_step(int ti, struct track *t, int dry_run)
{
    const char *p = t->p;
    for (;;) {
        p = skip_ws(p);
        char c = *p;
        if (c == 0 || c == ',') { t->p = NULL; return 0; }
        const char *cmd_pos = p;                      /* where an error should point */
        p++;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        int n;
        switch (c) {
        case 'o': if (!read_num(&p, &n) || n > 8) { t->p = cmd_pos; return -1; } t->octave = n; break;
        case '<': if (t->octave > 0) t->octave--; break;
        case '>': if (t->octave < 8) t->octave++; break;
        case 'l': if (!read_num(&p, &n) || n < 1 || n > 64) { t->p = cmd_pos; return -1; } t->len = n; break;
        case 't': if (!read_num(&p, &n) || n < 32 || n > 400) { t->p = cmd_pos; return -1; } t->tempo = n; break;
        case 'v': if (!read_num(&p, &n) || n > 15) { t->p = cmd_pos; return -1; } t->vol = n * 100 / 15; break;
        case 'w': if (!read_num(&p, &n) || n >= WAVE_COUNT) { t->p = cmd_pos; return -1; } t->wave = n; break;
        case 'r': case 'p': case 'a': case 'b': case 'c': case 'd': case 'e': case 'f': case 'g': {
            static const int8_t semis[7] = { 9, 11, 0, 2, 4, 5, 7 };           /* a b c d e f g */
            int semi = 0, rest = (c == 'r' || c == 'p');
            if (!rest) {
                semi = semis[c - 'a'];
                while (*p == '+' || *p == '#' || *p == '-') { semi += (*p == '-') ? -1 : 1; p++; }
            }
            int len = t->len, dots = 0;
            if (read_num(&p, &n)) { if (n < 1 || n > 64) { t->p = cmd_pos; return -1; } len = n; }
            while (*p == '.') { dots++; p++; }
            uint32_t frames = note_frames(len, dots, t->tempo ? t->tempo : 120);
            /* a following '&' ties this note to the next one of the same pitch */
            int tie = (*p == '&'); if (tie) p++;
            if (!dry_run) {
                if (!rest) {
                    int oct = t->octave;
                    if (c == 'b' || c == 'a') { /* A/B are in the same octave number as C..G of that octave */ }
                    uint32_t fx = note_freq_x100(semi, oct);
                    uint32_t sounding = tie ? frames : frames * 92u / 100u;    /* small gap between notes */
                    start_note(ti, fx, sounding, t->vol, t->wave);
                }
                t->next_at = frames;
            }
            t->p = p;
            return 1;
        }
        default: { t->p = cmd_pos; return -1; }
        }
    }
}

static void tracks_init_from(const char *text, int wave, int dry)
{
    (void)dry;
    int ti = 0;
    const char *p = text;
    for (; ti < NTRACKS - 1; ti++) {
        struct track *t = &tracks[ti];
        memset(t, 0, sizeof(*t));
        t->p = p; t->start = p; t->octave = 4; t->len = 4; t->vol = 70; t->wave = wave; t->tempo = 120;
        while (*p && *p != ',') p++;
        if (*p == ',') p++; else { ti++; break; }
    }
    for (; ti < NTRACKS - 1; ti++) memset(&tracks[ti], 0, sizeof(tracks[ti]));
}

int audio_mml_check(const char *mml, int *err_pos)
{
    struct track t; int count = 0;
    const char *p = mml;
    for (;;) {
        memset(&t, 0, sizeof(t));
        t.p = p; t.octave = 4; t.len = 4; t.vol = 70; t.tempo = 120;
        for (;;) {
            int r = track_step(0, &t, 1);
            if (r < 0) { if (err_pos) *err_pos = (int)(t.p ? t.p - mml : 0); return -1; }
            if (r == 0) break;
        }
        count++;
        while (*p && *p != ',') p++;
        if (*p == ',') p++; else break;
    }
    return count;
}

int audio_play_mml(const char *mml, int wave, int loop, int *err_pos)
{
    if (!backend) return -2;
    int n = audio_mml_check(mml, err_pos);
    if (n < 0) return -1;
    if (n > NTRACKS - 1) n = NTRACKS - 1;
    unsigned long f = irq_save();
    strlcpy(music_text, mml, sizeof(music_text));
    music_loop = loop;
    tracks_init_from(music_text, wave, 0);
    irq_restore(f);
    audio_wake();
    return n;
}

static void sequencer_advance(uint32_t frames)
{
    for (int i = 0; i < NTRACKS; i++) {
        struct track *t = &tracks[i];
        if (!t->p) continue;
        if (t->next_at > frames) { t->next_at -= frames; continue; }
        uint32_t carry = frames - t->next_at;
        t->next_at = 0;
        int guard = 0;
        while (t->p && t->next_at <= carry && guard++ < 64) {
            int r = track_step(i, t, 0);
            if (r <= 0) { t->p = NULL; break; }
            if (t->next_at <= carry) { carry -= t->next_at; t->next_at = 0; }
            else { t->next_at -= carry; carry = 0; break; }
        }
    }
    /* loop the music when every music track has finished */
    if (music_loop) {
        int any = 0;
        for (int i = 0; i < NTRACKS - 1; i++) if (tracks[i].p) any = 1;
        if (!any && music_text[0]) {
            int wave = tracks[0].wave;
            tracks_init_from(music_text, wave, 0);
        }
    }
}

/* --------------------------------------------------------------- mixer */
static inline int32_t osc(struct voice *v)
{
    uint32_t ph = v->phase;
    switch (v->wave) {
    case WAVE_SINE:   return sine_table[ph >> 22];
    case WAVE_SQUARE: return (ph & 0x80000000u) ? -14000 : 14000;
    case WAVE_SAW:    return (int32_t)(ph >> 17) - 16384;                   /* -16384..16383 */
    case WAVE_TRI: {
        int32_t x = (int32_t)(ph >> 16) - 32768;                             /* -32768..32767 */
        if (x < 0) x = -x;                                                   /* 0..32768 */
        return (x - 16384) * 2;
    }
    default: {
        lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
        return (int32_t)(lfsr & 0x7FFF) - 16384;
    }
    }
}

static void render(int16_t *out, unsigned frames)
{
    in_render = 1;
    if (paused) { memset(out, 0, (size_t)frames * 4u); in_render = 0; return; }
    unsigned done = 0;
    int pl = 0, pr = 0;
    while (done < frames) {
        unsigned n = frames - done < BLOCK ? frames - done : BLOCK;
        sequencer_advance(n);
        for (unsigned i = 0; i < n; i++) {
            int32_t l = 0, r = 0;
            for (int vi = 0; vi < NVOICES; vi++) {
                struct voice *v = &voices[vi];
                if (!v->active) continue;
                if (v->is_pcm) {
                    uint32_t idx = v->pcm_pos >> 16;
                    if (idx >= v->pcm_frames) {
                        if (v->pcm_loop) { v->pcm_pos = 0; idx = 0; }
                        else { v->active = 0; if (v->pcm_owned) { kfree((void *)v->pcm); v->pcm = NULL; } continue; }
                    }
                    uint32_t frac = (v->pcm_pos >> 8) & 0xFF;
                    uint32_t nx = idx + 1 < v->pcm_frames ? idx + 1 : idx;
                    int32_t a0 = v->pcm[idx * 2], a1 = v->pcm[nx * 2];
                    int32_t b0 = v->pcm[idx * 2 + 1], b1 = v->pcm[nx * 2 + 1];
                    int32_t sl = a0 + (((a1 - a0) * (int32_t)frac) >> 8);
                    int32_t sr = b0 + (((b1 - b0) * (int32_t)frac) >> 8);
                    l += (sl * v->vol) >> 8; r += (sr * v->vol) >> 8;
                    v->pcm_pos += v->pcm_step;
                    continue;
                }
                uint32_t pos = v->pos;
                if (pos >= v->total) { v->active = 0; continue; }
                uint32_t env = 65535;
                if (pos < v->attack) env = pos * 65535u / v->attack;
                else if (pos + v->release > v->total) env = (v->total - pos) * 65535u / v->release;
                int32_t s = osc(v);
                s = (int32_t)(((int64_t)s * env) >> 16);
                s = (s * v->vol) >> 8;
                l += s; r += s;
                v->phase += v->inc;
                v->pos = pos + 1;
            }
            /* master gain + soft limiter */
            int32_t g = muted ? 0 : master_volume * master_volume * 256 / 10000;
            l = (l * 140 >> 8) * g >> 8;
            r = (r * 140 >> 8) * g >> 8;
            if (l > 24000) l = 24000 + ((l - 24000) >> 2);
            if (l < -24000) l = -24000 + ((l + 24000) >> 2);
            if (r > 24000) r = 24000 + ((r - 24000) >> 2);
            if (r < -24000) r = -24000 + ((r + 24000) >> 2);
            if (l > 32767) l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;
            out[(done + i) * 2] = (int16_t)l;
            out[(done + i) * 2 + 1] = (int16_t)r;
            int al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
            if (al > pl) pl = al;
            if (ar > pr) pr = ar;
        }
        done += n;
    }
    in_render = 0;
    rendered += frames;
    peak_l = pl > peak_l ? pl : peak_l - peak_l / 8;
    peak_r = pr > peak_r ? pr : peak_r - peak_r / 8;
}

static int anything_active(void)
{
    for (int i = 0; i < NVOICES; i++) if (voices[i].active) return 1;
    for (int i = 0; i < NTRACKS; i++) if (tracks[i].p) return 1;
    return 0;
}

/* ----------------------------------------------------------- front end */
int audio_init(void)
{
    memset(voices, 0, sizeof(voices));
    memset(tracks, 0, sizeof(tracks));
    if (ac97_ready()) { backend = 2; ac97_set_producer(render); ac97_set_hw_volume(100); }
    else backend = 1;                                          /* PC speaker is always "there" on a PC */
    return backend;
}

int audio_available(void) { return backend != 0; }

void audio_tick(void)
{
    tick_counter++;
    if (offline) return;
    if (backend == 2) {
        if (ac97_running()) {
            ac97_pump();
            if (!anything_active()) {
                if (++idle_ticks > 150) ac97_stop();           /* ~1.5 s of silence: stop DMA */
            } else idle_ticks = 0;
        }
    } else if (backend == 1) {
        if (!paused) sequencer_advance(RATE / 100u);
        if (speaker_active && speaker_off_in && --speaker_off_in == 0) { speaker_off(); speaker_active = 0; }
        else if (speaker_active && !speaker_off_in) { speaker_off(); speaker_active = 0; }
    }
}

void audio_set_volume(int pct) { if (pct < 0) pct = 0; if (pct > 100) pct = 100; master_volume = pct; }
int  audio_get_volume(void) { return master_volume; }
void audio_set_muted(int m) { muted = m; }
void audio_set_ui_sounds(int on) { ui_sounds = on; }
int  audio_ui_sounds(void) { return ui_sounds; }

int audio_tone(int freq, int ms, int wave, int vol)
{
    if (!backend || freq < 20 || freq > 20000 || ms <= 0) return -1;
    if (ms > 60000) ms = 60000;
    if (wave < 0 || wave >= WAVE_COUNT) wave = WAVE_SINE;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    uint32_t frames = (uint32_t)((uint64_t)RATE * (uint32_t)ms / 1000u);
    if (backend == 1) { speaker_on((uint32_t)freq); speaker_active = 1; speaker_off_in = (uint32_t)ms / 10u + 1u; return 0; }
    start_note(SFX_TRACK + 1, (uint32_t)freq * 100u, frames, vol, wave);
    return 0;
}

static const char *const sfx_mml[SFX_COUNT] = {
    "t300 v9 o6 c64",
    "t200 v8 o5 e32 a32 >c16",
    "t200 v8 o6 c32 <a32 e16",
    "t160 v10 o6 e16 c16 g8",
    "t180 v12 w1 o3 c16 r32 c8",
    "t150 v11 o4 c8 e8 g8 >c4 <g8 >c2",
    "t200 v10 o5 g16 >c16 e8",
};

void audio_sfx(enum audio_sfx which)
{
    if (!backend || (unsigned)which >= SFX_COUNT) return;
    if (which != SFX_STARTUP && which != SFX_ERROR && !ui_sounds) return;
    if (muted) return;
    unsigned long f = irq_save();
    struct track *t = &tracks[SFX_TRACK];
    memset(t, 0, sizeof(*t));
    strlcpy(sfx_text, sfx_mml[which], sizeof(sfx_text));
    t->p = sfx_text; t->start = sfx_text; t->octave = 4; t->len = 4; t->vol = 70; t->tempo = 120;
    t->wave = which == SFX_ERROR ? WAVE_SQUARE : (which == SFX_STARTUP ? WAVE_TRI : WAVE_SINE);
    irq_restore(f);
    audio_wake();
}

void audio_stop(void)
{
    unsigned long f = irq_save();
    for (int i = 0; i < NVOICES; i++) {
        if (voices[i].pcm_owned && voices[i].pcm) { kfree((void *)voices[i].pcm); }
        voices[i].active = 0; voices[i].pcm = NULL; voices[i].pcm_owned = 0;
    }
    for (int i = 0; i < NTRACKS - 1; i++) tracks[i].p = NULL;
    music_loop = 0; music_text[0] = 0; paused = 0;
    irq_restore(f);
    if (backend == 1) { speaker_off(); speaker_active = 0; }
}

void audio_pause(int p) { paused = p; }

int audio_busy(void)
{
    unsigned long f = irq_save();
    int b = anything_active() || speaker_active;
    irq_restore(f);
    return b;
}

void audio_get_status(struct audio_status *s)
{
    memset(s, 0, sizeof(*s));
    s->backend = backend; s->volume = master_volume; s->muted = muted;
    s->ui_sounds = ui_sounds;
    for (int i = 0; i < NVOICES; i++) if (voices[i].active) { s->voices_active++; if (voices[i].is_pcm) s->wav_playing = 1; }
    for (int i = 0; i < NTRACKS - 1; i++) if (tracks[i].p) s->music_playing = 1;
    s->music_loop = music_loop;
    s->level_left = peak_l * 100 / 32768; s->level_right = peak_r * 100 / 32768;
    if (s->level_left > 100) s->level_left = 100;
    if (s->level_right > 100) s->level_right = 100;
    if (backend == 2) { struct ac97_info i; ac97_get_info(&i); s->underruns = i.underruns; }
    s->samples_rendered = rendered;
}

/* ----------------------------------------------------------------- WAV */
const char *audio_wav_error(int code)
{
    switch (code) {
    case 0: return "ok";
    case -1: return "file not found";
    case -2: return "disk read error";
    case -3: return "not a RIFF/WAVE file";
    case -4: return "unsupported format (need 8/16-bit PCM, mono or stereo)";
    case -5: return "out of memory";
    case -6: return "no audio device";
    case -7: return "empty audio data";
    case -8: return "file too large (limit 6 MiB)";
    default: return "error";
    }
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int audio_play_wav(const char *name, int loop, struct wav_info *info)
{
    if (!backend) return -6;
    if (backend == 1) return -6;                              /* PCM needs the AC'97 DAC */
    int32_t size = vfs_file_size(name);
    if (size < 0) return -1;
    if (size > 6 * 1024 * 1024) return -8;
    uint8_t *file = (uint8_t *)kmalloc((size_t)size + 8);
    if (!file) return -5;
    uint32_t got = 0;
    if (vfs_read_all(name, (char *)file, (uint32_t)size, &got) != 0) { kfree(file); return -2; }
    if (got < 44 || memcmp(file, "RIFF", 4) != 0 || memcmp(file + 8, "WAVE", 4) != 0) { kfree(file); return -3; }

    uint32_t rate = 0, data_off = 0, data_len = 0; uint16_t ch = 0, bits = 0, fmt = 0;
    uint32_t p = 12;
    while (p + 8 <= got) {
        uint32_t clen = le32(file + p + 4);
        if (memcmp(file + p, "fmt ", 4) == 0 && clen >= 16 && p + 8 + 16 <= got) {
            fmt = le16(file + p + 8); ch = le16(file + p + 10); rate = le32(file + p + 12); bits = le16(file + p + 22);
            if (fmt == 0xFFFE && clen >= 26) fmt = le16(file + p + 8 + 24);          /* WAVE_FORMAT_EXTENSIBLE subformat */
        } else if (memcmp(file + p, "data", 4) == 0) {
            data_off = p + 8; data_len = clen;
            if (data_off + data_len > got) data_len = got - data_off;
            break;
        }
        p += 8 + clen + (clen & 1u);
    }
    if (fmt != 1 || (ch != 1 && ch != 2) || (bits != 8 && bits != 16) || rate < 4000 || rate > 192000) { kfree(file); return -4; }
    if (!data_len) { kfree(file); return -7; }
    uint32_t frames = data_len / ((bits / 8u) * ch);
    if (!frames) { kfree(file); return -7; }

    int16_t *pcm = (int16_t *)kmalloc((size_t)frames * 4u);
    if (!pcm) { kfree(file); return -5; }
    const uint8_t *d = file + data_off;
    for (uint32_t i = 0; i < frames; i++) {
        int16_t l, r;
        if (bits == 8) { l = (int16_t)(((int)d[i * ch] - 128) << 8); r = ch == 2 ? (int16_t)(((int)d[i * ch + 1] - 128) << 8) : l; }
        else { l = (int16_t)le16(d + i * ch * 2); r = ch == 2 ? (int16_t)le16(d + i * 4 + 2) : l; }
        pcm[i * 2] = l; pcm[i * 2 + 1] = r;
    }
    kfree(file);

    unsigned long f = irq_save();
    struct voice *v = voice_alloc();
    if (v->pcm_owned && v->pcm) kfree((void *)v->pcm);
    memset(v, 0, sizeof(*v));
    v->active = 1; v->is_pcm = 1; v->vol = 256; v->track = 0xFF;
    v->pcm = pcm; v->pcm_frames = frames; v->pcm_owned = 1; v->pcm_loop = loop;
    v->pcm_step = (uint32_t)(((uint64_t)rate << 16) / RATE);
    irq_restore(f);
    if (info) { info->rate = rate; info->channels = ch; info->bits = bits; info->frames = frames; info->ms = (uint32_t)((uint64_t)frames * 1000u / rate); }
    audio_wake();
    return 0;
}

/* ------------------------------------------------------ offline render */
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

int audio_render_wav(const char *mml, int wave, const char *filename, uint32_t *ms_out)
{
    int err;
    if (audio_mml_check(mml, &err) < 0) return -1;
    const uint32_t out_rate = RATE / 2;                       /* 24 kHz mono */
    const uint32_t max_secs = 120;
    unsigned long f = irq_save();
    offline = 1;
    irq_restore(f);

    /* Save and clear live state so the render is deterministic. */
    static struct voice saved_voices[NVOICES];
    static struct track saved_tracks[NTRACKS];
    int saved_loop = music_loop, saved_muted = muted, saved_paused = paused;
    static char saved_text[1024];
    memcpy(saved_voices, voices, sizeof(voices)); memcpy(saved_tracks, tracks, sizeof(tracks));
    strlcpy(saved_text, music_text, sizeof(saved_text));
    memset(voices, 0, sizeof(voices));
    music_loop = 0; muted = 0; paused = 0;
    strlcpy(music_text, mml, sizeof(music_text));
    tracks_init_from(music_text, wave, 0);

    size_t cap = (size_t)out_rate * 2u * 8u;
    uint8_t *buf = (uint8_t *)kmalloc(44 + cap);
    int rc = 0;
    size_t samples = 0;
    if (!buf) rc = -5;
    else {
        static int16_t chunk[512 * 2];
        uint32_t silent_frames = 0, total_frames = 0;
        while (total_frames < RATE * max_secs && !kabort_requested()) {
            render(chunk, 512);
            total_frames += 512;
            int active = anything_active();
            for (unsigned i = 0; i < 512; i += 2) {
                int32_t m = ((int32_t)chunk[i * 2] + chunk[i * 2 + 1] + chunk[i * 2 + 2] + chunk[i * 2 + 3]) / 4;
                if (44 + (samples + 1) * 2 > cap + 44) {
                    size_t ncap = cap * 2;
                    uint8_t *nb = (uint8_t *)krealloc(buf, 44 + ncap);
                    if (!nb) { rc = -5; break; }
                    buf = nb; cap = ncap;
                }
                buf[44 + samples * 2] = (uint8_t)m; buf[44 + samples * 2 + 1] = (uint8_t)(m >> 8);
                samples++;
            }
            if (rc) break;
            if (!active) { silent_frames += 512; if (silent_frames > RATE / 10u) break; } else silent_frames = 0;
        }
        if (!rc) {
            uint32_t data = (uint32_t)(samples * 2);
            memcpy(buf, "RIFF", 4); put_le32(buf + 4, 36 + data); memcpy(buf + 8, "WAVEfmt ", 8);
            put_le32(buf + 16, 16); buf[20] = 1; buf[21] = 0; buf[22] = 1; buf[23] = 0;
            put_le32(buf + 24, out_rate); put_le32(buf + 28, out_rate * 2); buf[32] = 2; buf[33] = 0; buf[34] = 16; buf[35] = 0;
            memcpy(buf + 36, "data", 4); put_le32(buf + 40, data);
            int w = vfs_write(filename, (const char *)buf, 44 + data);
            if (w != 0) rc = w == -4 ? -9 : -10;
            if (ms_out) *ms_out = (uint32_t)((uint64_t)samples * 1000u / out_rate);
        }
        kfree(buf);
    }

    f = irq_save();
    memcpy(voices, saved_voices, sizeof(voices)); memcpy(tracks, saved_tracks, sizeof(tracks));
    strlcpy(music_text, saved_text, sizeof(music_text));
    music_loop = saved_loop; muted = saved_muted; paused = saved_paused;
    offline = 0;
    irq_restore(f);
    return rc;
}

/* --------------------------------------------------------------- songs */
static const struct { const char *name; const char *mml; } songs[] = {
    { "scale",   "t140 l8 o4 c d e f g a b >c <b a g f e d c4" },
    { "twinkle", "t110 l4 o4 c c g g a a g2 f f e e d d c2 g g f f e e d2 g g f f e e d2 c c g g a a g2 f f e e d d c2,"
                 "t110 l2 o3 c e f c f c g c f c g e f c g c" },
    { "ode",     "t120 l4 o4 e e f g g f e d c c d e e.8 d8 d2 e e f g g f e d c c d e d.8 c8 c2 "
                 "d d e c d e8 f8 e c d e8 f8 e d c d <g2 e e f g g f e d c c d e d.8 c8 c2,"
                 "t120 l2 o3 c g c g c g c g g c g c g c g c" },
    { "elise",   "t135 l16 o5 e d+ e d+ e <b >d c <a8 r16 c e a b8 r16 e g+ b >c8 r16 e >e d+ e d+ e <b >d c <a8 "
                 "r16 c e a b8 r16 e c b a4,"
                 "t135 l16 o3 r4 r8 a e a r8 r8 e g+ e r8" },
    { "arp",     "t160 l16 o3 c e g >c e g >c e <<g >c e g <e c e g >c e <g e c <b g d >g b >d g b >g d <b g d" },
    { "march",   "t112 l8 o4 c4 c c4 e g4 g e4 c4 e g >c2 <b a g f e d c4,"
                 "t112 l4 o2 c g c g f c f c" },
};
#define NSONGS (int)(sizeof(songs) / sizeof(songs[0]))
const char *audio_song_lookup(const char *name)
{
    for (int i = 0; i < NSONGS; i++) if (strcasecmp(songs[i].name, name) == 0) return songs[i].mml;
    return NULL;
}
const char *audio_song_name(int i) { return (i >= 0 && i < NSONGS) ? songs[i].name : NULL; }
int audio_song_count(void) { return NSONGS; }
