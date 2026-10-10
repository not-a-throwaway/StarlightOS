/*
 * cmd_sound.c -- audio shell commands.
 *   sound  beep  tone  play  wav  synth  vol
 */
#include "shell.h"
#include "audio.h"
#include "ac97.h"
#include "vfs.h"
#include "terminal.h"
#include "kstring.h"
#include "kprintf.h"
#include "kheap.h"
#include "ktime.h"
#include "ksched.h"
#include "keyboard.h"
#include "gui.h"

enum { C_LGRAY = 7, C_DGRAY = 8, C_LGREEN = 10, C_LCYAN = 11, C_LRED = 12, C_YELLOW = 14 };
static void fg(int c) { terminal_set_color((uint8_t)c, (uint8_t)(terminal_get_color() >> 4)); }
static void fg_reset(void) { fg(C_LGRAY); }
static void err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void err(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fg(C_LRED); kvprintf_err(fmt, ap); fg_reset();
    va_end(ap);
}

static const char *const wave_names[] = { "sine", "square", "saw", "triangle", "noise" };

static int parse_wave(const char *s)
{
    for (int i = 0; i < WAVE_COUNT; i++) if (!strcasecmp(s, wave_names[i])) return i;
    if (!strcasecmp(s, "tri")) return WAVE_TRI;
    if (!strcasecmp(s, "sq")) return WAVE_SQUARE;
    return -1;
}

static int need_audio(void)
{
    if (!audio_available()) { err("sound: no audio device\n"); return 0; }
    return 1;
}

static void bar(int level, int width, int fgcol)
{
    int n = level * width / 100;
    for (int i = 0; i < width; i++) {
        fg(i < n ? (i * 100 / width > 80 ? C_LRED : i * 100 / width > 60 ? C_YELLOW : fgcol) : C_DGRAY);
        terminal_putchar(i < n ? '#' : '.');
    }
    fg_reset();
}

/* Blocks while audio is busy, with a progress readout. Ctrl-C stops playback. */
static int wait_playing(uint32_t expected_ms, int show_meter)
{
    uint32_t t0 = ktime_ms();
    uint32_t last = 0;
    while (audio_busy()) {
        if (kabort_requested()) { audio_stop(); kprintf("\n(stopped)\n"); return 130; }
        uint32_t now = ktime_ms() - t0;
        if (show_meter && now - last >= 80) {
            last = now;
            struct audio_status st; audio_get_status(&st);
            kprintf("\r  %2u.%u s ", now / 1000, (now % 1000) / 100);
            if (expected_ms) kprintf("/ %u.%u s  ", expected_ms / 1000, (expected_ms % 1000) / 100);
            bar(st.level_left, 14, C_LGREEN); kprintf(" "); bar(st.level_right, 14, C_LGREEN); kprintf("   ");
        }
        kidle();
        if (now > 10 * 60 * 1000u) break;
    }
    if (show_meter) kprintf("\r%60s\r", "");
    return 0;
}

static int cmd_beep(int argc, char **argv)
{
    uint32_t hz = 880, ms = 150;
    if (argc >= 2 && kparse_uint(argv[1], &hz) != 0) { err("usage: beep [HZ] [MS]\n"); return 1; }
    if (argc >= 3 && kparse_uint(argv[2], &ms) != 0) { err("usage: beep [HZ] [MS]\n"); return 1; }
    if (!need_audio()) return 1;
    if (audio_tone((int)hz, (int)ms, WAVE_SINE, 80) != 0) { err("beep: frequency must be 20-20000 Hz and duration > 0\n"); return 1; }
    return wait_playing(ms, 0);
}

