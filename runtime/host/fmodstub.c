// ua_host's built-in FMOD (docs/plugins.md, "FMOD"): the libfmod / libfmodstudio a desktop build's
// FMOD for Unity integration P/Invokes (DllImport "fmodstudio", and "fmod" in older integrations),
// for ports without FMOD's own ARM libraries. Written from FMOD's public API names and documented
// results (FMOD 1.10's Studio and core C API, as the integration declares them); no FMOD code,
// header or data is used.
//
// Silent stand-in: every call succeeds with plausible results, so that the integration and the
// game run as with an FMOD that plays nothing: the system initializes, banks load (and report
// LOADED, so nothing waits for their samples), paths have GUIDs, event descriptions and instances
// are valid handles, a started instance is STOPPED at once (nothing waits for an end), parameters,
// volumes, pause and paused states are kept and given back (the options menu's bus volumes read
// back what it set), strings come back empty, callbacks are stored and never called. Unknown
// entry points of the API return FMOD_OK and write nothing.
//
// Handles are numbers, not pointers: a type tag, a slot and its generation (a released instance's
// handle stops being valid; slots are reused).
//
// Floats: the armeabi-v7a player's Mono passes them in core registers (softfp), so the entry
// points that take a float by value are built for the base AAPCS on 32-bit ARM (pcs("aapcs"));
// floats returned through pointers need nothing.
#define _GNU_SOURCE
#include "fmodstub.h"

#include <ctype.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if defined(__arm__) && defined(__ARM_PCS_VFP)
#define SOFTFP __attribute__((pcs("aapcs")))
#else
#define SOFTFP
#endif

// FMOD_RESULT values the stand-in uses
#define OK 0
#define ERR_INVALID_HANDLE 30
#define ERR_INVALID_PARAM 31
#define ERR_NOTREADY 46
#define ERR_TRUNCATED 51
#define ERR_EVENT_NOTFOUND 74

#define LOADING_STATE_LOADED 3
#define PLAYBACK_STOPPED 2

static pthread_mutex_t M = PTHREAD_MUTEX_INITIALIZER;
static int g_trace = -1;
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[fmod] %s\n", b);
}
#define TRACE(...) do { if (g_trace < 0) { const char *t = getenv("UA_FMOD_TRACE"); g_trace = t && *t == '1'; } if (g_trace) say(__VA_ARGS__); } while (0)

// ------------------------------------------------------------------------------------- handles
enum { T_SYS = 1, T_CORE, T_BANK, T_DESC, T_INST, T_PARAM, T_BUS, T_VCA, T_CG, T_SOUND };
#define SLOTS 4096
typedef struct { uint16_t gen; uint8_t used; } slot_hdr;

static uintptr_t mk(int type, int slot, uint16_t gen) { return ((uintptr_t)type << 28) | ((uintptr_t)gen << 12) | (uintptr_t)slot; }
static int h_type(uintptr_t h) { return (int)(h >> 28) & 15; }
static int h_slot(uintptr_t h) { return (int)(h & 0xfff); }
static uint16_t h_gen(uintptr_t h) { return (uint16_t)((h >> 12) & 0xffff); }

// banks
typedef struct { slot_hdr h; char path[256]; } bank_t;
static bank_t g_bank[256];
// event descriptions: by GUID
typedef struct { slot_hdr h; uint8_t id[16]; } desc_t;
static desc_t g_desc[SLOTS];
// instances
typedef struct {
    slot_hdr h;
    int desc;
    float vol, pitch, prop[8];
    int paused, released;
    uintptr_t userdata;
    void *cb;
    uint32_t mask;
} inst_t;
static inst_t g_inst[SLOTS];
// parameter instances: an instance's parameter by name, its value
typedef struct { slot_hdr h; int inst; uint16_t inst_gen; char name[64]; float value; } param_t;
static param_t g_param[SLOTS];
// buses and VCAs: by path, their volume, pause, mute
typedef struct { slot_hdr h; char path[192]; float vol; int paused, mute; } bus_t;
static bus_t g_bus[256], g_vca[64];
// sounds (programmer sounds): nothing to play in the silent stand-in
static float g_master_vol = 1.0f;
static uintptr_t g_sys_user, g_core_user;
// paths the game looked up: path -> GUID (an FNV-1a hash of the lower-case path, 16 bytes) and back
typedef struct { char path[192]; uint8_t id[16]; } pathid_t;
static pathid_t *g_paths; static int g_npaths, g_cpaths;

static int slot_new(slot_hdr *base, size_t stride, int n)
{
    for (int i = 1; i < n; i++) {
        slot_hdr *s = (slot_hdr *)((char *)base + stride * i);
        if (!s->used) { s->used = 1; s->gen++; if (!s->gen) s->gen = 1; return i; }
    }
    return -1;
}
// the slot of a valid handle of that type, or -1
static int slot_of(uintptr_t h, int type, slot_hdr *base, size_t stride, int n)
{
    if (h_type(h) != type) return -1;
    int i = h_slot(h);
    if (i <= 0 || i >= n) return -1;
    slot_hdr *s = (slot_hdr *)((char *)base + stride * i);
    return s->used && s->gen == h_gen(h) ? i : -1;
}
#define SLOT(hd, T, arr) slot_of((uintptr_t)(hd), T, &arr[0].h, sizeof arr[0], (int)(sizeof arr / sizeof arr[0]))
#define NEW(arr) slot_new(&arr[0].h, sizeof arr[0], (int)(sizeof arr / sizeof arr[0]))
#define H(T, arr, i) mk(T, i, arr[i].h.gen)

