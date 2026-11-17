// ua_host's own FMOD Studio player (docs/fmod-player.md; docs/plugins.md, "FMOD"): plays a game's
// FMOD banks from the files pmu's setup writes (setup-rs, "fmod": per bank <Bank>.uafm.json with its
// events resolved into instruments, boxes, controllers, and <Bank>.uafm.pak with its samples as
// standard Ogg Vorbis). The FMOD stand-in (fmodstub.c) answers the game's FMOD calls with it.
// Nothing of FMOD's code or data is in here.
//
// What it plays (step 1): an event's timeline (asynchronous instruments triggered as the cursor
// enters their boxes, time-locked ones following it; markers and tempo for the game's marker and
// beat callbacks; transition markers and regions, with their parameter conditions, chance and bar /
// beat quantization: loops and jumps), its parameter sheets (instruments triggered by a parameter's
// range), single, multi (sequential, random, shuffle; weights) and programmer instruments,
// instrument volume, pitch, loop count, trigger chance and delay, random volume / pitch modulators,
// AHDSR envelopes on the event's master and on instruments (stop with fade-out = the release), the
// event's tracks and master with their volumes, controllers (automation) on track / master /
// instrument volume, pitch and fade and on the panner's position, by the timeline or a parameter
// (with the parameter's seek speed), the mixer's group buses (volumes, the game's bus volume,
// pause, mute, stop), event polyphony. Not: effects other than the panner (reverb returns, EQ,
// compressors), snapshots, 3D, event instruments, scatterers, sustain points, transition timelines.
//
// The mixer: blocks of 256 frames; voices decode Ogg Vorbis (stb_vorbis, 512 frames a refill),
// resample with their pitch by linear interpolation, mix with a per-block gain ramp into float
// stereo; a trigger inside a block starts at its exact frame. A voice below -60 dB is virtual (not
// decoded, its position kept), as FMOD virtualizes inaudible voices. At most UA_FMOD_VOICES (48)
// voices: a new one past that replaces the quietest. A peak limiter keeps the player's mix under
// 0.98 (UA_FMOD_LIMITER=0: off). A sample's stream is mapped from its bank's pak only while voices
// use it (a 32-bit game has no address space for every pak at once).
#define _GNU_SOURCE
#include "fmplay.h"

#include <alloca.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#include "../thirdparty/stb/stb_vorbis.c"

#define BLK 256                 // frames a control block
#define DEC_N 512               // frames decoded a refill
#define MAXINST 512             // instances
#define MAXP 24                 // parameters an event
#define TL_RATE 48000.0         // timeline units a second
#define MAXCMD 4096
#define MAXCB 1024

static void (*g_log)(const char *) = NULL;
static void logf_(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (g_log) g_log(b); else fprintf(stderr, "[fmod] %s\n", b);
}
void fmp_set_log(void (*log)(const char *)) { g_log = log; }

static double cpu_now(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static double wall_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

#define GROW(p, n, cap) do { if ((n) >= (cap)) { (cap) = (cap) ? (cap) * 2 : 16; (p) = realloc((p), (size_t)(cap) * sizeof *(p)); } } while (0)

// ------------------------------------------------------------------------------------------ JSON
// (the Wwise player's reader: values in a tree, strings decoded in place)
typedef struct jv { char t; int n; double num; char *s; struct jv *v; char **k; } jv;

static char *js_ws(char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }
static char *js_str(char **pp)
{
    char *p = *pp + 1, *o = p, *start = p;
    while (*p && *p != '"') {
        if (*p != '\\') { *o++ = *p++; continue; }
        p++;
        char c = *p++;
        switch (c) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u': {
            unsigned u = 0;
            for (int i = 0; i < 4 && *p; i++, p++) u = u * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
            if (u < 0x80) *o++ = (char)u;
            else if (u < 0x800) { *o++ = (char)(0xc0 | u >> 6); *o++ = (char)(0x80 | (u & 63)); }
            else { *o++ = (char)(0xe0 | u >> 12); *o++ = (char)(0x80 | ((u >> 6) & 63)); *o++ = (char)(0x80 | (u & 63)); }
            break;
        }
        case 0: p--; break;
        default: *o++ = c;
        }
    }
    if (*p == '"') p++;
    *o = 0;
    *pp = p;
    return start;
}
static int js_val(char **pp, jv *out, int depth)
{
    char *p = js_ws(*pp);
    memset(out, 0, sizeof *out);
    if (depth > 64) return 0;
    if (*p == '{' || *p == '[') {
        int obj = *p == '{', cap = 0;
        out->t = obj ? 'o' : 'a';
        p = js_ws(p + 1);
        if (*p == (obj ? '}' : ']')) { *pp = p + 1; return 1; }
        for (;;) {
            char *key = NULL;
            if (obj) {
                if (*p != '"') return 0;
                key = js_str(&p);
                p = js_ws(p);
                if (*p != ':') return 0;
                p++;
            }
            if (out->n >= cap) {
                cap = cap ? cap * 2 : 4;
                out->v = realloc(out->v, cap * sizeof *out->v);
                if (obj) out->k = realloc(out->k, cap * sizeof *out->k);
            }
            if (obj) out->k[out->n] = key;
            if (!js_val(&p, &out->v[out->n++], depth + 1)) return 0;
            p = js_ws(p);
            if (*p == ',') { p = js_ws(p + 1); continue; }
            if (*p == (obj ? '}' : ']')) { *pp = p + 1; return 1; }
            return 0;
        }
    }
    if (*p == '"') { out->t = 's'; out->s = js_str(&p); *pp = p; return 1; }
    if (!strncmp(p, "true", 4)) { out->t = 'b'; out->num = 1; *pp = p + 4; return 1; }
    if (!strncmp(p, "false", 5)) { out->t = 'b'; *pp = p + 5; return 1; }
    if (!strncmp(p, "null", 4)) { out->t = 'n'; *pp = p + 4; return 1; }
    char *e;
    out->num = strtod(p, &e);
    if (e == p) return 0;
    out->t = 'd';
    *pp = e;
    return 1;
}
static void js_free(jv *v)
{
    for (int i = 0; i < v->n; i++) js_free(&v->v[i]);
    free(v->v);
    free(v->k);
}
static jv *jget(const jv *o, const char *key)
{
    if (!o || o->t != 'o') return NULL;
    for (int i = 0; i < o->n; i++) if (!strcmp(o->k[i], key)) return &o->v[i];
    return NULL;
}
static double jnum(const jv *o, const char *key, double d)
{
    jv *v = jget(o, key);
    return v && (v->t == 'd' || v->t == 'b') ? v->num : d;
}
static double jat(const jv *a, int i, double d) { return a && a->t == 'a' && i < a->n && a->v[i].t == 'd' ? a->v[i].num : d; }
static const char *jstr(const jv *o, const char *key) { jv *v = jget(o, key); return v && v->t == 's' ? v->s : ""; }
static int jlen(const jv *a) { return a && (a->t == 'a' || a->t == 'o') ? a->n : 0; }

static int hexguid(const char *s, fmp_guid g)
{
    if (strlen(s) != 32) return 0;
    for (int i = 0; i < 16; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + k];
            v = v * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
        }
        g[i] = (uint8_t)v;
    }
    return 1;
}

// ---------------------------------------------------------------------------------------- tables
// hash maps: 16-byte GUIDs and strings to an int
typedef struct { uint8_t (*k)[16]; char **s; int32_t *v; uint32_t cap, n; } hmap;

static uint32_t h_guid(const uint8_t *g) { uint32_t h; memcpy(&h, g, 4); return h * 2654435761u ^ g[15]; }
static uint32_t h_str(const char *s) { uint32_t h = 2166136261u; for (; *s; s++) h = (h ^ (uint8_t)(*s | ((*s >= 'A' && *s <= 'Z') ? 32 : 0))) * 16777619u; return h; }
static const uint8_t g_zero[16];

static void hm_put(hmap *m, const uint8_t *g, const char *s, int32_t v);
static void hm_grow(hmap *m)
{
    hmap n = { 0 };
    n.cap = m->cap ? m->cap * 2 : 64;
    n.k = calloc(n.cap, 16);
    n.s = calloc(n.cap, sizeof *n.s);
    n.v = calloc(n.cap, sizeof *n.v);
    for (uint32_t i = 0; i < m->cap; i++)
        if (m->s[i] || memcmp(m->k[i], g_zero, 16)) hm_put(&n, m->s[i] ? NULL : m->k[i], m->s[i], m->v[i]);
    free(m->k); free(m->s); free(m->v);
    *m = n;
}
// a GUID key (s NULL) or a string key (case-insensitive; the string is kept, not copied)
static void hm_put(hmap *m, const uint8_t *g, const char *s, int32_t v)
{
    if ((m->n + 1) * 2 > m->cap) hm_grow(m);
    uint32_t i = (s ? h_str(s) : h_guid(g)) & (m->cap - 1);
    for (;; i = (i + 1) & (m->cap - 1)) {
        int empty = !m->s[i] && !memcmp(m->k[i], g_zero, 16);
        if (empty) break;
        if (s ? m->s[i] && !strcasecmp(m->s[i], s) : !m->s[i] && !memcmp(m->k[i], g, 16)) { m->v[i] = v; return; }
    }
    if (s) m->s[i] = (char *)s; else memcpy(m->k[i], g, 16);
    m->v[i] = v;
    m->n++;
}
static int32_t hm_get(const hmap *m, const uint8_t *g, const char *s)
{
    if (!m->cap) return -1;
    for (uint32_t i = (s ? h_str(s) : h_guid(g)) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->s[i] && !memcmp(m->k[i], g_zero, 16)) return -1;
        if (s ? m->s[i] && !strcasecmp(m->s[i], s) : !m->s[i] && !memcmp(m->k[i], g, 16)) return m->v[i];
    }
}

// ------------------------------------------------------------------------------------------ data
enum { K_WAVE, K_MULTI, K_PROG, K_NONE };
enum { ON_MASTER, ON_TRACK, ON_INST, ON_PAN };
enum { P_VOL, P_PITCH, P_FADE, P_POS };

// a sample: its stream in the bank's pak, mapped only while voices (or the decoder cache) use it
// (a 32-bit game's address space has no room for every pak at once: Blasphemous's are 414 MB)
typedef struct {
    const uint8_t *p;           // the stream, while mapped
    uint32_t len, samples, off;
    uint8_t ch, avail;
    int fd, refs;
    void *map;
    size_t maplen;
} media_t;
typedef struct { float x, y, s; } pt_t;
typedef struct { uint8_t on, prop; int16_t i, by; int n; pt_t *pts; } ctrl_t;
typedef struct { int inst; double start, len; } box_t;
typedef struct { int param, n; box_t *b; } sheet_t;
typedef struct { uint32_t pos; const char *name; } marker_t;
typedef struct { uint32_t pos; float bpm; int up, low; } tempo_t;
typedef struct { int p; float lo, hi; } cond_t;
typedef struct { uint32_t at, end, to; int ncond; cond_t *cond; float chance; int qu, qm; } jump_t;
typedef struct {
    uint8_t kind, env;
    int media, loop, track, nkids, mode, sel, max;
    float vol, pitch, chance, d0, d1, rvol, rpitch, rstart;
    float e[7];                 // AHDSR: initial, peak, sustain (dB), attack, hold, decay, release (s)
    int *kids;
    float *w, wsum;
    int ncond;                  // conditions: parameters in ranges while the timeline triggers it
    cond_t *cond;
    const char *name;
    int seq, last;              // sequential position, the last pick (shuffle avoids it)
} inst_t;
typedef struct { const char *name; float min, max, def, up, down; } param_t;
typedef struct {
    fmp_guid id;
    int bank, bus, max, steal, snapshot, persistent, oneshot, live;
    float in_vol, vol, pitch;
    uint32_t len;
    int nparams, ntracks, ninst, nasync, nlocked, nsheets, nmarkers, ntempo, njumps, nctrl;
    param_t *params;
    float *track_vol;
    inst_t *insts;
    box_t *async_b, *locked_b;
    sheet_t *sheets;
    marker_t *markers;
    tempo_t *tempo;
    jump_t *jumps;
    ctrl_t *ctrl;
    uint8_t env;
    float e[7];
} event_t;
typedef struct {
    fmp_guid id;
    int out, kind;
    float vol;                  // dB, the bank's
    float game_vol;             // the game's setVolume (linear)
    int paused, mute;
} bus_t;
typedef struct {
    char *name;
    jv root;
    char *text;
    int fd;                     // the pak, open (its samples are mapped one by one)
    size_t paklen;
    media_t *media;
    int nmedia, loaded;
    int *events, nevents;
    uint64_t *keys;             // sound table: sorted key hashes and their samples
    int *key_media, nkeys;
} bank_t;

