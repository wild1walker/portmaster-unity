// The Wwise player offline (wwplay.c, docs/plugins.md "The player"): plays a script of events, or a
// bench mix of them, into a WAV file (or nowhere), as fast as it can, and reports the player's CPU.
// Never a sound device.
//
//   wwrender [-r rate] [-o out.wav] <data dir> <script>
//     script lines: <seconds> <command> [args]; names are hashed as Wwise does (or #<id>):
//       bank <name>                     post <event> [game object]       state <group> <value>
//       switch <group> <value> [obj]    rtpc <name> <value>              stop <game object>
//       stopall                         end
//   wwrender --bench [-r rate] [-o out.wav] [-s seconds] [-e events/s] [-m music event]
//            [-t state event,state event,...] [-T state seconds] <data dir> <events.txt>
//     every bank in the data dir; events.txt as the earlier Wwise bench's (lines "E <category>
//     <event> <fx> <type>" and "S <switch group> <values>"): its mix of categories (10 % with
//     effects, 40 % WPN, 20 % ENM, 12 % OBJ, 8 % BOSS, 5 % CHR, the rest), each on one of 128
//     game objects that is stopped first, with random switches; the music event at the start, the
//     state events in turn every -T seconds (45). CPU: this thread's (mix and calls) per second
//     of audio, as % of one core.
#define _GNU_SOURCE
#include "wwplay.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static uint32_t ak_id(const char *s)
{
    if (*s == '#') return (uint32_t)strtoul(s + 1, NULL, 10);
    uint32_t h = 2166136261u;
    for (; *s; s++) { h *= 16777619u; h ^= (uint32_t)(unsigned char)tolower((unsigned char)*s); }
    return h;
}
static unsigned long g_ended;
static void h_done(uint32_t id) { (void)id; g_ended++; }
static void h_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); fprintf(stderr, "[wwise] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap); }
static const ua_wwise_host HOST = { UA_WWISE_BACKEND_VERSION, h_done, NULL, NULL, ak_id, NULL, h_log };

static const ua_wwise_backend *B;
static int RATE = 48000;
static wwplay_wav W;
static double g_t;                  // seconds rendered
static float buf[512 * 2];
static uint32_t g_pid;
static double cpu(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void render_to(double t)
{
    while (g_t < t) {
        int n = (int)((t - g_t) * RATE + 0.5);
        if (n > 512) n = 512;
        if (n <= 0) break;
        memset(buf, 0, sizeof buf);
        B->mix(buf, n, RATE, 2);
        if (W.f) wwplay_wav_write(&W, buf, n);
        g_t += (double)n / RATE;
    }
}

static uint32_t post(const char *ev, uint64_t obj) { uint32_t p = ++g_pid; if (!B->post_event(ak_id(ev), obj, p, 0)) g_ended++; return p; }

static void load_all(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    uint32_t id;
    int init = 0;
    while (d && (e = readdir(d))) if (!strcasecmp(e->d_name, "Init.uaww.json")) init = 1;
    if (init) B->load_bank("Init.bnk", 0, NULL, 0, &id);         // the buses first, as a game does
    if (d) rewinddir(d);
    while (d && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 10 && !strcmp(e->d_name + n - 10, ".uaww.json") && strcasecmp(e->d_name, "Init.uaww.json")) {
            char name[256];
            snprintf(name, sizeof name, "%.*s.bnk", (int)(n - 10), e->d_name);
            B->load_bank(name, 0, NULL, 0, &id);
        }
    }
    if (d) closedir(d);
}

static void report(double c0)
{
    wwplay_stats s;
    wwplay_get_stats(&s);
    double c = cpu() - c0;
    printf("rendered %.1f s at %d Hz: %lu events posted, %lu played, %lu ended; %lu voices started, %lu dropped at the cap, "
           "%lu limited, %d playing now (peak %d), %lu decode errors\n", g_t, RATE, s.posted, s.played, g_ended, s.voices_started,
           s.dropped, s.limited, s.voices, s.voices_peak, s.decode_errors);
    printf("cpu: %.3f s for %.1f s of audio = %.2f %% of one core (mix %.2f %%, calls %.2f %%)\n", c, g_t, 100 * c / g_t,
           100 * s.mix_cpu_s / g_t, 100 * s.call_cpu_s / g_t);
}

static int script(const char *dir, const char *path)
{
    FILE *f = strcmp(path, "-") ? fopen(path, "r") : stdin;
    if (!f) { perror(path); return 1; }
    char line[512];
    double c0 = cpu();
    while (fgets(line, sizeof line, f)) {
        char cmd[32] = "", a[128] = "", b[128] = "", c[64] = "";
        double t;
        if (line[0] == '#' || sscanf(line, "%lf %31s %127s %127s %63s", &t, cmd, a, b, c) < 2) continue;
        render_to(t);
        uint32_t id;
        if (!strcmp(cmd, "bank")) B->load_bank(a, 0, NULL, 0, &id);
        else if (!strcmp(cmd, "post")) post(a, *b ? strtoull(b, NULL, 10) : 1);
        else if (!strcmp(cmd, "state")) B->set_state(ak_id(a), ak_id(b));
        else if (!strcmp(cmd, "switch")) B->set_switch(ak_id(a), ak_id(b), *c ? strtoull(c, NULL, 10) : 1);
        else if (!strcmp(cmd, "rtpc")) B->set_rtpc(ak_id(a), (float)atof(b), 0, 0, 1);
        else if (!strcmp(cmd, "stop")) B->stop_all(strtoull(a, NULL, 10), 0);
        else if (!strcmp(cmd, "stopall")) B->stop_all(0, 1);
        else if (!strcmp(cmd, "end")) break;
        else fprintf(stderr, "unknown command: %s", line);
    }
    if (f != stdin) fclose(f);
    (void)dir;
    report(c0);
    return 0;
}

// ---- the bench mix (as the earlier Wwise bench: the same choice of events) ----
typedef struct { char cat[16], name[96]; int fx; } ev_t;
static ev_t *evs;
static int nev, nsw, nswv[8];
static char *swg[8], **swv[8];
static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static int excluded(const char *n)
{
    static const char *const bad[] = { "oop", "OOP", "_LP", "_lp", "Amb", "AMB", "amb", "_MUS_", "Play_MUS", "Start", "start", "Charge", "charge", 0 };
    for (int i = 0; bad[i]; i++) if (strstr(n, bad[i])) return 1;
    return 0;
}
static int load_events(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 0; }
    static char line[16384];
    evs = calloc(4096, sizeof *evs);
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *a = strtok(line, "\t");
        if (!a) continue;
        if (!strcmp(a, "S") && nsw < 8) {
            char *g = strtok(NULL, "\t"), *vals = strtok(NULL, "\t");
            if (!g || !vals) continue;
            swg[nsw] = strdup(g);
            swv[nsw] = calloc(512, sizeof(char *));
            for (char *v = strtok(vals, " "); v && nswv[nsw] < 512; v = strtok(NULL, " ")) swv[nsw][nswv[nsw]++] = strdup(v);
            nsw++;
        } else if (!strcmp(a, "E") && nev < 4096) {
            char *cat = strtok(NULL, "\t"), *name = strtok(NULL, "\t"), *fx = strtok(NULL, "\t");
            if (!cat || !name || !fx || excluded(name)) continue;
            snprintf(evs[nev].cat, sizeof evs[nev].cat, "%s", cat);
            snprintf(evs[nev].name, sizeof evs[nev].name, "%s", name);
            evs[nev++].fx = atoi(fx);
        }
    }
    fclose(f);
    return nev > 0;
}
static const ev_t *pick(void)
{
    static const char *const cats[] = { "WPN", "ENM", "OBJ", "BOSS", "CHR" };
    static const int w[] = { 40, 20, 12, 8, 5 };
    unsigned r = rnd() % 100;
    int wantfx = r < 10;
    const char *cat = NULL;
    if (!wantfx) {
        int acc = 10;
        for (int i = 0; i < 5; i++) { acc += w[i]; if ((int)r < acc) { cat = cats[i]; break; } }
    }
    for (int tries = 0; tries < 20000; tries++) {
        const ev_t *e = &evs[rnd() % nev];
        if (wantfx ? e->fx : (cat ? !strcmp(e->cat, cat) && !e->fx : !e->fx)) return e;
    }
    return &evs[0];
}

