// The built-in Wwise (docs/plugins.md, "Wwise"): a libAkSoundEngine for games whose Wwise
// integration P/Invokes Audiokinetic's sound engine, which has no ARM build in a desktop game.
// Without it the integration throws at its first call (the game's loading code with it, and the
// integration's LateUpdate every frame), and code waiting for Wwise's callbacks (a bank loaded, an
// event's end) waits forever.
//
// The entry points are the game's own: android-data writes one fn line per P/Invoke of the
// integration into plugins.map (its name, result and argument classes, read from the game's
// assemblies), and the game's AKRESULT and AkCallbackType constants as const lines; Wwise's C#
// integration has a different set in every version, so nothing here lists them. Each entry point
// is a trampoline (wwise_tramp.S) whose arguments the dispatcher reads by that signature, as the
// caller passed them (the armeabi-v7a player's softfp Mono, or AAPCS64), and answers:
//   - the calls of the engine's API that matter (below, by name with CSharp_ and __SWIG_<n> cut
//     off): Init/Term/IsInitialized, base paths and language, LoadBank (by name, ID or memory;
//     with a cookie: the bank callback), UnloadBank/PrepareBank, PostEvent (a new playing ID; with
//     a cookie and AK_EndOfEvent: the end of event), RTPCs, states and switches (stored, so the
//     Get calls give back what was set), triggers, game objects and positions, RenderAudio,
//     GetIDFromString (Wwise's FNV-1 hash), and the callback queue the integration polls every
//     frame (AkCallbackSerializer_Lock/Unlock and the accessors of its records);
//   - SWIG's object plumbing generically: new_<Class> makes an object of the stub's, delete_ frees
//     it, <Class>_<field>_get/_set read and write its fields (strings are not given back: NULL),
//     <Class>_SWIGUpcast is the object itself;
//   - everything else by its result class: AKRESULT (a signed int: SWIG's enums) AK_Success,
//     bool false, an unsigned int, a pointer or a string 0.
// A sound backend (wwise.h) can plug into the same calls: UA_WWISE_BACKEND=<library.so>, or
// ua_host's own player (wwplay.c) with UA_WWISE_DATA=<dir>. Without one the game is silent and its
// events end at once. UA_WWISE_TRACE=1 logs every call.
#define _GNU_SOURCE
#include "wwise.h"
#include "plugins.h"

#pragma weak wwplay_backend_get     // ua_host links the player (wwplay.c); the stub's unit test does not

#include <ctype.h>
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define UA_AK_TRAMPS 4096       // = wwise_tramp.S
#define AK_MAXARGS 24
#define LOG(...) fprintf(stderr, "[wwise] " __VA_ARGS__)

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int g_trace = -1;

// --------------------------------------------------------------------------------------- names

// Wwise's ID of a name: 32-bit FNV-1 of the lower-case name (AK::SoundEngine::GetIDFromString)
static uint32_t ak_id(const char *s)
{
    uint32_t h = 2166136261u;
    for (; s && *s; s++) { h *= 16777619u; h ^= (uint32_t)(unsigned char)tolower((unsigned char)*s); }
    return h;
}

// a bank's ID from its file name: the ID of the name without folder and extension ("SFX.bnk" ->
// "SFX"), which is the ID in the bank's own header
static uint32_t bank_id(const char *name)
{
    if (!name) return 0;
    const char *b = strrchr(name, '/');
    b = b ? b + 1 : name;
    char n[256];
    snprintf(n, sizeof n, "%s", b);
    char *dot = strrchr(n, '.');
    if (dot && dot != n) *dot = 0;
    return ak_id(n);
}

static uint32_t fnv32(const char *s)        // field keys
{
    uint32_t h = 2166136261u;
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h ? h : 1;
}

// --------------------------------------------------------------------------------------- the game's constants

static struct { char name[96]; long long value; } *g_const;
static int g_nconst;

static uint32_t constant(const char *name, uint32_t dflt)
{
    for (int i = 0; i < g_nconst; i++) if (!strcmp(g_const[i].name, name)) return (uint32_t)g_const[i].value;
    return dflt;
}

static uint32_t AK_SUCCESS = UA_AK_SUCCESS, AK_FAIL = UA_AK_FAIL;
static uint32_t CB_END_OF_EVENT = 1, CB_BANK = 0x40000000u;

// --------------------------------------------------------------------------------------- objects

// What new_<Class> returns, and the callback queue's records: a block whose fields are kept by the
// key of their accessor ("AkCallbackInfo_pCookie"). Pointers the game made itself (Marshal memory)
// are never touched: an object is found by its address in a table.
typedef struct ak_obj {
    int n, cap;
    struct { uint32_t key; ua_ak_value v; } *f;
    struct ak_obj *next;        // the callback queue
    char cls[48];
} ak_obj;

static ak_obj **g_objs;         // open addressing by address
static unsigned g_ocap, g_on;

static unsigned ohash(const void *p, unsigned cap) { return (unsigned)(((uintptr_t)p >> 4) * 2654435761u) & (cap - 1); }

static void otab_put(ak_obj *o)
{
    if ((g_on + 1) * 2 > g_ocap) {
        unsigned ncap = g_ocap ? g_ocap * 2 : 256;
        ak_obj **n = calloc(ncap, sizeof *n);
        for (unsigned i = 0; i < g_ocap; i++)
            if (g_objs[i] && g_objs[i] != (ak_obj *)1) {
                unsigned j = ohash(g_objs[i], ncap);
                while (n[j]) j = (j + 1) & (ncap - 1);
                n[j] = g_objs[i];
            }
        free(g_objs);
        g_objs = n; g_ocap = ncap;
    }
    unsigned j = ohash(o, g_ocap);
    while (g_objs[j] && g_objs[j] != (ak_obj *)1) j = (j + 1) & (g_ocap - 1);
    g_objs[j] = o;
    g_on++;
}

static ak_obj *otab_find(const void *p)
{
    if (!p || !g_ocap) return NULL;
    unsigned j = ohash(p, g_ocap);
    for (unsigned k = 0; k < g_ocap && g_objs[j]; k++, j = (j + 1) & (g_ocap - 1))
        if (g_objs[j] == p) return g_objs[j];
    return NULL;
}

static void otab_del(ak_obj *o)
{
    if (!g_ocap) return;
    unsigned j = ohash(o, g_ocap);
    for (unsigned k = 0; k < g_ocap && g_objs[j]; k++, j = (j + 1) & (g_ocap - 1))
        if (g_objs[j] == o) { g_objs[j] = (ak_obj *)1; g_on--; return; }   // (a tombstone)
}