static char g_dir[PATH_MAX];
static int g_open;
static pthread_mutex_t L = PTHREAD_MUTEX_INITIALIZER;       // the player's state (mix, loading)
static pthread_mutex_t QL = PTHREAD_MUTEX_INITIALIZER;      // the call queue and the callback queue

static bank_t *g_banks; static int g_nbanks, g_cbanks;
static event_t *g_events; static int g_nevents, g_cevents;
static bus_t *g_buses; static int g_nbuses, g_cbuses;
static hmap g_event_ids, g_bus_ids, g_paths, g_bus_paths;
static char **g_path_of; static int g_npath_of, g_cpath_of;    // GUID -> path (g_guid_path index)
static hmap g_guid_path;
static int g_master_bus = -1;
static float g_master_vol = 1.0f;
static int g_all_paused;

// ------------------------------------------------------------------------------------- instances
typedef struct {
    // the game's side (its threads)
    uint32_t gen;
    int ev, used, released, created_sent;
    float gp[MAXP], gvol, gpitch;
    int gpaused;
    uint32_t mask;
    volatile int pending_start;     // a start queued, not yet run by the mixer
    // published by the mixer
    volatile int state;
    volatile int pos_ms;
    // the mixer's
    int active, stopping, failed;
    uint32_t mgen;
    double cursor;
    float pv[MAXP], pt[MAXP], vol, pitch;
    int paused;
    uint8_t *trig;                  // per async box, then per sheet box
    int ntrig;
    double t;                       // seconds since start (the master envelope)
    double stop_t;                  // seconds since the fade-out began
    float env_at_stop;
    uint64_t order;
    int voices, progs;
    long last_beat;                 // the last beat delivered (index), -1 none
} inst_rt;
static inst_rt g_inst[MAXINST];
static uint64_t g_order;

// ------------------------------------------------------------------------------------------ voices
typedef struct {
    int used, slot, ii, top, box, bkind;     // instrument, its top instrument, box (bkind 0 async, 1 locked, 2 sheet, 3 programmer)
    uint32_t gen;
    const media_t *m;
    int media_bank, media_ix;
    stb_vorbis *vb;
    int16_t buf[2][DEC_N + 1];
    int n, p;
    uint32_t frac, step, next, end;
    int loops;                  // passes left (0: forever)
    int stop_loop;              // untriggered: this pass is the last
    long delay, remain;         // frames before it starts; frames before it is cut (-1: none)
    float g[2];                 // the gains of the last block
    float rvol, rpitch;         // drawn at the start
    double t, rel_t;            // seconds playing; seconds into the release (-1: none)
    float rel_from;
    float fade;                 // frames of a stop's fade left (0: none), then the voice ends
    int fade_len;
    int prog;                   // a programmer sound's request (-1)
    int virt;                   // inaudible: not decoded, its position advanced (vpos, source frames)
    double vpos;
    float level;                // its last gain (for the cap)
} voice_t;
static voice_t *g_voices; static int g_maxv = 48;
static int g_rate = 48000;
static uint32_t g_rng = 0x9e3779b9u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static float frand(void) { return (rnd() >> 8) * (1.0f / 16777216.0f); }
static float db2lin(float db) { return db <= -80 ? 0 : powf(10.0f, db / 20); }

// ---------------------------------------------------------------------------------------- the queue
enum { Q_START, Q_STOP, Q_PARAM, Q_VOL, Q_PITCH, Q_PAUSE, Q_POS, Q_PROG, Q_BUSVOL, Q_BUSPAUSE, Q_BUSMUTE, Q_BUSSTOP, Q_MASTER, Q_PAUSEALL, Q_UNLOAD };
typedef struct { int op, slot, i, j; uint32_t gen; float f; } cmd_t;
static cmd_t g_q[MAXCMD]; static int g_qn;
static fmp_cb g_cb[MAXCB]; static int g_cbn;
static volatile double g_last_mix;      // wall time of the last mix (0: none yet)

typedef struct { fmp_stats s; } stats_t;
static fmp_stats g_st;

static void push(cmd_t c)
{
    double t0 = cpu_now();
    pthread_mutex_lock(&QL);
    if (g_qn < MAXCMD) g_q[g_qn++] = c;
    pthread_mutex_unlock(&QL);
    g_st.call_cpu_s += cpu_now() - t0;
}
static void cb_push(fmp_cb c)           // (the mixer's thread, or the game's)
{
    pthread_mutex_lock(&QL);
    if (g_cbn < MAXCB) g_cb[g_cbn++] = c;
    pthread_mutex_unlock(&QL);
}

// ---------------------------------------------------------------------------------------- loading
static int load_json(const char *path, jv *root, char **text)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *t = malloc((size_t)n + 1);
    if (!t || fread(t, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(t); return 0; }
    fclose(f);
    t[n] = 0;
    char *p = t;
    if (!js_val(&p, root, 0)) { js_free(root); free(t); return 0; }
    *text = t;
    return 1;
}

static int bus_add(const fmp_guid id)
{
    int i = hm_get(&g_bus_ids, id, NULL);
    if (i >= 0) return i;
    GROW(g_buses, g_nbuses, g_cbuses);
    bus_t *b = &g_buses[g_nbuses];
    memset(b, 0, sizeof *b);
    memcpy(b->id, id, 16);
    b->out = -1;
    b->game_vol = 1.0f;
    hm_put(&g_bus_ids, b->id, NULL, g_nbuses);
    return g_nbuses++;
}

static float *floats(const jv *a, int *n)
{
    *n = jlen(a);
    float *v = calloc((size_t)*n + 1, sizeof *v);
    for (int i = 0; i < *n; i++) v[i] = (float)jat(a, i, 0);
    return v;
}

static box_t *boxes(const jv *a, int *n)
{
    *n = jlen(a);
    box_t *b = calloc((size_t)*n + 1, sizeof *b);
    for (int i = 0; i < *n; i++) b[i] = (box_t){ (int)jat(&a->v[i], 0, -1), jat(&a->v[i], 1, 0), jat(&a->v[i], 2, 0) };
    return b;
}

static void env_read(const jv *a, uint8_t *has, float *e)
{
    if (jlen(a) < 7) return;
    *has = 1;
    for (int k = 0; k < 7; k++) e[k] = (float)jat(a, k, 0);
}

