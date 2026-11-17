// fmrender: ua_host's FMOD player (fmplay.c) offline, into a WAV file, without a game or a sound
// device (docs/fmod-player.md, "Checking it").
//
//   fmrender [-r rate] -o out.wav <data dir> <script|->      a script, lines "<seconds> <command>":
//       bank <name>                     load a bank (its .uafm.json / .pak in <data dir>)
//       play <event path> [as <name>]   create and start an instance (named <name>, else the path)
//       param <name> <parameter> <v>    set a parameter of a named instance
//       stop <name> [now]               stop it (fading out, or at once)
//       busvol <bus path> <v>           a bus's volume (the game's options)
//       key <key>                       the sound table key programmer instruments get from now on
//       end                             stop rendering
//     callbacks (markers, beats, stops) are printed as they come: "<seconds> <instance> marker Attack"
//   fmrender --bench [-s secs] [-e events/s] [-m music event] [-r rate] [-o out.wav] <data dir> <events.txt>
//     plays the music and random one-shot events (one path a line in events.txt) on the banks of
//     <data dir> as fast as it can, and prints the player's CPU per second of audio
#define _GNU_SOURCE
#include "fmplay.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *g_wav; static long g_frames; static int g_rate = 48000;
static void wav_header(void)
{
    uint32_t data = (uint32_t)g_frames * 4, riff = 36 + data, sr = (uint32_t)g_rate, br = sr * 4, fmt = 16;
    uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
    fseek(g_wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, g_wav); fwrite(&riff, 4, 1, g_wav); fwrite("WAVEfmt ", 1, 8, g_wav);
    fwrite(&fmt, 4, 1, g_wav); fwrite(&pcm, 2, 1, g_wav); fwrite(&ch, 2, 1, g_wav); fwrite(&sr, 4, 1, g_wav);
    fwrite(&br, 4, 1, g_wav); fwrite(&ba, 2, 1, g_wav); fwrite(&bits, 2, 1, g_wav);
    fwrite("data", 1, 4, g_wav); fwrite(&data, 4, 1, g_wav);
    fseek(g_wav, 0, SEEK_END);
}
static double g_peak, g_sq; static long g_n;
static void render(int frames)
{
    static float buf[2 * 4096];
    while (frames > 0) {
        int n = frames < 1024 ? frames : 1024;
        memset(buf, 0, sizeof(float) * 2 * (size_t)n);
        fmp_mix(buf, n, g_rate, 2);
        if (g_wav) {
            int16_t o[2 * 1024];
            for (int i = 0; i < 2 * n; i++) {
                float x = buf[i];
                double a = x < 0 ? -x : x;
                if (a > g_peak) g_peak = a;
                g_sq += (double)x * x; g_n++;
                o[i] = (int16_t)(x > 1 ? 32767 : x < -1 ? -32767 : x * 32767);
            }
            fwrite(o, 4, (size_t)n, g_wav);
            g_frames += n;
        }
        frames -= n;
    }
}

typedef struct { char name[128]; uint32_t id; } named;
static named g_named[256]; static int g_nnamed;
static double g_now;
static char g_key[256];

static void callbacks(void)
{
    fmp_update();
    fmp_cb c;
    while (fmp_poll(&c)) {
        const char *who = "?";
        for (int i = 0; i < g_nnamed; i++) if (g_named[i].id == c.inst) who = g_named[i].name;
        switch (c.type) {
        case FMP_CB_TIMELINE_MARKER: printf("%8.3f %s marker %s (%d ms)\n", g_now, who, c.name, c.position_ms); break;
        case FMP_CB_TIMELINE_BEAT: printf("%8.3f %s beat bar %d beat %d (%d ms, %.0f bpm %d/%d)\n", g_now, who, c.bar, c.beat, c.position_ms, c.tempo, c.upper, c.lower); break;
        case FMP_CB_STOPPED: printf("%8.3f %s stopped\n", g_now, who); break;
        case FMP_CB_CREATE_PROGRAMMER_SOUND: {
            int snd = g_key[0] ? fmp_sound(g_key) : -1;
            printf("%8.3f %s programmer sound %s: key %s -> %d (%d ms)\n", g_now, who, c.name ? c.name : "", g_key, snd, snd >= 0 ? fmp_sound_length_ms(snd) : 0);
            fmp_programmer_sound(c.inst, c.prog, snd);
            break;
        }
        default: break;
        }
    }
}

static uint32_t play(const char *path, const char *as)
{
    fmp_guid g;
    if (!fmp_lookup(path, g)) { fprintf(stderr, "fmrender: %s: no such path\n", path); return 0; }
    int ev = fmp_event(g);
    if (ev < 0) { fprintf(stderr, "fmrender: %s: no event (its bank not loaded?)\n", path); return 0; }
    uint32_t id = fmp_create(ev);
    if (!id) return 0;
    fmp_set_callback_mask(id, FMP_CB_TIMELINE_MARKER | FMP_CB_TIMELINE_BEAT | FMP_CB_STOPPED | FMP_CB_CREATE_PROGRAMMER_SOUND);
    fmp_start(id);
    if (g_nnamed < 256) { snprintf(g_named[g_nnamed].name, 128, "%s", as ? as : path); g_named[g_nnamed++].id = id; }
    return id;
}
static uint32_t by_name(const char *n)
{
    for (int i = g_nnamed - 1; i >= 0; i--) if (!strcmp(g_named[i].name, n)) return g_named[i].id;
    return 0;
}

