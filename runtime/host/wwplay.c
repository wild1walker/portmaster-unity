// ua_host's own Wwise player (docs/plugins.md, "Wwise", "The player"): the sound backend of the
// built-in Wwise stand-in (wwise.c), selected by UA_WWISE_DATA=<dir>. pmu's setup writes, per bank,
// <Bank>.uaww.json (the bank's hierarchy, events, buses, RTPC curves and default game syncs) and
// <Bank>.uaww.pak (its media: standard Ogg Vorbis, Wwise's IMA ADPCM blocks, 16-bit PCM), whose
// format is pmu's (setup-rs, "wwise"); nothing of Audiokinetic's code or data is in here.
//
// What it plays (step 1; the format: docs/wwise-player.md): events' actions (play, stop, pause and
// resume, their _all forms with exceptions, on one game object or all, with delays and fades; states,
// switches, RTPCs, bus and node volumes); random (weights, avoid-repeat), sequence, switch (a game
// object's switch or a state) and layer containers, step or continuous (a delay transition kept);
// a voice's volume, pitch and output bus from its whole chain of parents (actor-mixers included),
// random ranges, states, RTPC curves; loops (the media's loop points), sound delays, playback
// limits (the oldest instance goes); bus volumes, their states and RTPCs (the game's sliders);
// interactive music: a music switch's decision tree on states and switches picks a playlist (an
// item tree of sequences and random groups, step or continuous, with loops) or a segment; a
// segment's tracks play their clips (trimmed, looped; random / sequence / switch subtracks); the
// next segment's entry cue meets the current one's exit cue; a change of the switch's choice takes
// effect at the exit cue, or at once when the transition rule syncs immediately or to a grid, bar
// or beat (not followed), with the rule's fades; the end-of-event callbacks (host->event_done).
// Not: effects, HDR, 3D positioning and panning, filters, transition segments, beat-synced
// transitions, crossfades between a continuous container's children.
//
// The mixer: voices are decoded in blocks of 512 16-bit frames (IMA ADPCM by a table, PCM; Ogg
// Vorbis by stb_vorbis), resampled to the output's rate by linear interpolation (pitch included) and
// added with a gain ramp per block into 32-bit integer sums (mono sources four frames at a time with
// NEON), which become the float stereo mix, through a brick-wall peak limiter (UA_WWISE_LIMITER=0:
// off); UA_WWISE_VOICES (48) voices at most: a new voice past that replaces the quietest (the
// oldest of equals), or is dropped if it is the quietest. The host
// calls mix() on its audio output's thread (the game's own OpenSL ES player: one device); the game's
// calls are queued for it, so they never wait for a mix (only LoadBank takes the player's lock). UA_WWISE_STATS=<s> (60; 0 = off): a log line
// with events, voices and the player's CPU every <s> seconds; UA_WWISE_CAPTURE=<file.wav>: the
// player's own mix written to a file as well (UA_WWISE_CAPTURE_SECS, 120 s).
#define _GNU_SOURCE
#include "wwplay.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
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

#define DEC_N 512            // frames decoded per refill
#define MAXCHAIN 16
#define MAXSEQ 128
#define MAXMUS 16
#define MAXPEND 64
#define MINFADE_MS 5.0f

static const ua_wwise_host *H;
static pthread_mutex_t L = PTHREAD_MUTEX_INITIALIZER;

static void logf_(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (H && H->log) H->log("%s", b); else fprintf(stderr, "[wwise] %s\n", b);
}

static double cpu_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static double wall_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

#define GROW(p, n, cap) do { if ((n) >= (cap)) { (cap) = (cap) ? (cap) * 2 : 64; (p) = realloc((p), (size_t)(cap) * sizeof *(p)); } } while (0)

// ------------------------------------------------------------------------------------------ JSON
// A small reader for the setup's files: values in a tree, strings decoded in place.
typedef struct jv { char t; int n; double num; char *s; struct jv *v; char **k; } jv;   // t: n b d s a o

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
    return v && (v->t == 'd' || v->t == 'b') ? v->num : v && v->t == 's' ? strtod(v->s, NULL) : d;
}
static uint32_t jid_v(const jv *v) { return !v ? 0 : v->t == 's' ? (uint32_t)strtoul(v->s, NULL, 10) : v->t == 'd' ? (uint32_t)v->num : 0; }
static uint32_t jid(const jv *o, const char *key) { return jid_v(jget(o, key)); }
static const char *jstr(const jv *o, const char *key) { jv *v = jget(o, key); return v && v->t == 's' ? v->s : ""; }
static uint32_t idkey(const char *s) { return (uint32_t)strtoul(s, NULL, 10); }

// ---------------------------------------------------------------------------------------- tables
typedef struct { uint32_t *k; int32_t *v; uint32_t cap, n; } idmap;

static int32_t im_get(const idmap *m, uint32_t k)
{
    if (!m->cap || !k) return -1;
    for (uint32_t i = (k * 2654435761u) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (m->k[i] == k) return m->v[i];
        if (!m->k[i]) return -1;
    }
}

static void im_put(idmap *m, uint32_t k, int32_t v)    // the first value of a key stays
{
    if (!k) return;
    if ((m->n + 1) * 2 > m->cap) {
        idmap g = { 0 };
        g.cap = m->cap ? m->cap * 2 : 64;
        g.k = calloc(g.cap, sizeof *g.k);
        g.v = calloc(g.cap, sizeof *g.v);
        for (uint32_t i = 0; i < m->cap; i++) if (m->k[i]) im_put(&g, m->k[i], m->v[i]);
        free(m->k); free(m->v);
        *m = g;
    }
    uint32_t i = (k * 2654435761u) & (m->cap - 1);
    while (m->k[i] && m->k[i] != k) i = (i + 1) & (m->cap - 1);
    if (m->k[i]) return;
    m->k[i] = k; m->v[i] = v; m->n++;
}
static void im_free(idmap *m) { free(m->k); free(m->v); memset(m, 0, sizeof *m); }

enum { C_PCM, C_ADPCM, C_VORBIS };
typedef struct {
    const uint8_t *p;
    uint32_t len, rate, samples, loop_s, loop_e;
    uint16_t ch, block;
    uint8_t codec, has_loop;
} media_t;

enum { N_SOUND, N_RANDOM, N_SEQUENCE, N_SWITCH, N_LAYER, N_MIXER, N_MSWITCH, N_PLAYLIST, N_SEGMENT, N_TRACK, N_OTHER };
enum { T_NORMAL, T_RANDOM, T_SEQUENCE, T_SWITCH };
typedef struct { uint32_t group, value; float vol, pitch; } nstate_t;      // added while group = value
typedef struct { uint32_t key, val; int leaf, first, n; } dec_t;           // a music switch's decision tree
typedef struct { uint8_t kind; int loop, avoid, nkids, first; float weight; uint32_t seg; } item_t;  // a playlist's
typedef struct { int nsrc, ndst; uint32_t *src, *dst; float fout, fin; int sync; } rule_t;
typedef struct { uint32_t media; int sub; float at, begin, end, src; } clip_t;
enum { I_SEGMENT, I_SEQUENCE, I_SEQSTEP, I_RANDOM, I_RANDSTEP };

typedef struct {
    uint32_t id, parent, bus, group, dflt, media;
    uint8_t type, state, cont, trans_delay, tkind, limit_newest;
    float vol, pitch, rv0, rv1, rp0, rp1, delay_ms, trans_ms, dur_ms, entry_ms, exit_ms, prob;
    int loop, limit, avoid;
    int nkids; uint32_t *kids; float *w;        // children
    int nmap; uint32_t *mval; int *moff, *mcnt; uint32_t *mkids;   // switch: value -> children; track: value -> subtrack
    int nst; nstate_t *st;
    int nargs; uint32_t *agroup; uint8_t *astate; int dfirst, dn; uint32_t dleaf;     // music switch
    int item, nitems, nrules; rule_t *rules;    // playlist: its item tree, g_items[item .. item + nitems)
    int nclips, subtracks; clip_t *clips;       // track
    // at run time
    int seqpos, active, nhist, hist[4];
    float rtpc_db, rtpc_cents, extra_db;
} node_t;

enum { A_PLAY, A_STOP, A_PAUSE, A_RESUME, A_STOPALL, A_PAUSEALL, A_RESUMEALL, A_STATE, A_SWITCH, A_BUSVOL, A_VOL, A_RTPC,
       A_RESETBUSVOL, A_RESETVOL, A_OTHER };
typedef struct {
    uint8_t type, bus, object, absolute;
    uint32_t target, group, value;
    float fade_ms, delay_ms, amount;
    int nexcept; uint32_t *except;
} act_t;
typedef struct { int first, n; } event_t;
typedef struct { uint32_t id, parent; float vol, rtpc_db, extra_db, gain; int busy, nst; nstate_t *st; } bus_t;
typedef struct { uint32_t rtpc, target; uint8_t pitch, dbscale; int n; float *x, *y; uint8_t *interp; } bind_t;
typedef struct { char name[128]; uint32_t id; const uint8_t *pak; size_t paklen; } bank_t;

static char g_dir[PATH_MAX];
static media_t *g_media; static int g_nmedia, g_cmedia; static idmap g_mmap;
static node_t *g_nodes; static int g_nnodes, g_cnodes; static idmap g_nmap;
static act_t *g_acts; static int g_nacts, g_cacts;
static event_t *g_evs; static int g_nevs, g_cevs; static idmap g_emap;
static bus_t *g_buses; static int g_nbuses, g_cbuses; static idmap g_bmap;
static bind_t *g_binds; static int g_nbinds, g_cbinds;
static bank_t *g_banks; static int g_nbanks, g_cbanks;
static dec_t *g_dec; static int g_ndec, g_cdec;
static item_t *g_items; static int g_nitems, g_citems;
static int *g_ikids; static int g_nikids, g_cikids;
static int g_bus_dirty = 1;
static unsigned g_gen = 1;      // voice gains and pitches are recomputed when it changes

// game syncs
typedef struct { uint32_t group, value; uint64_t obj; } sync_t;
static sync_t *g_states, *g_switches, *g_dstates, *g_dswitches;
static int g_nstates, g_cstates, g_nswitches, g_cswitches, g_ndstates, g_cdstates, g_ndswitches, g_cdswitches;
typedef struct { uint32_t id; float value; } rtpcv_t;
typedef struct { uint32_t group, from, to; float ms; } sfade_t;    // a state group's fade time (from = to = 0: its default)
static sfade_t *g_sfade; static int g_nsfade, g_csfade;
static rtpcv_t *g_rtpcv, *g_rtpcd; static int g_nrtpcv, g_crtpcv, g_nrtpcd, g_crtpcd;

static sync_t *sync_find(sync_t *t, int n, uint32_t group, uint64_t obj)
{
    for (int i = 0; i < n; i++) if (t[i].group == group && t[i].obj == obj) return &t[i];
    return NULL;
}
static uint32_t state_get(uint32_t group)
{
    sync_t *s = sync_find(g_states, g_nstates, group, 0);
    if (!s) s = sync_find(g_dstates, g_ndstates, group, 0);
    return s ? s->value : 0;
}
static uint32_t switch_get(uint32_t group, uint64_t obj)
{
    sync_t *s = sync_find(g_switches, g_nswitches, group, obj);
    if (!s) s = sync_find(g_dswitches, g_ndswitches, group, 0);
    return s ? s->value : 0;
}
static float state_offsets(const nstate_t *st, int n, int pitch)
{
    float r = 0;
    for (int i = 0; i < n; i++) if (state_get(st[i].group) == st[i].value) r += pitch ? st[i].pitch : st[i].vol;
    return r;
}

// ---------------------------------------------------------------------------------------- loading
static int load_json(const char *path, jv *root, char **text)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = n > 0 ? malloc(n + 1) : NULL;
    if (!b || fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return 0; }
    fclose(f);
    b[n] = 0;
    char *p = b;
    if (!js_val(&p, root, 0) || root->t != 'o') { logf_("%s: not valid JSON (near byte %ld)", path, (long)(p - b)); js_free(root); free(b); return -1; }
    *text = b;
    return 1;
}

static void range(const jv *o, const char *key, float *lo, float *hi)
{
    jv *v = jget(o, key);
    *lo = *hi = 0;
    if (v && v->t == 'a' && v->n >= 2) { *lo = (float)v->v[0].num; *hi = (float)v->v[1].num; }
}

static uint32_t *idlist(const jv *a, int *n)
{
    *n = 0;
    if (!a || (a->t != 'a' && a->t != 's' && a->t != 'd')) return NULL;
    if (a->t != 'a') { uint32_t *r = malloc(sizeof *r); r[0] = jid_v(a); *n = 1; return r; }
    uint32_t *r = malloc((a->n + 1) * sizeof *r);
    for (int i = 0; i < a->n; i++) r[i] = jid_v(&a->v[i]);
    *n = a->n;
    return r;
}

static int ntype(const char *t)
{
    static const char *const T[] = { "sound", "random", "sequence", "switch", "layer", "mixer", "music_switch", "playlist", "segment", "track" };
    for (int i = 0; i < 10; i++) if (!strcmp(t, T[i])) return i;
    return N_OTHER;
}

static int atype(const char *t)
{
    static const char *const T[] = { "play", "stop", "pause", "resume", "stop_all", "pause_all", "resume_all", "set_state", "set_switch",
                                     "set_bus_volume", "set_volume", "set_rtpc", "reset_bus_volume", "reset_volume" };
    for (int i = 0; i < 14; i++) if (!strcmp(t, T[i])) return i;
    return A_OTHER;
}