static const uintptr_t SYS = ((uintptr_t)T_SYS << 28) | 1, CORE = ((uintptr_t)T_CORE << 28) | 1, MASTER_CG = ((uintptr_t)T_CG << 28) | 1;

static void path_guid(const char *path, uint8_t out[16])
{
    uint64_t a = 1469598103934665603ull, b = 1099511628211ull ^ 0x9e3779b97f4a7c15ull;
    for (const char *p = path; *p; p++) {
        int c = tolower((unsigned char)*p);
        a = (a ^ (uint64_t)c) * 1099511628211ull;
        b = (b ^ (uint64_t)c) * 0x100000001b3ull + 0x9e3779b97f4a7c15ull;
    }
    memcpy(out, &a, 8);
    memcpy(out + 8, &b, 8);
}

static void put_str(char *dst, int size, int *retrieved, const char *s)
{
    int n = (int)strlen(s) + 1;
    if (retrieved) *retrieved = n;
    if (dst && size > 0) {
        int k = n < size ? n : size;
        memcpy(dst, s, (size_t)k);
        dst[k - 1] = 0;
    }
}
static int str_result(int size, const char *s) { return (int)strlen(s) + 1 > size ? ERR_TRUNCATED : OK; }

static int desc_for(const uint8_t id[16])
{
    for (int i = 1; i < SLOTS; i++) if (g_desc[i].h.used && !memcmp(g_desc[i].id, id, 16)) return i;
    int i = NEW(g_desc);
    if (i > 0) memcpy(g_desc[i].id, id, 16);
    return i;
}
static const char *path_of(const uint8_t id[16])
{
    for (int i = 0; i < g_npaths; i++) if (!memcmp(g_paths[i].id, id, 16)) return g_paths[i].path;
    return "";
}

// ------------------------------------------------------------------------------------- results
static int r_ok(void) { return OK; }

// --- core
static int Memory_GetStats(int *cur, int *max, int blocking) { (void)blocking; if (cur) *cur = 0; if (max) *max = 0; return OK; }
static int System_Create(uintptr_t *sys) { if (sys) *sys = CORE; return OK; }
static int System_GetVersion(uintptr_t s, uint32_t *v) { (void)s; if (v) *v = 0x00011013; return OK; }   // 1.10.13, the integration's
static int System_GetMasterChannelGroup(uintptr_t s, uintptr_t *cg) { (void)s; if (cg) *cg = MASTER_CG; return OK; }
static int System_GetSoftwareFormat(uintptr_t s, int *rate, int *mode, int *raw) { (void)s; if (rate) *rate = 48000; if (mode) *mode = 3; if (raw) *raw = 2; return OK; }
static int System_GetDSPBufferSize(uintptr_t s, uint32_t *len, int *n) { (void)s; if (len) *len = 1024; if (n) *n = 4; return OK; }
static int System_GetNumDrivers(uintptr_t s, int *n) { (void)s; if (n) *n = 1; return OK; }
static int System_LoadPlugin(uintptr_t s, const char *f, uint32_t *handle, uint32_t prio) { (void)s; (void)prio; TRACE("LoadPlugin(%s)", f ? f : ""); if (handle) *handle = 1; return OK; }
static int System_SetUserData(uintptr_t s, uintptr_t u) { (void)s; g_core_user = u; return OK; }
static int System_GetUserData(uintptr_t s, uintptr_t *u) { (void)s; if (u) *u = g_core_user; return OK; }
SOFTFP static int ChannelGroup_SetVolume(uintptr_t cg, float v) { if (cg == MASTER_CG) g_master_vol = v; return OK; }
static int ChannelGroup_GetVolume(uintptr_t cg, float *v) { if (v) *v = cg == MASTER_CG ? g_master_vol : 1.0f; return OK; }
static int Sound_GetLength(uintptr_t s, uint32_t *len, int unit) { (void)s; (void)unit; if (len) *len = 0; return OK; }
static int Sound_GetSubSound(uintptr_t s, int i, uintptr_t *sub) { (void)i; if (sub) *sub = s; return OK; }
static int System_CreateSound(uintptr_t sys, const void *name, uint32_t mode, void *ex, uintptr_t *sound) { (void)sys; (void)name; (void)mode; (void)ex; if (sound) *sound = ((uintptr_t)T_SOUND << 28) | 1; return OK; }