static ak_obj *obj_new(const char *cls)
{
    ak_obj *o = calloc(1, sizeof *o);
    snprintf(o->cls, sizeof o->cls, "%s", cls);
    otab_put(o);
    return o;
}

static void obj_free(ak_obj *o)
{
    otab_del(o);
    for (int i = 0; i < o->n; i++) if (o->f[i].key & 0x80000000u) free(o->f[i].v.p);   // string copies
    free(o->f);
    free(o);
}

static ua_ak_value *obj_field(ak_obj *o, uint32_t key, int add)
{
    key &= 0x7fffffffu;
    for (int i = 0; i < o->n; i++) if ((o->f[i].key & 0x7fffffffu) == key) return &o->f[i].v;
    if (!add) return NULL;
    if (o->n == o->cap) { o->cap = o->cap ? o->cap * 2 : 8; o->f = realloc(o->f, o->cap * sizeof *o->f); }
    o->f[o->n].key = key;
    o->f[o->n].v.u = 0;
    return &o->f[o->n++].v;
}

static void obj_set(ak_obj *o, const char *field, uint64_t v) { obj_field(o, fnv32(field), 1)->u = v; }

static void obj_set_str(ak_obj *o, uint32_t key, const char *s)
{
    ua_ak_value *v = obj_field(o, key, 1);
    for (int i = 0; i < o->n; i++)
        if (&o->f[i].v == v) { if (o->f[i].key & 0x80000000u) free(v->p); o->f[i].key |= 0x80000000u; }
    v->p = s ? strdup(s) : NULL;
}

// globals of SWIG (<name>_get/_set without an object)
static ak_obj g_globals;

// --------------------------------------------------------------------------------------- state

static int g_init;                          // Init succeeded, Term not called
static char g_base[4][PATH_MAX];            // SetBasePath, AddBasePath (the first ones), SetDecodedBankPath
static int g_nbase;
static char g_language[64];
static uint32_t g_next_playing = 1;
static ak_obj *g_queue, **g_queue_tail = &g_queue;   // callbacks waiting for the game's Lock
static ak_obj *g_locked;                    // what the game reads between Lock and Unlock
static unsigned long g_calls, g_banks, g_events, g_callbacks;

// stored game syncs: RTPCs (by ID and game object, 0 = global), states, switches
typedef struct { uint32_t id; uint64_t obj; ua_ak_value v; } sync_ent;
static sync_ent *g_rtpc, *g_state, *g_switch;
static int g_nrtpc, g_nstate, g_nswitch, g_crtpc, g_cstate, g_cswitch;

static ua_ak_value *sync_slot(sync_ent **t, int *n, int *cap, uint32_t id, uint64_t obj, int add)
{
    for (int i = 0; i < *n; i++) if ((*t)[i].id == id && (*t)[i].obj == obj) return &(*t)[i].v;
    if (!add) return NULL;
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 32; *t = realloc(*t, *cap * sizeof **t); }
    (*t)[*n].id = id; (*t)[*n].obj = obj; (*t)[*n].v.u = 0;
    return &(*t)[(*n)++].v;
}

// --------------------------------------------------------------------------------------- backend

static const ua_wwise_backend *B;
static int g_backend_tried;

static void host_log(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    LOG("%s%s", b, *b && b[strlen(b) - 1] == '\n' ? "" : "\n");
}

static void queue_push(ak_obj *h)
{
    h->next = NULL;
    *g_queue_tail = h;
    g_queue_tail = &h->next;
    g_callbacks++;
}

// a callback record: the header (pPackage = the game's cookie, eType) and its info object
static ak_obj *callback_new(uintptr_t cookie, uint32_t type, ak_obj **info)
{
    ak_obj *h = obj_new("AkSerializedCallbackHeader"), *d = obj_new("AkCallbackInfo");
    obj_set(h, "AkSerializedCallbackHeader_pPackage", cookie);
    obj_set(h, "AkSerializedCallbackHeader_eType", type);
    obj_set(h, "AkSerializedCallbackHeader_data", (uintptr_t)d);
    obj_set(d, "AkCallbackInfo_pCookie", cookie);
    *info = d;
    return h;
}

// events the game asked callbacks for: playing ID -> cookie, game object, event, flags
typedef struct { uint32_t playing, event, flags; uint64_t obj; uintptr_t cookie; } live_event;
static live_event *g_live;
static int g_nlive, g_clive;

static void end_of_event_locked(uint32_t playing)
{
    for (int i = 0; i < g_nlive; i++) {
        if (g_live[i].playing != playing) continue;
        live_event e = g_live[i];
        g_live[i] = g_live[--g_nlive];
        if (e.cookie && (e.flags & CB_END_OF_EVENT)) {
            ak_obj *d, *h = callback_new(e.cookie, CB_END_OF_EVENT, &d);
            obj_set(d, "AkCallbackInfo_gameObjID", e.obj);
            obj_set(d, "AkEventCallbackInfo_playingID", e.playing);
            obj_set(d, "AkEventCallbackInfo_eventID", e.event);
            queue_push(h);
        }
        return;
    }
}

static void host_event_done(uint32_t playing)
{
    pthread_mutex_lock(&mu);
    end_of_event_locked(playing);
    pthread_mutex_unlock(&mu);
}

static void host_event_callback(uint32_t playing, uint32_t type, int n, const char *const *field, const ua_ak_value *value)
{
    pthread_mutex_lock(&mu);
    for (int i = 0; i < g_nlive; i++) {
        if (g_live[i].playing != playing || !g_live[i].cookie || !(g_live[i].flags & type)) continue;
        ak_obj *d, *h = callback_new(g_live[i].cookie, type, &d);
        obj_set(d, "AkCallbackInfo_gameObjID", g_live[i].obj);
        obj_set(d, "AkEventCallbackInfo_playingID", playing);
        obj_set(d, "AkEventCallbackInfo_eventID", g_live[i].event);
        // a field by its name in any info class: the accessor keys of the classes that have it
        static const char *const classes[] = { "AkCallbackInfo", "AkEventCallbackInfo", "AkMarkerCallbackInfo", "AkDurationCallbackInfo",
            "AkMusicSyncCallbackInfo", "AkMusicPlaylistCallbackInfo", "AkMIDIEventCallbackInfo", "AkDynamicSequenceItemCallbackInfo" };
        for (int k = 0; k < n; k++)
            for (unsigned c = 0; c < sizeof classes / sizeof classes[0]; c++) {
                char key[128];
                snprintf(key, sizeof key, "%s_%s", classes[c], field[k]);
                obj_set(d, key, value[k].u);
            }
        queue_push(h);
        break;
    }
    pthread_mutex_unlock(&mu);
}