// "states": {"<group>": {"<state>": {"vol": dB, "pitch": cents}}}
static nstate_t *parse_states(const jv *o, int *n)
{
    *n = 0;
    if (!o || o->t != 'o') return NULL;
    int total = 0;
    for (int i = 0; i < o->n; i++) total += o->v[i].t == 'o' ? o->v[i].n : 0;
    if (!total) return NULL;
    nstate_t *r = malloc(total * sizeof *r);
    for (int i = 0; i < o->n; i++)
        for (int k = 0; o->v[i].t == 'o' && k < o->v[i].n; k++)
            r[(*n)++] = (nstate_t){ idkey(o->k[i]), idkey(o->v[i].k[k]), (float)jnum(&o->v[i].v[k], "vol", 0), (float)jnum(&o->v[i].v[k], "pitch", 0) };
    return r;
}

// a decision tree level: {"<value>": "<node>" | {next level}}; the level's entries are consecutive
static void parse_dec(const jv *o, int *first, int *n)
{
    *first = g_ndec;
    *n = o && o->t == 'o' ? o->n : 0;
    for (int i = 0; i < *n; i++) { GROW(g_dec, g_ndec, g_cdec); memset(&g_dec[g_ndec++], 0, sizeof *g_dec); }
    for (int i = 0; i < *n; i++) {
        int f = 0, k = 0;
        uint32_t key = idkey(o->k[i]), val = 0, leaf = o->v[i].t != 'o';
        if (leaf) val = jid_v(&o->v[i]); else parse_dec(&o->v[i], &f, &k);
        g_dec[*first + i] = (dec_t){ key, val, (int)leaf, f, k };
    }
}

static int parse_item(const jv *o, int depth)
{
    if (!o || o->t != 'o' || depth > 32) return -1;
    GROW(g_items, g_nitems, g_citems);
    int me = g_nitems++;
    item_t it = { 0 };
    const char *k = jstr(o, "kind");
    it.seg = jid(o, "segment");
    it.kind = it.seg ? I_SEGMENT : !strcmp(k, "sequence_step") ? I_SEQSTEP : !strcmp(k, "random") ? I_RANDOM : !strcmp(k, "random_step") ? I_RANDSTEP : I_SEQUENCE;
    it.loop = (int)jnum(o, "loop", 1);
    it.weight = (float)jnum(o, "weight", 100);
    it.avoid = (int)jnum(o, "avoid_repeat", 0);
    jv *c = jget(o, "children");
    int kids[256], nk = 0;
    for (int i = 0; c && c->t == 'a' && i < c->n && nk < 256; i++) { int ci = parse_item(&c->v[i], depth + 1); if (ci >= 0) kids[nk++] = ci; }
    it.first = g_nikids;
    it.nkids = nk;
    for (int i = 0; i < nk; i++) { GROW(g_ikids, g_nikids, g_cikids); g_ikids[g_nikids++] = kids[i]; }
    g_items[me] = it;
    return me;
}

static void add_node(uint32_t id, const jv *o)
{
    if (im_get(&g_nmap, id) >= 0) return;
    GROW(g_nodes, g_nnodes, g_cnodes);
    node_t *n = &g_nodes[g_nnodes];
    memset(n, 0, sizeof *n);
    n->id = id;
    n->type = ntype(jstr(o, "type"));
    n->media = jid(o, "media");
    n->bus = jid(o, "bus");
    n->parent = jid(o, "parent");
    n->vol = (float)jnum(o, "vol", 0);
    n->pitch = (float)jnum(o, "pitch", 0);
    range(o, "rand_vol", &n->rv0, &n->rv1);
    range(o, "rand_pitch", &n->rp0, &n->rp1);
    n->loop = (int)jnum(o, "loop", 1);
    n->delay_ms = (float)jnum(o, "delay_ms", 0);
    n->limit = (int)jnum(o, "limit", 0);
    n->limit_newest = jnum(o, "limit_newest", 0) != 0;
    n->prob = (float)jnum(o, "prob", 100);
    n->avoid = (int)jnum(o, "avoid_repeat", 0);
    n->cont = jnum(o, "continuous", 0) != 0;
    n->trans_delay = !strcmp(jstr(o, "transition"), "delay");
    n->trans_ms = (float)jnum(o, "transition_ms", 0);
    n->state = jnum(o, "state", 0) != 0;
    n->group = jid(o, "group");
    n->dflt = jid(o, "default");
    n->dur_ms = (float)jnum(o, "duration_ms", 0);
    n->entry_ms = (float)jnum(o, "entry_ms", 0);
    n->exit_ms = (float)jnum(o, "exit_ms", 0);
    n->item = -1;
    n->kids = idlist(jget(o, "children"), &n->nkids);
    n->st = parse_states(jget(o, "states"), &n->nst);
    jv *w = jget(o, "weights");
    if (w && w->t == 'a' && w->n == n->nkids && n->nkids) {
        n->w = malloc(n->nkids * sizeof *n->w);
        for (int i = 0; i < n->nkids; i++) n->w[i] = (float)w->v[i].num;
    }
    jv *m = jget(o, "map");
    if (n->type == N_TRACK) {           // a switch track: value -> subtrack
        jv *sw = jget(o, "switch");
        m = jget(sw, "map");
        n->group = jid(sw, "group");
        n->state = jnum(sw, "state", 0) != 0;
        n->dflt = jid(sw, "default");
        const char *k = jstr(o, "kind");
        n->tkind = !strcmp(k, "random") ? T_RANDOM : !strcmp(k, "sequence") ? T_SEQUENCE : !strcmp(k, "switch") ? T_SWITCH : T_NORMAL;
        n->subtracks = (int)jnum(o, "subtracks", 1);
        jv *c = jget(o, "clips");
        if (c && c->t == 'a' && c->n) {
            n->clips = calloc(c->n, sizeof *n->clips);
            for (int i = 0; i < c->n; i++) {
                const jv *x = &c->v[i];
                n->clips[n->nclips++] = (clip_t){ jid(x, "media"), (int)jnum(x, "sub", 0), (float)jnum(x, "at_ms", 0),
                                                  (float)jnum(x, "begin_ms", 0), (float)jnum(x, "end_ms", 0), (float)jnum(x, "src_ms", 0) };
            }
        }
    }
    if (m && m->t == 'o' && m->n) {
        n->nmap = m->n;
        n->mval = malloc(m->n * sizeof *n->mval);
        n->moff = malloc(m->n * sizeof *n->moff);
        n->mcnt = malloc(m->n * sizeof *n->mcnt);
        int total = 0;
        for (int i = 0; i < m->n; i++) total += m->v[i].t == 'a' ? m->v[i].n : 1;
        n->mkids = malloc((total + 1) * sizeof *n->mkids);
        total = 0;
        for (int i = 0; i < m->n; i++) {
            int k;
            uint32_t *l = idlist(&m->v[i], &k);
            n->mval[i] = idkey(m->k[i]);
            n->moff[i] = total;
            n->mcnt[i] = k;
            if (k) memcpy(n->mkids + total, l, k * sizeof *l);
            total += k;
            free(l);
        }
    }
    if (n->type == N_MSWITCH) {
        jv *a = jget(o, "args"), *t = jget(o, "tree");
        n->nargs = a && a->t == 'a' ? a->n : 0;
        if (n->nargs) {
            n->agroup = malloc(n->nargs * sizeof *n->agroup);
            n->astate = malloc(n->nargs);
            for (int i = 0; i < n->nargs; i++) { n->agroup[i] = jid(&a->v[i], "group"); n->astate[i] = jnum(&a->v[i], "state", 1) != 0; }
        }
        if (t && t->t == 'o') parse_dec(t, &n->dfirst, &n->dn); else n->dleaf = jid_v(t);
    }
    if (n->type == N_PLAYLIST) { n->item = parse_item(jget(o, "items"), 0); n->nitems = n->item >= 0 ? g_nitems - n->item : 0; }
    jv *r = jget(o, "rules");
    if (r && r->t == 'a' && r->n) {
        n->rules = calloc(r->n, sizeof *n->rules);
        for (int i = 0; i < r->n; i++) {
            rule_t *x = &n->rules[n->nrules++];
            x->src = idlist(jget(&r->v[i], "src"), &x->nsrc);
            x->dst = idlist(jget(&r->v[i], "dst"), &x->ndst);
            x->fout = (float)jnum(&r->v[i], "fade_out_ms", 0);
            x->fin = (float)jnum(&r->v[i], "fade_in_ms", 0);
            x->sync = (int)jnum(&r->v[i], "sync", 7);
        }
    }
    im_put(&g_nmap, id, g_nnodes++);
}

static float curve_at(const bind_t *b, float x)
{
    float y = b->y[b->n - 1];
    if (x <= b->x[0]) y = b->y[0];
    else
        for (int i = 1; i < b->n; i++)
            if (x <= b->x[i]) {
                float dx = b->x[i] - b->x[i - 1];
                y = b->interp[i - 1] == 9 || dx <= 0 ? b->y[i - 1] : b->y[i - 1] + (b->y[i] - b->y[i - 1]) * (x - b->x[i - 1]) / dx;
                break;
            }
    if (b->dbscale) y = y <= -1 ? -96 : 20 * log10f(y + 1);
    return y;
}

static float rtpc_get(uint32_t id)          // the game's value, else the Init bank's default, else 0
{
    for (int i = 0; i < g_nrtpcv; i++) if (g_rtpcv[i].id == id) return g_rtpcv[i].value;
    for (int i = 0; i < g_nrtpcd; i++) if (g_rtpcd[i].id == id) return g_rtpcd[i].value;
    return 0;
}

// the RTPCs' contributions to buses and nodes, again (a value or the bindings changed)
static void rtpc_apply(void)
{
    for (int i = 0; i < g_nbinds; i++) {
        bind_t *b = &g_binds[i];
        int bi = im_get(&g_bmap, b->target), ni = im_get(&g_nmap, b->target);
        if (bi >= 0) g_buses[bi].rtpc_db = 0;
        if (ni >= 0) g_nodes[ni].rtpc_cents = g_nodes[ni].rtpc_db = 0;
    }
    for (int i = 0; i < g_nbinds; i++) {
        bind_t *b = &g_binds[i];
        int bi = im_get(&g_bmap, b->target), ni = im_get(&g_nmap, b->target);
        float y = curve_at(b, rtpc_get(b->rtpc));
        if (bi >= 0 && !b->pitch) g_buses[bi].rtpc_db += y;
        if (ni >= 0) { if (b->pitch) g_nodes[ni].rtpc_cents += y; else g_nodes[ni].rtpc_db += y; }
    }
    g_bus_dirty = 1;
    g_gen++;
}