// --- Studio: system
static int Studio_System_Create(uintptr_t *sys, uint32_t header) { TRACE("Studio_System_Create(%#x)", header); if (sys) *sys = SYS; return OK; }
static int Studio_System_IsValid(uintptr_t s) { return s == SYS; }
static int Studio_System_Initialize(uintptr_t s, int maxch, uint32_t sf, uint32_t f, void *x)
{
    (void)s; (void)x;
    static int once;
    if (!once++) say("built-in FMOD (silent): Studio::System::initialize(%d channels, studio flags %#x, flags %#x); no sound", maxch, sf, f);
    return OK;
}
static int Studio_System_GetLowLevelSystem(uintptr_t s, uintptr_t *core) { (void)s; if (core) *core = CORE; return OK; }
static int Studio_System_SetUserData(uintptr_t s, uintptr_t u) { (void)s; g_sys_user = u; return OK; }
static int Studio_System_GetUserData(uintptr_t s, uintptr_t *u) { (void)s; if (u) *u = g_sys_user; return OK; }
static int Studio_System_LoadBankFile(uintptr_t s, const char *file, uint32_t flags, uintptr_t *bank)
{
    (void)s; (void)flags;
    pthread_mutex_lock(&M);
    int i = 0;
    for (int k = 1; k < 256; k++) if (g_bank[k].h.used && !strcmp(g_bank[k].path, file ? file : "")) { i = k; break; }
    if (!i) {
        i = NEW(g_bank);
        if (i > 0) snprintf(g_bank[i].path, sizeof g_bank[i].path, "%s", file ? file : "");
    }
    uintptr_t h = i > 0 ? H(T_BANK, g_bank, i) : 0;
    pthread_mutex_unlock(&M);
    TRACE("LoadBankFile(%s)", file ? file : "");
    if (bank) *bank = h;
    return h ? OK : ERR_INVALID_PARAM;
}
static int Studio_System_LoadBankMemory(uintptr_t s, const void *buf, int len, int mode, uint32_t flags, uintptr_t *bank)
{
    (void)buf; (void)len; (void)mode;
    char name[32];
    snprintf(name, sizeof name, "(memory %d)", len);
    return Studio_System_LoadBankFile(s, name, flags, bank);
}
static int Studio_System_LookupID(uintptr_t s, const char *path, uint8_t id[16])
{
    (void)s;
    if (!path || !*path) return ERR_EVENT_NOTFOUND;
    path_guid(path, id);
    pthread_mutex_lock(&M);
    int known = 0;
    for (int i = 0; i < g_npaths; i++) if (!memcmp(g_paths[i].id, id, 16)) { known = 1; break; }
    if (!known) {
        if (g_npaths >= g_cpaths) { g_cpaths = g_cpaths ? g_cpaths * 2 : 256; g_paths = realloc(g_paths, (size_t)g_cpaths * sizeof *g_paths); }
        snprintf(g_paths[g_npaths].path, sizeof g_paths[0].path, "%s", path);
        memcpy(g_paths[g_npaths++].id, id, 16);
    }
    pthread_mutex_unlock(&M);
    return OK;
}
static int Studio_System_LookupPath(uintptr_t s, const uint8_t id[16], char *buf, int size, int *ret)
{
    (void)s;
    pthread_mutex_lock(&M);
    const char *p = path_of(id);
    put_str(buf, size, ret, p);
    int r = str_result(size, p);
    pthread_mutex_unlock(&M);
    return r;
}
static int Studio_System_GetEventByID(uintptr_t s, const uint8_t id[16], uintptr_t *desc)
{
    (void)s;
    pthread_mutex_lock(&M);
    int i = desc_for(id);
    uintptr_t h = i > 0 ? H(T_DESC, g_desc, i) : 0;
    pthread_mutex_unlock(&M);
    if (desc) *desc = h;
    return h ? OK : ERR_EVENT_NOTFOUND;
}
static int Studio_System_GetEvent(uintptr_t s, const char *path, uintptr_t *desc)
{
    uint8_t id[16];
    int r = Studio_System_LookupID(s, path, id);
    return r ? r : Studio_System_GetEventByID(s, id, desc);
}
static int bus_get(bus_t *arr, int n, int type, const char *path, uintptr_t *out)
{
    pthread_mutex_lock(&M);
    int i = 0;
    for (int k = 1; k < n; k++) if (arr[k].h.used && !strcasecmp(arr[k].path, path ? path : "")) { i = k; break; }
    if (!i) {
        i = slot_new(&arr[0].h, sizeof arr[0], n);
        if (i > 0) { snprintf(arr[i].path, sizeof arr[i].path, "%s", path ? path : ""); arr[i].vol = 1.0f; arr[i].paused = arr[i].mute = 0; }
    }
    uintptr_t h = i > 0 ? mk(type, i, arr[i].h.gen) : 0;
    pthread_mutex_unlock(&M);
    if (out) *out = h;
    return h ? OK : ERR_EVENT_NOTFOUND;
}
static int Studio_System_GetBus(uintptr_t s, const char *path, uintptr_t *bus) { (void)s; return bus_get(g_bus, 256, T_BUS, path, bus); }
static int Studio_System_GetVCA(uintptr_t s, const char *path, uintptr_t *vca) { (void)s; return bus_get(g_vca, 64, T_VCA, path, vca); }
static int Studio_System_GetBankCount(uintptr_t s, int *n)
{
    (void)s;
    int c = 0;
    for (int i = 1; i < 256; i++) c += g_bank[i].h.used;
    if (n) *n = c;
    return OK;
}
static int Studio_System_GetBankList(uintptr_t s, uintptr_t *arr, int cap, int *n)
{
    (void)s;
    int c = 0;
    for (int i = 1; i < 256 && c < cap; i++) if (g_bank[i].h.used) arr[c++] = H(T_BANK, g_bank, i);
    if (n) *n = c;
    return OK;
}
static int Studio_System_GetSoundInfo(uintptr_t s, const char *key, void *info) { (void)s; (void)key; (void)info; return ERR_EVENT_NOTFOUND; }
static int Studio_System_GetNumListeners(uintptr_t s, int *n) { (void)s; if (n) *n = 1; return OK; }
static int Studio_ParseID(const char *str, uint8_t id[16])
{
    // "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}": FMOD's GUID struct (Data1..3 little-endian)
    if (!str || strlen(str) != 38 || str[0] != '{') return ERR_INVALID_PARAM;
    uint8_t b[16];
    int k = 0;
    for (const char *p = str + 1; *p && *p != '}' && k < 16;) {
        if (*p == '-') { p++; continue; }
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) return ERR_INVALID_PARAM;
        char t[3] = { p[0], p[1], 0 };
        b[k++] = (uint8_t)strtoul(t, NULL, 16);
        p += 2;
    }
    if (k != 16) return ERR_INVALID_PARAM;
    uint8_t g[16] = { b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6] };
    memcpy(g + 8, b + 8, 8);
    if (id) memcpy(id, g, 16);
    return OK;
}