static int is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }

static const char *host_file(const char *name, char *buf, unsigned size)
{
    if (!name || !*name) return NULL;
    if (name[0] == '/' && is_file(name)) { snprintf(buf, size, "%s", name); return buf; }
    char base[4][PATH_MAX];
    int nb;
    pthread_mutex_lock(&mu);
    nb = g_nbase;
    memcpy(base, g_base, sizeof base);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < nb; i++) {
        char p[PATH_MAX * 2];
        const char *b = base[i];
        char tmp[PATH_MAX];
        if (!strncmp(b, "jar:", 4) || !strncmp(b, "file:", 5)) {             // inside the APK/OBB: beside it
            snprintf(p, sizeof p, "%.4000s%s%.4000s", b, b[strlen(b) - 1] == '/' ? "" : "/", name);
            if (plugins_asset_path(p, tmp, sizeof tmp)) { snprintf(buf, size, "%s", tmp); return buf; }
            continue;
        }
        snprintf(p, sizeof p, "%.4000s%s%.4000s", b, b[0] && b[strlen(b) - 1] == '/' ? "" : "/", name);
        if (is_file(p)) { snprintf(buf, size, "%s", p); return buf; }
    }
    return NULL;
}

static const ua_wwise_host g_host = { UA_WWISE_BACKEND_VERSION, host_event_done, host_event_callback, host_file, ak_id, constant, host_log };

// the audio output (runtime/android/opensles.c); weak: the unit tests link no shims
extern void android_sles_set_mix(void (*fn)(float *, int, int, int), int rate) __attribute__((weak));

static void backend_load(void)
{
    if (g_backend_tried) return;
    g_backend_tried = 1;
    const char *p = getenv("UA_WWISE_BACKEND");
    const ua_wwise_backend *b = NULL;
    if (p && *p) {
        void *h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
        ua_wwise_backend_fn get = h ? (ua_wwise_backend_fn)dlsym(h, "ua_wwise_backend_get") : NULL;
        b = get ? get(&g_host) : NULL;
        if (!b || b->version < 1 || b->version > UA_WWISE_BACKEND_VERSION) {
            LOG("backend %s: %s: silent\n", p, !h ? dlerror() : !get ? "no ua_wwise_backend_get()" : !b ? "declined" : "another interface version");
            return;
        }
    } else if (!wwplay_backend_get || !(b = wwplay_backend_get(&g_host))) {
        LOG("silent (no sound backend: UA_WWISE_BACKEND, UA_WWISE_DATA)\n");
        return;
    }
    B = b;
    LOG("backend %s (%s)\n", b->name ? b->name : "?", p && *p ? p : "built in");
    if (b->version >= 2 && b->mix) {
        const char *r = getenv("UA_AUDIO_RATE");
        int rate = r && atoi(r) >= 8000 && atoi(r) <= 192000 ? atoi(r) : 48000;
        if (android_sles_set_mix) android_sles_set_mix(b->mix, rate);
        else LOG("no audio output in this program: the backend's mix is not played\n");
    }
}

// --------------------------------------------------------------------------------------- entry points

typedef struct entry entry;
typedef ua_ak_value (*handler)(const entry *e, ua_ak_value *a, const ua_ak_call *c);
struct entry {
    char *name;                 // CSharp_PostEvent__SWIG_6
    char api[96];               // PostEvent
    char ret;
    int n;
    char kind[AK_MAXARGS];      // b i u l f d p r z s S F D
    unsigned char cnt[AK_MAXARGS];
    char cls[AK_MAXARGS][4];
    const char *clsp[AK_MAXARGS];
    handler h;
    uint32_t key;               // accessors: the field's key
    unsigned long calls;
};
static entry *g_ent;
static int g_nent;

// the arguments of a handler, by class: the n-th of these classes
static int nth(const entry *e, const char *classes, int n)
{
    for (int i = 0; i < e->n; i++) if (strchr(classes, e->kind[i]) && n-- == 0) return i;
    return -1;
}

// an ID given as a name (z) or a number (u/i) at argument i: the ID (and the name, if one)
static uint32_t id_at(const entry *e, ua_ak_value *a, int i, const char **name)
{
    if (name) *name = NULL;
    if (i < 0 || i >= e->n) return 0;
    if (e->kind[i] == 'z') { if (name) *name = a[i].z; return ak_id(a[i].z); }
    return (uint32_t)a[i].u;
}

static uint64_t obj_at(const entry *e, ua_ak_value *a, int i)
{
    if (i < 0 || i >= e->n) return 0;
    return e->kind[i] == 'l' ? a[i].u : (uint64_t)(uint32_t)a[i].u;     // (32-bit game objects before Wwise 2017)
}

static ua_ak_value R(uint64_t v) { ua_ak_value r; r.u = v; return r; }
#define OK R(AK_SUCCESS)

static ua_ak_value h_init(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)e; (void)a; (void)c;
    int r = B && B->init ? B->init(g_nbase ? g_base[0] : "", g_language) : (int)AK_SUCCESS;
    g_init = r == (int)AK_SUCCESS;
    LOG("Init: %s\n", g_init ? "ok" : "the backend failed");
    return R(r);
}
static ua_ak_value h_term(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)e; (void)a; (void)c;
    if (B && B->term) B->term();
    g_init = 0;
    return R(0);
}
static ua_ak_value h_isinit(const entry *e, ua_ak_value *a, const ua_ak_call *c) { (void)e; (void)a; (void)c; return R(g_init); }

static ua_ak_value h_path(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)c;
    int i = nth(e, "z", 0);
    if (i < 0 || !a[i].z) return OK;
    pthread_mutex_lock(&mu);
    if (!strcmp(e->api, "SetCurrentLanguage")) snprintf(g_language, sizeof g_language, "%s", a[i].z);
    else if (!strcmp(e->api, "SetBasePath") || g_nbase < 4) {
        int k = !strcmp(e->api, "SetBasePath") ? 0 : g_nbase;
        snprintf(g_base[k], sizeof g_base[k], "%s", a[i].z);
        if (k >= g_nbase) g_nbase = k + 1;
    }
    pthread_mutex_unlock(&mu);
    return OK;
}

static ua_ak_value h_getid(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)c;
    int i = nth(e, "z", 0);
    return R(i >= 0 ? ak_id(a[i].z) : 0);
}