static void load_tables(const jv *root, const uint8_t *pak, size_t paklen, const char *name)
{
    jv *t = jget(root, "media");
    int bad = 0;
    for (int i = 0; t && t->t == 'o' && i < t->n; i++) {
        uint32_t id = idkey(t->k[i]);
        const jv *o = &t->v[i];
        if (im_get(&g_mmap, id) >= 0) continue;
        uint32_t off = (uint32_t)jnum(o, "off", 0), len = (uint32_t)jnum(o, "len", 0);
        if (!pak || (uint64_t)off + len > paklen) { bad++; continue; }
        GROW(g_media, g_nmedia, g_cmedia);
        media_t *m = &g_media[g_nmedia];
        memset(m, 0, sizeof *m);
        m->p = pak + off;
        m->len = len;
        const char *c = jstr(o, "codec");
        m->codec = !strcmp(c, "vorbis") ? C_VORBIS : !strcmp(c, "adpcm") ? C_ADPCM : C_PCM;
        m->rate = (uint32_t)jnum(o, "rate", 48000);
        m->ch = (uint16_t)jnum(o, "channels", 1);
        m->samples = (uint32_t)jnum(o, "samples", 0);
        m->block = (uint16_t)jnum(o, "block_align", 36 * m->ch);
        if (!m->ch || !m->rate) { bad++; continue; }
        if (m->codec == C_PCM && !m->samples) m->samples = len / (2 * m->ch);
        if (m->codec == C_ADPCM && (m->block < 8 * m->ch || m->block % m->ch)) { bad++; continue; }
        jv *lp = jget(o, "loop");
        if (lp && lp->t == 'a' && lp->n >= 2) {
            m->has_loop = 1;
            m->loop_s = (uint32_t)lp->v[0].num;
            m->loop_e = (uint32_t)lp->v[1].num;
            if (m->loop_e > m->samples || m->loop_e <= m->loop_s) m->loop_e = m->samples;
            if (m->loop_s >= m->loop_e) m->has_loop = 0;
        }
        im_put(&g_mmap, id, g_nmedia++);
    }
    if (bad) logf_("bank %s: %d media outside the pak or not readable: left out", name, bad);
    t = jget(root, "nodes");
    for (int i = 0; t && t->t == 'o' && i < t->n; i++) add_node(idkey(t->k[i]), &t->v[i]);
    t = jget(root, "events");
    for (int i = 0; t && t->t == 'o' && i < t->n; i++) {
        uint32_t id = idkey(t->k[i]);
        const jv *l = &t->v[i];
        if (im_get(&g_emap, id) >= 0 || l->t != 'a') continue;
        GROW(g_evs, g_nevs, g_cevs);
        g_evs[g_nevs] = (event_t){ g_nacts, 0 };
        for (int k = 0; k < l->n; k++) {
            const jv *a = &l->v[k];
            GROW(g_acts, g_nacts, g_cacts);
            act_t *x = &g_acts[g_nacts++];
            memset(x, 0, sizeof *x);
            x->type = atype(jstr(a, "action"));
            x->target = jid(a, "target");
            x->bus = jnum(a, "bus", 0) != 0;
            x->object = jnum(a, "object", 0) != 0;
            x->group = jid(a, "group");
            x->value = jid(a, "value");
            x->fade_ms = (float)jnum(a, "fade_ms", 0);
            x->delay_ms = (float)jnum(a, "delay_ms", 0);
            x->amount = (float)jnum(a, "amount", 0);
            x->absolute = !strcmp(jstr(a, "meaning"), "absolute");
            x->except = idlist(jget(a, "except"), &x->nexcept);
            g_evs[g_nevs].n++;
        }
        im_put(&g_emap, id, g_nevs++);
    }
    t = jget(root, "buses");
    for (int i = 0; t && t->t == 'o' && i < t->n; i++) {
        uint32_t id = idkey(t->k[i]);
        if (im_get(&g_bmap, id) >= 0) continue;
        GROW(g_buses, g_nbuses, g_cbuses);
        bus_t *b = &g_buses[g_nbuses];
        memset(b, 0, sizeof *b);
        b->id = id;
        b->parent = jid(&t->v[i], "parent");
        b->vol = (float)jnum(&t->v[i], "vol", 0);
        b->gain = 1;
        b->st = parse_states(jget(&t->v[i], "states"), &b->nst);
        im_put(&g_bmap, id, g_nbuses++);
    }
    t = jget(root, "rtpc");
    for (int i = 0; t && t->t == 'o' && i < t->n; i++) {
        const jv *l = &t->v[i];
        for (int k = 0; l->t == 'a' && k < l->n; k++) {
            const jv *b = &l->v[k], *c = jget(&l->v[k], "curve");
            if (!c || c->t != 'a' || !c->n) continue;
            int pitch = !strcmp(jstr(b, "prop"), "pitch");
            if (!pitch && strcmp(jstr(b, "prop"), "volume")) continue;
            GROW(g_binds, g_nbinds, g_cbinds);
            bind_t *x = &g_binds[g_nbinds++];
            x->rtpc = idkey(t->k[i]);
            x->target = jid(b, "target");
            x->pitch = (uint8_t)pitch;
            x->dbscale = !strcmp(jstr(b, "scaling"), "db");
            x->n = c->n;
            x->x = malloc(c->n * sizeof *x->x);
            x->y = malloc(c->n * sizeof *x->y);
            x->interp = malloc(c->n);
            for (int j = 0; j < c->n; j++) {
                const jv *pt = &c->v[j];
                x->x[j] = pt->t == 'a' && pt->n >= 2 ? (float)pt->v[0].num : 0;
                x->y[j] = pt->t == 'a' && pt->n >= 2 ? (float)pt->v[1].num : 0;
                x->interp[j] = pt->t == 'a' && pt->n >= 3 ? (uint8_t)pt->v[2].num : 4;
            }
        }
    }
    jv *d = jget(root, "defaults");
    t = jget(d, "states");
    for (int i = 0; t && t->t == 'o' && i < t->n; i++)
        if (!sync_find(g_dstates, g_ndstates, idkey(t->k[i]), 0)) { GROW(g_dstates, g_ndstates, g_cdstates); g_dstates[g_ndstates++] = (sync_t){ idkey(t->k[i]), jid_v(&t->v[i]), 0 }; }
    t = jget(d, "switches");
    for (int i = 0; t && t->t == 'o' && i < t->n; i++)
        if (!sync_find(g_dswitches, g_ndswitches, idkey(t->k[i]), 0)) { GROW(g_dswitches, g_ndswitches, g_cdswitches); g_dswitches[g_ndswitches++] = (sync_t){ idkey(t->k[i]), jid_v(&t->v[i]), 0 }; }
    t = jget(d, "rtpc");
    for (int i = 0; t && t->t == 'o' && i < t->n; i++) { GROW(g_rtpcd, g_nrtpcd, g_crtpcd); g_rtpcd[g_nrtpcd++] = (rtpcv_t){ idkey(t->k[i]), (float)t->v[i].num }; }
    t = jget(d, "state_groups");
    for (int i = 0; t && t->t == 'o' && i < t->n; i++) {
        uint32_t g = idkey(t->k[i]);
        GROW(g_sfade, g_nsfade, g_csfade);
        g_sfade[g_nsfade++] = (sfade_t){ g, 0, 0, (float)jnum(&t->v[i], "fade_ms", 0) };
        jv *tr = jget(&t->v[i], "transitions");
        for (int k = 0; tr && tr->t == 'a' && k < tr->n; k++) {
            const jv *x = &tr->v[k];
            if (x->t != 'a' || x->n < 3) continue;
            GROW(g_sfade, g_nsfade, g_csfade);
            g_sfade[g_nsfade++] = (sfade_t){ g, jid_v(&x->v[0]), jid_v(&x->v[1]), (float)x->v[2].num };
        }
    }
    rtpc_apply();
}

static int bank_load(const char *name, uint32_t *bank_id)
{
    char base[128];
    const char *s = strrchr(name, '/');
    snprintf(base, sizeof base, "%s", s ? s + 1 : name);
    char *dot = strrchr(base, '.');
    if (dot && !strcasecmp(dot, ".bnk")) *dot = 0;
    for (int i = 0; i < g_nbanks; i++)
        if (!strcasecmp(g_banks[i].name, base)) { if (g_banks[i].id) *bank_id = g_banks[i].id; return 1; }
    char path[PATH_MAX + 160];
    snprintf(path, sizeof path, "%s/%s.uaww.json", g_dir, base);
    jv root;
    char *text = NULL;
    double t0 = wall_now();
    int r = load_json(path, &root, &text);
    if (r <= 0) {
        if (!r) logf_("bank %s: no %s: its sounds stay silent", name, path);
        return 0;
    }
    if (jnum(&root, "format", 1) > 1) logf_("bank %s: format %g, newer than this player's (1): reading what it knows", base, jnum(&root, "format", 1));
    snprintf(path, sizeof path, "%s/%s.uaww.pak", g_dir, base);
    const uint8_t *pak = NULL;
    size_t paklen = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0) {
        void *m = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) { pak = m; paklen = st.st_size; }
    }
    if (fd >= 0) close(fd);
    int n0 = g_nnodes, e0 = g_nevs, m0 = g_nmedia;
    load_tables(&root, pak, paklen, base);
    GROW(g_banks, g_nbanks, g_cbanks);
    bank_t *b = &g_banks[g_nbanks++];
    snprintf(b->name, sizeof b->name, "%s", base);
    b->id = jid(jget(&root, "bank"), "id");
    b->pak = pak;
    b->paklen = paklen;
    if (b->id) *bank_id = b->id;
    logf_("bank %s: %d events, %d nodes, %d media (pak %.1f MB%s), %d buses, %d RTPC curves in %.0f ms", base, g_nevs - e0, g_nnodes - n0,
          g_nmedia - m0, paklen / 1048576.0, pak ? "" : ": none", g_nbuses, g_nbinds, (wall_now() - t0) * 1000);
    js_free(&root);
    free(text);
    return 1;
}

// ---------------------------------------------------------------------------------------- voices
typedef struct {
    int used, node, media, nchain, chain[MAXCHAIN];     // chain: the node and its parents (for stop, limits, RTPCs, states)
    uint32_t playing, bus, serial;
    uint64_t obj;
    float db, cents;                // the chain's volume and pitch, random parts included
    float gt, gc;                   // target gain (without fade), gain at the end of the last block
    float fade, fade_rate;          // fade 0..1, change per frame; fading to 0 ends the voice
    int stopping, paused, loops, delay, seq, mus, clip;
    long budget;                    // a music clip: source frames left to play (looping the media); -1 none
    unsigned gen;
    int glide;                      // frames over which the gain moves to a new target (a state's fade)
    uint32_t step, frac, p, n;      // 16.16 step through buf; buf[c][0] is the frame before buf[c][1..n]
    uint32_t next, end;             // the next source frame to decode, the end of this pass
    stb_vorbis *vb;
    int16_t buf[2][DEC_N + 1];
} voice_t;

typedef struct { int used; uint32_t id, event; uint64_t obj; int refs; } play_t;
typedef struct { uint32_t playing; uint64_t obj; float fade_ms; int delay, seq, mus; } ctx_t;
typedef struct { int used, node, pos, left, active, advance; ctx_t c; } seq_t;
typedef struct { int active, pos, left, cur, step, nhist, hist[4]; } ist_t;     // a playlist item's state
typedef struct {
    int used, root, target, seg, nseg, nvoices, ended, lone, nist;
    uint64_t seg_start, next_start;     // the current segment's start (its timeline's 0) and exit cue
    float fade_in, fade_out;            // the transition rule's fades for the next segment
    ist_t *ist;
    ctx_t c;
} mus_t;
typedef struct { int used, act; uint64_t due; uint32_t playing; uint64_t obj; } pend_t;    // a delayed action

static voice_t *g_v;
static int g_maxv = 48, g_nv, g_vpeak;
static uint32_t g_serial;
static play_t *g_play; static int g_nplay, g_cplay;
static seq_t g_seq[MAXSEQ];
static mus_t g_mus[MAXMUS];
static pend_t g_pend[MAXPEND];
static uint32_t *g_done; static int g_ndone, g_cdone;
static uint64_t g_clock;            // frames mixed
static int g_glide;                 // a state change's fade for the voices' gains (frames)
static int g_rate = 48000, g_suspended;
static uint32_t g_rng = 0x9e3779b9u;
static float *g_scratch; static int32_t *g_acc; static int g_scap;     // the mix; its integer sums (mono, then stereo)
static double g_last_mix_wall, g_stats_wall, g_stats_every = 60;
static wwplay_stats g_st, g_st_last;
static wwplay_wav g_cap;
static unsigned long g_cap_frames;

static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static float frand(void) { return (rnd() >> 8) * (1.0f / 16777216.0f); }
static float rrange(float lo, float hi) { return lo == hi ? lo : lo + (hi - lo) * frand(); }
static float db2lin(float db) { return db <= -96 ? 0 : powf(10.0f, db / 20); }
static int ms2f(float ms) { return ms <= 0 ? 0 : (int)(ms * g_rate / 1000 + 0.5f); }

static play_t *play_find(uint32_t id)
{
    for (int i = 0; i < g_nplay; i++) if (g_play[i].used && g_play[i].id == id) return &g_play[i];
    return NULL;
}
static void play_ref(uint32_t id, int d)
{
    play_t *p = play_find(id);
    if (!p) return;
    p->refs += d;
    if (p->refs <= 0) {
        p->used = 0;
        GROW(g_done, g_ndone, g_cdone);
        g_done[g_ndone++] = id;
        while (g_nplay && !g_play[g_nplay - 1].used) g_nplay--;
    }
}

static void flush_done(void)        // the events that ended, to the host (no lock held)
{
    uint32_t buf[64];
    for (;;) {
        pthread_mutex_lock(&L);
        int n = g_ndone < 64 ? g_ndone : 64;
        memcpy(buf, g_done + g_ndone - n, n * sizeof *buf);
        g_ndone -= n;
        pthread_mutex_unlock(&L);
        if (!n) return;
        for (int i = 0; i < n; i++) if (H && H->event_done) H->event_done(buf[i]);
    }
}

static float bus_gain_i(int i, int depth)
{
    bus_t *b = &g_buses[i];
    if (b->busy == 2 || depth > 32) return b->gain;
    b->busy = 2;
    int p = b->parent && b->parent != b->id ? im_get(&g_bmap, b->parent) : -1;
    b->gain = db2lin(b->vol + b->rtpc_db + b->extra_db + state_offsets(b->st, b->nst, 0)) * (p >= 0 ? bus_gain_i(p, depth + 1) : 1);
    return b->gain;
}
static float bus_gain(uint32_t id)
{
    if (g_bus_dirty) {
        for (int i = 0; i < g_nbuses; i++) g_buses[i].busy = 0;
        for (int i = 0; i < g_nbuses; i++) bus_gain_i(i, 0);
        g_bus_dirty = 0;
    }
    int i = im_get(&g_bmap, id);
    return i >= 0 ? g_buses[i].gain : 1;
}

static void voice_params(voice_t *v)
{
    float db = v->db, cents = v->cents;
    for (int i = 0; i < v->nchain; i++) {
        node_t *n = &g_nodes[v->chain[i]];
        db += n->rtpc_db + n->extra_db;
        cents += n->rtpc_cents;
        if (n->nst) { db += state_offsets(n->st, n->nst, 0); cents += state_offsets(n->st, n->nst, 1); }
    }
    v->gt = db2lin(db) * bus_gain(v->bus);
    double st = (double)g_media[v->media].rate / g_rate * pow(2.0, cents / 1200.0) * 65536.0;
    v->step = st < 1 ? 1 : st > 16 * 65536.0 ? 16 * 65536 : (uint32_t)(st + 0.5);
    v->gen = g_gen;
}

static int in_chain(const int *chain, int n, int ni) { for (int i = 0; i < n; i++) if (chain[i] == ni) return 1; return 0; }

static void chain_active(voice_t *v, int d) { for (int i = 0; i < v->nchain; i++) g_nodes[v->chain[i]].active += d; }

static void vpool_put(int media, stb_vorbis *vb);