static void event_load(event_t *e, const jv *o, int bank)
{
    e->bank = bank;
    const char *bus = jstr(o, "bus");
    fmp_guid g;
    e->bus = hexguid(bus, g) && memcmp(g, g_zero, 16) ? bus_add(g) : g_master_bus;
    e->in_vol = (float)jnum(o, "in_vol", 0);
    e->vol = (float)jnum(o, "vol", 0);
    e->pitch = (float)jnum(o, "pitch", 0);
    e->max = (int)jnum(o, "max", 0);
    e->steal = (int)jnum(o, "steal", 0);
    e->snapshot = jnum(o, "snapshot", 0) != 0;
    e->len = (uint32_t)jnum(o, "len", 0);
    const jv *ps = jget(o, "params");
    e->nparams = jlen(ps) < MAXP ? jlen(ps) : MAXP;
    e->params = calloc((size_t)e->nparams + 1, sizeof *e->params);
    for (int i = 0; i < e->nparams; i++) {
        const jv *p = &ps->v[i];
        e->params[i] = (param_t){ jstr(p, "name"), (float)jnum(p, "min", 0), (float)jnum(p, "max", 1), (float)jnum(p, "def", 0), (float)jnum(p, "up", 0), (float)jnum(p, "down", 0) };
    }
    const jv *tr = jget(o, "tracks");
    e->ntracks = jlen(tr);
    e->track_vol = calloc((size_t)e->ntracks + 1, sizeof *e->track_vol);
    for (int i = 0; i < e->ntracks; i++) e->track_vol[i] = (float)jnum(&tr->v[i], "vol", 0);
    const jv *is = jget(o, "insts");
    e->ninst = jlen(is);
    e->insts = calloc((size_t)e->ninst + 1, sizeof *e->insts);
    for (int i = 0; i < e->ninst; i++) {
        const jv *x = &is->v[i];
        inst_t *n = &e->insts[i];
        const char *k = jstr(x, "k");
        n->kind = !strcmp(k, "wave") ? K_WAVE : !strcmp(k, "multi") ? K_MULTI : !strcmp(k, "prog") ? K_PROG : K_NONE;
        n->media = (int)jnum(x, "m", -1);
        n->vol = (float)jnum(x, "vol", 0);
        n->pitch = (float)jnum(x, "pitch", 0);
        n->chance = (float)jnum(x, "chance", 100);
        n->loop = (int)jnum(x, "loop", 0);
        n->track = (int)jnum(x, "track", -1);
        n->rvol = (float)jnum(x, "rvol", 0);
        n->rpitch = (float)jnum(x, "rpitch", 0);
        n->rstart = (float)jnum(x, "rstart", 0);
        n->max = (int)jnum(x, "max", 0);
        const jv *d = jget(x, "delay");
        n->d0 = (float)jat(d, 0, 0); n->d1 = (float)jat(d, 1, 0);
        env_read(jget(x, "ahdsr"), &n->env, n->e);
        n->name = jstr(x, "name");
        n->mode = (int)jnum(x, "mode", 0);
        n->sel = (int)jnum(x, "sel", 1);
        const jv *kids = jget(x, "kids");
        n->nkids = jlen(kids);
        n->kids = calloc((size_t)n->nkids + 1, sizeof *n->kids);
        for (int c = 0; c < n->nkids; c++) n->kids[c] = (int)jat(kids, c, 0);
        int nw = 0;
        n->w = floats(jget(x, "w"), &nw);
        if (nw != n->nkids) { free(n->w); n->w = NULL; }
        n->wsum = 0;
        for (int c = 0; n->w && c < n->nkids; c++) n->wsum += n->w[c];
        n->last = -1;
        const jv *c = jget(x, "if");
        n->ncond = jlen(c);
        n->cond = calloc((size_t)n->ncond + 1, sizeof *n->cond);
        for (int k = 0; k < n->ncond; k++) n->cond[k] = (cond_t){ (int)jat(&c->v[k], 0, -1), (float)jat(&c->v[k], 1, 0), (float)jat(&c->v[k], 2, 0) };
    }
    e->async_b = boxes(jget(o, "async"), &e->nasync);
    e->locked_b = boxes(jget(o, "locked"), &e->nlocked);
    const jv *sh = jget(o, "sheets");
    e->nsheets = jlen(sh);
    e->sheets = calloc((size_t)e->nsheets + 1, sizeof *e->sheets);
    for (int i = 0; i < e->nsheets; i++) {
        e->sheets[i].param = (int)jat(&sh->v[i], 0, -1);
        e->sheets[i].b = boxes(sh->v[i].n > 1 ? &sh->v[i].v[1] : NULL, &e->sheets[i].n);
    }
    const jv *mk = jget(o, "markers");
    e->nmarkers = jlen(mk);
    e->markers = calloc((size_t)e->nmarkers + 1, sizeof *e->markers);
    for (int i = 0; i < e->nmarkers; i++) {
        const jv *m = &mk->v[i];
        e->markers[i].pos = (uint32_t)jat(m, 0, 0);
        e->markers[i].name = m->n > 1 && m->v[1].t == 's' ? m->v[1].s : "";
    }
    const jv *tp = jget(o, "tempo");
    e->ntempo = jlen(tp);
    e->tempo = calloc((size_t)e->ntempo + 1, sizeof *e->tempo);
    for (int i = 0; i < e->ntempo; i++) {
        const jv *t = &tp->v[i];
        e->tempo[i] = (tempo_t){ (uint32_t)jat(t, 0, 0), (float)jat(t, 1, 120), (int)jat(t, 2, 4), (int)jat(t, 3, 4) };
        if (e->tempo[i].bpm <= 0) e->tempo[i].bpm = 120;
        if (e->tempo[i].up <= 0) e->tempo[i].up = 4;
        if (e->tempo[i].low <= 0) e->tempo[i].low = 4;
    }
    const jv *jp = jget(o, "jumps");
    e->njumps = jlen(jp);
    e->jumps = calloc((size_t)e->njumps + 1, sizeof *e->jumps);
    for (int i = 0; i < e->njumps; i++) {
        const jv *j = &jp->v[i];
        jump_t *x = &e->jumps[i];
        x->at = (uint32_t)jnum(j, "at", 0);
        x->end = (uint32_t)jnum(j, "end", x->at);
        x->to = (uint32_t)jnum(j, "to", 0);
        x->chance = (float)jnum(j, "chance", 100);
        const jv *q = jget(j, "quant");
        x->qu = (int)jat(q, 0, 0); x->qm = (int)jat(q, 1, 1);
        const jv *c = jget(j, "if");
        x->ncond = jlen(c);
        x->cond = calloc((size_t)x->ncond + 1, sizeof *x->cond);
        for (int k = 0; k < x->ncond; k++) x->cond[k] = (cond_t){ (int)jat(&c->v[k], 0, -1), (float)jat(&c->v[k], 1, 0), (float)jat(&c->v[k], 2, 0) };
    }
    const jv *ct = jget(o, "ctrl");
    e->nctrl = jlen(ct);
    e->ctrl = calloc((size_t)e->nctrl + 1, sizeof *e->ctrl);
    for (int i = 0; i < e->nctrl; i++) {
        const jv *c = &ct->v[i];
        ctrl_t *x = &e->ctrl[i];
        const char *on = jstr(c, "on"), *pr = jstr(c, "prop");
        x->on = !strcmp(on, "master") ? ON_MASTER : !strcmp(on, "track") ? ON_TRACK : !strcmp(on, "inst") ? ON_INST : ON_PAN;
        x->prop = !strcmp(pr, "vol") ? P_VOL : !strcmp(pr, "pitch") ? P_PITCH : !strcmp(pr, "fade") ? P_FADE : P_POS;
        x->i = (int16_t)jnum(c, "i", -1);
        x->by = (int16_t)jnum(c, "by", -1);
        const jv *pts = jget(c, "pts");
        x->n = jlen(pts);
        x->pts = calloc((size_t)x->n + 1, sizeof *x->pts);
        for (int k = 0; k < x->n; k++) x->pts[k] = (pt_t){ (float)jat(&pts->v[k], 0, 0), (float)jat(&pts->v[k], 1, 0), (float)jat(&pts->v[k], 2, 0) };
    }
    env_read(jget(o, "ahdsr"), &e->env, e->e);
    // an event that never ends by itself: parameter sheets, a looping instrument, or a jump back
    e->persistent = e->nsheets > 0;
    for (int i = 0; i < e->ninst; i++) if (e->insts[i].loop < 0) e->persistent = 1;
    for (int i = 0; i < e->njumps; i++) if (e->jumps[i].to < e->jumps[i].at && !e->jumps[i].ncond) e->persistent = 1;
    e->oneshot = !e->persistent;
}

static char *bank_base(const char *name, char *out, size_t size)
{
    const char *b = strrchr(name, '/');
    b = b ? b + 1 : name;
    snprintf(out, size, "%s", b);
    size_t n = strlen(out);
    if (n > 5 && !strcasecmp(out + n - 5, ".bank")) out[n - 5] = 0;
    return out;
}