// a bank: by name, by ID, or (pointer, size) in memory; then optionally (callback, cookie), a
// memory pool and the bank ID out
static ua_ak_value bank_op(const entry *e, ua_ak_value *a, int load)
{
    const char *name = NULL;
    uint32_t id = 0, size = 0;
    const void *mem = NULL;
    int i, rest;
    if (e->n > 0 && e->kind[0] == 'z') { name = a[0].z; id = bank_id(name); rest = 1; }
    else if (e->n > 1 && e->kind[0] == 'p' && strchr("ui", e->kind[1])) { mem = a[0].p; size = (uint32_t)a[1].u; rest = 2; }
    else { id = e->n > 0 ? (uint32_t)a[0].u : 0; rest = 1; }
    if (!strcmp(e->api, "UnloadBank") && e->n > 1 && e->kind[1] == 'p') rest = 2;      // (name | ID, in-memory pointer, ...)
    if (!strcmp(e->api, "PrepareBank")) { rest = 2; name = e->n > 1 && e->kind[1] == 'z' ? a[1].z : NULL; id = name ? bank_id(name) : e->n > 1 ? (uint32_t)a[1].u : 0; }
    // the cookie: the second of the last (callback, cookie) pair of pointers after the bank
    uintptr_t cookie = 0;
    int pool = -1;
    for (i = rest; i + 1 < e->n; i++)
        if (e->kind[i] == 'p' && e->kind[i + 1] == 'p') cookie = (uintptr_t)a[i + 1].p;
    for (i = rest; i < e->n; i++) if (e->kind[i] == 'i') { pool = (int)a[i].i; break; }
    int r = (int)AK_SUCCESS;
    uint32_t bank = id;
    if (load && strcmp(e->api, "PrepareBank")) {
        if (B && B->load_bank) r = B->load_bank(name, id, mem, size, &bank);
        if (mem && !bank && size >= 16 && !memcmp(mem, "BKHD", 4)) memcpy(&bank, (const char *)mem + 12, 4);   // the bank's own ID
    } else if (!load && B && B->unload_bank) r = B->unload_bank(bank);
    int out = nth(e, "r", 0);
    if (out >= 0 && a[out].p && load) memcpy(a[out].p, &bank, 4);
    pthread_mutex_lock(&mu);
    if (load) g_banks++;
    if (cookie) {                               // asynchronous: the result goes to the bank callback
        ak_obj *d, *h = callback_new(cookie, CB_BANK, &d);
        obj_set(d, "AkBankCallbackInfo_bankID", bank);
        obj_set(d, "AkBankCallbackInfo_inMemoryBankPtr", (uintptr_t)mem);
        obj_set(d, "AkBankCallbackInfo_loadResult", (uint32_t)r);
        obj_set(d, "AkBankCallbackInfo_memPoolId", (uint32_t)pool);
        queue_push(h);
        r = (int)AK_SUCCESS;
    }
    pthread_mutex_unlock(&mu);
    if (g_banks <= 8 && load) LOG("%s %s%s -> %u%s\n", e->api, name ? name : mem ? "(from memory)" : "(by ID)", cookie ? " (callback)" : "",
                                  bank, r == (int)AK_SUCCESS ? "" : " (failed)");
    return R((uint32_t)r);
}
static ua_ak_value h_loadbank(const entry *e, ua_ak_value *a, const ua_ak_call *c) { (void)c; return bank_op(e, a, 1); }
static ua_ak_value h_unloadbank(const entry *e, ua_ak_value *a, const ua_ak_call *c) { (void)c; return bank_op(e, a, 0); }

// PostEvent(event name | ID, game object, [flags, callback, cookie, [externals ..., [playing ID]]])
static ua_ak_value h_postevent(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)c;
    const char *name;
    uint32_t event = id_at(e, a, 0, &name);
    uint64_t obj = obj_at(e, a, 1);
    uint32_t flags = e->n > 2 && strchr("ui", e->kind[2]) ? (uint32_t)a[2].u : 0;
    uintptr_t cookie = e->n > 4 && e->kind[3] == 'p' && e->kind[4] == 'p' ? (uintptr_t)a[4].p : 0;
    pthread_mutex_lock(&mu);
    uint32_t playing = g_next_playing++;
    if (!g_next_playing) g_next_playing = 1;
    if (g_nlive == g_clive) { g_clive = g_clive ? g_clive * 2 : 64; g_live = realloc(g_live, g_clive * sizeof *g_live); }
    g_live[g_nlive++] = (live_event){ playing, event, flags, obj, cookie };
    g_events++;
    pthread_mutex_unlock(&mu);
    int playing_on = B && B->post_event ? B->post_event(event, obj, playing, flags) : 0;
    if (!playing_on) host_event_done(playing);     // silent: it ended already
    return R(playing);
}

static ua_ak_value h_stop(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)c;
    if (!strcmp(e->api, "StopPlayingID")) { if (B && B->stop && e->n > 0) B->stop((uint32_t)a[0].u); }
    else if (B && B->stop_all) B->stop_all(e->n > 0 ? obj_at(e, a, 0) : 0, e->n == 0);
    return R(0);
}

// the game cancels callbacks it no longer wants: the queue loses them too (the integration would
// otherwise find no package for them, log an error and stop reading the queue for that frame)
static ua_ak_value h_cancel(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)c;
    if (e->n < 1) return R(0);
    int by_cookie = !!strstr(e->api, "Cookie"), by_obj = !!strstr(e->api, "GameObject");
    pthread_mutex_lock(&mu);
    for (ak_obj **pp = &g_queue; *pp;) {
        ak_obj *h = *pp;
        ua_ak_value *pk = obj_field(h, fnv32("AkSerializedCallbackHeader_pPackage"), 0);
        ua_ak_value *dv = obj_field(h, fnv32("AkSerializedCallbackHeader_data"), 0);
        ak_obj *d = dv ? (ak_obj *)dv->p : NULL;
        ua_ak_value *pid = d ? obj_field(d, fnv32("AkEventCallbackInfo_playingID"), 0) : NULL;
        ua_ak_value *go = d ? obj_field(d, fnv32("AkCallbackInfo_gameObjID"), 0) : NULL;
        ua_ak_value *ty = obj_field(h, fnv32("AkSerializedCallbackHeader_eType"), 0);
        int bank = ty && (uint32_t)ty->u == CB_BANK;
        int drop = !strcmp(e->api, "CancelBankCallbackCookie") ? bank && pk && pk->u == (uintptr_t)a[0].p :
                   bank ? 0 : by_cookie ? pk && pk->u == (uintptr_t)a[0].p : by_obj ? go && go->u == obj_at(e, a, 0) :
                   pid && (uint32_t)pid->u == (uint32_t)a[0].u;
        if (drop) {
            *pp = h->next;
            if (g_queue_tail == &h->next) g_queue_tail = pp;
            if (d) obj_free(d);
            obj_free(h);
        } else pp = &h->next;
    }
    for (int i = 0; i < g_nlive; i++) {
        live_event *l = &g_live[i];
        if (strcmp(e->api, "CancelBankCallbackCookie") &&
            (by_cookie ? l->cookie == (uintptr_t)a[0].p : by_obj ? l->obj == obj_at(e, a, 0) : l->playing == (uint32_t)a[0].u))
            l->cookie = 0;
    }
    pthread_mutex_unlock(&mu);
    return R(0);
}