static void voice_free(voice_t *v)
{
    if (!v->used) return;
    if (v->vb) vpool_put(v->media, v->vb);
    v->vb = NULL;
    v->used = 0;
    g_nv--;
    if (!v->stopping) chain_active(v, -1);
    if (v->seq >= 0 && g_seq[v->seq].used && --g_seq[v->seq].active <= 0) g_seq[v->seq].advance = 1;
    if (v->mus >= 0 && g_mus[v->mus].used) g_mus[v->mus].nvoices--;
    play_ref(v->playing, -1);
}

static void voice_stop(voice_t *v, float fade_ms)
{
    if (!v->used) return;
    if (v->delay > 0 || v->paused) { voice_free(v); return; }   // not heard yet (or paused): gone at once
    if (!v->stopping) { v->stopping = 1; chain_active(v, -1); }
    if (fade_ms < MINFADE_MS) fade_ms = MINFADE_MS;
    float r = -1.0f / ms2f(fade_ms);
    if (v->fade_rate >= 0 || r < v->fade_rate) v->fade_rate = r;
}

static void voice_start_pass(voice_t *v)
{
    const media_t *m = &g_media[v->media];
    v->end = v->loops != 1 && m->has_loop && !v->clip ? m->loop_e : m->samples;
}

// a voice of a sound (ni), or of a music track (ni) playing a clip's media from frame `from` for
// `budget` source frames; its volume, pitch and bus from ni's chain of parents
static voice_t *voice_new(int ni, uint32_t media, uint32_t from, long budget, const ctx_t *c)
{
    node_t *n = &g_nodes[ni];
    int mi = im_get(&g_mmap, media ? media : n->media);
    if (mi < 0 || !g_media[mi].samples) return NULL;
    voice_t tmp;
    memset(&tmp, 0, offsetof(voice_t, buf));
    tmp.media = mi;
    tmp.node = ni;
    int busset = 0;
    for (int k = ni, depth = 0; k >= 0 && depth < 64; depth++) {
        node_t *x = &g_nodes[k];
        if (tmp.nchain < MAXCHAIN) tmp.chain[tmp.nchain++] = k;
        tmp.db += x->vol + rrange(x->rv0, x->rv1);
        tmp.cents += x->pitch + rrange(x->rp0, x->rp1);
        if (!busset && x->bus) { tmp.bus = x->bus; busset = 1; }
        int p = x->parent ? im_get(&g_nmap, x->parent) : -1;
        k = p == k ? -1 : p;
    }
    for (int i = 0; i < tmp.nchain; i++) {           // limits: the oldest instance under that node goes
        node_t *x = &g_nodes[tmp.chain[i]];
        if (x->limit <= 0 || x->active < x->limit) continue;
        if (x->limit_newest) { g_st.limited++; return NULL; }     // or the new one does not play
        voice_t *old = NULL;
        for (int k = 0; k < g_maxv; k++)
            if (g_v[k].used && !g_v[k].stopping && in_chain(g_v[k].chain, g_v[k].nchain, tmp.chain[i]) && (!old || g_v[k].serial < old->serial)) old = &g_v[k];
        if (old) { voice_stop(old, 0); g_st.limited++; }
    }
    voice_params(&tmp);
    if (g_nv >= g_maxv) {                            // the quietest goes (the oldest of equals), or this one
        voice_t *q = NULL;
        float gq = 0;
        for (int i = 0; i < g_maxv; i++) {
            voice_t *x = &g_v[i];
            if (!x->used) continue;
            float gx = x->stopping ? 0 : x->gt;
            if (!q || gx < gq || (gx == gq && x->serial < q->serial)) { q = x; gq = gx; }
        }
        g_st.dropped++;
        if (!q || gq > tmp.gt) return NULL;
        voice_free(q);
    }
    voice_t *v = NULL;
    for (int i = 0; i < g_maxv; i++) if (!g_v[i].used) { v = &g_v[i]; break; }
    if (!v) return NULL;
    memcpy(v, &tmp, offsetof(voice_t, buf));
    v->used = 1;
    v->playing = c->playing;
    v->obj = c->obj;
    v->serial = ++g_serial;
    v->clip = media != 0;
    v->loops = media ? 0 : n->loop;
    v->budget = media ? budget : -1;
    v->next = from;
    v->delay = c->delay + (media ? 0 : ms2f(n->delay_ms));
    v->seq = c->seq;
    v->mus = c->mus;
    v->p = 1;
    v->buf[0][0] = v->buf[1][0] = 0;
    v->fade = c->fade_ms > 0 ? 0 : 1;
    v->fade_rate = c->fade_ms > 0 ? 1.0f / ms2f(c->fade_ms) : 0;
    v->gc = v->gt * v->fade;
    voice_start_pass(v);
    chain_active(v, 1);
    if (v->seq >= 0) g_seq[v->seq].active++;
    if (v->mus >= 0) g_mus[v->mus].nvoices++;
    play_ref(v->playing, 1);
    if (++g_nv > g_vpeak) g_vpeak = g_nv;
    g_st.voices_started++;
    return v;
}

// ---------------------------------------------------------------------------------------- decoding
static const int16_t IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int8_t IMA_INDEX[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

// one channel's part of a Wwise IMA block: a header (sample, step index, a reserved byte), then
// the nibbles, low first; the header's sample is the first frame and the last nibble is unused.
// Decoded by a table: each (step index, code) gives the difference (high bits) and the next step
// index times 16 (low 11 bits), so a sample is a load, an add and a saturation.
static int32_t g_ima[89 * 16];
static void ima_tables(void)
{
    for (int i = 0; i < 89; i++)
        for (int c = 0; c < 16; c++) {
            int d = ((2 * (c & 7) + 1) * IMA_STEP[i]) >> 3, n = i + IMA_INDEX[c];
            n = n < 0 ? 0 : n > 88 ? 88 : n;
            g_ima[i * 16 + c] = (int32_t)((uint32_t)(c & 8 ? -d : d) << 11) | n * 16;
        }
}

static inline int sat16(int x)
{
#if defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 6
    __asm__("ssat %0, #16, %1" : "=r"(x) : "r"(x));
    return x;
#else
    return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
#endif
}

static void ima_block(const uint8_t *b, int16_t *out, int upto)
{
    int s = (int16_t)(b[0] | b[1] << 8), ix = (b[2] > 88 ? 88 : b[2]) * 16;
    const uint8_t *d = b + 4;
    out[0] = (int16_t)s;
    int i = 1;
    for (; i + 1 < upto; i += 2) {
        int byte = *d++, e = g_ima[ix + (byte & 15)];
        s = sat16(s + (e >> 11));
        ix = e & 0x7f0;
        out[i] = (int16_t)s;
        e = g_ima[ix + (byte >> 4)];
        s = sat16(s + (e >> 11));
        ix = e & 0x7f0;
        out[i + 1] = (int16_t)s;
    }
    if (i < upto) out[i] = (int16_t)sat16(s + (g_ima[ix + (*d & 15)] >> 11));
}

static int dec_adpcm(voice_t *v, const media_t *m, int at, int want)
{
    int ch = m->ch, sub = m->block / ch, fpb = (sub - 4) * 2, got = 0;
    int16_t tmp[1024];
    if (fpb > 1024) return -1;
    while (got < want) {
        uint32_t blk = v->next / fpb, in = v->next % fpb;
        size_t off = (size_t)blk * m->block;
        if (off + 4u * ch >= m->len) break;
        int bsub = sub, bfpb = fpb;
        if (off + m->block > m->len) { bsub = (int)((m->len - off) / ch); bfpb = (bsub - 4) * 2; }    // a short last block
        if ((int)in >= bfpb) break;
        int k = bfpb - (int)in < want - got ? bfpb - (int)in : want - got;
        for (int c = 0; c < (ch > 1 ? 2 : 1); c++) {
            int16_t *o = v->buf[c] + at + got;
            if (!in && k == bfpb) ima_block(m->p + off + (size_t)c * bsub, o, k);
            else { ima_block(m->p + off + (size_t)c * bsub, tmp, in + k); memcpy(o, tmp + in, k * sizeof *o); }
        }
        got += k;
        v->next += k;
    }
    return got;
}

static int dec_pcm(voice_t *v, const media_t *m, int at, int want)
{
    int ch = m->ch;
    uint32_t have = m->len / (2 * ch);
    if (v->next >= have) return 0;
    if ((uint32_t)want > have - v->next) want = have - v->next;
    const uint8_t *s = m->p + (size_t)v->next * 2 * ch;
    for (int c = 0; c < (ch > 1 ? 2 : 1); c++) {
        int16_t *o = v->buf[c] + at;
        for (int i = 0; i < want; i++) o[i] = (int16_t)(s[(i * ch + c) * 2] | s[(i * ch + c) * 2 + 1] << 8);
    }
    v->next += want;
    return want;
}

// Ogg Vorbis decoders of ended voices, kept to play the same media again without reading its setup
// header (codebooks) once more
#define VPOOL 8
static struct { int media; stb_vorbis *vb; uint32_t at; } g_vpool[VPOOL];
static uint32_t g_vpool_clock;

static void vpool_put(int media, stb_vorbis *vb)
{
    int k = 0;
    for (int i = 0; i < VPOOL; i++) {
        if (!g_vpool[i].vb) { k = i; break; }
        if (g_vpool[i].at < g_vpool[k].at) k = i;
    }
    if (g_vpool[k].vb) stb_vorbis_close(g_vpool[k].vb);
    g_vpool[k].media = media;
    g_vpool[k].vb = vb;
    g_vpool[k].at = ++g_vpool_clock;
}

static stb_vorbis *vpool_get(int media)
{
    for (int i = 0; i < VPOOL; i++)
        if (g_vpool[i].vb && g_vpool[i].media == media) {
            stb_vorbis *vb = g_vpool[i].vb;
            g_vpool[i].vb = NULL;
            if (stb_vorbis_seek_start(vb)) return vb;
            stb_vorbis_close(vb);
        }
    return NULL;
}

static int dec_vorbis(voice_t *v, const media_t *m, int at, int want)
{
    if (!v->vb) {
        int err = 0;
        v->vb = vpool_get(v->media);
        if (!v->vb) v->vb = stb_vorbis_open_memory(m->p, (int)m->len, &err, NULL);
        if (!v->vb) return -1;
        if (v->next && !stb_vorbis_seek(v->vb, v->next)) return -1;
    }
    short *o[2] = { v->buf[0] + at, v->buf[1] + at };
    int k = stb_vorbis_get_samples_short(v->vb, m->ch > 1 ? 2 : 1, o, want);
    if (k > 0) v->next += k;
    return k;
}

static int dec_seek(voice_t *v, uint32_t to)
{
    v->next = to;
    if (v->vb) {
        int ok = to ? stb_vorbis_seek(v->vb, to) : stb_vorbis_seek_start(v->vb);
        if (!ok) return 0;
    }
    return 1;
}

// buf[c][0] = the last frame so far, then up to DEC_N new frames; 0 at the sound's end
static int refill(voice_t *v)
{
    const media_t *m = &g_media[v->media];
    int ch2 = m->ch > 1;
    v->buf[0][0] = v->buf[0][v->n];
    if (ch2) v->buf[1][0] = v->buf[1][v->n];
    v->p -= v->n;
    int got = 0, empty = 0;
    while (got < DEC_N) {
        if (v->next >= v->end) {
            if (v->loops == 1 || empty > 1) break;
            if (v->loops > 1) v->loops--;
            empty++;
            if (!dec_seek(v, m->has_loop && !v->clip ? m->loop_s : 0)) { g_st.decode_errors++; v->loops = 1; break; }
            voice_start_pass(v);
            continue;
        }
        if (v->budget == 0) break;
        int want = DEC_N - got;
        if ((uint32_t)want > v->end - v->next) want = v->end - v->next;
        if (v->budget > 0 && want > v->budget) want = (int)v->budget;
        int k = m->codec == C_ADPCM ? dec_adpcm(v, m, 1 + got, want) : m->codec == C_VORBIS ? dec_vorbis(v, m, 1 + got, want) : dec_pcm(v, m, 1 + got, want);
        if (k < 0) { g_st.decode_errors++; v->loops = 1; break; }
        if (k == 0) { v->end = v->next; if (v->loops == 1) break; continue; }   // shorter than said
        empty = 0;
        got += k;
        if (v->budget > 0) v->budget -= k;
    }
    v->n = got;
    return got > 0;
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
// a mono run four frames at a time: the eight samples gathered by lane, interpolated, scaled by
// the gain (ramping by dg a frame) and added in vectors; the rest one by one
static void mono_neon(const int16_t *b, int32_t *o, int run, uint32_t *pp, uint32_t *frp, uint32_t stp, int32_t g, int32_t dg)
{
    uint32_t p = *pp, fr = *frp;
    const uint32_t sv[4] = { 0, stp, 2 * stp, 3 * stp };
    const int32_t gv[4] = { g, g + dg, g + 2 * dg, g + 3 * dg };
    const uint32x4_t st4 = vld1q_u32(sv), mask = vdupq_n_u32(0xffff);
    int32x4_t g4 = vld1q_s32(gv);
    const int32x4_t dg4 = vdupq_n_s32(4 * dg);
    int k = 0;
    for (; k + 4 <= run; k += 4) {
        uint32_t f1 = fr + stp, f2 = f1 + stp, f3 = f2 + stp, f4 = f3 + stp;
        const int16_t *q0 = b + p, *q1 = q0 + (f1 >> 16), *q2 = q0 + (f2 >> 16), *q3 = q0 + (f3 >> 16);
        int16x4_t a = vdup_n_s16(0), c = vdup_n_s16(0);
        a = vld1_lane_s16(q0, a, 0); c = vld1_lane_s16(q0 + 1, c, 0);
        a = vld1_lane_s16(q1, a, 1); c = vld1_lane_s16(q1 + 1, c, 1);
        a = vld1_lane_s16(q2, a, 2); c = vld1_lane_s16(q2 + 1, c, 2);
        a = vld1_lane_s16(q3, a, 3); c = vld1_lane_s16(q3 + 1, c, 3);
        int32x4_t t = vreinterpretq_s32_u32(vshrq_n_u32(vandq_u32(vaddq_u32(vdupq_n_u32(fr), st4), mask), 2));
        int32x4_t sm = vaddq_s32(vmovl_s16(a), vshrq_n_s32(vmulq_s32(vsubl_s16(c, a), t), 14));
        int32x4_t r = vshrq_n_s32(vmulq_s32(sm, vshrq_n_s32(g4, 8)), 4);
        vst1q_s32(o + k, vaddq_s32(vld1q_s32(o + k), r));
        g4 = vaddq_s32(g4, dg4);
        p += f4 >> 16; fr = f4 & 0xffff;
    }
    g += dg * k;
    for (; k < run; k++) {
        int a = b[p], sm = a + (((b[p + 1] - a) * (int)(fr >> 2)) >> 14);
        o[k] += (sm * (g >> 8)) >> 4;
        g += dg;
        fr += stp; p += fr >> 16; fr &= 0xffff;
    }
    *pp = p; *frp = fr;
}
#define WW_NEON 1
#endif

// adds n frames into the integer sums (a mono source into mono[], a stereo one into st[] by pairs),
// the gain going from g to g1, resampled by linear interpolation; 0 when the sound ended. Integer
// arithmetic: 16-bit samples, the gain in 1/2^20 (used as 1/2^12), 2^23 a full-scale sample at 1.0
static int voice_mix(voice_t *v, int32_t *mono, int32_t *st, int n, float g0, float g1)
{
    int32_t g = (int32_t)(g0 * 1048576.0f), dg = (int32_t)((g1 - g0) * 1048576.0f / n);
    int stereo = g_media[v->media].ch > 1, done = 0;
    if (g0 > 1500 || g1 > 1500) g = dg = 0;      // beyond +63 dB: a mistake, not a sound
    while (done < n) {
        while (v->p >= v->n) if (!refill(v)) return 0;
        uint64_t avail = ((((uint64_t)(v->n - v->p)) << 16) - v->frac + v->step - 1) / v->step;
        int run = (uint64_t)(n - done) < avail ? n - done : (int)avail;
        uint32_t p = v->p, fr = v->frac, stp = v->step;
        if (!stereo) {
            const int16_t *b = v->buf[0];
            int32_t *o = mono + done;
            if (stp == 65536 && !fr && !dg) {   // the output's rate, no pitch: no interpolation
                int32_t gq = g >> 8;
                for (int k = 0; k < run; k++) o[k] += (b[p + k] * gq) >> 4;
                p += run;
            }
#ifdef WW_NEON
            else { mono_neon(b, o, run, &p, &fr, stp, g, dg); g += dg * run; }
#else
            else if (!dg) {
                int32_t gq = g >> 8;
                for (int k = 0; k < run; k++) {
                    int a = b[p], s = a + (((b[p + 1] - a) * (int)(fr >> 2)) >> 14);
                    o[k] += (s * gq) >> 4;
                    fr += stp; p += fr >> 16; fr &= 0xffff;
                }
            } else
                for (int k = 0; k < run; k++) {
                    int a = b[p], s = a + (((b[p + 1] - a) * (int)(fr >> 2)) >> 14);
                    o[k] += (s * (g >> 8)) >> 4;
                    g += dg;
                    fr += stp; p += fr >> 16; fr &= 0xffff;
                }
#endif
        } else {
            const int16_t *b0 = v->buf[0], *b1 = v->buf[1];
            int32_t *o = st + 2 * done;
            for (int k = 0; k < run; k++) {
                int t = (int)(fr >> 2), a0 = b0[p], a1 = b1[p], gq = g >> 8;
                int s0 = a0 + (((b0[p + 1] - a0) * t) >> 14), s1 = a1 + (((b1[p + 1] - a1) * t) >> 14);
                o[2 * k] += (s0 * gq) >> 4;
                o[2 * k + 1] += (s1 * gq) >> 4;
                g += dg;
                fr += stp; p += fr >> 16; fr &= 0xffff;
            }
        }
        v->p = p; v->frac = fr;
        done += run;
    }
    return 1;
}

// ---------------------------------------------------------------------------------------- playing
static int play_node(int ni, const ctx_t *c, int depth);
static int play_id(uint32_t id, const ctx_t *c, int depth) { int ni = im_get(&g_nmap, id); return ni >= 0 ? play_node(ni, c, depth) : 0; }

// a weighted pick that avoids the last `avoid` picks (hist, newest first)
static int pick_weighted(int n, const float *w, int wstride, int avoid, int *hist, int *nhist)
{
    if (n <= 0) return -1;
    if (avoid > n - 1) avoid = n - 1;
    if (avoid > 4) avoid = 4;
    for (int pass = 0; pass < 2; pass++) {
        float total = 0;
        for (int i = 0; i < n; i++) {
            int skip = 0;
            for (int h = 0; !pass && h < *nhist && h < avoid; h++) skip |= hist[h] == i;
            if (!skip) total += w ? (w[i * wstride] > 0 ? w[i * wstride] : 0) : 1;
        }
        if (total <= 0) continue;
        float r = frand() * total;
        int k = -1;
        for (int i = 0; i < n; i++) {
            int skip = 0;
            for (int h = 0; !pass && h < *nhist && h < avoid; h++) skip |= hist[h] == i;
            float wi = w ? (w[i * wstride] > 0 ? w[i * wstride] : 0) : 1;
            if (skip || wi <= 0) continue;
            k = i;
            if (r < wi) break;
            r -= wi;
        }
        if (k >= 0) {
            memmove(hist + 1, hist, 3 * sizeof *hist);
            hist[0] = k;
            if (*nhist < 4) (*nhist)++;
            return k;
        }
    }
    return -1;
}

static int pick_child(node_t *n)
{
    if (!n->nkids) return -1;
    if (n->type == N_SEQUENCE) { int k = n->seqpos % n->nkids; n->seqpos = (k + 1) % n->nkids; return k; }
    return pick_weighted(n->nkids, n->w, 1, n->avoid, n->hist, &n->nhist);
}

static const uint32_t *switch_kids(const node_t *n, uint64_t obj, int *cnt)
{
    uint32_t val = n->state ? state_get(n->group) : switch_get(n->group, obj);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n->nmap; i++)
            if (n->mval[i] == val) { *cnt = n->mcnt[i]; return n->mkids + n->moff[i]; }
        val = n->dflt;
    }
    *cnt = 0;
    return NULL;
}