// --- banks
static int Studio_Bank_IsValid(uintptr_t b) { return SLOT(b, T_BANK, g_bank) > 0; }
static int Studio_Bank_Unload(uintptr_t b)
{
    pthread_mutex_lock(&M);
    int i = SLOT(b, T_BANK, g_bank);
    if (i > 0) g_bank[i].h.used = 0;
    pthread_mutex_unlock(&M);
    return i > 0 ? OK : ERR_INVALID_HANDLE;
}
static int Studio_Bank_GetLoadingState(uintptr_t b, int *st) { if (st) *st = LOADING_STATE_LOADED; return SLOT(b, T_BANK, g_bank) > 0 ? OK : ERR_INVALID_HANDLE; }
static int Studio_Bank_GetPath(uintptr_t b, char *buf, int size, int *ret)
{
    int i = SLOT(b, T_BANK, g_bank);
    if (i <= 0) return ERR_INVALID_HANDLE;
    // "bank:/<name>" from the file name without its folder and extension
    char p[300];
    const char *f = g_bank[i].path, *sl = strrchr(f, '/');
    snprintf(p, sizeof p, "bank:/%s", sl ? sl + 1 : f);
    char *dot = strrchr(p, '.');
    if (dot && !strcasecmp(dot, ".bank")) *dot = 0;
    put_str(buf, size, ret, p);
    return str_result(size, p);
}
static int zero_count(uintptr_t x, int *n) { (void)x; if (n) *n = 0; return OK; }
static int zero_list(uintptr_t x, void *arr, int cap, int *n) { (void)x; (void)arr; (void)cap; if (n) *n = 0; return OK; }

// --- event descriptions
static int Studio_EventDescription_IsValid(uintptr_t d) { return SLOT(d, T_DESC, g_desc) > 0; }
static int Studio_EventDescription_GetID(uintptr_t d, uint8_t id[16]) { int i = SLOT(d, T_DESC, g_desc); if (i <= 0) return ERR_INVALID_HANDLE; memcpy(id, g_desc[i].id, 16); return OK; }
static int Studio_EventDescription_GetPath(uintptr_t d, char *buf, int size, int *ret)
{
    int i = SLOT(d, T_DESC, g_desc);
    if (i <= 0) return ERR_INVALID_HANDLE;
    pthread_mutex_lock(&M);
    const char *p = path_of(g_desc[i].id);
    put_str(buf, size, ret, p);
    int r = str_result(size, p);
    pthread_mutex_unlock(&M);
    return r;
}
static int Studio_EventDescription_GetSampleLoadingState(uintptr_t d, int *st) { (void)d; if (st) *st = LOADING_STATE_LOADED; return OK; }
static int Studio_EventDescription_IsOneshot(uintptr_t d, int *b) { (void)d; if (b) *b = 1; return OK; }
static int Studio_EventDescription_GetLength(uintptr_t d, int *ms) { (void)d; if (ms) *ms = 0; return OK; }
static int Studio_EventDescription_GetInstanceCount(uintptr_t d, int *n)
{
    int i = SLOT(d, T_DESC, g_desc), c = 0;
    for (int k = 1; k < SLOTS; k++) c += g_inst[k].h.used && !g_inst[k].released && g_inst[k].desc == i;
    if (n) *n = c;
    return OK;
}
static int Studio_EventDescription_CreateInstance(uintptr_t d, uintptr_t *inst)
{
    int di = SLOT(d, T_DESC, g_desc);
    if (di <= 0) { if (inst) *inst = 0; return ERR_INVALID_HANDLE; }
    pthread_mutex_lock(&M);
    int i = NEW(g_inst);
    uintptr_t h = 0;
    if (i > 0) {
        inst_t *x = &g_inst[i];
        uint16_t gen = x->h.gen;
        memset(x, 0, sizeof *x);
        x->h.used = 1; x->h.gen = gen;
        x->desc = di; x->vol = x->pitch = 1.0f;
        h = H(T_INST, g_inst, i);
    }
    pthread_mutex_unlock(&M);
    if (inst) *inst = h;
    return h ? OK : ERR_NOTREADY;
}