// RTPCs: SetRTPCValue(rtpc, value, [game object, ...]); ...ByPlayingID(rtpc, value, playing ID, ...);
// ResetRTPCValue(rtpc, [game object, ...]); GetRTPCValue(rtpc, game object, playing ID, out value, ref type)
static ua_ak_value h_rtpc(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)c;
    const char *name;
    uint32_t id = id_at(e, a, 0, &name);
    int byplaying = !!strstr(e->api, "ByPlayingID");
    if (!strcmp(e->api, "GetRTPCValue")) {
        uint64_t obj = obj_at(e, a, 1);
        int out = nth(e, "r", 0);
        pthread_mutex_lock(&mu);
        ua_ak_value *v = sync_slot(&g_rtpc, &g_nrtpc, &g_crtpc, id, obj, 0);
        if (!v) v = sync_slot(&g_rtpc, &g_nrtpc, &g_crtpc, id, 0, 0);
        float f = v ? v->f : 0;
        pthread_mutex_unlock(&mu);
        if (out >= 0 && a[out].p) memcpy(a[out].p, &f, 4);
        return v ? OK : R(AK_FAIL);
    }
    int reset = !strncmp(e->api, "Reset", 5);
    float value = 0;
    int fi = nth(e, "f", 0);
    if (fi >= 0) value = a[fi].f;
    int oi = reset ? 1 : 2;
    uint64_t obj = !byplaying && e->n > oi ? obj_at(e, a, oi) : 0;
    uint32_t playing = byplaying && e->n > 2 ? (uint32_t)a[2].u : 0;
    int every = !byplaying && e->n <= oi;
    if (!byplaying) {
        pthread_mutex_lock(&mu);
        sync_slot(&g_rtpc, &g_nrtpc, &g_crtpc, id, every ? 0 : obj, 1)->f = value;
        pthread_mutex_unlock(&mu);
    }
    if (B && B->set_rtpc) B->set_rtpc(id, value, obj, playing, every);
    return OK;
}

static ua_ak_value h_state(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)c;
    int is_switch = !!strstr(e->api, "Switch");
    uint32_t group = id_at(e, a, 0, NULL);
    if (!strncmp(e->api, "Get", 3)) {
        uint64_t obj = is_switch ? obj_at(e, a, 1) : 0;
        int out = nth(e, "r", 0);
        pthread_mutex_lock(&mu);
        ua_ak_value *v = is_switch ? sync_slot(&g_switch, &g_nswitch, &g_cswitch, group, obj, 0) : sync_slot(&g_state, &g_nstate, &g_cstate, group, 0, 0);
        uint32_t s = v ? (uint32_t)v->u : 0;
        pthread_mutex_unlock(&mu);
        if (out >= 0 && a[out].p) memcpy(a[out].p, &s, 4);
        return OK;
    }
    uint32_t value = id_at(e, a, 1, NULL);
    uint64_t obj = is_switch ? obj_at(e, a, 2) : 0;
    pthread_mutex_lock(&mu);
    if (is_switch) sync_slot(&g_switch, &g_nswitch, &g_cswitch, group, obj, 1)->u = value;
    else sync_slot(&g_state, &g_nstate, &g_cstate, group, 0, 1)->u = value;
    pthread_mutex_unlock(&mu);
    if (is_switch && B && B->set_switch) B->set_switch(group, value, obj);
    if (!is_switch && B && B->set_state) B->set_state(group, value);
    return OK;
}

static ua_ak_value h_trigger(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)c;
    if (B && B->post_trigger) B->post_trigger(id_at(e, a, 0, NULL), obj_at(e, a, 1));
    return OK;
}

static ua_ak_value h_gameobj(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)c;
    if (B && B->game_object) {
        int zi = nth(e, "z", 0);
        B->game_object(obj_at(e, a, 0), zi >= 0 ? a[zi].z : NULL, strncmp(e->api, "Unregister", 10) != 0);
    }
    return OK;
}

// AkTransform: Set(position, front, top as 9 floats | 3 AkVectors), SetPosition, SetOrientation;
// SetObjectPosition(game object, transform) gives the backend the floats
static const char *const TF[9] = { "px", "py", "pz", "fx", "fy", "fz", "tx", "ty", "tz" };
static ua_ak_value h_transform(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)c;
    pthread_mutex_lock(&mu);
    ak_obj *o = e->n > 0 ? otab_find(a[0].p) : NULL;
    if (o) {
        int first = !strcmp(e->api, "AkTransform_SetOrientation") ? 3 : 0;
        int k = first;
        for (int i = 1; i < e->n && k < 9; i++) {
            if (e->kind[i] == 'f') { obj_set(o, TF[k++], a[i].u); continue; }
            ak_obj *v = e->kind[i] == 'p' ? otab_find(a[i].p) : NULL;        // an AkVector of the stub's
            const char *xyz[3] = { "AkVector_X", "AkVector_Y", "AkVector_Z" };
            for (int j = 0; j < 3 && k < 9; j++) {
                ua_ak_value *f = v ? obj_field(v, fnv32(xyz[j]), 0) : NULL;
                obj_set(o, TF[k++], f ? f->u : 0);
            }
        }
    }
    pthread_mutex_unlock(&mu);
    return R(0);
}

static ua_ak_value h_setposition(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)c;
    if (!B || !B->set_position || e->n < 2) return OK;
    float f[9] = { 0 };
    pthread_mutex_lock(&mu);
    ak_obj *o = otab_find(a[1].p);
    for (int k = 0; o && k < 9; k++) { ua_ak_value *v = obj_field(o, fnv32(TF[k]), 0); if (v) f[k] = v->f; }
    pthread_mutex_unlock(&mu);
    B->set_position(obj_at(e, a, 0), f, f + 3, f + 6);
    return OK;
}

static ua_ak_value h_render(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)e; (void)a; (void)c;
    if (B && B->render) B->render();
    return OK;
}

static ua_ak_value h_suspend(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)a; (void)c;
    if (B && B->suspend) B->suspend(!strcmp(e->api, "Suspend"));
    return OK;
}