// a continuous container: its children one after another, `loop` times
static int seq_step(int si)
{
    seq_t *s = &g_seq[si];
    node_t *n = &g_nodes[s->node];
    for (int tries = 0; tries <= n->nkids; tries++) {
        if (s->pos >= n->nkids) {
            s->pos = 0;
            if (s->left == 1) break;
            if (s->left > 1) s->left--;
        }
        int k = n->type == N_SEQUENCE ? s->pos : pick_child(n);
        s->pos++;
        if (k < 0) break;
        ctx_t c = s->c;
        c.seq = si;
        if (n->trans_delay && s->advance) c.delay += ms2f(n->trans_ms);
        s->advance = 0;
        if (play_id(n->kids[k], &c, 1) > 0 && s->active > 0) return 1;
    }
    s->used = 0;
    play_ref(s->c.playing, -1);
    return 0;
}

static int seq_new(int ni, const ctx_t *c)
{
    for (int i = 0; i < MAXSEQ; i++) {
        if (g_seq[i].used) continue;
        seq_t *s = &g_seq[i];
        memset(s, 0, sizeof *s);
        s->used = 1;
        s->node = ni;
        s->left = g_nodes[ni].loop;
        s->c = *c;
        play_ref(c->playing, 1);
        return seq_step(i);
    }
    return 0;
}

// --- interactive music
// what a music switch plays now: its decision tree on its arguments' values ("0" matches any)
static int music_target(int ni, uint64_t obj, int depth)
{
    node_t *n = &g_nodes[ni];
    if (n->type != N_MSWITCH) return ni;
    if (depth > 8) return -1;
    uint32_t leaf = n->dleaf;
    if (n->dn) {
        int first = n->dfirst, cnt = n->dn;
        leaf = 0;
        for (int a = 0; cnt > 0; a++) {
            uint32_t val = a < n->nargs ? (n->astate[a] ? state_get(n->agroup[a]) : switch_get(n->agroup[a], obj)) : 0;
            const dec_t *hit = NULL;
            for (int i = 0; i < cnt; i++) if (g_dec[first + i].key == val) hit = &g_dec[first + i];
            for (int i = 0; !hit && i < cnt; i++) if (g_dec[first + i].key == 0) hit = &g_dec[first + i];
            if (!hit) break;
            if (hit->leaf) { leaf = hit->val; break; }
            first = hit->first;
            cnt = hit->n;
        }
    }
    int ci = leaf ? im_get(&g_nmap, leaf) : -1;
    return ci >= 0 && ci != ni ? music_target(ci, obj, depth + 1) : -1;
}

// the next segment of a playlist's item tree; -1 when the item's visit is over
static int item_next(mus_t *m, int it, int depth)
{
    if (it < 0 || depth > 32) return -1;
    item_t *x = &g_items[it];
    ist_t *s = &m->ist[it - g_nodes[m->target].item];
    if (x->kind == I_SEGMENT) {         // a segment, `loop` times (0: forever)
        if (!s->active) { s->active = 1; s->left = x->loop; }
        else if (s->left == 1) { s->active = 0; return -1; }
        else if (s->left > 1) s->left--;
        int si = im_get(&g_nmap, x->seg);
        return si >= 0 && g_nodes[si].type == N_SEGMENT ? si : -1;
    }
    if (!x->nkids) return -1;
    const int *kids = g_ikids + x->first;
    if (!s->active) {
        s->active = 1;
        s->left = x->loop;
        s->pos = 0;
        if (x->kind == I_SEQSTEP) s->cur = s->step++ % x->nkids;
        else if (x->kind != I_SEQUENCE) {
            float w[256];
            int n = x->nkids < 256 ? x->nkids : 256;
            for (int i = 0; i < n; i++) w[i] = g_items[kids[i]].weight;
            s->cur = pick_weighted(n, w, 1, x->avoid, s->hist, &s->nhist);
        }
    }
    for (int guard = 0; guard < 2 * x->nkids + 2; guard++) {
        int child = x->kind == I_SEQUENCE ? s->pos : s->cur;
        if (child < 0 || child >= x->nkids) break;
        int r = item_next(m, kids[child], depth + 1);
        if (r >= 0) return r;
        if (x->kind == I_SEQSTEP || x->kind == I_RANDSTEP) break;
        if (x->kind == I_SEQUENCE && ++s->pos < x->nkids) continue;
        if (s->left == 1) break;
        if (s->left > 1) s->left--;
        s->pos = 0;
        if (x->kind == I_RANDOM) {
            float w[256];
            int n = x->nkids < 256 ? x->nkids : 256;
            for (int i = 0; i < n; i++) w[i] = g_items[kids[i]].weight;
            s->cur = pick_weighted(n, w, 1, x->avoid, s->hist, &s->nhist);
        }
    }
    s->active = 0;
    return -1;
}

static int mus_retarget(mus_t *m, int target)
{
    if (target == m->target) return 0;
    m->target = target;
    m->lone = 0;
    free(m->ist);
    m->ist = NULL;
    m->nist = target >= 0 && g_nodes[target].type == N_PLAYLIST ? g_nodes[target].nitems : 0;
    if (m->nist) m->ist = calloc(m->nist, sizeof *m->ist);
    return 1;
}

// the next segment: from the current target (the switch's choice now)
static int mus_next(int mi)
{
    mus_t *m = &g_mus[mi];
    mus_retarget(m, music_target(m->root, m->c.obj, 0));
    if (m->target < 0) return -1;
    node_t *t = &g_nodes[m->target];
    if (t->type == N_SEGMENT) { if (m->lone) return -1; m->lone = 1; return m->target; }
    if (t->type != N_PLAYLIST || !m->ist) return -1;
    return item_next(m, t->item, 0);
}

static uint64_t seg_entry(int seg) { return seg >= 0 ? (uint64_t)ms2f(g_nodes[seg].entry_ms) : 0; }

static float seg_exit(const node_t *s)     // the exit cue; without one the end, or the clips' end
{
    if (s->exit_ms > 0) return s->exit_ms;
    if (s->dur_ms > 0) return s->dur_ms;
    float e = 0;
    for (int i = 0; i < s->nkids; i++) {
        int ti = im_get(&g_nmap, s->kids[i]);
        for (int k = 0; ti >= 0 && k < g_nodes[ti].nclips; k++) {
            clip_t *c = &g_nodes[ti].clips[k];
            int mi = im_get(&g_mmap, c->media);
            float src = c->src > 0 ? c->src : mi >= 0 ? g_media[mi].samples * 1000.0f / g_media[mi].rate : 0;
            if (c->at + src + c->end > e) e = c->at + src + c->end;
        }
    }
    return e > 10 ? e : 10;
}