static int script(FILE *f)
{
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *p = line, *e;
        double t = strtod(p, &e);
        if (e == p) continue;
        for (p = e; *p == ' ' || *p == '\t'; p++) {}
        p[strcspn(p, "\r\n")] = 0;
        long to = (long)(t * g_rate) - (long)(g_now * g_rate + 0.5);
        double start = g_now;
        for (long done = 0; done < to; done += 1024) {
            int k = to - done < 1024 ? (int)(to - done) : 1024;
            render(k);
            g_now = start + (double)(done + k) / g_rate;
            callbacks();
        }
        if (t > g_now) g_now = t;
        char a[512] = "", b[512] = "", c[512] = "";
        if (!strncmp(p, "bank ", 5)) fmp_load_bank(p + 5);
        else if (!strncmp(p, "play ", 5)) {
            char *as = strstr(p, " as ");
            if (as) *as = 0;
            play(p + 5, as ? as + 4 : NULL);
        } else if (sscanf(p, "param %511s %511s %511s", a, b, c) == 3) {
            uint32_t id = by_name(a);
            int ev = fmp_inst_event(id), i = fmp_event_param_index(ev, b);
            if (i < 0) fprintf(stderr, "fmrender: %s: no parameter %s\n", a, b); else fmp_set_param(id, i, (float)atof(c));
        } else if (sscanf(p, "stop %511s %511s", a, b) >= 1) fmp_stop(by_name(a), !strcmp(b, "now"));
        else if (!strncmp(p, "busvol ", 7)) {
            char *sp = strrchr(p, ' ');
            *sp = 0;
            fmp_bus_set_volume(fmp_bus(p + 7), (float)atof(sp + 1));
        } else if (!strncmp(p, "key ", 4)) snprintf(g_key, sizeof g_key, "%s", p + 4);
        else if (!strcmp(p, "end")) break;
        else fprintf(stderr, "fmrender: ? %s\n", p);
    }
    return 0;
}

static double cpu(void) { struct timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static int bench(const char *list, double secs, double eps, const char *music)
{
    FILE *f = fopen(list, "r");
    if (!f) { perror(list); return 1; }
    char line[512], *paths[4096];
    int n = 0;
    while (n < 4096 && fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; if (*line) paths[n++] = strdup(line); }
    fclose(f);
    if (music) play(music, "music");
    double next = 0, c0 = cpu();
    uint32_t r = 12345;
    long frames = (long)(secs * g_rate);
    for (long done = 0; done < frames; done += 512) {
        g_now = (double)done / g_rate;
        while (n && g_now >= next) {
            r = r * 1103515245 + 12345;
            uint32_t id = play(paths[(r >> 8) % (uint32_t)n], "sfx");
            if (id) fmp_release(id);
            g_nnamed = music ? 1 : 0;
            next += 1.0 / eps;
        }
        render(512);
        callbacks();
    }
    double used = cpu() - c0;
    fmp_stats s;
    fmp_get_stats(&s);
    printf("bench: %.0f s of audio at %d Hz, %.1f events/s: %.3f s CPU = %.2f %% of a core (mix %.2f %%); %lu voices started, peak %d, %lu dropped at the cap, %lu decode errors\n",
           secs, g_rate, eps, used, 100 * used / secs, 100 * s.mix_cpu_s / secs, s.voices_started, s.voices_peak, s.dropped, s.decode_errors);
    return 0;
}

int main(int argc, char **argv)
{
    const char *out = NULL, *music = NULL;
    double secs = 60, eps = 60;
    int b = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) g_rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) eps = atof(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) music = argv[++i];
        else if (!strcmp(argv[i], "--bench")) b = 1;
        else break;
    }
    if (argc - i < 2) {
        fprintf(stderr, "usage: fmrender [-r rate] -o out.wav <data dir> <script|->\n"
                        "       fmrender --bench [-s secs] [-e events/s] [-m music event] [-r rate] [-o out.wav] <data dir> <events.txt>\n");
        return 2;
    }
    if (!fmp_open(argv[i])) return 1;
    if (out) { g_wav = fopen(out, "wb"); if (!g_wav) { perror(out); return 1; } wav_header(); }
    int r;
    if (b) {
        // the banks: every .uafm.json of the folder, the strings bank first
        char cmd[1200];
        snprintf(cmd, sizeof cmd, "ls '%s' | grep '\\.uafm\\.json$' | sed 's/\\.uafm\\.json$//' | sort -r", argv[i]);
        FILE *ls = popen(cmd, "r");
        char name[512];
        while (ls && fgets(name, sizeof name, ls)) { name[strcspn(name, "\r\n")] = 0; if (strstr(name, ".strings")) fmp_load_bank(name); }
        if (ls) pclose(ls);
        ls = popen(cmd, "r");
        while (ls && fgets(name, sizeof name, ls)) { name[strcspn(name, "\r\n")] = 0; if (!strstr(name, ".strings") && strncmp(name, "VoiceOver_ES", 12)) fmp_load_bank(name); }
        if (ls) pclose(ls);
        r = bench(argv[i + 1], secs, eps, music);
    } else {
        FILE *f = strcmp(argv[i + 1], "-") ? fopen(argv[i + 1], "r") : stdin;
        if (!f) { perror(argv[i + 1]); return 1; }
        r = script(f);
        fmp_stats st;
        fmp_get_stats(&st);
        printf("%lu instances started, %lu voices started (peak %d at once, %lu dropped at the cap), %lu decode errors, mix %.3f s CPU\n",
               st.started, st.voices_started, st.voices_peak, st.dropped, st.decode_errors, st.mix_cpu_s);
    }
    if (g_wav) {
        wav_header();
        fclose(g_wav);
        printf("%s: %.1f s, peak %.3f, rms %.4f\n", out, (double)g_frames / g_rate, g_peak, g_n ? sqrt(g_sq / g_n) : 0);
    }
    return r;
}