// --- event instances
#define INST(h) SLOT(h, T_INST, g_inst)
static void inst_free_params(int i)
{
    for (int k = 1; k < SLOTS; k++) if (g_param[k].h.used && g_param[k].inst == i) g_param[k].h.used = 0;
}
static int Studio_EventInstance_IsValid(uintptr_t e) { return INST(e) > 0; }
static int Studio_EventInstance_GetDescription(uintptr_t e, uintptr_t *d) { int i = INST(e); if (i <= 0) return ERR_INVALID_HANDLE; if (d) *d = H(T_DESC, g_desc, g_inst[i].desc); return OK; }
static int Studio_EventInstance_Start(uintptr_t e) { return INST(e) > 0 ? OK : ERR_INVALID_HANDLE; }
static int Studio_EventInstance_Stop(uintptr_t e, int mode) { (void)mode; return INST(e) > 0 ? OK : ERR_INVALID_HANDLE; }
static int Studio_EventInstance_Release(uintptr_t e)
{
    pthread_mutex_lock(&M);
    int i = INST(e);
    if (i > 0) { inst_free_params(i); g_inst[i].h.used = 0; }
    pthread_mutex_unlock(&M);
    return i > 0 ? OK : ERR_INVALID_HANDLE;
}
static int Studio_EventInstance_GetPlaybackState(uintptr_t e, int *st) { if (st) *st = PLAYBACK_STOPPED; return INST(e) > 0 ? OK : ERR_INVALID_HANDLE; }
SOFTFP static int Studio_EventInstance_SetVolume(uintptr_t e, float v) { int i = INST(e); if (i <= 0) return ERR_INVALID_HANDLE; g_inst[i].vol = v; return OK; }
static int Studio_EventInstance_GetVolume(uintptr_t e, float *v, float *fv) { int i = INST(e); float x = i > 0 ? g_inst[i].vol : 0; if (v) *v = x; if (fv) *fv = x; return i > 0 ? OK : ERR_INVALID_HANDLE; }
SOFTFP static int Studio_EventInstance_SetPitch(uintptr_t e, float v) { int i = INST(e); if (i <= 0) return ERR_INVALID_HANDLE; g_inst[i].pitch = v; return OK; }
static int Studio_EventInstance_GetPitch(uintptr_t e, float *v, float *fv) { int i = INST(e); float x = i > 0 ? g_inst[i].pitch : 0; if (v) *v = x; if (fv) *fv = x; return i > 0 ? OK : ERR_INVALID_HANDLE; }
SOFTFP static int Studio_EventInstance_SetProperty(uintptr_t e, int k, float v) { int i = INST(e); if (i <= 0) return ERR_INVALID_HANDLE; if (k >= 0 && k < 8) g_inst[i].prop[k] = v; return OK; }
static int Studio_EventInstance_GetProperty(uintptr_t e, int k, float *v) { int i = INST(e); if (i <= 0) return ERR_INVALID_HANDLE; if (v) *v = k >= 0 && k < 8 ? g_inst[i].prop[k] : 0; return OK; }
static int Studio_EventInstance_SetPaused(uintptr_t e, int p) { int i = INST(e); if (i <= 0) return ERR_INVALID_HANDLE; g_inst[i].paused = p != 0; return OK; }
static int Studio_EventInstance_GetPaused(uintptr_t e, int *p) { int i = INST(e); if (i <= 0) return ERR_INVALID_HANDLE; if (p) *p = g_inst[i].paused; return OK; }
static int Studio_EventInstance_GetTimelinePosition(uintptr_t e, int *ms) { if (ms) *ms = 0; return INST(e) > 0 ? OK : ERR_INVALID_HANDLE; }
static int Studio_EventInstance_SetUserData(uintptr_t e, uintptr_t u) { int i = INST(e); if (i <= 0) return ERR_INVALID_HANDLE; g_inst[i].userdata = u; return OK; }
static int Studio_EventInstance_GetUserData(uintptr_t e, uintptr_t *u) { int i = INST(e); if (i <= 0) { if (u) *u = 0; return ERR_INVALID_HANDLE; } if (u) *u = g_inst[i].userdata; return OK; }
static int Studio_EventInstance_SetCallback(uintptr_t e, void *cb, uint32_t mask) { int i = INST(e); if (i <= 0) return ERR_INVALID_HANDLE; g_inst[i].cb = cb; g_inst[i].mask = mask; return OK; }
static int Studio_EventInstance_GetChannelGroup(uintptr_t e, uintptr_t *cg) { (void)e; if (cg) *cg = 0; return ERR_NOTREADY; }
static int Studio_EventInstance_GetParameterCount(uintptr_t e, int *n) { if (n) *n = 0; return INST(e) > 0 ? OK : ERR_INVALID_HANDLE; }