static int track_sub(node_t *t, uint64_t obj)
{
    switch (t->tkind) {
    case T_RANDOM: return t->subtracks > 0 ? (int)(rnd() % t->subtracks) : 0;
    case T_SEQUENCE: return t->subtracks > 0 ? t->seqpos++ % t->subtracks : 0;
    case T_SWITCH: { int cnt; const uint32_t *k = switch_kids(t, obj, &cnt); return cnt ? (int)k[0] : 0; }
    }
    return -1;                          // all clips
}

// a segment's tracks: each clip from its place on the segment's timeline (looping its media when
// the clip is longer), from `at` (output frames)
static void mus_start_segment(int mi, int seg, uint64_t at)
{
    mus_t *m = &g_mus[mi];
    node_t *s = &g_nodes[seg];
    m->seg = seg;
    m->seg_start = at;
    ctx_t c = m->c;
    c.mus = mi;
    c.fade_ms = m->fade_in;
    for (int i = 0; i < s->nkids; i++) {
        int ti = im_get(&g_nmap, s->kids[i]);
        if (ti < 0 || g_nodes[ti].type != N_TRACK) continue;
        node_t *t = &g_nodes[ti];
        int sub = track_sub(t, c.obj);
        for (int k = 0; k < t->nclips; k++) {
            clip_t *cl = &t->clips[k];
            int mdi = im_get(&g_mmap, cl->media);
            if (mdi < 0 || (sub >= 0 && cl->sub != sub)) continue;
            media_t *md = &g_media[mdi];
            float src = cl->src > 0 ? cl->src : md->samples * 1000.0f / md->rate;
            float t0 = cl->at + cl->begin, t1 = cl->at + src + cl->end;
            if (t0 < 0) t0 = 0;
            if (t1 <= t0) continue;
            double pos = fmod((double)(t0 - cl->at), src);
            uint32_t from = (uint32_t)(pos * md->rate / 1000);
            long budget = (long)((t1 - t0) * md->rate / 1000);
            if (from >= md->samples) from = 0;
            c.delay = (int)(at + (uint64_t)ms2f(t0) > g_clock ? at + (uint64_t)ms2f(t0) - g_clock : 0);
            voice_new(ti, cl->media, from, budget > 0 ? budget : 1, &c);
        }
    }
    m->fade_in = 0;
    m->next_start = at + (uint64_t)ms2f(seg_exit(s));
}

// the next segment starts where its entry cue meets the current one's exit cue
static void mus_schedule(int mi, int frames)
{
    mus_t *m = &g_mus[mi];
    for (int guard = 0; !m->ended && guard < 16; guard++) {
        if (m->nseg < 0) { if (m->target >= 0) m->ended = 1; break; }    // no target: silent until a change
        uint64_t e = seg_entry(m->nseg), at = m->next_start > e ? m->next_start - e : 0;
        if (at >= g_clock + frames) break;
        if (at < g_clock) at = g_clock;
        if (m->fade_out > 0)            // the transition's fade on what plays (its post-exit)
            for (int k = 0; k < g_maxv; k++) if (g_v[k].used && g_v[k].mus == mi) voice_stop(&g_v[k], m->fade_out);
        m->fade_out = 0;
        mus_start_segment(mi, m->nseg, at);
        if (m->next_start <= at) m->next_start = at + 1;
        m->nseg = mus_next(mi);
    }
    if (m->ended && m->nvoices <= 0) { m->used = 0; free(m->ist); m->ist = NULL; play_ref(m->c.playing, -1); }
}

static int music_new(int ni, const ctx_t *c)
{
    for (int i = 0; i < MAXMUS; i++) {
        if (g_mus[i].used) continue;
        mus_t *m = &g_mus[i];
        memset(m, 0, sizeof *m);
        m->used = 1;
        m->root = ni;
        m->c = *c;
        m->target = -2;                 // nothing yet: mus_next picks it
        m->seg = -1;
        m->fade_in = c->fade_ms;
        m->nseg = mus_next(i);
        m->next_start = g_clock + (uint64_t)c->delay + seg_entry(m->nseg);
        play_ref(c->playing, 1);
        mus_schedule(i, 0);
        return m->used;
    }
    return 0;
}

static int id_in(const uint32_t *l, int n, uint32_t id) { for (int i = 0; i < n; i++) if (l[i] == id || l[i] == 0xffffffffu) return 1; return 0; }

// the transition rule between two of a switch's children: the last that matches (Wwise reads the
// matrix from the bottom up)
static const rule_t *mus_rule(int root, int from, int to)
{
    const rule_t *r = NULL;
    uint32_t f = from >= 0 ? g_nodes[from].id : 0, t = to >= 0 ? g_nodes[to].id : 0;
    node_t *n = &g_nodes[root];
    for (int i = 0; i < n->nrules; i++)
        if (id_in(n->rules[i].src, n->rules[i].nsrc, f) && id_in(n->rules[i].dst, n->rules[i].ndst, t)) r = &n->rules[i];
    return r;
}

// a state or switch changed: where a music switch now picks another child, the next segment comes
// from it: at once (the rule's sync immediate, or on a grid, bar or beat, which step 1 does not
// follow), else at the current segment's exit cue; with the rule's fades
static void music_sync_changed(void)
{
    for (int i = 0; i < MAXMUS; i++) {
        mus_t *m = &g_mus[i];
        if (!m->used || m->ended) continue;
        int target = music_target(m->root, m->c.obj, 0);
        if (target == m->target) continue;
        const rule_t *r = mus_rule(m->root, m->target, target);
        int now = r && r->sync >= 0 && r->sync <= 3;
        m->fade_in = r ? r->fin : 0;
        m->fade_out = r ? r->fout : 0;
        m->nseg = mus_next(i);
        if (!now) continue;
        for (int k = 0; k < g_maxv; k++) if (g_v[k].used && g_v[k].mus == i) voice_stop(&g_v[k], m->fade_out > 30 ? m->fade_out : 30);
        m->fade_out = 0;
        m->next_start = g_clock + seg_entry(m->nseg);
    }
}

static int play_node(int ni, const ctx_t *c, int depth)
{
    node_t *n = &g_nodes[ni];
    int started = 0, cnt;
    if (depth > 32 || (n->prob < 100 && frand() * 100 >= n->prob)) return 0;     // its play chance
    switch (n->type) {
    case N_SOUND: return voice_new(ni, 0, 0, -1, c) != NULL;
    case N_RANDOM:
    case N_SEQUENCE:
        if (n->cont) return seq_new(ni, c);
        { int k = pick_child(n); return k >= 0 ? play_id(n->kids[k], c, depth + 1) : 0; }
    case N_SWITCH: {
        const uint32_t *k = switch_kids(n, c->obj, &cnt);
        for (int i = 0; i < cnt; i++) started += play_id(k[i], c, depth + 1);
        return started;
    }
    case N_LAYER:
        for (int i = 0; i < n->nkids; i++) started += play_id(n->kids[i], c, depth + 1);
        return started;
    case N_MSWITCH: case N_PLAYLIST: case N_SEGMENT:
        return music_new(ni, c);
    }
    return 0;
}

// is ni at or under target (in the parent links)?
static int is_under(int ni, int target)
{
    for (int depth = 0; ni >= 0 && depth < 64; depth++) {
        if (ni == target) return 1;
        uint32_t p = g_nodes[ni].parent;
        int pi = p ? im_get(&g_nmap, p) : -1;
        ni = pi == ni ? -1 : pi;
    }
    return 0;
}

static int excepted(const act_t *a, const voice_t *v)
{
    for (int i = 0; i < a->nexcept; i++) {
        int ni = im_get(&g_nmap, a->except[i]);
        if (ni >= 0 && in_chain(v->chain, v->nchain, ni)) return 1;
    }
    return 0;
}

// stop / pause / resume what a node (and what is under it) plays, or everything (target -1);
// on one game object or all
static void node_action(int type, int target, int one_obj, uint64_t obj, float fade_ms, const act_t *a)
{
    for (int i = 0; i < g_maxv; i++) {
        voice_t *v = &g_v[i];
        if (!v->used || (one_obj && v->obj != obj)) continue;
        if (target >= 0 ? !in_chain(v->chain, v->nchain, target) && !is_under(v->node, target) : a && excepted(a, v)) continue;
        if (type == A_STOP) voice_stop(v, fade_ms);
        else v->paused = type == A_PAUSE;
    }
    if (type != A_STOP) return;
    for (int i = 0; i < MAXSEQ; i++) {
        seq_t *s = &g_seq[i];
        if (s->used && (!one_obj || s->c.obj == obj) && (target < 0 || is_under(s->node, target))) { s->left = 1; s->pos = 1 << 30; }
    }
    for (int i = 0; i < MAXMUS; i++) {
        mus_t *m = &g_mus[i];
        if (m->used && (!one_obj || m->c.obj == obj) && (target < 0 || is_under(m->root, target))) m->ended = 1;
    }
}

static void stop_where(int (*match)(uint32_t playing, uint64_t obj, uint64_t a, uint32_t b), uint64_t a, uint32_t b, float fade_ms)
{
    for (int i = 0; i < g_maxv; i++) if (g_v[i].used && match(g_v[i].playing, g_v[i].obj, a, b)) voice_stop(&g_v[i], fade_ms);
    for (int i = 0; i < MAXSEQ; i++) if (g_seq[i].used && match(g_seq[i].c.playing, g_seq[i].c.obj, a, b)) { g_seq[i].left = 1; g_seq[i].pos = 1 << 30; }
    for (int i = 0; i < MAXMUS; i++) if (g_mus[i].used && match(g_mus[i].c.playing, g_mus[i].c.obj, a, b)) g_mus[i].ended = 1;
    for (int i = 0; i < MAXPEND; i++) if (g_pend[i].used && match(g_pend[i].playing, g_pend[i].obj, a, b)) { g_pend[i].used = 0; play_ref(g_pend[i].playing, -1); }
}
static int m_all(uint32_t p, uint64_t o, uint64_t a, uint32_t b) { (void)p; (void)o; (void)a; (void)b; return 1; }
static int m_obj(uint32_t p, uint64_t o, uint64_t a, uint32_t b) { (void)p; (void)b; return o == a; }
static int m_playing(uint32_t p, uint64_t o, uint64_t a, uint32_t b) { (void)o; (void)a; return p == b; }

static void state_set(uint32_t group, uint32_t value)
{
    uint32_t old = state_get(group);   // what it was (the default before the game set one)
    sync_t *s = sync_find(g_states, g_nstates, group, 0);
    if (!s) { GROW(g_states, g_nstates, g_cstates); s = &g_states[g_nstates++]; s->group = group; s->obj = 0; s->value = old; }
    if (s->value == value) return;
    float ms = -1;                      // the group's fade: a transition's, else its default
    for (int i = 0; i < g_nsfade; i++)
        if (g_sfade[i].group == group && ((g_sfade[i].from == old && g_sfade[i].to == value) || (ms < 0 && !g_sfade[i].from && !g_sfade[i].to)))
            ms = g_sfade[i].ms;
    s->value = value;
    g_bus_dirty = 1;
    g_gen++;
    if (ms > 0 && ms2f(ms) > g_glide) g_glide = ms2f(ms);
    music_sync_changed();
}

static void switch_set(uint32_t group, uint32_t value, uint64_t obj)
{
    sync_t *s = sync_find(g_switches, g_nswitches, group, obj);
    if (!s) { GROW(g_switches, g_nswitches, g_cswitches); s = &g_switches[g_nswitches++]; s->group = group; s->obj = obj; s->value = 0; }
    if (s->value == value) return;
    s->value = value;
    for (int i = 0; i < MAXMUS; i++) if (g_mus[i].used && g_mus[i].c.obj == obj) { music_sync_changed(); break; }
}

static void rtpc_set(uint32_t id, float value)
{
    rtpcv_t *v = NULL;
    for (int i = 0; i < g_nrtpcv; i++) if (g_rtpcv[i].id == id) v = &g_rtpcv[i];
    if (v && v->value == value) return;
    if (!v) { GROW(g_rtpcv, g_nrtpcv, g_crtpcv); v = &g_rtpcv[g_nrtpcv++]; v->id = id; }
    v->value = value;
    for (int i = 0; i < g_nbinds; i++) if (g_binds[i].rtpc == id) { rtpc_apply(); break; }
}

static int run_action(const act_t *a, uint32_t playing, uint64_t obj, int delay)
{
    int ni = a->target && !a->bus ? im_get(&g_nmap, a->target) : -1;
    switch (a->type) {
    case A_PLAY: {
        if (ni < 0) return 0;
        ctx_t c = { playing, obj, a->fade_ms, delay, -1, -1 };
        return play_node(ni, &c, 0);
    }
    case A_STOP: case A_PAUSE: case A_RESUME:
        if (ni >= 0) node_action(a->type, ni, a->object, obj, a->fade_ms, a);
        return 0;
    case A_STOPALL: case A_PAUSEALL: case A_RESUMEALL:
        node_action(a->type == A_STOPALL ? A_STOP : a->type == A_PAUSEALL ? A_PAUSE : A_RESUME, -1, a->object, obj, a->fade_ms, a);
        return 0;
    case A_STATE: state_set(a->group, a->value); return 0;
    case A_SWITCH: switch_set(a->group, a->value, obj); return 0;
    case A_RTPC: rtpc_set(a->group, a->absolute ? a->amount : rtpc_get(a->group) + a->amount); return 0;
    case A_BUSVOL: case A_VOL: case A_RESETBUSVOL: case A_RESETVOL: {
        int reset = a->type == A_RESETBUSVOL || a->type == A_RESETVOL, bi = im_get(&g_bmap, a->target);
        float *x = bi >= 0 && (a->bus || a->type == A_BUSVOL || a->type == A_RESETBUSVOL) ? &g_buses[bi].extra_db
                 : im_get(&g_nmap, a->target) >= 0 ? &g_nodes[im_get(&g_nmap, a->target)].extra_db : NULL;
        if (x) { *x = reset ? 0 : a->absolute ? a->amount : *x + a->amount; g_bus_dirty = 1; g_gen++; }
        return 0;
    }
    }
    return 0;
}