// the callback queue the integration reads every frame (AkCallbackManager.PostCallbacks): Lock
// gives the first record (the header objects' pNext link the rest), Unlock frees them
static ua_ak_value h_cb_lock(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)e; (void)a; (void)c;
    pthread_mutex_lock(&mu);
    ak_obj *first = g_queue;
    g_locked = g_queue;
    for (ak_obj *h = g_queue; h; h = h->next) obj_set(h, "AkSerializedCallbackHeader_pNext", (uintptr_t)h->next);
    g_queue = NULL; g_queue_tail = &g_queue;
    pthread_mutex_unlock(&mu);
    return R((uintptr_t)first);
}

static ua_ak_value h_cb_unlock(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)e; (void)a; (void)c;
    pthread_mutex_lock(&mu);
    for (ak_obj *h = g_locked, *n; h; h = n) {
        n = h->next;
        ua_ak_value *dv = obj_field(h, fnv32("AkSerializedCallbackHeader_data"), 0);
        if (dv && otab_find(dv->p)) obj_free((ak_obj *)dv->p);
        obj_free(h);
    }
    g_locked = NULL;
    pthread_mutex_unlock(&mu);
    return R(0);
}

static ua_ak_value h_cb_term(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    h_cb_unlock(e, a, c);
    pthread_mutex_lock(&mu);
    g_locked = g_queue;
    g_queue = NULL; g_queue_tail = &g_queue;
    pthread_mutex_unlock(&mu);
    return h_cb_unlock(e, a, c);
}

static ua_ak_value h_getdata(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)c;
    pthread_mutex_lock(&mu);
    ak_obj *h = e->n > 0 ? otab_find(a[0].p) : NULL;
    ua_ak_value *v = h ? obj_field(h, fnv32("AkSerializedCallbackHeader_data"), 0) : NULL;
    uintptr_t d = v ? (uintptr_t)v->p : 0;
    pthread_mutex_unlock(&mu);
    return R(d);
}

static const struct { const char *api; handler h; } HANDLERS[] = {
    { "Init", h_init }, { "Term", h_term }, { "IsInitialized", h_isinit },
    { "SetBasePath", h_path }, { "AddBasePath", h_path }, { "SetDecodedBankPath", h_path }, { "SetCurrentLanguage", h_path },
    { "GetIDFromString", h_getid },
    { "LoadBank", h_loadbank }, { "LoadAndDecodeBank", h_loadbank }, { "LoadAndDecodeBankFromMemory", h_loadbank },
    { "PrepareBank", h_loadbank }, { "UnloadBank", h_unloadbank },
    { "PostEvent", h_postevent },
    { "StopPlayingID", h_stop }, { "StopAll", h_stop },
    { "CancelEventCallback", h_cancel }, { "CancelEventCallbackCookie", h_cancel },
    { "CancelEventCallbackGameObject", h_cancel }, { "CancelBankCallbackCookie", h_cancel },
    { "SetRTPCValue", h_rtpc }, { "SetRTPCValueByPlayingID", h_rtpc }, { "ResetRTPCValue", h_rtpc }, { "GetRTPCValue", h_rtpc },
    { "SetState", h_state }, { "GetState", h_state }, { "SetSwitch", h_state }, { "GetSwitch", h_state },
    { "PostTrigger", h_trigger },
    { "RegisterGameObjInternal", h_gameobj }, { "RegisterGameObjInternal_WithName", h_gameobj },
    { "UnregisterGameObjInternal", h_gameobj }, { "RegisterGameObj", h_gameobj }, { "UnregisterGameObj", h_gameobj },
    { "AkTransform_Set", h_transform }, { "AkTransform_SetPosition", h_transform }, { "AkTransform_SetOrientation", h_transform },
    { "SetObjectPosition", h_setposition },
    { "RenderAudio", h_render }, { "Suspend", h_suspend }, { "WakeupFromSuspend", h_suspend },
    { "AkCallbackSerializer_Lock", h_cb_lock }, { "AkCallbackSerializer_Unlock", h_cb_unlock },
    { "AkCallbackSerializer_Term", h_cb_term }, { "AkSerializedCallbackHeader_GetData", h_getdata },
};

// SWIG's plumbing and the defaults
static ua_ak_value h_new(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)a; (void)c;
    pthread_mutex_lock(&mu);
    ak_obj *o = obj_new(e->api + 4);
    pthread_mutex_unlock(&mu);
    return R((uintptr_t)o);
}
static ua_ak_value h_delete(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)c;
    pthread_mutex_lock(&mu);
    ak_obj *o = e->n > 0 ? otab_find(a[0].p) : NULL;
    if (o) obj_free(o);
    pthread_mutex_unlock(&mu);
    return R(0);
}
static ua_ak_value h_get(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)c;
    ua_ak_value r = R(0);
    if (e->ret == 'z') return r;                // (Mono frees a string result: none is given)
    pthread_mutex_lock(&mu);
    ak_obj *o = e->n == 0 ? &g_globals : otab_find(a[0].p);
    ua_ak_value *v = o ? obj_field(o, e->key, 0) : NULL;
    if (v) r = *v;
    // an embedded struct of the settings objects (AkDeviceSettings.threadProperties, AkPlatformInitSettings.
    // threadLEngine, ...): SWIG's getter gives its address, never NULL; a game may set its fields
    // (dwAffinityMask), and a NULL makes the wrapper null and throws in the middle of Init
    else if (o && o != &g_globals && e->ret == 'p' && strstr(e->api, "_thread")) {
        ak_obj *sub = obj_new("AkThreadProperties");
        obj_field(o, e->key, 1)->p = sub;
        r.p = sub;
    }
    pthread_mutex_unlock(&mu);
    return r;
}
static ua_ak_value h_set(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)c;
    int vi = e->n == 1 ? 0 : 1;
    pthread_mutex_lock(&mu);
    ak_obj *o = e->n == 1 ? &g_globals : otab_find(a[0].p);
    if (o && vi < e->n) {
        if (e->kind[vi] == 'z') obj_set_str(o, e->key, a[vi].z);
        else obj_field(o, e->key, 1)->u = a[vi].u;
    }
    pthread_mutex_unlock(&mu);
    return R(0);
}
static ua_ak_value h_upcast(const entry *e, ua_ak_value *a, const ua_ak_call *c) { (void)c; return e->n > 0 ? a[0] : R(0); }
static ua_ak_value h_default(const entry *e, ua_ak_value *a, const ua_ak_call *c)
{
    (void)a; (void)c;
    return R(e->ret == 'i' ? AK_SUCCESS : 0);
}