static int bench(const char *dir, const char *events, double secs, double rate, const char *music, char *states, double state_every)
{
    if (!load_events(events)) return 1;
    load_all(dir);
    char *st[16];
    int nst = 0;
    for (char *s = states ? strtok(states, ",") : NULL; s && nst < 16; s = strtok(NULL, ",")) st[nst++] = s;
    const uint64_t MUSIC = 2, EMIT0 = 1000;
    const int NEMIT = 128;
    double c0 = cpu(), acc = 0, next_state = state_every;
    int slot = 0, si = 0;
    if (music) { post(music, MUSIC); if (nst) post(st[si++ % nst], MUSIC); }
    for (double t = 0; t < secs; t += 1.0 / 60) {          // a game's frames: events, then RenderAudio
        acc += rate / 60;
        while (acc >= 1) {
            acc -= 1;
            const ev_t *e = pick();
            uint64_t go = EMIT0 + (slot++ % NEMIT);
            B->stop_all(go, 0);
            for (int g = 0; g < nsw; g++)
                if (!strcmp(swg[g], "WPN_Guns") || rnd() % 4 == 0) B->set_switch(ak_id(swg[g]), ak_id(swv[g][rnd() % nswv[g]]), go);
            post(e->name, go);
        }
        B->render();
        if (nst && t >= next_state) { post(st[si++ % nst], MUSIC); next_state += state_every; }
        render_to(t + 1.0 / 60);
    }
    report(c0);
    return 0;
}

int main(int argc, char **argv)
{
    int bench_mode = 0, i = 1;
    const char *out = NULL, *music = NULL;
    char *states = NULL;
    double secs = 60, rate = 60, state_every = 45;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--bench")) bench_mode = 1;
        else if (i + 1 < argc && !strcmp(argv[i], "-r")) RATE = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "-o")) out = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "-s")) secs = atof(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "-e")) rate = atof(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "-m")) music = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "-t")) states = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "-T")) state_every = atof(argv[++i]);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (argc - i < 2) {
        fprintf(stderr, "usage: wwrender [-r rate] [-o out.wav] <data dir> <script>\n"
                        "       wwrender --bench [-r rate] [-o out.wav] [-s secs] [-e events/s] [-m music event] [-t state events] [-T secs] <data dir> <events.txt>\n");
        return 2;
    }
    if (!getenv("UA_WWISE_STATS")) setenv("UA_WWISE_STATS", "0", 1);
    B = wwplay_open(&HOST, argv[i]);
    if (!B) return 1;
    if (out && !wwplay_wav_open(&W, out, RATE, 2)) { perror(out); return 1; }
    int r = bench_mode ? bench(argv[i], argv[i + 1], secs, rate, music, states, state_every) : script(argv[i], argv[i + 1]);
    wwplay_wav_close(&W);
    wwplay_close();
    return r;
}