// --------------------------------------------------------------------------------------- the mix
static void stats_log(double now)
{
    wwplay_stats s = g_st, d;
    double dt = now - g_stats_wall;
    if (dt <= 0) return;
    d.posted = s.posted - g_st_last.posted;
    d.played = s.played - g_st_last.played;
    d.voices_started = s.voices_started - g_st_last.voices_started;
    d.dropped = s.dropped - g_st_last.dropped;
    d.mix_cpu_s = s.mix_cpu_s - g_st_last.mix_cpu_s;
    d.call_cpu_s = s.call_cpu_s - g_st_last.call_cpu_s;
    d.mixed_s = s.mixed_s - g_st_last.mixed_s;
    logf_("player, last %.0f s: %lu events (%lu played), %lu voices started, %d now (peak %d, %lu dropped at the cap of %d), "
          "%d music, mix %.2f %% of a core, calls %.2f %%, %.1f s mixed at %d Hz",
          dt, d.posted, d.played, d.voices_started, g_nv, g_vpeak, d.dropped, g_maxv, s.musics,
          100 * d.mix_cpu_s / dt, 100 * d.call_cpu_s / dt, d.mixed_s, g_rate);
    g_st_last = s;
    g_stats_wall = now;
    g_vpeak = g_nv;
}

// The master's brick-wall peak limiter (Gungeon's master bus has one in Wwise; a flood of voices
// would clip without it): the mix delayed by two blocks of 32 frames (1.3 ms at 48 kHz), each
// block's gain the ceiling over its peak; while a block goes out the gain ramps to the smaller of
// its own and the next block's, so no sample passes the ceiling and the gain never jumps; it
// recovers with a 60 ms time constant. UA_WWISE_LIMITER=0 turns it off.
#define LIM_N 32
#define LIM_CEIL 0.98f
static struct { float d[2][2 * LIM_N], need[2], peak, g, dg, rel; int i, w, on, rate; } g_lim = { .need = { 1, 1 }, .g = 1, .on = 1 };

static void limiter(float *x, int frames)
{
    if (g_lim.rate != g_rate) { g_lim.rate = g_rate; g_lim.rel = 1 - expf(-(float)LIM_N / (0.06f * g_rate)); }
    for (int k = 0; k < frames;) {
        int n = LIM_N - g_lim.i < frames - k ? LIM_N - g_lim.i : frames - k;
        float *d = &g_lim.d[g_lim.w][2 * g_lim.i], *o = x + 2 * k, g = g_lim.g, dg = g_lim.dg, pk = g_lim.peak;
        for (int j = 0; j < 2 * n; j += 2) {
            float l = o[j], r = o[j + 1], al = fabsf(l), ar = fabsf(r);
            o[j] = d[j] * g;                    // what came in two blocks ago
            o[j + 1] = d[j + 1] * g;
            g += dg;
            d[j] = l; d[j + 1] = r;
            pk = al > pk ? al : pk;
            pk = ar > pk ? ar : pk;
        }
        g_lim.g = g; g_lim.peak = pk;
        k += n;
        if ((g_lim.i += n) < LIM_N) continue;
        // a block is in: the one going out next is the other slot's; ramp over it to the gain both need
        g_lim.i = 0;
        g_lim.need[g_lim.w] = pk > LIM_CEIL ? LIM_CEIL / pk : 1;
        g_lim.peak = 0;
        g_lim.w ^= 1;
        float t = g_lim.need[0] < g_lim.need[1] ? g_lim.need[0] : g_lim.need[1], up = g + (1 - g) * g_lim.rel;
        if (t > up) t = up;
        g_lim.dg = (t - g) / LIM_N;
    }
}

static void drain_locked(void);

static void mix_locked(float *out, int frames)
{
    int32_t *mono = g_acc, *st = g_acc + frames;
    memset(out, 0, (size_t)frames * 2 * sizeof *out);
    if (g_suspended) return;
    drain_locked();
    memset(g_acc, 0, (size_t)frames * 3 * sizeof *g_acc);
    for (int i = 0; i < MAXPEND; i++) {          // delayed actions that are due
        pend_t *p = &g_pend[i];
        if (!p->used || p->due >= g_clock + frames) continue;
        p->used = 0;
        run_action(&g_acts[p->act], p->playing, p->obj, (int)(p->due > g_clock ? p->due - g_clock : 0));
        play_ref(p->playing, -1);
    }
    for (int i = 0; i < MAXSEQ; i++) if (g_seq[i].used && g_seq[i].advance && g_seq[i].active <= 0) seq_step(i);
    for (int i = 0; i < MAXMUS; i++) if (g_mus[i].used) mus_schedule(i, frames);
    for (int i = 0; i < g_maxv; i++) {
        voice_t *v = &g_v[i];
        if (!v->used || v->paused) continue;
        int off = 0, n = frames;
        if (v->delay) {
            if (v->delay >= frames) { v->delay -= frames; continue; }
            off = v->delay; n -= off; v->delay = 0;
        }
        if (v->gen != g_gen) { voice_params(v); if (g_glide > v->glide) v->glide = g_glide; }
        float f1 = v->fade + v->fade_rate * n, g1;
        if (f1 >= 1) { f1 = 1; if (v->fade_rate > 0) v->fade_rate = 0; }
        if (f1 < 0) f1 = 0;
        g1 = v->gt * f1;
        if (v->glide > n) { g1 = v->gc + (g1 - v->gc) * n / v->glide; v->glide -= n; }
        else v->glide = 0;
        int alive = voice_mix(v, mono + off, st + 2 * off, n, v->gc, g1);
        v->gc = g1;
        v->fade = f1;
        if (!alive || (v->fade_rate < 0 && f1 <= 0)) voice_free(v);
    }
    int nm = 0;
    for (int i = 0; i < MAXMUS; i++) if (g_mus[i].used) { nm++; if (g_mus[i].ended && g_mus[i].nvoices <= 0) mus_schedule(i, 0); }
    g_st.musics = nm;
    g_glide = 0;
    g_clock += frames;
    const float k = 1.0f / 8388608, km = k * 0.70710678f;     // mono sources centred: -3 dB a side
    for (int i = 0; i < frames; i++) {
        float m = mono[i] * km;
        out[2 * i] = st[2 * i] * k + m;
        out[2 * i + 1] = st[2 * i + 1] * k + m;
    }
    if (g_lim.on) limiter(out, frames);
}

static void mix(float *buf, int frames, int rate, int channels)
{
    if (frames <= 0 || channels <= 0) return;
    double c0 = cpu_now();
    static int said;
    if (!said) { said = 1; logf_("player: the output takes the mix: %d Hz, %d channels, %d frames a buffer", rate, channels, frames); }
    pthread_mutex_lock(&L);
    if (rate > 0 && rate != g_rate) { g_rate = rate; g_gen++; }
    if (frames > g_scap) {
        g_scap = frames;
        g_scratch = realloc(g_scratch, (size_t)g_scap * 2 * sizeof *g_scratch);
        g_acc = realloc(g_acc, (size_t)g_scap * 3 * sizeof *g_acc);
    }
    float *m = g_scratch;
    mix_locked(m, frames);
    g_last_mix_wall = wall_now();
    if (g_cap.f) {
        wwplay_wav_write(&g_cap, m, frames);
        if (g_cap.frames >= g_cap_frames) { wwplay_wav_close(&g_cap); logf_("capture written (%lu frames)", g_cap_frames); }
    }
    pthread_mutex_unlock(&L);
    if (channels == 2) for (int i = 0; i < frames * 2; i++) buf[i] += m[i];
    else if (channels == 1) for (int i = 0; i < frames; i++) buf[i] += 0.5f * (m[2 * i] + m[2 * i + 1]);
    else for (int i = 0; i < frames; i++) { buf[i * channels] += m[2 * i]; buf[i * channels + 1] += m[2 * i + 1]; }
    flush_done();
    pthread_mutex_lock(&L);
    g_st.mix_cpu_s += cpu_now() - c0;
    g_st.mixed_s += (double)frames / g_rate;
    pthread_mutex_unlock(&L);
}

// ---------------------------------------------------------------------------------------- backend
// The game's calls do not wait for the mixer: they go into a queue that the mixer reads at the start
// of its next block (as Wwise runs posted events at its next audio frame), so a mix in progress never
// holds up the game's thread. Only LoadBank takes the player's lock. PostEvent answers at once: a
// playing ID for an event the banks have (Wwise's answer too, also when it plays nothing: its end
// comes through host->event_done), 0 for one they do not.
enum { Q_POST, Q_STOP, Q_STOPALL, Q_RTPC, Q_STATE, Q_SWITCH, Q_UNREG, Q_EAOE, Q_TERM };
typedef struct { uint8_t op; int32_t i; uint32_t a, b; uint64_t obj; float f; } cmd_t;
#define QN 4096
static cmd_t g_q[QN];
static unsigned g_qh, g_qt;             // read and write counts
static pthread_mutex_t Q = PTHREAD_MUTEX_INITIALIZER;

static void do_post(uint32_t event, uint64_t obj, uint32_t playing)
{
    int ei = im_get(&g_emap, event);
    if (ei < 0) { GROW(g_done, g_ndone, g_cdone); g_done[g_ndone++] = playing; return; }
    play_t *p = NULL;
    for (int i = 0; i < g_nplay && !p; i++) if (!g_play[i].used) p = &g_play[i];
    if (!p) { GROW(g_play, g_nplay, g_cplay); p = &g_play[g_nplay++]; }
    *p = (play_t){ 1, playing, event, obj, 1 };     // one ref while the actions run
    event_t *e = &g_evs[ei];
    for (int i = 0; i < e->n; i++) {
        act_t *a = &g_acts[e->first + i];
        if (a->delay_ms > 0 && a->type != A_PLAY) {
            for (int k = 0; k < MAXPEND; k++)
                if (!g_pend[k].used) {
                    g_pend[k] = (pend_t){ 1, e->first + i, g_clock + ms2f(a->delay_ms), playing, obj };
                    play_ref(playing, 1);
                    break;
                }
            continue;
        }
        run_action(a, playing, obj, ms2f(a->delay_ms));
    }
    p = play_find(playing);
    if (p && p->refs > 1) g_st.played++;
    play_ref(playing, -1);                      // nothing started: it ends now
}

// ExecuteActionOnEvent: stop (0), pause (1) or resume (2) what an event plays
static void do_eaoe(uint32_t ev, int action, uint64_t obj, float ms, uint32_t pid)
{
    for (int i = 0; i < g_maxv; i++) {
        voice_t *v = &g_v[i];
        if (!v->used) continue;
        play_t *p = play_find(v->playing);
        if (!p || p->event != ev || (obj != ~0ull && v->obj != obj) || (pid && v->playing != pid)) continue;
        if (action == 0) voice_stop(v, ms);
        else if (action == 1) v->paused = 1;
        else if (action == 2) v->paused = 0;
    }
    if (action == 0)
        for (int i = 0; i < MAXMUS; i++) {
            mus_t *m = &g_mus[i];
            play_t *p = m->used ? play_find(m->c.playing) : NULL;
            if (p && p->event == ev && (obj == ~0ull || m->c.obj == obj) && (!pid || m->c.playing == pid)) m->ended = 1;
        }
}

static void run_cmd(const cmd_t *c)
{
    switch (c->op) {
    case Q_POST: __atomic_add_fetch(&g_st.posted, 1, __ATOMIC_RELAXED); do_post(c->a, c->obj, c->b); break;
    case Q_STOP: stop_where(m_playing, 0, c->b, 0); break;
    case Q_STOPALL: stop_where(c->i ? m_all : m_obj, c->obj, 0, 0); break;
    case Q_RTPC: rtpc_set(c->a, c->f); break;
    case Q_STATE: state_set(c->a, c->b); break;
    case Q_SWITCH: switch_set(c->a, c->b, c->obj); break;
    case Q_UNREG:                               // Wwise stops what an unregistered object plays
        stop_where(m_obj, c->obj, 0, 0);
        for (int i = 0; i < g_nswitches;) if (g_switches[i].obj == c->obj) g_switches[i] = g_switches[--g_nswitches]; else i++;
        break;
    case Q_EAOE: do_eaoe(c->a, c->i, c->obj, c->f, c->b); break;
    case Q_TERM:
        stop_where(m_all, 0, 0, 0);
        for (int i = 0; i < g_maxv; i++) voice_free(&g_v[i]);
        break;
    }
}

static void drain_locked(void)              // the queued calls, in order (the player's lock held)
{
    cmd_t c[64];
    for (;;) {
        pthread_mutex_lock(&Q);
        int n = 0;
        while (g_qh != g_qt && n < 64) c[n++] = g_q[g_qh++ % QN];
        pthread_mutex_unlock(&Q);
        if (!n) return;
        for (int i = 0; i < n; i++) run_cmd(&c[i]);
    }
}