// --------------------------------------------------------------------------------------- dispatch

// A string argument as UTF-8. An integration may marshal its strings as UTF-16 ([MarshalAs(LPWStr)]:
// a macOS build of Wwise 2017.2's): a first character and a NUL, then more of the same (a
// one-character UTF-16 string reads the same as a C string). Everything below sees narrow strings.
#define AK_WIDE_STRINGS 4
#define AK_WIDE_BYTES 768
static const char *ak_narrow(const char *z, char *buf)
{
    const unsigned char *u = (const unsigned char *)z;
    if (!u[0] || u[1] || !u[2] || u[3]) return z;
    int o = 0;
    for (; (u[0] || u[1]) && o < AK_WIDE_BYTES - 4; u += 2) {
        unsigned c = u[0] | (unsigned)u[1] << 8;
        if (c < 0x80) buf[o++] = (char)c;
        else if (c < 0x800) { buf[o++] = (char)(0xC0 | c >> 6); buf[o++] = (char)(0x80 | (c & 63)); }
        else { buf[o++] = (char)(0xE0 | c >> 12); buf[o++] = (char)(0x80 | (c >> 6 & 63)); buf[o++] = (char)(0x80 | (c & 63)); }
    }
    buf[o] = 0;
    return buf;
}

static ua_ak_value call(entry *e, ua_ak_value *a)
{
    char wide[AK_WIDE_STRINGS][AK_WIDE_BYTES];
    for (int i = 0, k = 0; i < e->n && k < AK_WIDE_STRINGS; i++)
        if (e->kind[i] == 'z' && a[i].z) {
            const char *n = ak_narrow(a[i].z, wide[k]);
            if (n != a[i].z) { a[i].z = n; k++; }
        }
    ua_ak_call c = { e->name, e->api, e->ret, e->n, e->clsp, a };
    if (!g_backend_tried) { pthread_mutex_lock(&mu); backend_load(); pthread_mutex_unlock(&mu); }
    __atomic_add_fetch(&g_calls, 1, __ATOMIC_RELAXED);
    unsigned long n = __atomic_add_fetch(&e->calls, 1, __ATOMIC_RELAXED);
    ua_ak_value r;
    if (!(B && B->call && B->call(&c, &r))) r = e->h(e, a, &c);
    (void)n;
    if (g_trace > 0) {
        char b[256];
        int k = 0;
        for (int i = 0; i < e->n && k < (int)sizeof b - 40; i++)
            k += e->kind[i] == 'z' ? snprintf(b + k, sizeof b - k, "%s\"%.24s\"", i ? ", " : "", a[i].z ? a[i].z : "")
               : e->kind[i] == 'f' ? snprintf(b + k, sizeof b - k, "%s%g", i ? ", " : "", a[i].f)
               : snprintf(b + k, sizeof b - k, "%s%#llx", i ? ", " : "", (unsigned long long)a[i].u);
        LOG("%s(%s) -> %#llx\n", e->name, k ? b : "", (unsigned long long)r.u);
    }
    return r;
}

static uint64_t encode(char ret, ua_ak_value r)
{
    switch (ret) {
    case 'b': return r.u ? 1 : 0;
    case 'i': case 'u': return (uint32_t)r.u;
    case 'f': { uint32_t b; memcpy(&b, &r.f, 4); return b; }
    case 'p': case 'z': return (uintptr_t)r.p;
    case 'v': return 0;
    default: return r.u;             // l, d
    }
}

// 32-bit ARM, base AAPCS (the caller's r0-r3 and stack arguments as one run of words)
uint64_t ua_ak_dispatch32(unsigned k, const uint32_t *w);
uint64_t ua_ak_dispatch32(unsigned k, const uint32_t *w)
{
    entry *e = &g_ent[k];
    ua_ak_value a[AK_MAXARGS];
    int wi = 0;
    for (int i = 0; i < e->n; i++) {
        int n = e->cnt[i];
        switch (e->kind[i]) {
        case 'b': case 'i': a[i].i = (int32_t)w[wi++]; break;
        case 'u': a[i].u = w[wi++]; break;
        case 'p': case 'r': case 'z': a[i].u = 0; a[i].p = (void *)(uintptr_t)w[wi++]; break;
        case 'f': a[i].u = 0; memcpy(&a[i].f, &w[wi++], 4); break;
        case 'l': wi = (wi + 1) & ~1; a[i].u = w[wi] | (uint64_t)w[wi + 1] << 32; wi += 2; break;
        case 'd': wi = (wi + 1) & ~1; memcpy(&a[i].d, &w[wi], 8); wi += 2; break;
        case 's': case 'F': a[i].p = (void *)&w[wi]; wi += n; break;                      // by value: its words
        case 'S': wi = (wi + 1) & ~1; a[i].p = (void *)&w[wi]; wi += n; break;
        case 'D': wi = (wi + 1) & ~1; a[i].p = (void *)&w[wi]; wi += 2 * n; break;
        default: a[i].u = 0;
        }
    }
    return encode(e->ret, call(e, a));
}

// AArch64, AAPCS64: x0-x7 (regs[0..7]), d0-d7 (regs[8..15]), 8-byte stack slots
uint64_t ua_ak_dispatch64(unsigned k, const uint64_t *regs, const uint64_t *stack);
uint64_t ua_ak_dispatch64(unsigned k, const uint64_t *regs, const uint64_t *stack)
{
    entry *e = &g_ent[k];
    ua_ak_value a[AK_MAXARGS];
    uint64_t tmp[AK_MAXARGS][4];
    int ngrn = 0, nsrn = 0, nsaa = 0;
    for (int i = 0; i < e->n; i++) {
        int n = e->cnt[i];
        char c = e->kind[i];
        uint64_t v;
        switch (c) {
        case 'b': case 'i': case 'u': case 'l': case 'p': case 'r': case 'z':
            v = ngrn < 8 ? regs[ngrn++] : stack[nsaa++];
            if (c == 'b' || c == 'i') a[i].i = (int32_t)v;
            else if (c == 'u') a[i].u = (uint32_t)v;
            else a[i].u = v;
            break;
        case 'f': case 'd':
            v = nsrn < 8 ? regs[8 + nsrn++] : stack[nsaa++];
            a[i].u = 0;
            if (c == 'f') { uint32_t b = (uint32_t)v; memcpy(&a[i].f, &b, 4); } else memcpy(&a[i].d, &v, 8);
            break;
        case 'F': case 'D': {                   // a homogeneous float aggregate: one v register each
            int sz = c == 'F' ? 4 : 8;
            if (nsrn + n <= 8) {
                for (int j = 0; j < n; j++) memcpy((char *)tmp[i] + j * sz, &regs[8 + nsrn + j], sz);
                nsrn += n;
                a[i].p = tmp[i];
            } else {
                nsrn = 8;
                a[i].p = (void *)&stack[nsaa];
                nsaa += (n * sz + 7) / 8;
            }
            break;
        }
        case 's': case 'S': {                   // up to 16 bytes in x registers, else by reference
            int words = (n * 4 + 7) / 8;
            if (n * 4 > 16) { v = ngrn < 8 ? regs[ngrn++] : stack[nsaa++]; a[i].p = (void *)(uintptr_t)v; }
            else if (ngrn + words <= 8) { memcpy(tmp[i], &regs[ngrn], words * 8); ngrn += words; a[i].p = tmp[i]; }
            else { ngrn = 8; a[i].p = (void *)&stack[nsaa]; nsaa += words; }
            break;
        }
        default: a[i].u = 0;
        }
    }
    return encode(e->ret, call(e, a));
}