static int cmd_tone(int argc, char **argv)
{
    uint32_t hz, ms = 1000, vol = 80; int wave = WAVE_SINE;
    if (argc < 2 || kparse_uint(argv[1], &hz) != 0) { err("usage: tone HZ [MS] [WAVE] [VOLUME%%]   waves: sine square saw triangle noise\n"); return 1; }
    if (argc >= 3 && kparse_uint(argv[2], &ms) != 0) { err("tone: bad duration\n"); return 1; }
    if (argc >= 4 && (wave = parse_wave(argv[3])) < 0) { err("tone: unknown waveform '%s'\n", argv[3]); return 1; }
    if (argc >= 5 && kparse_uint(argv[4], &vol) != 0) { err("tone: bad volume\n"); return 1; }
    if (!need_audio()) return 1;
    if (audio_tone((int)hz, (int)ms, wave, (int)vol) != 0) { err("tone: frequency must be 20-20000 Hz\n"); return 1; }
    kprintf("%u Hz %s for %u ms\n", hz, wave_names[wave], ms);
    return wait_playing(ms, 1);
}

static void list_songs(void)
{
    kprintf("Built-in songs:");
    for (int i = 0; i < audio_song_count(); i++) kprintf(" %s", audio_song_name(i));
    kprintf("\n");
}

static void mml_help(void)
{
    kprintf("usage: play [-d] [-l] [-w WAVE] (SONG | \"MML\" | -f FILE.MML)\n"
            "  -d detach (return immediately)   -l loop   -w sine|square|saw|triangle|noise\n");
    list_songs();
    kprintf("MML:  c d e f g a b  notes   + / - sharp / flat   r rest   o4 octave   < > octave down/up\n"
            "      l8 default length   4. dotted   t120 tempo   v12 volume 0-15   w2 waveform\n"
            "      '&' ties notes;  ',' starts another track (up to 3), e.g.  \"t120 c e g, o3 c2 g2\"\n");
}

static int cmd_play(int argc, char **argv)
{
    int detach = 0, loop = 0, wave = WAVE_SQUARE;
    const char *arg = NULL, *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d")) detach = 1;
        else if (!strcmp(argv[i], "-l")) loop = 1;
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) { wave = parse_wave(argv[++i]); if (wave < 0) { err("play: unknown waveform\n"); return 1; } }
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { mml_help(); return 0; }
        else if (!arg) arg = argv[i];
    }
    if (!arg && !file) { mml_help(); return 1; }
    if (!need_audio()) return 1;

    char *filetext = NULL;
    const char *mml = arg;
    if (file) {
        size_t n; filetext = shell_read_file(file, &n);
        if (!filetext) { err("play: cannot read %s\n", file); return 1; }
        if (n > 1000) { err("play: MML file too long (max 1000 bytes)\n"); kfree(filetext); return 1; }
        for (char *p = filetext; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        mml = filetext;
    } else {
        const char *song = audio_song_lookup(arg);
        if (song) { mml = song; kprintf("Playing '%s'%s\n", arg, loop ? " (looping)" : ""); }
    }
    int errpos = 0;
    int tracks = audio_play_mml(mml, wave, loop, &errpos);
    if (tracks < 0) {
        err("play: syntax error at character %d:\n  %s\n  ", errpos + 1, mml);
        fg(C_LRED); for (int i = 0; i < errpos; i++) kprintf(" "); kprintf("^\n"); fg_reset();
        kfree(filetext);
        return 1;
    }
    kfree(filetext);
    if (detach || loop) {
        kprintf("%d track%s playing in the background. Use 'sound stop' to stop.\n", tracks, tracks == 1 ? "" : "s");
        if (!detach) { /* looping in foreground until Ctrl-C */ return wait_playing(0, 1) , audio_stop(), 0; }
        return 0;
    }
    return wait_playing(0, 1);
}

static int cmd_wav(int argc, char **argv)
{
    int loop = 0, detach = 0; const char *file = NULL;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-l")) loop = 1; else if (!strcmp(argv[i], "-d")) detach = 1; else file = argv[i]; }
    if (!file) { err("usage: wav [-l] [-d] FILE.WAV     (8/16-bit PCM, mono or stereo, any sample rate)\n"); return 1; }
    if (!need_audio()) return 1;
    struct wav_info wi;
    int r = audio_play_wav(file, loop, &wi);
    if (r != 0) { err("wav: %s: %s\n", file, audio_wav_error(r)); return 1; }
    kprintf("%s: %u Hz, %u-bit, %s, %u.%02u s\n", file, wi.rate, wi.bits, wi.channels == 2 ? "stereo" : "mono", wi.ms / 1000, (wi.ms % 1000) / 10);
    if (detach) return 0;
    return wait_playing(wi.ms, 1) == 130 ? 130 : (loop ? (audio_stop(), 0) : 0);
}