static void push(cmd_t c)
{
    pthread_mutex_lock(&Q);
    while (g_qt - g_qh >= QN) {                 // full (no output reads the mix): run them here
        pthread_mutex_unlock(&Q);
        pthread_mutex_lock(&L);
        drain_locked();
        pthread_mutex_unlock(&L);
        flush_done();
        pthread_mutex_lock(&Q);
    }
    g_q[g_qt++ % QN] = c;
    pthread_mutex_unlock(&Q);
}

static int b_init(const char *base, const char *lang) { (void)base; (void)lang; return UA_AK_SUCCESS; }

static int b_load_bank(const char *name, uint32_t id, const void *data, uint32_t size, uint32_t *bank_id)
{
    (void)data; (void)size;
    double c0 = cpu_now();
    pthread_mutex_lock(&L);
    drain_locked();                             // what came before, on the banks loaded before
    if (name) bank_load(name, bank_id);
    else if (id) {
        int known = 0;
        for (int i = 0; i < g_nbanks; i++) if (g_banks[i].id == id) known = 1;
        if (!known) logf_("bank %u (by ID or from memory): not known to the player", id);
    }
    g_st.call_cpu_s += cpu_now() - c0;
    pthread_mutex_unlock(&L);
    flush_done();
    return UA_AK_SUCCESS;
}

static int b_unload_bank(uint32_t id) { (void)id; return UA_AK_SUCCESS; }   // the data stays (mapped, small)

static int b_post_event(uint32_t event, uint64_t obj, uint32_t playing, uint32_t flags)
{
    (void)flags;
    if (im_get(&g_emap, event) < 0) {           // the banks load on this thread: no race with LoadBank
        __atomic_add_fetch(&g_st.posted, 1, __ATOMIC_RELAXED);
        return 0;
    }
    push((cmd_t){ Q_POST, 0, event, playing, obj, 0 });
    return 1;
}

static void b_stop(uint32_t playing) { push((cmd_t){ Q_STOP, 0, 0, playing, 0, 0 }); }
static void b_stop_all(uint64_t obj, int every) { push((cmd_t){ Q_STOPALL, every, 0, 0, obj, 0 }); }
static void b_set_rtpc(uint32_t id, float value, uint64_t obj, uint32_t playing, int every)
{
    (void)obj; (void)playing; (void)every;      // step 1: every value is global
    push((cmd_t){ Q_RTPC, 0, id, 0, 0, value });
}
static void b_set_state(uint32_t group, uint32_t state) { push((cmd_t){ Q_STATE, 0, group, state, 0, 0 }); }
static void b_set_switch(uint32_t group, uint32_t value, uint64_t obj) { push((cmd_t){ Q_SWITCH, 0, group, value, obj, 0 }); }
static void b_game_object(uint64_t obj, const char *name, int registered) { (void)name; if (!registered) push((cmd_t){ Q_UNREG, 0, 0, 0, obj, 0 }); }
static void b_suspend(int on) { __atomic_store_n(&g_suspended, on, __ATOMIC_RELAXED); }
static void b_term(void)
{
    push((cmd_t){ Q_TERM, 0, 0, 0, 0, 0 });
    pthread_mutex_lock(&L);
    drain_locked();
    pthread_mutex_unlock(&L);
    flush_done();
}

static void b_render(void)                      // once a frame, on the game's thread
{
    double now = wall_now();
    pthread_mutex_lock(&L);
    int stalled = g_last_mix_wall > 0 ? now - g_last_mix_wall > 0.5 : now - g_stats_wall > 1.0;
    int busy = g_nv > 0 || g_nplay > 0 || g_qh != g_qt;
    double since = g_last_mix_wall > 0 ? now - g_last_mix_wall : 0;
    pthread_mutex_unlock(&L);
    // no output reads the mix (none open, or it stopped): time goes on silently, so events end
    if (stalled && busy) {
        int frames = (int)((since > 0 && since < 1 ? since : 0.5) * g_rate);
        float *tmp = malloc((size_t)frames * 2 * sizeof *tmp);
        if (tmp) { memset(tmp, 0, (size_t)frames * 2 * sizeof *tmp); mix(tmp, frames, g_rate, 2); free(tmp); }
    }
    if (g_stats_every > 0 && now - g_stats_wall >= g_stats_every) { pthread_mutex_lock(&L); stats_log(now); pthread_mutex_unlock(&L); }
}

// ExecuteActionOnEvent(event, action, [game object, [transition ms, [curve, [playing ID]]]]):
// stop, pause or resume what that event plays
static int b_call(const ua_ak_call *c, ua_ak_value *ret)
{
    if (strcmp(c->api, "ExecuteActionOnEvent") || c->nargs < 2) return 0;
    uint32_t ev = c->args[0][0] == 'z' ? (c->a[0].z && H && H->id ? H->id(c->a[0].z) : 0) : (uint32_t)c->a[0].u;
    int action = (int)c->a[1].i;
    uint64_t obj = c->nargs > 2 ? c->a[2].u : ~0ull;
    float ms = c->nargs > 3 ? (float)c->a[3].i : 0;
    uint32_t pid = c->nargs > 5 ? (uint32_t)c->a[5].u : 0;
    push((cmd_t){ Q_EAOE, action, ev, pid, obj, ms });
    ret->i = UA_AK_SUCCESS;
    return 1;
}

static const ua_wwise_backend g_backend = {
    .version = UA_WWISE_BACKEND_VERSION, .name = "ua_host player (wwplay)",
    .call = b_call, .init = b_init, .term = b_term, .render = b_render, .suspend = b_suspend,
    .load_bank = b_load_bank, .unload_bank = b_unload_bank, .post_event = b_post_event, .stop = b_stop,
    .stop_all = b_stop_all, .set_rtpc = b_set_rtpc, .set_state = b_set_state, .set_switch = b_set_switch,
    .game_object = b_game_object, .mix = mix,
};

void wwplay_close(void)
{
    pthread_mutex_lock(&L);
    for (int i = 0; g_v && i < g_maxv; i++) if (g_v[i].vb) stb_vorbis_close(g_v[i].vb);
    for (int i = 0; i < VPOOL; i++) { if (g_vpool[i].vb) stb_vorbis_close(g_vpool[i].vb); g_vpool[i].vb = NULL; }
    free(g_v); g_v = NULL; g_nv = 0;
    for (int i = 0; i < g_nnodes; i++) {
        node_t *n = &g_nodes[i];
        free(n->kids); free(n->w); free(n->mval); free(n->moff); free(n->mcnt); free(n->mkids);
        free(n->st); free(n->agroup); free(n->astate); free(n->clips);
        for (int k = 0; k < n->nrules; k++) { free(n->rules[k].src); free(n->rules[k].dst); }
        free(n->rules);
    }
    for (int i = 0; i < g_nacts; i++) free(g_acts[i].except);
    for (int i = 0; i < g_nbuses; i++) free(g_buses[i].st);
    for (int i = 0; i < MAXMUS; i++) free(g_mus[i].ist);
    free(g_dec); free(g_items); free(g_ikids); free(g_rtpcd); free(g_sfade);
    g_dec = NULL; g_items = NULL; g_ikids = NULL; g_rtpcd = NULL; g_sfade = NULL;
    g_ndec = g_cdec = g_nitems = g_citems = g_nikids = g_cikids = g_nrtpcd = g_crtpcd = g_nsfade = g_csfade = 0;
    for (int i = 0; i < g_nbinds; i++) { free(g_binds[i].x); free(g_binds[i].y); free(g_binds[i].interp); }
    for (int i = 0; i < g_nbanks; i++) if (g_banks[i].pak) munmap((void *)g_banks[i].pak, g_banks[i].paklen);
    free(g_media); free(g_nodes); free(g_acts); free(g_evs); free(g_buses); free(g_binds); free(g_banks);
    free(g_states); free(g_switches); free(g_dstates); free(g_dswitches); free(g_rtpcv); free(g_play); free(g_done);
    im_free(&g_mmap); im_free(&g_nmap); im_free(&g_emap); im_free(&g_bmap);
    g_media = NULL; g_nodes = NULL; g_acts = NULL; g_evs = NULL; g_buses = NULL; g_binds = NULL; g_banks = NULL;
    g_states = g_switches = g_dstates = g_dswitches = NULL; g_rtpcv = NULL; g_play = NULL; g_done = NULL;
    g_nmedia = g_cmedia = g_nnodes = g_cnodes = g_nacts = g_cacts = g_nevs = g_cevs = g_nbuses = g_cbuses = 0;
    g_nbinds = g_cbinds = g_nbanks = g_cbanks = g_nstates = g_cstates = g_nswitches = g_cswitches = 0;
    g_ndstates = g_cdstates = g_ndswitches = g_cdswitches = g_nrtpcv = g_crtpcv = g_nplay = g_cplay = g_ndone = g_cdone = 0;
    memset(g_seq, 0, sizeof g_seq); memset(g_mus, 0, sizeof g_mus); memset(g_pend, 0, sizeof g_pend);
    if (g_cap.f) wwplay_wav_close(&g_cap);
    memset(&g_st, 0, sizeof g_st); memset(&g_st_last, 0, sizeof g_st_last);
    g_clock = 0; g_serial = 0; g_vpeak = 0; g_suspended = 0; g_last_mix_wall = 0; g_rng = 0x9e3779b9u;
    g_qh = g_qt = 0;
    memset(&g_lim, 0, sizeof g_lim);
    g_lim.need[0] = g_lim.need[1] = g_lim.g = 1;
    pthread_mutex_unlock(&L);
}

const ua_wwise_backend *wwplay_open(const ua_wwise_host *host, const char *dir)
{
    wwplay_close();
    struct stat st;
    if (!dir || !*dir || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        H = host;
        logf_("player: no data folder %s (UA_WWISE_DATA): silent", dir ? dir : "");
        return NULL;
    }
    pthread_mutex_lock(&L);
    H = host;
    ima_tables();
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    const char *e = getenv("UA_WWISE_VOICES");
    g_maxv = e && atoi(e) > 0 ? (atoi(e) > 512 ? 512 : atoi(e)) : 48;
    g_v = calloc(g_maxv, sizeof *g_v);
    e = getenv("UA_WWISE_STATS");
    g_stats_every = e && *e ? atof(e) : 60;
    e = getenv("UA_WWISE_LIMITER");
    g_lim.on = !(e && !strcmp(e, "0"));
    g_stats_wall = wall_now();
    e = getenv("UA_AUDIO_RATE");
    g_rate = e && atoi(e) >= 8000 && atoi(e) <= 192000 ? atoi(e) : 48000;
    e = getenv("UA_WWISE_CAPTURE");
    if (e && *e) {
        const char *s = getenv("UA_WWISE_CAPTURE_SECS");
        g_cap_frames = (unsigned long)((s && atof(s) > 0 ? atof(s) : 120) * g_rate);
        if (!wwplay_wav_open(&g_cap, e, g_rate, 2)) logf_("capture %s: %s", e, strerror(errno));
    }
    pthread_mutex_unlock(&L);
    logf_("player: data %s, %d voices at most%s%s", dir, g_maxv, g_cap.f ? ", capturing to " : "", g_cap.f ? e : "");
    return &g_backend;
}

const ua_wwise_backend *wwplay_backend_get(const ua_wwise_host *host)
{
    const char *d = getenv("UA_WWISE_DATA");
    return d && *d ? wwplay_open(host, d) : NULL;
}

void wwplay_get_stats(wwplay_stats *s)      // the queued calls run first
{
    pthread_mutex_lock(&L);
    drain_locked();
    *s = g_st;
    s->voices = g_nv;
    s->voices_peak = g_vpeak;
    int np = 0;
    for (int i = 0; i < g_nplay; i++) np += g_play[i].used;
    s->playing = np;
    pthread_mutex_unlock(&L);
}

// ---------------------------------------------------------------------------------------- WAV
static void wav_header(wwplay_wav *w)
{
    uint32_t data = (uint32_t)(w->frames * w->ch * 2), v;
    uint8_t h[44];
    memcpy(h, "RIFF", 4); v = 36 + data; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t fmt = 1, ch = (uint16_t)w->ch, ba = (uint16_t)(2 * w->ch), bits = 16;
    memcpy(h + 20, &fmt, 2); memcpy(h + 22, &ch, 2); v = w->rate; memcpy(h + 24, &v, 4);
    v = w->rate * ba; memcpy(h + 28, &v, 4); memcpy(h + 32, &ba, 2); memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    long pos = ftell(w->f);
    fseek(w->f, 0, SEEK_SET);
    fwrite(h, 1, 44, w->f);
    if (pos > 44) fseek(w->f, pos, SEEK_SET);
}

int wwplay_wav_open(wwplay_wav *w, const char *path, int rate, int ch)
{
    memset(w, 0, sizeof *w);
    w->f = fopen(path, "wb");
    if (!w->f) return 0;
    w->rate = rate;
    w->ch = ch;
    wav_header(w);
    return 1;
}

void wwplay_wav_write(wwplay_wav *w, const float *buf, int frames)
{
    if (!w->f) return;
    int16_t s[1024];
    int n = frames * w->ch;
    for (int i = 0; i < n;) {
        int k = n - i < 1024 ? n - i : 1024;
        for (int j = 0; j < k; j++) { float x = buf[i + j] * 32767.0f; s[j] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : lrintf(x)); }
        fwrite(s, 2, k, w->f);
        i += k;
    }
    w->frames += frames;
    w->since += frames;
    if (w->since >= (unsigned long)w->rate) { w->since = 0; wav_header(w); }
}

void wwplay_wav_close(wwplay_wav *w)
{
    if (!w->f) return;
    wav_header(w);
    fclose(w->f);
    w->f = NULL;
}