// a parameter of an instance by name: its slot, made on first use
static int param_for(int inst, const char *name)
{
    for (int k = 1; k < SLOTS; k++)
        if (g_param[k].h.used && g_param[k].inst == inst && g_param[k].inst_gen == g_inst[inst].h.gen && !strcasecmp(g_param[k].name, name)) return k;
    int k = NEW(g_param);
    if (k > 0) {
        g_param[k].inst = inst;
        g_param[k].inst_gen = g_inst[inst].h.gen;
        snprintf(g_param[k].name, sizeof g_param[k].name, "%s", name);
        g_param[k].value = 0;
    }
    return k;
}
static int Studio_EventInstance_GetParameter(uintptr_t e, const char *name, uintptr_t *p)
{
    pthread_mutex_lock(&M);
    int i = INST(e), k = i > 0 && name ? param_for(i, name) : -1;
    uintptr_t h = k > 0 ? H(T_PARAM, g_param, k) : 0;
    pthread_mutex_unlock(&M);
    if (p) *p = h;
    return h ? OK : i > 0 ? ERR_EVENT_NOTFOUND : ERR_INVALID_HANDLE;
}
SOFTFP static int Studio_EventInstance_SetParameterValue(uintptr_t e, const char *name, float v)
{
    pthread_mutex_lock(&M);
    int i = INST(e), k = i > 0 && name ? param_for(i, name) : -1;
    if (k > 0) g_param[k].value = v;
    pthread_mutex_unlock(&M);
    return k > 0 ? OK : ERR_INVALID_HANDLE;
}
static int Studio_EventInstance_GetParameterValue(uintptr_t e, const char *name, float *v, float *fv)
{
    pthread_mutex_lock(&M);
    int i = INST(e), k = i > 0 && name ? param_for(i, name) : -1;
    float x = k > 0 ? g_param[k].value : 0;
    pthread_mutex_unlock(&M);
    if (v) *v = x;
    if (fv) *fv = x;
    return k > 0 ? OK : ERR_INVALID_HANDLE;
}
#define PARAM(h) SLOT(h, T_PARAM, g_param)
static int Studio_ParameterInstance_IsValid(uintptr_t p) { int k = PARAM(p); return k > 0 && g_inst[g_param[k].inst].h.used && g_inst[g_param[k].inst].h.gen == g_param[k].inst_gen; }
SOFTFP static int Studio_ParameterInstance_SetValue(uintptr_t p, float v) { int k = PARAM(p); if (k <= 0) return ERR_INVALID_HANDLE; g_param[k].value = v; return OK; }
static int Studio_ParameterInstance_GetValue(uintptr_t p, float *v) { int k = PARAM(p); if (v) *v = k > 0 ? g_param[k].value : 0; return k > 0 ? OK : ERR_INVALID_HANDLE; }

// --- buses and VCAs
static bus_t *bus_of(uintptr_t h) { int t = h_type(h); int i = t == T_BUS ? SLOT(h, T_BUS, g_bus) : t == T_VCA ? SLOT(h, T_VCA, g_vca) : -1; return i > 0 ? (t == T_BUS ? &g_bus[i] : &g_vca[i]) : NULL; }
static int Studio_Bus_IsValid(uintptr_t b) { return h_type(b) == T_BUS && bus_of(b) != NULL; }
static int Studio_VCA_IsValid(uintptr_t b) { return h_type(b) == T_VCA && bus_of(b) != NULL; }
SOFTFP static int Studio_Bus_SetVolume(uintptr_t b, float v) { bus_t *x = bus_of(b); if (!x) return ERR_INVALID_HANDLE; x->vol = v; return OK; }
static int Studio_Bus_GetVolume(uintptr_t b, float *v, float *fv) { bus_t *x = bus_of(b); float y = x ? x->vol : 0; if (v) *v = y; if (fv) *fv = y; return x ? OK : ERR_INVALID_HANDLE; }
static int Studio_Bus_SetPaused(uintptr_t b, int p) { bus_t *x = bus_of(b); if (!x) return ERR_INVALID_HANDLE; x->paused = p != 0; return OK; }
static int Studio_Bus_GetPaused(uintptr_t b, int *p) { bus_t *x = bus_of(b); if (p) *p = x ? x->paused : 0; return x ? OK : ERR_INVALID_HANDLE; }
static int Studio_Bus_SetMute(uintptr_t b, int m) { bus_t *x = bus_of(b); if (!x) return ERR_INVALID_HANDLE; x->mute = m != 0; return OK; }
static int Studio_Bus_GetMute(uintptr_t b, int *m) { bus_t *x = bus_of(b); if (m) *m = x ? x->mute : 0; return x ? OK : ERR_INVALID_HANDLE; }
static int Studio_Bus_GetPath(uintptr_t b, char *buf, int size, int *ret) { bus_t *x = bus_of(b); if (!x) return ERR_INVALID_HANDLE; put_str(buf, size, ret, x->path); return str_result(size, x->path); }
static int Studio_Bus_GetChannelGroup(uintptr_t b, uintptr_t *cg) { (void)b; if (cg) *cg = 0; return ERR_NOTREADY; }   // as before the bus has a mix