static int cmd_synth(int argc, char **argv)
{
    int wave = WAVE_SQUARE;
    const char *out = NULL, *src = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w") && i + 1 < argc) { wave = parse_wave(argv[++i]); if (wave < 0) { err("synth: unknown waveform\n"); return 1; } }
        else if (!out) out = argv[i];
        else if (!src) src = argv[i];
    }
    if (!out || !src) { err("usage: synth [-w WAVE] OUT.WAV (SONG | \"MML\")   renders to a 24 kHz mono WAV file\n"); return 1; }
    const char *song = audio_song_lookup(src);
    const char *mml = song ? song : src;
    kprintf("Rendering to %s... ", out);
    uint32_t ms = 0;
    int r = audio_render_wav(mml, wave, out, &ms);
    if (r == -1) { int ep; audio_mml_check(mml, &ep); err("syntax error at character %d\n", ep + 1); return 1; }
    if (r == -5) { err("out of memory\n"); return 1; }
    if (r == -9) { err("disk full\n"); return 1; }
    if (r) { err("could not write %s (use an 8.3 name)\n", out); return 1; }
    kprintf("done: %u.%02u s\n", ms / 1000, (ms % 1000) / 10);
    return 0;
}

static int cmd_vol(int argc, char **argv)
{
    uint32_t v;
    if (argc < 2) { kprintf("Volume: %d%%%s\n", audio_get_volume(), "" ); return 0; }
    if (!strcmp(argv[1], "up")) v = (uint32_t)audio_get_volume() + 10;
    else if (!strcmp(argv[1], "down")) v = audio_get_volume() >= 10 ? (uint32_t)audio_get_volume() - 10 : 0;
    else if (kparse_uint(argv[1], &v) != 0) { err("usage: vol [0-100|up|down]\n"); return 1; }
    if (v > 100) v = 100;
    audio_set_volume((int)v);
    kprintf("Volume: %u%%  ", v); bar((int)v, 20, C_LGREEN); kprintf("\n");
    { char m[32]; ksnprintf(m, sizeof(m), "Volume %u%%", v); gui_toast(m); audio_sfx(SFX_CLICK); }
    return 0;
}