// --------------------------------------------------------------------------------------- the table

extern const char ua_ak_tramps[];

static int parse_classes(entry *e, const char *args)
{
    e->n = 0;
    if (!args || !strcmp(args, "-")) return 1;
    const char *p = args;
    while (*p) {
        if (e->n >= AK_MAXARGS) return 0;
        char k = *p++;
        int c = 1;
        if (!strchr("biulfdprzsSFD", k)) return 0;
        if (strchr("sSFD", k)) { c = (int)strtol(p, (char **)&p, 10); if (c < 1 || c > 16 || (strchr("FD", k) && c > 4)) return 0; }
        e->kind[e->n] = k; e->cnt[e->n] = (unsigned char)c;
        if (strchr("sSFD", k)) snprintf(e->cls[e->n], sizeof e->cls[0], "%c%d", k, c);
        else { e->cls[e->n][0] = k; e->cls[e->n][1] = 0; }
        e->clsp[e->n] = e->cls[e->n];
        e->n++;
        if (*p == ',') p++;
        else if (*p) return 0;
    }
    return 1;
}

static handler pick(entry *e)
{
    const char *a = e->api;
    for (unsigned i = 0; i < sizeof HANDLERS / sizeof HANDLERS[0]; i++) if (!strcmp(a, HANDLERS[i].api)) return HANDLERS[i].h;
    size_t l = strlen(a);
    if (!strncmp(a, "new_", 4)) return h_new;
    if (!strncmp(a, "delete_", 7)) return h_delete;
    if (l > 11 && !strcmp(a + l - 11, "_SWIGUpcast")) return h_upcast;
    int get = l > 4 && !strcmp(a + l - 4, "_get"), set = l > 4 && !strcmp(a + l - 4, "_set");
    // <Class>_<field>_get(object) / _set(object, value); a global: _get() / _set(value)
    if ((get && (e->n == 0 || (e->n == 1 && e->kind[0] == 'p'))) || (set && (e->n == 1 || (e->n == 2 && e->kind[0] == 'p')))) {
        char f[96];
        snprintf(f, sizeof f, "%.*s", (int)(l - 4), a);
        e->key = fnv32(f);
        return get ? h_get : h_set;
    }
    return h_default;
}

const so_shim *wwise_stub_table(const char *module, const ua_ak_fn *fns, int nfns, const ua_ak_const *consts, int nconsts)
{
    if (g_trace < 0) g_trace = getenv("UA_WWISE_TRACE") && atoi(getenv("UA_WWISE_TRACE")) > 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nconsts; i++) {
        g_const = realloc(g_const, (g_nconst + 1) * sizeof *g_const);
        snprintf(g_const[g_nconst].name, sizeof g_const[0].name, "%s", consts[i].name);
        g_const[g_nconst++].value = consts[i].value;
    }
    AK_SUCCESS = constant("AKRESULT.AK_Success", UA_AK_SUCCESS);
    AK_FAIL = constant("AKRESULT.AK_Fail", UA_AK_FAIL);
    CB_END_OF_EVENT = constant("AkCallbackType.AK_EndOfEvent", 1);
    CB_BANK = constant("AkCallbackType.AK_Bank", 0x40000000u);
    if (!g_ent) g_ent = calloc(UA_AK_TRAMPS, sizeof *g_ent);
    so_shim *tab = calloc(nfns + 1, sizeof *tab);
    int n = 0, bad = 0, full = 0, accessors = 0, handled = 0, queue = 0;
    for (int i = 0; i < nfns; i++) {
        if (g_nent >= UA_AK_TRAMPS) { full++; continue; }
        entry *e = &g_ent[g_nent];
        memset(e, 0, sizeof *e);
        if (!fns[i].ret || strlen(fns[i].ret) != 1 || !strchr("vbiulfdpz", fns[i].ret[0]) || !parse_classes(e, fns[i].args)) { bad++; continue; }
        e->name = strdup(fns[i].entry);
        e->ret = fns[i].ret[0];
        const char *a = e->name;
        if (!strncmp(a, "CSharp_", 7)) a += 7;
        snprintf(e->api, sizeof e->api, "%s", a);
        char *sw = strstr(e->api, "__SWIG_");
        if (sw) *sw = 0;
        e->h = pick(e);
        accessors += e->h == h_get || e->h == h_set || e->h == h_new || e->h == h_delete || e->h == h_upcast;
        handled += e->h != h_default && e->h != h_get && e->h != h_set && e->h != h_new && e->h != h_delete && e->h != h_upcast;
        queue += !strcmp(e->api, "AkCallbackSerializer_Lock") || !strcmp(e->api, "AkSerializedCallbackHeader_pNext_get");
        tab[n].name = e->name;
        tab[n].fn = (void *)(ua_ak_tramps + 8 * g_nent);
        n++;
        g_nent++;
    }
    pthread_mutex_unlock(&mu);
    if (!n) { free(tab); return NULL; }
    LOG("%s: %d entry points (%d handled by name, %d SWIG accessors, the rest by their result)%s%s; "
        "AK_Success %u, AK_EndOfEvent %#x, AK_Bank %#x\n", module, n, handled, accessors,
        bad ? ", some not read" : "", full ? ", too many: some left out" : "", AK_SUCCESS, CB_END_OF_EVENT, CB_BANK);
    if (queue < 2)
        LOG("%s: no callback queue with accessors (AkCallbackSerializer_Lock, AkSerializedCallbackHeader_pNext_get): an integration "
            "older than Wwise 2017.1 reads its callbacks itself, and gets none from the stub\n", module);
    return tab;
}