int fmp_load_bank(const char *name)
{
    if (!g_open || !name) return -1;
    char base[256];
    bank_base(name, base, sizeof base);
    pthread_mutex_lock(&L);
    for (int i = 0; i < g_nbanks; i++)
        if (!strcasecmp(g_banks[i].name, base)) {
            g_banks[i].loaded = 1;
            pthread_mutex_unlock(&L);
            return i;
        }
    char p[PATH_MAX + 300];
    snprintf(p, sizeof p, "%s/%s.uafm.json", g_dir, base);
    bank_t b = { 0 };
    if (!load_json(p, &b.root, &b.text)) {
        pthread_mutex_unlock(&L);
        logf_("bank %s: no %s (it plays nothing)", base, p);
        return -1;
    }
    b.name = strdup(base);
    b.loaded = 1;
    int bi = g_nbanks;
    snprintf(p, sizeof p, "%s/%s.uafm.pak", g_dir, base);
    b.fd = open(p, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (b.fd >= 0 && fstat(b.fd, &st) == 0) b.paklen = (size_t)st.st_size;
    const jv *media = jget(&b.root, "media");
    b.nmedia = jlen(media);
    b.media = calloc((size_t)b.nmedia + 1, sizeof *b.media);
    for (int i = 0; i < b.nmedia; i++) {
        const jv *m = &media->v[i];
        double off = jnum(m, "off", -1), len = jnum(m, "len", 0);
        b.media[i].samples = (uint32_t)jnum(m, "samples", 0);
        b.media[i].ch = (uint8_t)jnum(m, "ch", 1);
        b.media[i].fd = b.fd;
        if (off >= 0 && b.fd >= 0 && off + len <= (double)b.paklen) { b.media[i].off = (uint32_t)off; b.media[i].len = (uint32_t)len; b.media[i].avail = 1; }
    }
    // paths (a strings bank)
    const jv *paths = jget(&b.root, "paths");
    for (int i = 0; i < jlen(paths); i++) {
        fmp_guid g;
        if (!hexguid(paths->v[i].s ? paths->v[i].s : "", g)) continue;
        hm_put(&g_paths, NULL, paths->k[i], g_npath_of);
        GROW(g_path_of, g_npath_of, g_cpath_of);
        g_path_of[g_npath_of] = paths->k[i];
        hm_put(&g_guid_path, g, NULL, g_npath_of);
        g_npath_of++;
        if (!strncmp(paths->k[i], "bus:/", 5)) {
            int bu = bus_add(g);
            hm_put(&g_bus_paths, NULL, paths->k[i], bu);
            if (!strcmp(paths->k[i], "bus:/")) g_master_bus = bu;
        }
    }
    // buses: their routing and volumes (the same bus in several banks: the last read)
    const jv *buses = jget(&b.root, "buses");
    for (int i = 0; i < jlen(buses); i++) {
        fmp_guid g, o;
        if (!hexguid(buses->k[i], g)) continue;
        int bu = bus_add(g);
        const jv *x = &buses->v[i];
        g_buses[bu].vol = (float)jnum(x, "vol", 0);
        g_buses[bu].out = hexguid(jstr(x, "out"), o) ? bus_add(o) : -1;
        if (!strcmp(jstr(x, "kind"), "master") && g_buses[bu].out < 0 && g_master_bus < 0) g_master_bus = bu;
    }
    // events
    const jv *evs = jget(&b.root, "events");
    b.events = calloc((size_t)jlen(evs) + 1, sizeof *b.events);
    for (int i = 0; i < jlen(evs); i++) {
        fmp_guid g;
        if (!hexguid(evs->k[i], g)) continue;
        int ei = hm_get(&g_event_ids, g, NULL);
        if (ei < 0) {
            GROW(g_events, g_nevents, g_cevents);
            ei = g_nevents++;
            memset(&g_events[ei], 0, sizeof g_events[ei]);
            memcpy(g_events[ei].id, g, 16);
            hm_put(&g_event_ids, g_events[ei].id, NULL, ei);
        }
        event_load(&g_events[ei], &evs->v[i], bi);
        b.events[b.nevents++] = ei;
    }
    // sound table (programmer sounds)
    const jv *snd = jget(&b.root, "sounds");
    b.nkeys = jlen(snd);
    b.keys = calloc((size_t)b.nkeys + 1, sizeof *b.keys);
    b.key_media = calloc((size_t)b.nkeys + 1, sizeof *b.key_media);
    for (int i = 0; i < b.nkeys; i++) {
        b.keys[i] = strtoull(snd->k[i], NULL, 16);
        b.key_media[i] = (int)snd->v[i].num;
    }
    // the tree is read only here (what plays keeps its own copies, and its strings point into b.text, kept): freed,
    // 4 - 5 MB of small blocks for Blasphemous's banks
    js_free(&b.root);
    memset(&b.root, 0, sizeof b.root);
    GROW(g_banks, g_nbanks, g_cbanks);
    g_banks[g_nbanks++] = b;
    pthread_mutex_unlock(&L);
    logf_("bank %s: %d events, %d samples%s", base, b.nevents, b.nmedia, b.fd >= 0 ? "" : " (no sample data)");
    return bi;
}

void fmp_unload_bank(int bank)
{
    if (bank < 0 || bank >= g_nbanks) return;
    push((cmd_t){ Q_UNLOAD, 0, bank, 0, 0, 0 });
}
const char *fmp_bank_name(int bank) { return bank >= 0 && bank < g_nbanks ? g_banks[bank].name : ""; }
int fmp_bank_count_events(int bank) { return bank >= 0 && bank < g_nbanks ? g_banks[bank].nevents : 0; }
int fmp_bank_event(int bank, int i) { return bank >= 0 && bank < g_nbanks && i >= 0 && i < g_banks[bank].nevents ? g_banks[bank].events[i] : -1; }

int fmp_lookup(const char *path, fmp_guid out)
{
    int i = path ? hm_get(&g_paths, NULL, path) : -1;
    if (i < 0) return 0;
    for (uint32_t k = 0; k < g_guid_path.cap; k++)
        if (g_guid_path.v[k] == i && !g_guid_path.s[k] && memcmp(g_guid_path.k[k], g_zero, 16)) { memcpy(out, g_guid_path.k[k], 16); return 1; }
    return 0;
}
int fmp_path(const fmp_guid id, char *buf, int size)
{
    int i = hm_get(&g_guid_path, id, NULL);
    if (i < 0) return 0;
    int n = (int)strlen(g_path_of[i]);
    if (buf && size > 0) snprintf(buf, (size_t)size, "%s", g_path_of[i]);
    return n;
}

int fmp_event(const fmp_guid id) { return hm_get(&g_event_ids, id, NULL); }
void fmp_event_id(int ev, fmp_guid out) { if (ev >= 0 && ev < g_nevents) memcpy(out, g_events[ev].id, 16); }
int fmp_event_params(int ev) { return ev >= 0 && ev < g_nevents ? g_events[ev].nparams : 0; }
const char *fmp_event_param(int ev, int i, float *min, float *max, float *def)
{
    if (ev < 0 || ev >= g_nevents || i < 0 || i >= g_events[ev].nparams) return NULL;
    const param_t *p = &g_events[ev].params[i];
    if (min) *min = p->min;
    if (max) *max = p->max;
    if (def) *def = p->def;
    return p->name;
}
int fmp_event_param_index(int ev, const char *name)
{
    if (ev < 0 || ev >= g_nevents || !name) return -1;
    for (int i = 0; i < g_events[ev].nparams; i++) if (!strcasecmp(g_events[ev].params[i].name, name)) return i;
    return -1;
}
int fmp_event_length_ms(int ev) { return ev >= 0 && ev < g_nevents ? (int)(g_events[ev].len / 48) : 0; }
int fmp_event_oneshot(int ev) { return ev >= 0 && ev < g_nevents ? g_events[ev].oneshot : 1; }
int fmp_event_is_snapshot(int ev) { return ev >= 0 && ev < g_nevents ? g_events[ev].snapshot : 0; }
int fmp_event_instances(int ev)
{
    int n = 0;
    for (int i = 1; i < MAXINST; i++) n += g_inst[i].used && !g_inst[i].released && g_inst[i].ev == ev;
    return n;
}
float fmp_event_user_property(int ev, const char *name, int *found) { (void)ev; (void)name; if (found) *found = 0; return 0; }

int fmp_bus(const char *path) { return path ? hm_get(&g_bus_paths, NULL, path) : -1; }
int fmp_bus_by_id(const fmp_guid id) { return hm_get(&g_bus_ids, id, NULL); }

// ---------------------------------------------------------------------------------------- curves
// a segment's shape (FMOD's curve points have one, -1..1): 0 is a straight line; others bend it
// (a power curve stands in for FMOD's shapes)
static float shaped(float t, float s)
{
    if (s == 0) return t;
    float k = exp2f(-3.0f * s);
    return powf(t, k);
}
static float curve_at(const ctrl_t *c, float x)
{
    if (c->n <= 0) return 0;
    if (x <= c->pts[0].x) return c->pts[0].y;
    for (int i = 1; i < c->n; i++)
        if (x < c->pts[i].x) {
            const pt_t *a = &c->pts[i - 1], *b = &c->pts[i];
            float t = (x - a->x) / (b->x - a->x);
            return a->y + (b->y - a->y) * shaped(t, a->s);
        }
    return c->pts[c->n - 1].y;
}

// an AHDSR's value (dB for a volume) t seconds after the start, or `rel` seconds into the release
// from `from` (rel < 0: not releasing); *done at the release's end
static float env_db(const float *e, double t, double rel, float from, int *done)
{
    float init = e[0], peak = e[1], sus = e[2], a = e[3], h = e[4], d = e[5], r = e[6];
    if (done) *done = 0;
    if (rel >= 0) {
        if (rel >= r) { if (done) *done = 1; return -80; }
        float g0 = db2lin(from), g = g0 * (float)(1.0 - rel / (r > 0 ? r : 1e-6));
        return g > 0 ? 20 * log10f(g) : -80;
    }
    float g;
    if (t < a) g = db2lin(init) + (db2lin(peak) - db2lin(init)) * (float)(t / a);
    else if (t < a + h) g = db2lin(peak);
    else if (t < a + h + d) g = db2lin(peak) + (db2lin(sus) - db2lin(peak)) * (float)((t - a - h) / d);
    else g = db2lin(sus);
    return g > 0 ? 20 * log10f(g) : -80;
}

// ---------------------------------------------------------------------------------------- voices
static const media_t *media_of(int bank, int m)
{
    if (bank < 0 || bank >= g_nbanks || m < 0 || m >= g_banks[bank].nmedia) return NULL;
    const media_t *x = &g_banks[bank].media[m];
    return x->avail ? x : NULL;
}

static int media_get(media_t *m)
{
    if (m->refs++ > 0) return m->p != NULL;
    long pg = sysconf(_SC_PAGESIZE);
    size_t a = m->off - m->off % (size_t)pg;
    m->maplen = m->off + m->len - a;
    m->map = mmap(NULL, m->maplen, PROT_READ, MAP_SHARED, m->fd, (off_t)a);
    if (m->map == MAP_FAILED) { m->map = NULL; m->p = NULL; return 0; }
    m->p = (const uint8_t *)m->map + (m->off - a);
    return 1;
}
static void media_put(media_t *m)
{
    if (m->refs <= 0 || --m->refs > 0) return;
    if (m->map) munmap(m->map, m->maplen);
    m->map = NULL; m->p = NULL;
}

#define VPOOL 8
static struct { const media_t *m; stb_vorbis *vb; uint32_t at; } g_vpool[VPOOL];
static uint32_t g_vpool_clock;
static void vpool_put(const media_t *m, stb_vorbis *vb)
{
    int k = 0;
    for (int i = 0; i < VPOOL; i++) {
        if (!g_vpool[i].vb) { k = i; break; }
        if (g_vpool[i].at < g_vpool[k].at) k = i;
    }
    if (g_vpool[k].vb) { stb_vorbis_close(g_vpool[k].vb); media_put((media_t *)g_vpool[k].m); }
    g_vpool[k].m = m; g_vpool[k].vb = vb; g_vpool[k].at = ++g_vpool_clock;     // (the pool keeps the voice's reference)
}
static stb_vorbis *vpool_get(const media_t *m)
{
    for (int i = 0; i < VPOOL; i++)
        if (g_vpool[i].vb && g_vpool[i].m == m) {
            stb_vorbis *vb = g_vpool[i].vb;
            g_vpool[i].vb = NULL;
            media_put((media_t *)m);            // (the voice has its own reference)
            if (stb_vorbis_seek_start(vb)) return vb;
            stb_vorbis_close(vb);
        }
    return NULL;
}

static void voice_free(voice_t *v)
{
    if (v->vb) vpool_put(v->m, v->vb);
    else if (v->m) media_put((media_t *)v->m);
    if (v->prog >= 0) {
        fmp_cb c = { 0 };
        c.inst = ((uint32_t)g_inst[v->slot].mgen << 16) | (uint32_t)v->slot;
        c.type = FMP_CB_DESTROY_PROGRAMMER_SOUND; c.prog = v->prog; c.prog_sound = v->media_bank << 20 | v->media_ix;
        if (g_inst[v->slot].mask & FMP_CB_DESTROY_PROGRAMMER_SOUND) cb_push(c);
    }
    if (v->slot > 0) g_inst[v->slot].voices--;
    memset(v, 0, sizeof *v);
}

// the gain of a voice (its estimate, for the voice cap)
static voice_t *voice_new(int slot, const media_t *m, int bank, int ix)
{
    int free_i = -1;
    for (int i = 0; i < g_maxv; i++) if (!g_voices[i].used) { free_i = i; break; }
    if (free_i < 0) {
        // the cap: the quietest voice goes
        int q = 0;
        for (int i = 1; i < g_maxv; i++) if (g_voices[i].level < g_voices[q].level) q = i;
        voice_free(&g_voices[q]);
        g_st.dropped++;
        free_i = q;
    }
    voice_t *v = &g_voices[free_i];
    memset(v, 0, sizeof *v);
    if (!media_get((media_t *)m)) { media_put((media_t *)m); g_st.decode_errors++; return NULL; }
    v->used = 1; v->slot = slot; v->m = m; v->media_bank = bank; v->media_ix = ix;
    v->end = m->samples; v->prog = -1; v->rel_t = -1; v->remain = -1; v->level = 1;
    v->step = 65536;
    g_inst[slot].voices++;
    g_st.voices_started++;
    return v;
}

static int dec_seek(voice_t *v, uint32_t to)
{
    v->next = to;
    if (v->vb) return to ? stb_vorbis_seek(v->vb, to) : stb_vorbis_seek_start(v->vb);
    return 1;
}

static int refill(voice_t *v)
{
    int ch2 = v->m->ch > 1;
    v->buf[0][0] = v->buf[0][v->n];
    if (ch2) v->buf[1][0] = v->buf[1][v->n];
    v->p -= v->n;
    int got = 0, empty = 0;
    while (got < DEC_N) {
        if (v->next >= v->end) {
            if (v->loops == 1 || v->stop_loop || empty > 1) break;
            if (v->loops > 1) v->loops--;
            empty++;
            if (!dec_seek(v, 0)) { g_st.decode_errors++; v->loops = 1; break; }
            continue;
        }
        if (!v->vb) {
            int err = 0;
            v->vb = vpool_get(v->m);
            if (!v->vb) v->vb = stb_vorbis_open_memory(v->m->p, (int)v->m->len, &err, NULL);
            if (!v->vb) { g_st.decode_errors++; return 0; }
            if (v->next && !stb_vorbis_seek(v->vb, v->next)) { g_st.decode_errors++; return 0; }
        }
        int want = DEC_N - got;
        if ((uint32_t)want > v->end - v->next) want = (int)(v->end - v->next);
        short *o[2] = { v->buf[0] + 1 + got, v->buf[1] + 1 + got };
        int k = stb_vorbis_get_samples_short(v->vb, ch2 ? 2 : 1, o, want);
        if (k < 0) { g_st.decode_errors++; v->loops = 1; break; }
        if (k == 0) { v->end = v->next; if (v->loops == 1 || v->stop_loop) break; continue; }
        v->next += (uint32_t)k;
        empty = 0;
        got += k;
    }
    v->n = got;
    return got > 0;
}

// adds n frames into the float stereo out, the gains going from v->g to g1 (left, right),
// resampled by linear interpolation; 0 when the sound ended (*used: frames made)
static int voice_mix(voice_t *v, float *out, int n, const float g1[2], int *used)
{
    int stereo = v->m->ch > 1, done = 0;
    float gl = v->g[0], gr = v->g[1], dl = (g1[0] - gl) / n, dr = (g1[1] - gr) / n;
    const float sc = 1.0f / 32768.0f;
    int ended = 0;
    while (done < n) {
        if (v->p >= v->n) { if (!refill(v)) { ended = 1; break; } }
        uint64_t avail = ((((uint64_t)(v->n - v->p)) << 16) - v->frac + v->step - 1) / v->step;
        int run = (uint64_t)(n - done) < avail ? n - done : (int)avail;
        uint32_t p = (uint32_t)v->p, fr = v->frac, stp = v->step;
        float *o = out + 2 * done;
        if (!stereo) {
            const int16_t *b = v->buf[0];
            for (int k = 0; k < run; k++) {
                float a = b[p], s = (a + (b[p + 1] - a) * (fr * (1.0f / 65536))) * sc;
                o[2 * k] += s * gl; o[2 * k + 1] += s * gr;
                gl += dl; gr += dr;
                fr += stp; p += fr >> 16; fr &= 0xffff;
            }
        } else {
            const int16_t *b0 = v->buf[0], *b1 = v->buf[1];
            for (int k = 0; k < run; k++) {
                float t = fr * (1.0f / 65536), a0 = b0[p], a1 = b1[p];
                o[2 * k] += (a0 + (b0[p + 1] - a0) * t) * sc * gl;
                o[2 * k + 1] += (a1 + (b1[p + 1] - a1) * t) * sc * gr;
                gl += dl; gr += dr;
                fr += stp; p += fr >> 16; fr &= 0xffff;
            }
        }
        v->p = (int)p; v->frac = fr;
        done += run;
    }
    v->g[0] = g1[0]; v->g[1] = g1[1];
    *used = done;
    return !ended;
}

// ------------------------------------------------------------------------------------- instances
// controller values of an instance, refreshed each block
typedef struct {
    float master_db, master_pitch, master_pan;
    float track_db[64], track_pan[64];
    float *inst_db, *inst_pitch, *inst_fade;
    int ninst;
} ctlv_t;
static ctlv_t g_ctl[MAXINST];
static int g_prog_seq;
static int g_prog_ii[MAXINST];

static uint32_t inst_id(int slot) { return ((uint32_t)g_inst[slot].mgen << 16) | (uint32_t)slot; }

static void ctl_eval(int slot)
{
    inst_rt *x = &g_inst[slot];
    event_t *e = &g_events[x->ev];
    ctlv_t *c = &g_ctl[slot];
    if (c->ninst < e->ninst) {
        c->inst_db = realloc(c->inst_db, (size_t)e->ninst * sizeof(float));
        c->inst_pitch = realloc(c->inst_pitch, (size_t)e->ninst * sizeof(float));
        c->inst_fade = realloc(c->inst_fade, (size_t)e->ninst * sizeof(float));
        c->ninst = e->ninst;
    }
    c->master_db = c->master_pitch = c->master_pan = 0;
    for (int i = 0; i < 64; i++) c->track_db[i] = c->track_pan[i] = 0;
    for (int i = 0; i < e->ninst; i++) { c->inst_db[i] = c->inst_pitch[i] = 0; c->inst_fade[i] = 1; }
    for (int k = 0; k < e->nctrl; k++) {
        const ctrl_t *t = &e->ctrl[k];
        float in = t->by < 0 ? (float)x->cursor : t->by < e->nparams ? x->pv[t->by] : 0;
        float y = curve_at(t, in);
        if (t->on == ON_PAN) {
            if (t->i < 0) c->master_pan = y; else if (t->i < 64) c->track_pan[t->i] = y;
        } else if (t->on == ON_MASTER) {
            if (t->prop == P_VOL) c->master_db += y; else if (t->prop == P_PITCH) c->master_pitch += y;
        } else if (t->on == ON_TRACK && t->i >= 0 && t->i < 64) {
            if (t->prop == P_VOL) c->track_db[t->i] += y;
        } else if (t->on == ON_INST && t->i >= 0 && t->i < e->ninst) {
            if (t->prop == P_VOL) c->inst_db[t->i] += y;
            else if (t->prop == P_PITCH) c->inst_pitch[t->i] += y;
            else if (t->prop == P_FADE) c->inst_fade[t->i] *= y < 0 ? 0 : y;
        }
    }
}

// a bus chain's gain and pause
static float bus_chain(int b, int *paused)
{
    float g = 1;
    for (int d = 0; b >= 0 && b < g_nbuses && d < 16; d++, b = g_buses[b].out) {
        const bus_t *x = &g_buses[b];
        g *= db2lin(x->vol) * x->game_vol;
        if (x->mute) g = 0;
        if (x->paused && paused) *paused = 1;
    }
    return g;
}

static int pick_kid(inst_t *n)
{
    if (n->nkids <= 0) return -1;
    int k;
    switch (n->mode) {
    case 0: case 3: case 4:             // sequential (global / per instance: one position here)
        k = n->seq % n->nkids;
        n->seq = (n->seq + 1) % n->nkids;
        break;
    default:                            // random (2: shuffle, never the last one twice)
        for (int tries = 0;; tries++) {
            if (n->w && n->wsum > 0) {
                float r = frand() * n->wsum;
                for (k = 0; k < n->nkids - 1 && r >= n->w[k]; k++) r -= n->w[k];
            } else k = (int)(rnd() % (uint32_t)n->nkids);
            if (n->mode != 2 || n->nkids < 2 || k != n->last || tries > 8) break;
        }
    }
    n->last = k;
    return n->kids[k];
}

static void inst_play(int slot, int ii, int top, int bkind, int box, long delay, uint32_t seek, long remain, float rvol, float rpitch, int depth)
{
    inst_rt *x = &g_inst[slot];
    event_t *e = &g_events[x->ev];
    if (ii < 0 || ii >= e->ninst || depth > 8) return;
    inst_t *n = &e->insts[ii];
    for (int k = 0; k < n->ncond; k++) {
        int p = n->cond[k].p;
        float v = p >= 0 && p < e->nparams ? x->pv[p] : 0;
        if (v < n->cond[k].lo - 1e-4f || v > n->cond[k].hi + 1e-4f) return;
    }
    if (n->chance < 100 && frand() * 100 >= n->chance) return;
    if (n->rvol > 0) rvol -= frand() * n->rvol;
    if (n->rpitch > 0) rpitch += (frand() - 0.5f) * n->rpitch;
    if (depth == 0 && bkind != 1 && n->d1 > 0) delay += (long)((n->d0 + (n->d1 - n->d0) * frand()) * g_rate);
    if (n->kind == K_MULTI) {
        inst_play(slot, pick_kid(n), top, bkind, box, delay, seek, remain, rvol, rpitch, depth + 1);
        return;
    }
    if (n->kind == K_PROG) {
        if (x->mask & FMP_CB_CREATE_PROGRAMMER_SOUND) {
            fmp_cb c = { 0 };
            c.inst = inst_id(slot); c.type = FMP_CB_CREATE_PROGRAMMER_SOUND; c.name = n->name; c.prog = ++g_prog_seq;
            g_prog_ii[slot] = ii;
            x->progs++;
            cb_push(c);
        }
        return;
    }
    if (n->kind != K_WAVE) return;
    const media_t *m = media_of(e->bank, n->media);
    if (!m) return;
    if (n->rstart > 0 && !seek) seek = (uint32_t)(frand() * n->rstart / 100 * m->samples);
    if (seek >= m->samples && e->insts[top].loop == 0) return;
    voice_t *v = voice_new(slot, m, e->bank, n->media);
    if (!v) return;
    v->ii = ii; v->top = top; v->bkind = bkind; v->box = box;
    v->delay = delay; v->remain = remain; v->rvol = rvol; v->rpitch = rpitch;
    int lc = e->insts[top].loop;
    v->loops = lc < 0 ? 0 : lc + 1;
    if (seek) { v->next = seek % (m->samples ? m->samples : 1); }
}

static int cond_ok(const inst_rt *x, const event_t *e, const jump_t *j)
{
    for (int k = 0; k < j->ncond; k++) {
        int p = j->cond[k].p;
        float v = p >= 0 && p < e->nparams ? x->pv[p] : 0;
        if (v < j->cond[k].lo || v > j->cond[k].hi) return 0;
    }
    return 1;
}

static void stop_voices(int slot, int bkind, int box, int hard)
{
    for (int i = 0; i < g_maxv; i++) {
        voice_t *v = &g_voices[i];
        if (!v->used || v->slot != slot || (bkind >= 0 && (v->bkind != bkind || v->box != box))) continue;
        if (hard) v->fade = 1; else v->stop_loop = 1;
    }
}

// the beat grid: the tempo marker in force at pos and the beat length (timeline units)
static const tempo_t *tempo_at(const event_t *e, double pos, double *beat)
{
    const tempo_t *t = NULL;
    for (int i = 0; i < e->ntempo; i++) if (e->tempo[i].pos <= pos || !t) t = &e->tempo[i];
    if (t) *beat = TL_RATE * 60.0 / t->bpm * (4.0 / t->low);
    return t;
}

// the timeline's triggers, markers and beats in [c0, c1), the first frame of the span at `off`
static void span(int slot, double c0, double c1, long off)
{
    inst_rt *x = &g_inst[slot];
    event_t *e = &g_events[x->ev];
    double fpu = g_rate / TL_RATE;          // output frames a timeline unit
    if (!x->stopping) {
        for (int b = 0; b < e->nasync; b++) {
            const box_t *bx = &e->async_b[b];
            double s = bx->start, en = bx->start + bx->len;
            if (!x->trig[b] && fmax(c0, s) < fmin(c1, en)) {
                x->trig[b] = 1;
                inst_play(slot, bx->inst, bx->inst, 0, b, off + (long)((fmax(c0, s) - c0) * fpu), 0, -1, 0, 0, 0);
            } else if (x->trig[b] && (c1 >= en || c1 < s)) {
                x->trig[b] = 0;
                stop_voices(slot, 0, b, 0);
            }
        }
        for (int b = 0; b < e->nlocked; b++) {
            const box_t *bx = &e->locked_b[b];
            double s = bx->start, en = bx->start + bx->len;
            uint8_t *t = &x->trig[e->nasync + b];
            if (!*t && fmax(c0, s) < fmin(c1, en)) {
                *t = 1;
                double at = fmax(c0, s);
                inst_play(slot, bx->inst, bx->inst, 1, b, off + (long)((at - c0) * fpu), (uint32_t)(at - s), (long)((en - at) * fpu) + off + (long)((at - c0) * fpu), 0, 0, 0);
            } else if (*t && (c1 >= en || c1 < s)) *t = 0;
        }
    }
    if (x->mask & FMP_CB_TIMELINE_MARKER)
        for (int i = 0; i < e->nmarkers; i++)
            if (e->markers[i].pos >= c0 && e->markers[i].pos < c1 && e->markers[i].name[0]) {
                fmp_cb c = { 0 };
                c.inst = inst_id(slot); c.type = FMP_CB_TIMELINE_MARKER; c.name = e->markers[i].name;
                c.position_ms = (int)(e->markers[i].pos / 48);
                cb_push(c);
            }
    if ((x->mask & FMP_CB_TIMELINE_BEAT) && e->ntempo) {
        double bl;
        const tempo_t *t = tempo_at(e, c0, &bl);
        if (t && bl > 0) {
            long k0 = (long)ceil((c0 - t->pos) / bl), k1 = (long)ceil((c1 - t->pos) / bl);
            for (long k = k0 < 0 ? 0 : k0; k < k1; k++) {
                if (k == x->last_beat) continue;
                x->last_beat = k;
                fmp_cb c = { 0 };
                c.inst = inst_id(slot); c.type = FMP_CB_TIMELINE_BEAT;
                c.bar = (int)(k / t->up) + 1; c.beat = (int)(k % t->up) + 1;
                c.position_ms = (int)((t->pos + k * bl) / 48);
                c.tempo = t->bpm; c.upper = t->up; c.lower = t->low;
                cb_push(c);
            }
        }
    }
}

// the earliest jump in [c0, c1): its time and destination; 0 none
static int next_jump(int slot, double c0, double c1, double *when, double *to)
{
    inst_rt *x = &g_inst[slot];
    event_t *e = &g_events[x->ev];
    int found = 0;
    for (int i = 0; i < e->njumps; i++) {
        const jump_t *j = &e->jumps[i];
        double t;
        if (j->end == j->at) {
            if (!(j->at >= c0 && j->at < c1)) continue;
            t = j->at;
        } else {
            if (c1 <= j->at || c0 >= j->end) continue;
            t = fmax(c0, j->at);
            if (j->qu) {
                double bl;
                const tempo_t *tp = tempo_at(e, t, &bl);
                if (tp && bl > 0) {
                    double grid = bl * (j->qu == 1 ? tp->up : 1) * (j->qm > 0 ? j->qm : 1);
                    t = tp->pos + ceil((t - tp->pos) / grid - 1e-9) * grid;
                }
                if (t >= c1 || t >= j->end) continue;
            }
        }
        if (!cond_ok(x, e, j)) continue;
        if (j->chance < 100 && j->end == j->at && frand() * 100 >= j->chance) continue;
        if (!found || t < *when) { *when = t; *to = j->to; found = 1; }
    }
    return found;
}

static void inst_finish(int slot)
{
    inst_rt *x = &g_inst[slot];
    for (int i = 0; i < g_maxv; i++) if (g_voices[i].used && g_voices[i].slot == slot) voice_free(&g_voices[i]);
    x->active = 0;
    if (x->ev >= 0 && x->ev < g_nevents) g_events[x->ev].live--;
    __atomic_store_n(&x->state, FMP_STOPPED, __ATOMIC_RELEASE);
    if (x->mask & FMP_CB_STOPPED) { fmp_cb c = { 0 }; c.inst = inst_id(slot); c.type = FMP_CB_STOPPED; cb_push(c); }
}

static void inst_start(int slot)
{
    inst_rt *x = &g_inst[slot];
    if (x->active) inst_finish(slot);       // a restart
    event_t *e = &g_events[x->ev];
    // the event's polyphony: the oldest instance goes (steal mode 4, none: this one fails)
    if (e->max > 0 && e->live >= e->max) {
        int old = -1;
        for (int i = 1; i < MAXINST; i++)
            if (g_inst[i].active && g_inst[i].ev == x->ev && (old < 0 || g_inst[i].order < g_inst[old].order)) old = i;
        if (e->steal == 4 || old < 0) {
            __atomic_store_n(&x->state, FMP_STOPPED, __ATOMIC_RELEASE);
            if (x->mask & FMP_CB_START_FAILED) { fmp_cb c = { 0 }; c.inst = inst_id(slot); c.type = FMP_CB_START_FAILED; cb_push(c); }
            return;
        }
        inst_finish(old);
    }
    int nt = e->nasync + e->nlocked;
    for (int s = 0; s < e->nsheets; s++) nt += e->sheets[s].n;
    if (x->ntrig < nt) { x->trig = realloc(x->trig, (size_t)nt); x->ntrig = nt; }
    memset(x->trig, 0, (size_t)x->ntrig);
    x->active = 1; x->stopping = 0; x->cursor = 0; x->t = 0; x->stop_t = 0;
    x->voices = 0; x->progs = 0; x->last_beat = -1;
    x->order = ++g_order;
    e->live++;
    g_st.started++;
    __atomic_store_n(&x->state, FMP_PLAYING, __ATOMIC_RELEASE);
    if (x->mask & FMP_CB_STARTED) { fmp_cb c = { 0 }; c.inst = inst_id(slot); c.type = FMP_CB_STARTED; cb_push(c); }
}

// one block of an instance: parameters, controllers, the timeline, its end
static void inst_block(int slot, int n)
{
    inst_rt *x = &g_inst[slot];
    event_t *e = &g_events[x->ev];
    int bpaused = 0;
    bus_chain(e->bus, &bpaused);
    if (x->paused || bpaused || g_all_paused) return;
    double dt = (double)n / g_rate;
    for (int i = 0; i < e->nparams; i++) {              // parameters seek to the game's value
        float d = x->pt[i] - x->pv[i], sp = d > 0 ? e->params[i].up : e->params[i].down;
        if (sp <= 0 || fabsf(d) <= sp * (float)dt) x->pv[i] = x->pt[i];
        else x->pv[i] += (d > 0 ? 1 : -1) * sp * (float)dt;
    }
    double du = n * TL_RATE / g_rate, c0 = x->cursor, c1 = c0 + du;
    int has_tl = e->len > 0 || e->nmarkers > 0;
    if (!has_tl) c1 = c0;
    else {
        double when, to;
        if (next_jump(slot, c0, c1, &when, &to)) {
            span(slot, c0, when, 0);
            long done = (long)((when - c0) * g_rate / TL_RATE);
            // locked instruments restart from the new position; asynchronous ones play on
            for (int b = 0; b < e->nlocked; b++) { x->trig[e->nasync + b] = 0; stop_voices(slot, 1, b, 1); }
            for (int b = 0; b < e->nasync; b++) {
                const box_t *bx = &e->async_b[b];
                if (x->trig[b] && !(to >= bx->start && to < bx->start + bx->len)) { x->trig[b] = 0; stop_voices(slot, 0, b, 0); }
            }
            x->last_beat = -1;
            double rest = c1 - when;
            c0 = to; c1 = to + rest;
            span(slot, c0, c1, done);
        } else span(slot, c0, c1, 0);
        x->cursor = e->len && c1 > e->len && !e->njumps ? (double)e->len : c1;
    }
    // parameter sheets
    if (!x->stopping) {
        int o = e->nasync + e->nlocked;
        for (int s = 0; s < e->nsheets; s++) {
            const sheet_t *sh = &e->sheets[s];
            float v = sh->param >= 0 && sh->param < e->nparams ? x->pv[sh->param] : 0, mx = sh->param >= 0 && sh->param < e->nparams ? e->params[sh->param].max : 0;
            for (int b = 0; b < sh->n; b++, o++) {
                const box_t *bx = &sh->b[b];
                int in = v >= bx->start && (v < bx->start + bx->len || (v == mx && v <= bx->start + bx->len));
                if (in && !x->trig[o]) { x->trig[o] = 1; inst_play(slot, bx->inst, bx->inst, 2, o, 0, 0, -1, 0, 0, 0); }
                else if (!in && x->trig[o]) { x->trig[o] = 0; stop_voices(slot, 2, o, 0); }
            }
        }
    }
    x->t += dt;
    __atomic_store_n(&x->pos_ms, (int)(x->cursor / 48), __ATOMIC_RELAXED);
    if (x->stopping) {
        x->stop_t += dt;
        if (!e->env || x->stop_t >= e->e[6]) { stop_voices(slot, -1, 0, 1); x->stopping = 2; }
    }
    ctl_eval(slot);
}

// a fade-out to play when stopped with ALLOWFADEOUT: the event's AHDSR release, or its instruments'
static int e_release(int slot)
{
    const event_t *e = &g_events[g_inst[slot].ev];
    if (e->env && e->e[6] > 0) return 1;
    for (int i = 0; i < g_maxv; i++) {
        voice_t *v = &g_voices[i];
        if (v->used && v->slot == slot && e->insts[v->top].env && e->insts[v->top].e[6] > 0) return 1;
    }
    return 0;
}

static void inst_stop(int slot, int immediate)
{
    inst_rt *x = &g_inst[slot];
    if (!x->active) return;
    if (immediate || !e_release(slot)) { stop_voices(slot, -1, 0, 1); x->stopping = 2; }
    else if (!x->stopping) { x->stopping = 1; x->stop_t = 0; }
    __atomic_store_n(&x->state, FMP_STOPPING, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------------------- the mix
// a voice's gains for the end of this block (left, right) and its resampling step; 0: it is silent
static void voice_gains(voice_t *v, float g[2])
{
    inst_rt *x = &g_inst[v->slot];
    const event_t *e = &g_events[x->ev];
    const ctlv_t *c = &g_ctl[v->slot];
    const inst_t *top = &e->insts[v->top], *leaf = &e->insts[v->ii];
    int track = top->track;
    float db = e->vol + e->in_vol + c->master_db + top->vol + v->rvol + c->inst_db[v->top];
    if (v->ii != v->top) db += leaf->vol + c->inst_db[v->ii];
    if (track >= 0 && track < e->ntracks) db += e->track_vol[track] + (track < 64 ? c->track_db[track] : 0);
    if (e->env) {
        int done;
        db += env_db(e->e, x->t, x->stopping ? x->stop_t : -1, 0, &done);
    }
    if (top->env) {
        int done = 0;
        db += env_db(top->e, v->t, v->rel_t, 0, &done);
        if (done) v->fade = 1;
    }
    float fade = c->inst_fade[v->top] * (v->ii != v->top ? c->inst_fade[v->ii] : 1);
    float gain = db2lin(db) * fade * x->vol * bus_chain(e->bus, NULL) * g_master_vol;
    float pos = (track >= 0 && track < 64 ? c->track_pan[track] : 0) + c->master_pan;
    if (pos < -100) pos = -100;
    if (pos > 100) pos = 100;
    if (v->m->ch > 1) {
        g[0] = gain * (pos > 0 ? 1 - pos / 100 : 1);
        g[1] = gain * (pos < 0 ? 1 + pos / 100 : 1);
    } else {
        float a = (pos + 100) / 400 * 3.14159265f;
        g[0] = gain * cosf(a);
        g[1] = gain * sinf(a);
    }
    if (v->fade > 0) g[0] = g[1] = 0;
    v->level = gain;
    float st = e->pitch + c->master_pitch + top->pitch + v->rpitch + c->inst_pitch[v->top];
    if (v->ii != v->top) st += leaf->pitch + c->inst_pitch[v->ii];
    double ratio = exp2((double)st / 12) * x->pitch * 48000.0 / g_rate;
    if (ratio > 8) ratio = 8;
    v->step = (uint32_t)(ratio * 65536 + 0.5);
    if (!v->step) v->step = 1;
}

// a peak limiter on the player's mix: the gain drops at once to keep peaks at 0.98, recovers slowly

static float g_lim_g = 1;
static int g_lim_on = 1;
static void limiter(float *x, int frames)
{
    for (int i = 0; i < frames; i++) {
        float l = x[2 * i], r = x[2 * i + 1], pk = fmaxf(fabsf(l), fabsf(r));
        float want = pk > 0.98f ? 0.98f / pk : 1;
        if (want < g_lim_g) g_lim_g = want;
        else g_lim_g += (1 - g_lim_g) * 0.0003f;
        x[2 * i] = l * g_lim_g;
        x[2 * i + 1] = r * g_lim_g;
    }
}

static void drain_locked(void);

static float *g_acc; static int g_acc_cap;
static int g_busy;           // instances active or voices playing (else a block has nothing to do)
static void mix_block(float *out, int n)
{
    drain_locked();
    if (!g_busy) {
        for (int s = 1; s < MAXINST && !g_busy; s++) g_busy = g_inst[s].active;
        if (!g_busy) return;
    }
    for (int s = 1; s < MAXINST; s++) if (g_inst[s].active) inst_block(s, n);
    if (g_acc_cap < n) { g_acc = realloc(g_acc, (size_t)n * 2 * sizeof(float)); g_acc_cap = n; }
    memset(g_acc, 0, (size_t)n * 2 * sizeof(float));
    int live = 0;
    for (int i = 0; i < g_maxv; i++) {
        voice_t *v = &g_voices[i];
        if (!v->used) continue;
        inst_rt *x = &g_inst[v->slot];
        const event_t *e = &g_events[x->ev];
        int bp = 0;
        bus_chain(e->bus, &bp);
        if (x->paused || bp || g_all_paused) continue;
        live++;
        if (v->delay >= n) { v->delay -= n; continue; }
        int at = (int)v->delay, len = n - at;
        v->delay = 0;
        if (v->remain >= 0) {
            if (v->remain <= at) { voice_free(v); continue; }
            if (v->remain - at < len) len = (int)(v->remain - at);
            v->remain -= at + len;
        }
        float g[2];
        int fading = v->fade > 0;
        voice_gains(v, g);
        if (!v->g[0] && !v->g[1] && !fading && v->t == 0) { v->g[0] = g[0]; v->g[1] = g[1]; }   // no ramp in from silence at the start
        int used = 0, alive;
        if (g[0] + g[1] < 1e-3f && v->g[0] + v->g[1] < 1e-3f && !fading) {
            // inaudible (below -60 dB): FMOD virtualizes such voices; only the position moves on
            if (!v->virt) { v->virt = 1; v->vpos = (double)v->next - (v->n - v->p) - v->frac / 65536.0; v->n = v->p = 0; v->frac = 0; }
            v->vpos += (double)len * v->step / 65536.0;
            alive = 1;
            while (alive && v->vpos >= v->end) {
                if (v->loops == 1 || v->stop_loop) alive = 0;
                else { if (v->loops > 1) v->loops--; v->vpos -= v->end; }
            }
            used = len;
            v->g[0] = g[0]; v->g[1] = g[1];
        } else {
            if (v->virt) {      // audible again: the decoder goes to where the voice is
                v->virt = 0;
                v->n = v->p = 0; v->frac = 0;
                v->buf[0][0] = v->buf[1][0] = 0;
                if (!dec_seek(v, (uint32_t)v->vpos)) { voice_free(v); continue; }
            }
            alive = voice_mix(v, g_acc + 2 * at, len, g, &used);
        }
        v->t += (double)used / g_rate;
        if (v->rel_t >= 0) v->rel_t += (double)used / g_rate;
        if (!alive || fading || v->remain == 0) voice_free(v);
    }
    if (live > g_st.voices_peak) g_st.voices_peak = live;
    g_st.voices = live;
    int any = live;
    // instances that ended
    for (int s = 1; s < MAXINST; s++) {
        inst_rt *x = &g_inst[s];
        if (!x->active) continue;
        const event_t *e = &g_events[x->ev];
        int over = x->stopping == 2 || (!e->persistent && x->cursor >= e->len && !e->njumps) || (!e->persistent && e->len == 0);
        if (x->stopping == 2 && x->voices == 0) inst_finish(s);
        else if (x->stopping != 1 && over && x->voices == 0 && x->progs == 0 && !x->stopping) inst_finish(s);
        any |= x->active;
    }
    for (int i = 0; i < g_maxv && !any; i++) any = g_voices[i].used;
    g_busy = any;
    for (int i = 0; i < 2 * n; i++) out[i] += g_acc[i];
}

static void mix_locked(float *out, int frames)
{
    double t0 = cpu_now();
    if (g_lim_on) {
        // the limiter works on the player's own mix only
        float *tmp = alloca((size_t)frames * 2 * sizeof(float));
        memset(tmp, 0, (size_t)frames * 2 * sizeof(float));
        for (int d = 0; d < frames; d += BLK) mix_block(tmp + 2 * d, frames - d < BLK ? frames - d : BLK);
        limiter(tmp, frames);
        for (int i = 0; i < 2 * frames; i++) out[i] += tmp[i];
    } else
        for (int d = 0; d < frames; d += BLK) mix_block(out + 2 * d, frames - d < BLK ? frames - d : BLK);
    g_st.mix_cpu_s += cpu_now() - t0;
    g_st.mixed_s += (double)frames / g_rate;
}

void fmp_mix(float *buf, int frames, int rate, int channels)
{
    if (!g_open || frames <= 0) return;
    g_last_mix = wall_now();
    pthread_mutex_lock(&L);
    if (rate > 0) g_rate = rate;
    if (channels == 2) mix_locked(buf, frames);
    else {
        float *tmp = calloc((size_t)frames * 2, sizeof(float));
        mix_locked(tmp, frames);
        for (int i = 0; i < frames; i++)
            for (int c = 0; c < channels; c++) buf[i * channels + c] += c < 2 ? tmp[2 * i + c] : 0;
        free(tmp);
    }
    pthread_mutex_unlock(&L);
}

// ------------------------------------------------------------------------------------- the queue
static void run_cmd(const cmd_t *c)
{
    inst_rt *x = c->slot > 0 && c->slot < MAXINST ? &g_inst[c->slot] : NULL;
    switch (c->op) {
    case Q_START:
        x->mgen = c->gen; x->ev = c->i;
        x->vol = 1; x->pitch = 1;
        for (int i = 0; i < MAXP; i++) x->pv[i] = x->pt[i];
        inst_start(c->slot);
        __atomic_store_n(&x->pending_start, 0, __ATOMIC_RELEASE);
        break;
    case Q_STOP: if (x && x->mgen == c->gen) inst_stop(c->slot, c->i); break;
    case Q_PARAM:
        if (x && c->i >= 0 && c->i < MAXP) { x->pt[c->i] = c->f; if (!x->active || c->j) x->pv[c->i] = c->f; }
        break;
    case Q_VOL: if (x) x->vol = c->f; break;
    case Q_PITCH: if (x) x->pitch = c->f; break;
    case Q_PAUSE: if (x) x->paused = c->i; break;
    case Q_POS: if (x && x->active) { x->cursor = c->i * 48.0; for (int b = 0; b < g_events[x->ev].nlocked; b++) { x->trig[g_events[x->ev].nasync + b] = 0; stop_voices(c->slot, 1, b, 1); } } break;
    case Q_PROG:
        if (x && x->mgen == c->gen) {
            if (x->progs > 0) x->progs--;
            int b = c->j >= 0 ? c->j >> 20 : -1, m = c->j & 0xfffff;
            const media_t *md = media_of(b, m);
            if (md && x->active) {
                const event_t *e = &g_events[x->ev];
                voice_t *v = voice_new(c->slot, md, b, m);
                if (v) v->ii = v->top = g_prog_ii[c->slot] < e->ninst ? g_prog_ii[c->slot] : 0;
                if (v) { v->bkind = 3; v->box = -1; v->loops = 1; v->prog = c->i; }
            }
        }
        break;
    case Q_BUSVOL: if (c->i >= 0 && c->i < g_nbuses) g_buses[c->i].game_vol = c->f; break;
    case Q_BUSPAUSE: if (c->i >= 0 && c->i < g_nbuses) g_buses[c->i].paused = c->j; break;
    case Q_BUSMUTE: if (c->i >= 0 && c->i < g_nbuses) g_buses[c->i].mute = c->j; break;
    case Q_BUSSTOP:
        for (int s = 1; s < MAXINST; s++) {
            if (!g_inst[s].active) continue;
            for (int b = g_events[g_inst[s].ev].bus, d = 0; b >= 0 && d < 16; b = g_buses[b].out, d++)
                if (b == c->i) { inst_stop(s, c->j); break; }
        }
        break;
    case Q_MASTER: g_master_vol = c->f; break;
    case Q_PAUSEALL: g_all_paused = c->i; break;
    case Q_UNLOAD:
        for (int s = 1; s < MAXINST; s++) if (g_inst[s].active && g_events[g_inst[s].ev].bank == c->i) inst_stop(s, 1);
        if (c->i >= 0 && c->i < g_nbanks) g_banks[c->i].loaded = 0;
        break;
    }
}

static void drain_locked(void)
{
    static cmd_t q[MAXCMD];
    pthread_mutex_lock(&QL);
    int n = g_qn;
    memcpy(q, g_q, (size_t)n * sizeof *q);
    g_qn = 0;
    pthread_mutex_unlock(&QL);
    for (int i = 0; i < n; i++) run_cmd(&q[i]);
}

// ------------------------------------------------------------------------------- the game's side
static int slot_of_id(uint32_t id)
{
    int s = (int)(id & 0xffff);
    if (s <= 0 || s >= MAXINST || !g_inst[s].used || g_inst[s].gen != (id >> 16)) return -1;
    return s;
}

uint32_t fmp_create(int ev)
{
    if (ev < 0 || ev >= g_nevents) return 0;
    for (int s = 1; s < MAXINST; s++) {
        inst_rt *x = &g_inst[s];
        // free: not used by the game, and the mixer is done with it
        if (x->used || x->active || x->pending_start || __atomic_load_n(&x->state, __ATOMIC_ACQUIRE) != FMP_STOPPED) continue;
        x->gen = (x->gen + 1) & 0xffff;
        if (!x->gen) x->gen = 1;
        x->used = 1; x->released = 0; x->created_sent = 0; x->ev = ev; x->mask = 0;
        x->gvol = 1; x->gpitch = 1; x->gpaused = 0;
        const event_t *e = &g_events[ev];
        for (int i = 0; i < MAXP; i++) x->gp[i] = i < e->nparams ? e->params[i].def : 0;
        for (int i = 0; i < e->nparams && i < MAXP; i++) push((cmd_t){ Q_PARAM, s, i, 1, x->gen, x->gp[i] });
        return ((uint32_t)x->gen << 16) | (uint32_t)s;
    }
    return 0;
}
int fmp_valid(uint32_t id) { return slot_of_id(id) > 0; }
int fmp_inst_event(uint32_t id) { int s = slot_of_id(id); return s > 0 ? g_inst[s].ev : -1; }
void fmp_start(uint32_t id)
{
    int s = slot_of_id(id);
    if (s <= 0) return;
    g_inst[s].pending_start = 1;
    push((cmd_t){ Q_VOL, s, 0, 0, g_inst[s].gen, g_inst[s].gvol });
    push((cmd_t){ Q_START, s, g_inst[s].ev, 0, g_inst[s].gen, 0 });
    push((cmd_t){ Q_VOL, s, 0, 0, g_inst[s].gen, g_inst[s].gvol });
    push((cmd_t){ Q_PITCH, s, 0, 0, g_inst[s].gen, g_inst[s].gpitch });
    push((cmd_t){ Q_PAUSE, s, g_inst[s].gpaused, 0, g_inst[s].gen, 0 });
}
void fmp_stop(uint32_t id, int immediate) { int s = slot_of_id(id); if (s > 0) push((cmd_t){ Q_STOP, s, immediate, 0, g_inst[s].gen, 0 }); }
void fmp_release(uint32_t id) { int s = slot_of_id(id); if (s > 0) g_inst[s].released = 1; }
int fmp_state(uint32_t id)
{
    int s = slot_of_id(id);
    if (s <= 0) return FMP_STOPPED;
    if (g_inst[s].pending_start) return FMP_STARTING;
    return __atomic_load_n(&g_inst[s].state, __ATOMIC_ACQUIRE);
}
void fmp_set_param(uint32_t id, int i, float v)
{
    int s = slot_of_id(id);
    if (s <= 0 || i < 0 || i >= MAXP) return;
    const event_t *e = &g_events[g_inst[s].ev];
    if (i < e->nparams) { if (v < e->params[i].min) v = e->params[i].min; if (v > e->params[i].max) v = e->params[i].max; }
    g_inst[s].gp[i] = v;
    push((cmd_t){ Q_PARAM, s, i, 0, g_inst[s].gen, v });
}
float fmp_get_param(uint32_t id, int i) { int s = slot_of_id(id); return s > 0 && i >= 0 && i < MAXP ? g_inst[s].gp[i] : 0; }
void fmp_set_volume(uint32_t id, float v) { int s = slot_of_id(id); if (s > 0) { g_inst[s].gvol = v; push((cmd_t){ Q_VOL, s, 0, 0, g_inst[s].gen, v }); } }
float fmp_get_volume(uint32_t id) { int s = slot_of_id(id); return s > 0 ? g_inst[s].gvol : 0; }
void fmp_set_pitch(uint32_t id, float v) { int s = slot_of_id(id); if (s > 0) { g_inst[s].gpitch = v; push((cmd_t){ Q_PITCH, s, 0, 0, g_inst[s].gen, v }); } }
float fmp_get_pitch(uint32_t id) { int s = slot_of_id(id); return s > 0 ? g_inst[s].gpitch : 0; }
void fmp_set_paused(uint32_t id, int p) { int s = slot_of_id(id); if (s > 0) { g_inst[s].gpaused = p; push((cmd_t){ Q_PAUSE, s, p, 0, g_inst[s].gen, 0 }); } }
int fmp_get_paused(uint32_t id) { int s = slot_of_id(id); return s > 0 ? g_inst[s].gpaused : 0; }
int fmp_timeline_ms(uint32_t id) { int s = slot_of_id(id); return s > 0 ? __atomic_load_n(&g_inst[s].pos_ms, __ATOMIC_RELAXED) : 0; }
void fmp_set_timeline_ms(uint32_t id, int ms) { int s = slot_of_id(id); if (s > 0) push((cmd_t){ Q_POS, s, ms, 0, g_inst[s].gen, 0 }); }
void fmp_set_callback_mask(uint32_t id, uint32_t mask) { int s = slot_of_id(id); if (s > 0) g_inst[s].mask = mask; }
void fmp_programmer_sound(uint32_t id, int prog, int sound) { int s = slot_of_id(id); if (s > 0) push((cmd_t){ Q_PROG, s, prog, sound, g_inst[s].gen, 0 }); }

void fmp_bus_set_volume(int b, float v) { if (b >= 0 && b < g_nbuses) { g_buses[b].game_vol = v; push((cmd_t){ Q_BUSVOL, 0, b, 0, 0, v }); } }
float fmp_bus_get_volume(int b) { return b >= 0 && b < g_nbuses ? g_buses[b].game_vol : 1; }
void fmp_bus_set_paused(int b, int p) { push((cmd_t){ Q_BUSPAUSE, 0, b, p, 0, 0 }); }
int fmp_bus_get_paused(int b) { return b >= 0 && b < g_nbuses ? g_buses[b].paused : 0; }
void fmp_bus_set_mute(int b, int m) { push((cmd_t){ Q_BUSMUTE, 0, b, m, 0, 0 }); }
int fmp_bus_get_mute(int b) { return b >= 0 && b < g_nbuses ? g_buses[b].mute : 0; }
void fmp_bus_stop_all(int b, int immediate) { push((cmd_t){ Q_BUSSTOP, 0, b, immediate, 0, 0 }); }
void fmp_master_volume(float v) { push((cmd_t){ Q_MASTER, 0, 0, 0, 0, v }); }
float fmp_get_master_volume(void) { return g_master_vol; }
void fmp_pause_all(int p) { push((cmd_t){ Q_PAUSEALL, 0, p, 0, 0, 0 }); }

// sound tables: Jenkins' lookup3 hashlittle2 of the key (public domain), as the banks' tables keep them
#define ROT(x, k) (((x) << (k)) | ((x) >> (32 - (k))))
static uint64_t key_hash(const char *key)
{
    const uint8_t *k = (const uint8_t *)key;
    uint32_t len = (uint32_t)strlen(key), a, b, c;
    a = b = c = 0xdeadbeef + len;
    while (len > 12) {
        a += k[0] | k[1] << 8 | k[2] << 16 | (uint32_t)k[3] << 24;
        b += k[4] | k[5] << 8 | k[6] << 16 | (uint32_t)k[7] << 24;
        c += k[8] | k[9] << 8 | k[10] << 16 | (uint32_t)k[11] << 24;
        a -= c; a ^= ROT(c, 4); c += b; b -= a; b ^= ROT(a, 6); a += c; c -= b; c ^= ROT(b, 8); b += a;
        a -= c; a ^= ROT(c, 16); c += b; b -= a; b ^= ROT(a, 19); a += c; c -= b; c ^= ROT(b, 4); b += a;
        len -= 12; k += 12;
    }
    switch (len) {
    case 12: c += (uint32_t)k[11] << 24; /* fallthrough */
    case 11: c += (uint32_t)k[10] << 16; /* fallthrough */
    case 10: c += (uint32_t)k[9] << 8; /* fallthrough */
    case 9: c += k[8]; /* fallthrough */
    case 8: b += (uint32_t)k[7] << 24; /* fallthrough */
    case 7: b += (uint32_t)k[6] << 16; /* fallthrough */
    case 6: b += (uint32_t)k[5] << 8; /* fallthrough */
    case 5: b += k[4]; /* fallthrough */
    case 4: a += (uint32_t)k[3] << 24; /* fallthrough */
    case 3: a += (uint32_t)k[2] << 16; /* fallthrough */
    case 2: a += (uint32_t)k[1] << 8; /* fallthrough */
    case 1: a += k[0]; break;
    case 0: return (uint64_t)b << 32 | c;
    }
    c ^= b; c -= ROT(b, 14); a ^= c; a -= ROT(c, 11); b ^= a; b -= ROT(a, 25);
    c ^= b; c -= ROT(b, 16); a ^= c; a -= ROT(c, 4); b ^= a; b -= ROT(a, 14);
    c ^= b; c -= ROT(b, 24);
    return (uint64_t)b << 32 | c;
}
int fmp_sound(const char *key)
{
    if (!key) return -1;
    uint64_t h = key_hash(key);
    for (int b = g_nbanks - 1; b >= 0; b--) {
        const bank_t *k = &g_banks[b];
        if (!k->loaded || !k->nkeys) continue;
        int lo = 0, hi = k->nkeys - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (k->keys[mid] == h) return b << 20 | k->key_media[mid];
            if (k->keys[mid] < h) lo = mid + 1; else hi = mid - 1;
        }
    }
    return -1;
}
int fmp_sound_length_ms(int sound)
{
    int b = sound >> 20, m = sound & 0xfffff;
    if (b < 0 || b >= g_nbanks || m >= g_banks[b].nmedia) return 0;
    return (int)((uint64_t)g_banks[b].media[m].samples * 1000 / 48000);
}
int fmp_sound_channels(int sound)
{
    int b = sound >> 20, m = sound & 0xfffff;
    return b >= 0 && b < g_nbanks && m < g_banks[b].nmedia ? g_banks[b].media[m].ch : 0;
}

static fmp_cb g_poll[MAXCB]; static int g_npoll, g_ipoll;
void fmp_update(void)
{
    if (!g_open) return;
    double now = wall_now();
    // UA_FMOD_STATS=<s> (60; 0 off): a log line with the player's work every <s> seconds
    static double stats_at, stats_every = -1;
    static fmp_stats prev;
    if (stats_every < 0) { const char *e = getenv("UA_FMOD_STATS"); stats_every = e ? atof(e) : 60; }
    if (stats_every > 0) {
        if (stats_at == 0) stats_at = now;
        else if (now - stats_at >= stats_every) {
            fmp_stats c;
            fmp_get_stats(&c);
            double wall = now - stats_at;
            logf_("player, last %.0f s: %lu events started, %d playing, %lu voices started, %d now (peak %d, %lu dropped at the cap), mix %.2f %% of a core, calls %.2f %%, %.1f s mixed at %d Hz%s",
                  wall, c.started - prev.started, c.instances, c.voices_started - prev.voices_started, c.voices, c.voices_peak, c.dropped - prev.dropped,
                  100 * (c.mix_cpu_s - prev.mix_cpu_s) / wall, 100 * (c.call_cpu_s - prev.call_cpu_s) / wall, c.mixed_s - prev.mixed_s, g_rate,
                  c.decode_errors > prev.decode_errors ? ", decode errors" : "");
            prev = c;
            g_st.voices_peak = 0;
            stats_at = now;
        }
    }
    // no output reads the mix (none open, a test, a stall): advance it silently, so events end
    if (g_last_mix == 0 || now - g_last_mix > 0.5) {
        static double last;
        if (last == 0) last = now;
        int frames = (int)((now - last) * g_rate);
        if (frames > g_rate) frames = g_rate;
        if (frames >= BLK) {
            float *tmp = calloc((size_t)frames * 2, sizeof(float));
            pthread_mutex_lock(&L);
            mix_locked(tmp, frames);
            pthread_mutex_unlock(&L);
            free(tmp);
            last = now;
        }
    }
    pthread_mutex_lock(&QL);
    memcpy(g_poll, g_cb, (size_t)g_cbn * sizeof *g_cb);
    g_npoll = g_cbn; g_ipoll = 0;
    g_cbn = 0;
    pthread_mutex_unlock(&QL);
    // released instances that ended give their slot back (their DESTROYED callback)
    for (int s = 1; s < MAXINST; s++) {
        inst_rt *x = &g_inst[s];
        if (!x->used) continue;
        if ((x->mask & FMP_CB_CREATED) && !x->created_sent && g_npoll < MAXCB) {
            x->created_sent = 1;
            g_poll[g_npoll++] = (fmp_cb){ .inst = ((uint32_t)x->gen << 16) | (uint32_t)s, .type = FMP_CB_CREATED };
        }
        if (x->released && !x->pending_start && __atomic_load_n(&x->state, __ATOMIC_ACQUIRE) == FMP_STOPPED && !x->active) {
            if ((x->mask & FMP_CB_DESTROYED) && g_npoll < MAXCB)
                g_poll[g_npoll++] = (fmp_cb){ .inst = ((uint32_t)x->gen << 16) | (uint32_t)s, .type = FMP_CB_DESTROYED };
            x->used = 0;
        }
    }
}
int fmp_poll(fmp_cb *out)
{
    if (g_ipoll >= g_npoll) return 0;
    *out = g_poll[g_ipoll++];
    return 1;
}

int fmp_is_open(void) { return g_open; }
int fmp_open(const char *dir)
{
    fmp_close();
    if (!dir || !*dir) return 0;
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    const char *v = getenv("UA_FMOD_VOICES");
    g_maxv = v && atoi(v) > 0 ? atoi(v) : 48;
    if (g_maxv > 256) g_maxv = 256;
    g_voices = calloc((size_t)g_maxv, sizeof *g_voices);
    const char *l = getenv("UA_FMOD_LIMITER");
    g_lim_on = !(l && *l == '0');
    const char *r = getenv("UA_AUDIO_RATE");
    if (r && atoi(r) >= 8000) g_rate = atoi(r);
    for (int s = 0; s < MAXINST; s++) g_inst[s].state = FMP_STOPPED;
    g_open = 1;
    logf_("player on %s (%d voices)", g_dir, g_maxv);
    return 1;
}
void fmp_close(void)
{
    if (!g_open) return;
    pthread_mutex_lock(&L);
    for (int i = 0; i < g_maxv; i++) if (g_voices[i].used) voice_free(&g_voices[i]);
    free(g_voices); g_voices = NULL;
    g_open = 0;
    pthread_mutex_unlock(&L);
}
void fmp_get_stats(fmp_stats *s)
{
    *s = g_st;
    int n = 0;
    for (int i = 1; i < MAXINST; i++) n += g_inst[i].active;
    s->instances = n;
}