static int cmd_sound(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "status";
    if (!strcmp(sub, "status")) {
        struct audio_status st; audio_get_status(&st);
        fg(C_LCYAN); kprintf("Audio\n"); fg_reset();
        if (st.backend == 2) {
            struct ac97_info ai; ac97_get_info(&ai);
            kprintf("  device   Intel AC'97, %s\n", ac97_codec_name());
            kprintf("  format   48000 Hz, 16-bit, stereo   (I/O %04x/%04x, irq %u)\n", ai.nam_base, ai.nabm_base, ai.irq_line);
            kprintf("  stream   %s, %u buffers queued, %u underruns\n", ai.running ? "running" : "idle", ai.buffers_played, ai.underruns);
        } else if (st.backend == 1) kprintf("  device   PC speaker (single tone only, no WAV)\n");
        else kprintf("  device   none\n");
        kprintf("  volume   %d%%%s   UI sounds %s\n", st.volume, st.muted ? " (MUTED)" : "", st.ui_sounds ? "on" : "off");
        kprintf("  voices   %d active%s%s\n", st.voices_active, st.music_playing ? ", music playing" : "", st.wav_playing ? ", WAV playing" : "");
        kprintf("  level    L "); bar(st.level_left, 20, C_LGREEN); kprintf("  R "); bar(st.level_right, 20, C_LGREEN); kprintf("\n");
        kprintf("Commands: beep tone play wav synth vol; see 'sound help'\n");
        return 0;
    }
    if (!strcmp(sub, "help")) {
        kprintf("sound [status]        device, volume and meters\n"
                "sound test            left/right/centre sweep to check the speakers\n"
                "sound stop | pause | resume\n"
                "sound vol N | mute | unmute | ui on|off\n"
                "sound meter           live level meter (Ctrl-C to exit)\n"
                "sound songs           list built-in songs\n"
                "beep [HZ] [MS]   tone HZ MS [WAVE] [VOL]   play SONG|\"MML\"|-f FILE   wav FILE.WAV   synth OUT.WAV SONG\n");
        return 0;
    }
    if (!strcmp(sub, "songs")) { list_songs(); return 0; }
    if (!need_audio()) return 1;
    if (!strcmp(sub, "stop")) { audio_stop(); kprintf("Stopped.\n"); return 0; }
    if (!strcmp(sub, "pause")) { audio_pause(1); return 0; }
    if (!strcmp(sub, "resume")) { audio_pause(0); return 0; }
    if (!strcmp(sub, "mute")) { audio_set_muted(1); return 0; }
    if (!strcmp(sub, "unmute")) { audio_set_muted(0); return 0; }
    if (!strcmp(sub, "vol")) { char *a[3] = { argv[0], argc >= 3 ? argv[2] : NULL, NULL }; return cmd_vol(argc >= 3 ? 2 : 1, a); }
    if (!strcmp(sub, "ui")) {
        if (argc < 3 || (strcmp(argv[2], "on") && strcmp(argv[2], "off"))) { err("usage: sound ui on|off\n"); return 1; }
        audio_set_ui_sounds(!strcmp(argv[2], "on"));
        kprintf("UI sounds %s.\n", argv[2]);
        return 0;
    }
    if (!strcmp(sub, "test")) {
        kprintf("Playing a chord sweep: low -> high, then a short melody.\n");
        static const int notes[] = { 262, 330, 392, 523, 659, 784 };
        for (unsigned i = 0; i < sizeof(notes) / sizeof(notes[0]) && !kabort_requested(); i++) {
            audio_tone(notes[i], 220, i < 3 ? WAVE_SINE : WAVE_TRI, 80);
            kprintf("  %d Hz\n", notes[i]);
            ksleep_ms(240);
        }
        audio_sfx(SFX_SUCCESS);
        wait_playing(0, 0);
        kprintf("Done. If you heard nothing, check the host volume and that QEMU has an audio backend\n(e.g. -audiodev pa/coreaudio/alsa,id=snd0 -device AC97,audiodev=snd0).\n");
        return 0;
    }
    if (!strcmp(sub, "meter")) {
        kprintf("Live meter. Play something in the background first (play -d twinkle). Ctrl-C to exit.\n");
        while (!kabort_requested()) {
            struct audio_status st; audio_get_status(&st);
            kprintf("\r L "); bar(st.level_left, 30, C_LGREEN); kprintf("  R "); bar(st.level_right, 30, C_LGREEN);
            ksleep_ms(60);
        }
        kprintf("\n");
        return 0;
    }
    err("sound: unknown subcommand '%s' (try 'sound help')\n", sub);
    return 1;
}

const struct command sound_commands[] = {
    { "sound", "Sound", "sound [status|test|stop|vol N|mute|ui on|off|meter|songs|help]", "audio status and control", cmd_sound },
    { "beep",  "Sound", "beep [HZ] [MS]", "play a beep", cmd_beep },
    { "tone",  "Sound", "tone HZ [MS] [WAVE] [VOL]", "play a tone with a chosen waveform", cmd_tone },
    { "play",  "Sound", "play [-d] [-l] [-w WAVE] SONG|\"MML\"|-f FILE", "play built-in songs or MML music", cmd_play },
    { "wav",   "Sound", "wav [-l] [-d] FILE.WAV", "play a WAV file from disk", cmd_wav },
    { "aplay", "Sound", "aplay FILE.WAV", "play a WAV file from disk", cmd_wav },
    { "synth", "Sound", "synth [-w WAVE] OUT.WAV SONG|\"MML\"", "render music to a WAV file", cmd_synth },
    { "vol",   "Sound", "vol [0-100|up|down]", "show or set master volume", cmd_vol },
};
const int sound_command_count = (int)(sizeof(sound_commands) / sizeof(sound_commands[0]));