#define F(name, fn) { name, (void *)(fn) }
#include "fmodstub_names.h"     // FMOD_ENTRY_POINTS: every entry point of the API the integration declares

static so_shim *g_table;

const so_shim *fmodstub_table(void)
{
    if (g_table) return g_table;
    static const so_shim impl[] = {
        F("FMOD5_Memory_GetStats", Memory_GetStats),
        F("FMOD5_System_Create", System_Create),
        F("FMOD5_System_GetVersion", System_GetVersion),
        F("FMOD5_System_GetMasterChannelGroup", System_GetMasterChannelGroup),
        F("FMOD5_System_GetSoftwareFormat", System_GetSoftwareFormat),
        F("FMOD5_System_GetDSPBufferSize", System_GetDSPBufferSize),
        F("FMOD5_System_GetNumDrivers", System_GetNumDrivers),
        F("FMOD5_System_LoadPlugin", System_LoadPlugin),
        F("FMOD5_System_SetUserData", System_SetUserData),
        F("FMOD5_System_GetUserData", System_GetUserData),
        F("FMOD5_System_CreateSound", System_CreateSound),
        F("FMOD5_System_CreateStream", System_CreateSound),
        F("FMOD5_ChannelGroup_SetVolume", ChannelGroup_SetVolume),
        F("FMOD5_ChannelGroup_GetVolume", ChannelGroup_GetVolume),
        F("FMOD5_Sound_GetLength", Sound_GetLength),
        F("FMOD5_Sound_GetSubSound", Sound_GetSubSound),
        F("FMOD_Studio_System_Create", Studio_System_Create),
        F("FMOD_Studio_System_IsValid", Studio_System_IsValid),
        F("FMOD_Studio_System_Initialize", Studio_System_Initialize),
        F("FMOD_Studio_System_GetLowLevelSystem", Studio_System_GetLowLevelSystem),
        F("FMOD_Studio_System_SetUserData", Studio_System_SetUserData),
        F("FMOD_Studio_System_GetUserData", Studio_System_GetUserData),
        F("FMOD_Studio_System_LoadBankFile", Studio_System_LoadBankFile),
        F("FMOD_Studio_System_LoadBankMemory", Studio_System_LoadBankMemory),
        F("FMOD_Studio_System_LookupID", Studio_System_LookupID),
        F("FMOD_Studio_System_LookupPath", Studio_System_LookupPath),
        F("FMOD_Studio_System_GetEvent", Studio_System_GetEvent),
        F("FMOD_Studio_System_GetEventByID", Studio_System_GetEventByID),
        F("FMOD_Studio_System_GetBus", Studio_System_GetBus),
        F("FMOD_Studio_System_GetVCA", Studio_System_GetVCA),
        F("FMOD_Studio_System_GetBankCount", Studio_System_GetBankCount),
        F("FMOD_Studio_System_GetBankList", Studio_System_GetBankList),
        F("FMOD_Studio_System_GetSoundInfo", Studio_System_GetSoundInfo),
        F("FMOD_Studio_System_GetNumListeners", Studio_System_GetNumListeners),
        F("FMOD_Studio_ParseID", Studio_ParseID),
        F("FMOD_Studio_Bank_IsValid", Studio_Bank_IsValid),
        F("FMOD_Studio_Bank_Unload", Studio_Bank_Unload),
        F("FMOD_Studio_Bank_GetLoadingState", Studio_Bank_GetLoadingState),
        F("FMOD_Studio_Bank_GetSampleLoadingState", Studio_Bank_GetLoadingState),
        F("FMOD_Studio_Bank_GetPath", Studio_Bank_GetPath),
        F("FMOD_Studio_Bank_GetEventCount", zero_count),
        F("FMOD_Studio_Bank_GetBusCount", zero_count),
        F("FMOD_Studio_Bank_GetVCACount", zero_count),
        F("FMOD_Studio_Bank_GetStringCount", zero_count),
        F("FMOD_Studio_Bank_GetEventList", zero_list),
        F("FMOD_Studio_Bank_GetBusList", zero_list),
        F("FMOD_Studio_Bank_GetVCAList", zero_list),
        F("FMOD_Studio_EventDescription_IsValid", Studio_EventDescription_IsValid),
        F("FMOD_Studio_EventDescription_GetID", Studio_EventDescription_GetID),
        F("FMOD_Studio_EventDescription_GetPath", Studio_EventDescription_GetPath),
        F("FMOD_Studio_EventDescription_GetSampleLoadingState", Studio_EventDescription_GetSampleLoadingState),
        F("FMOD_Studio_EventDescription_IsOneshot", Studio_EventDescription_IsOneshot),
        F("FMOD_Studio_EventDescription_GetLength", Studio_EventDescription_GetLength),
        F("FMOD_Studio_EventDescription_GetInstanceCount", Studio_EventDescription_GetInstanceCount),
        F("FMOD_Studio_EventDescription_GetParameterCount", zero_count),
        F("FMOD_Studio_EventDescription_GetUserPropertyCount", zero_count),
        F("FMOD_Studio_EventDescription_CreateInstance", Studio_EventDescription_CreateInstance),
        F("FMOD_Studio_EventInstance_IsValid", Studio_EventInstance_IsValid),
        F("FMOD_Studio_EventInstance_GetDescription", Studio_EventInstance_GetDescription),
        F("FMOD_Studio_EventInstance_Start", Studio_EventInstance_Start),
        F("FMOD_Studio_EventInstance_Stop", Studio_EventInstance_Stop),
        F("FMOD_Studio_EventInstance_Release", Studio_EventInstance_Release),
        F("FMOD_Studio_EventInstance_GetPlaybackState", Studio_EventInstance_GetPlaybackState),
        F("FMOD_Studio_EventInstance_SetVolume", Studio_EventInstance_SetVolume),
        F("FMOD_Studio_EventInstance_GetVolume", Studio_EventInstance_GetVolume),
        F("FMOD_Studio_EventInstance_SetPitch", Studio_EventInstance_SetPitch),
        F("FMOD_Studio_EventInstance_GetPitch", Studio_EventInstance_GetPitch),
        F("FMOD_Studio_EventInstance_SetProperty", Studio_EventInstance_SetProperty),
        F("FMOD_Studio_EventInstance_GetProperty", Studio_EventInstance_GetProperty),
        F("FMOD_Studio_EventInstance_SetPaused", Studio_EventInstance_SetPaused),
        F("FMOD_Studio_EventInstance_GetPaused", Studio_EventInstance_GetPaused),
        F("FMOD_Studio_EventInstance_GetTimelinePosition", Studio_EventInstance_GetTimelinePosition),
        F("FMOD_Studio_EventInstance_SetUserData", Studio_EventInstance_SetUserData),
        F("FMOD_Studio_EventInstance_GetUserData", Studio_EventInstance_GetUserData),
        F("FMOD_Studio_EventInstance_SetCallback", Studio_EventInstance_SetCallback),
        F("FMOD_Studio_EventInstance_GetChannelGroup", Studio_EventInstance_GetChannelGroup),
        F("FMOD_Studio_EventInstance_GetParameterCount", Studio_EventInstance_GetParameterCount),
        F("FMOD_Studio_EventInstance_GetParameter", Studio_EventInstance_GetParameter),
        F("FMOD_Studio_EventInstance_SetParameterValue", Studio_EventInstance_SetParameterValue),
        F("FMOD_Studio_EventInstance_GetParameterValue", Studio_EventInstance_GetParameterValue),
        F("FMOD_Studio_ParameterInstance_IsValid", Studio_ParameterInstance_IsValid),
        F("FMOD_Studio_ParameterInstance_SetValue", Studio_ParameterInstance_SetValue),
        F("FMOD_Studio_ParameterInstance_GetValue", Studio_ParameterInstance_GetValue),
        F("FMOD_Studio_Bus_IsValid", Studio_Bus_IsValid),
        F("FMOD_Studio_Bus_SetVolume", Studio_Bus_SetVolume),
        F("FMOD_Studio_Bus_GetVolume", Studio_Bus_GetVolume),
        F("FMOD_Studio_Bus_SetPaused", Studio_Bus_SetPaused),
        F("FMOD_Studio_Bus_GetPaused", Studio_Bus_GetPaused),
        F("FMOD_Studio_Bus_SetMute", Studio_Bus_SetMute),
        F("FMOD_Studio_Bus_GetMute", Studio_Bus_GetMute),
        F("FMOD_Studio_Bus_GetPath", Studio_Bus_GetPath),
        F("FMOD_Studio_Bus_GetChannelGroup", Studio_Bus_GetChannelGroup),
        F("FMOD_Studio_VCA_IsValid", Studio_VCA_IsValid),
        F("FMOD_Studio_VCA_SetVolume", Studio_Bus_SetVolume),
        F("FMOD_Studio_VCA_GetVolume", Studio_Bus_GetVolume),
        F("FMOD_Studio_VCA_GetPath", Studio_Bus_GetPath),
    };
    static const char *const names[] = { FMOD_ENTRY_POINTS };
    int ni = (int)(sizeof impl / sizeof impl[0]), nn = (int)(sizeof names / sizeof names[0]);
    so_shim *t = calloc((size_t)(ni + nn + 1), sizeof *t);
    int n = 0;
    for (int i = 0; i < ni; i++) t[n++] = impl[i];
    for (int i = 0; i < nn; i++) {
        int dup = 0;
        for (int k = 0; k < ni && !dup; k++) dup = !strcmp(impl[k].name, names[i]);
        if (!dup) t[n++] = (so_shim){ names[i], (void *)r_ok };
    }
    t[n] = (so_shim){ NULL, NULL };
    g_table = t;
    return g_table;
}
