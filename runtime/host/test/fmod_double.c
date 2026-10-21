// fmod_double.c: an ABI-compatible test double of the FMOD Engine / FMOD Studio C API calls Unity's
// FMOD integration makes, to test the runtime's plumbing (plugins.c, the loader, Mono's P/Invoke,
// org.fmod.FMOD in runtime/jni) without FMOD. Written from FMOD's public API names and documented
// signatures; no FMOD code or header is used, and it makes no sound. Every call is logged to stderr
// as "[fmod-double <flavor>] Name(args) -> result", so two runs (Android library, glibc library)
// can be compared line by line.
//
// Two builds (runtime/host/test/plugins_test.sh):
//   -DDOUBLE_ANDROID: armeabi-v7a / arm64-v8a code linked like an NDK library (freestanding, softfp
//     on 32-bit ARM, DT_NEEDED libc.so): FMOD's Android libraries are this kind; it also calls the
//     Java side FMOD's Android library needs (org.fmod.FMOD) through JNI_OnLoad's JavaVM
//   otherwise: a glibc (armhf / aarch64) library, as FMOD's Linux libraries are; on 32-bit ARM its
//     float arguments arrive in VFP registers, i.e. through ua_host's softfp thunks
// Install as lib/libfmodstudio.so (and lib/libfmod.so for wrappers that import "fmod").
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef DOUBLE_ANDROID
#include "../../jni/include/jni.h"
#define FLAVOR "android"
// bionic's libc, resolved by the loader (no headers: this is built freestanding)
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
void *calloc(size_t, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
char *strncpy(char *, const char *, size_t);
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
long write(int, const void *, size_t);
int open(const char *, int, ...);
long read(int, void *, size_t);
long lseek(int, long, int);
int close(int);
#else
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__aarch64__)
#define FLAVOR "glibc-arm64"
#else
#define FLAVOR "glibc"
#endif
#endif

#define EXPORT __attribute__((visibility("default")))
#define OK 0
#define ERR_FILE_NOTFOUND 18
#define ERR_INVALID_HANDLE 30
#define ERR_INVALID_PARAM 31
#define ERR_EVENT_NOTFOUND 74
#define LOADED 3
#define PLAYING 0
#define STOPPED 2

static void say(const char *fmt, ...)
{
    char b[600];
    int n = snprintf(b, sizeof b, "[fmod-double " FLAVOR "] ");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof b - n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    write(2, b, n);
}

// ---- handles: one kind of object per FMOD class, with the path it was looked up by
enum { K_SYS = 1, K_CORE, K_BANK, K_DESC, K_INST, K_BUS, K_VCA, K_CG, K_SOUND };
typedef struct obj { uint32_t magic; int kind; char path[200]; uint32_t guid[4]; int state; float volume, pitch;
                     void *user; struct obj *desc, *next; } obj;
#define MAGIC 0x464d4f44u
static obj *g_all;

static int memcmp_(const uint32_t *a, const uint32_t *b) { for (int i = 0; i < 4; i++) if (a[i] != b[i]) return 1; return 0; }
static uint32_t hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }
static obj *make(int kind, const char *path)
{
    for (obj *o = g_all; o && path && kind != K_INST; o = o->next)
        if (o->kind == kind && !strcmp(o->path, path)) return o;
    obj *o = calloc(1, sizeof *o);
    o->magic = MAGIC; o->kind = kind; o->volume = 1.f; o->pitch = 1.f; o->state = STOPPED;
    if (path) { strncpy(o->path, path, sizeof o->path - 1); }
    uint32_t h = hash(o->path);
    o->guid[0] = h; o->guid[1] = h * 3; o->guid[2] = h * 5; o->guid[3] = (uint32_t)kind;
    o->next = g_all; g_all = o;
    return o;
}
static obj *by_guid(int kind, const uint32_t *g)
{
    for (obj *o = g_all; o; o = o->next)
        if (o->kind == kind && !memcmp_(o->guid, g)) return o;
    return NULL;
}
static int valid(void *p, int kind) { obj *o = p; return o && o->magic == MAGIC && (!kind || o->kind == kind); }
static const char *nm(void *p) { obj *o = p; return valid(p, 0) ? (o->path[0] ? o->path : "-") : "(bad handle)"; }
static void fl(char *b, size_t n, float f)        // floats printed the same in both flavors
{
    int neg = f < 0; if (neg) f = -f;
    long ip = (long)f; long fp = (long)((f - (float)ip) * 1000000.f + .5f);
    if (fp >= 1000000) { ip++; fp -= 1000000; }
    snprintf(b, n, "%s%ld.%06ld", neg ? "-" : "", ip, fp);
}
#define FL(var, f) char var[32]; fl(var, sizeof var, f)

#define HV(p, k) do { if (!valid(p, k)) { say("%s: invalid handle", __func__); return ERR_INVALID_HANDLE; } } while (0)

// ---- the Java side FMOD's Android library uses (org.fmod.FMOD), checked once
#ifdef DOUBLE_ANDROID
static JavaVM *g_vm;
EXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    g_vm = vm;
    say("JNI_OnLoad");
    return JNI_VERSION_1_6;
}
static void java_check(void)
{
    JNIEnv *env = NULL;
    if (!g_vm || (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK || !env) { say("java: no JavaVM (JNI_OnLoad not called)"); return; }
    jclass c = (*env)->FindClass(env, "org/fmod/FMOD");
    if (!c) { (*env)->ExceptionClear(env); say("java: no org.fmod.FMOD"); return; }
#define SZ(n) (*env)->CallStaticBooleanMethod(env, c, (*env)->GetStaticMethodID(env, c, n, "()Z"))
#define SI(n) (*env)->CallStaticIntMethod(env, c, (*env)->GetStaticMethodID(env, c, n, "()I"))
    int init = SZ("checkInit"), low = SZ("supportsLowLatency"), aa = SZ("supportsAAudio"), bt = SZ("isBluetoothOn");
    int rate = SI("getOutputSampleRate"), block = SI("getOutputBlockSize");
    jobject am = (*env)->CallStaticObjectMethod(env, c, (*env)->GetStaticMethodID(env, c, "getAssetManager", "()Landroid/content/res/AssetManager;"));
    jobject devs = (*env)->CallStaticObjectMethod(env, c, (*env)->GetStaticMethodID(env, c, "getAudioDevices", "(I)[Landroid/media/AudioDeviceInfo;"), 2);
    say("java: checkInit %d, supportsLowLatency %d, supportsAAudio %d, isBluetoothOn %d, rate %d, block %d, assets %s, %d output devices",
        init, low, aa, bt, rate, block, am ? "yes" : "null", devs ? (int)(*env)->GetArrayLength(env, devs) : -1);
#undef SZ
#undef SI
}
#else
static void java_check(void) { }
#endif

// ---- core (FMOD5_*: the integration's entry names)
EXPORT int FMOD5_Memory_GetStats(int *cur, int *max, int blocking) { if (cur) *cur = 1 << 20; if (max) *max = 2 << 20; say("Memory_GetStats(%d)", blocking); return OK; }
EXPORT int FMOD5_Debug_Initialize(unsigned flags, int mode, void *cb, const char *file) { (void)cb; say("Debug_Initialize(0x%x, %d, %s)", flags, mode, file ? file : "null"); return OK; }
EXPORT int FMOD5_Thread_SetAttributes(int type, long long affinity, int prio, unsigned stack) { say("Thread_SetAttributes(%d, 0x%llx, %d, %u)", type, affinity, prio, stack); return OK; }
EXPORT int FMOD5_System_Create(void **sys, unsigned version) { *sys = make(K_CORE, "core"); say("System_Create(0x%08x)", version); return OK; }
EXPORT int FMOD5_System_Release(void *s) { HV(s, K_CORE); say("System_Release"); return OK; }
EXPORT int FMOD5_System_SetOutput(void *s, int out) { HV(s, K_CORE); say("System_SetOutput(%d)", out); return OK; }
EXPORT int FMOD5_System_GetOutput(void *s, int *out) { HV(s, K_CORE); *out = 0; return OK; }
EXPORT int FMOD5_System_GetVersion(void *s, unsigned *v, unsigned *build) { HV(s, K_CORE); *v = 0x00020223; (void)build; say("System_GetVersion"); return OK; }
EXPORT int FMOD5_System_SetSoftwareChannels(void *s, int n) { HV(s, K_CORE); say("System_SetSoftwareChannels(%d)", n); return OK; }
EXPORT int FMOD5_System_SetSoftwareFormat(void *s, int rate, int mode, int raw) { HV(s, K_CORE); say("System_SetSoftwareFormat(%d, %d, %d)", rate, mode, raw); return OK; }
EXPORT int FMOD5_System_GetSoftwareFormat(void *s, int *rate, int *mode, int *raw) { HV(s, K_CORE); if (rate) *rate = 48000; if (mode) *mode = 3; if (raw) *raw = 0; return OK; }
EXPORT int FMOD5_System_SetDSPBufferSize(void *s, unsigned len, int n) { HV(s, K_CORE); say("System_SetDSPBufferSize(%u, %d)", len, n); return OK; }
EXPORT int FMOD5_System_GetDSPBufferSize(void *s, unsigned *len, int *n) { HV(s, K_CORE); if (len) *len = 1024; if (n) *n = 4; return OK; }
EXPORT int FMOD5_System_SetAdvancedSettings(void *s, void *a) { HV(s, K_CORE); say("System_SetAdvancedSettings(cbSize %d)", a ? *(int *)a : -1); return OK; }
EXPORT int FMOD5_System_GetAdvancedSettings(void *s, void *a) { HV(s, K_CORE); (void)a; return OK; }
EXPORT int FMOD5_System_SetCallback(void *s, void *cb, unsigned mask) { HV(s, K_CORE); (void)cb; say("System_SetCallback(0x%x)", mask); return OK; }
EXPORT int FMOD5_System_LoadPlugin(void *s, const char *file, unsigned *h, unsigned prio) { HV(s, K_CORE); (void)prio; if (h) *h = 1; say("System_LoadPlugin(%s)", file); return OK; }
EXPORT int FMOD5_System_GetChannelsPlaying(void *s, int *ch, int *real) { HV(s, K_CORE); if (ch) *ch = 0; if (real) *real = 0; return OK; }
EXPORT int FMOD5_System_GetCPUUsage(void *s, void *u) { HV(s, K_CORE); if (u) memset(u, 0, 5 * sizeof(float)); return OK; }
EXPORT int FMOD5_System_GetNumDrivers(void *s, int *n) { HV(s, K_CORE); *n = 1; return OK; }
EXPORT int FMOD5_System_MixerSuspend(void *s) { HV(s, K_CORE); say("System_MixerSuspend"); return OK; }
EXPORT int FMOD5_System_MixerResume(void *s) { HV(s, K_CORE); say("System_MixerResume"); return OK; }
EXPORT int FMOD5_System_Update(void *s) { HV(s, K_CORE); return OK; }
EXPORT int FMOD5_System_GetMasterChannelGroup(void *s, void **cg) { HV(s, K_CORE); *cg = make(K_CG, "master"); return OK; }
EXPORT int FMOD5_System_Set3DSettings(void *s, float doppler, float dist, float roll)
{ HV(s, K_CORE); FL(a, doppler); FL(b, dist); FL(c, roll); say("System_Set3DSettings(%s, %s, %s)", a, b, c); return OK; }
EXPORT int FMOD5_System_Get3DSettings(void *s, float *d, float *f, float *r) { HV(s, K_CORE); if (d) *d = 1; if (f) *f = 1; if (r) *r = 1; return OK; }
EXPORT int FMOD5_System_Set3DNumListeners(void *s, int n) { HV(s, K_CORE); say("System_Set3DNumListeners(%d)", n); return OK; }
static int create_sound(const char *what, void *s, const char *name, unsigned mode, void *ex, void **snd)
{
    HV(s, K_CORE); (void)ex;
    int fd = (mode & 0x10000808u) ? -2 : open(name, 0);
    say("System_%s(%s, 0x%x): %s", what, (mode & 0x10000808u) ? "(memory)" : name, mode, fd == -2 ? "memory" : fd >= 0 ? "file found" : "no such file");
    if (fd >= 0) close(fd);
    if (fd == -1) return ERR_FILE_NOTFOUND;
    *snd = make(K_SOUND, (mode & 0x10000808u) ? "memory" : name);
    return OK;
}
EXPORT int FMOD5_System_CreateSound(void *s, const char *n, unsigned m, void *e, void **o) { return create_sound("CreateSound", s, n, m, e, o); }
EXPORT int FMOD5_System_CreateStream(void *s, const char *n, unsigned m, void *e, void **o) { return create_sound("CreateStream", s, n, m, e, o); }
EXPORT int FMOD5_Sound_Release(void *s) { HV(s, K_SOUND); return OK; }
EXPORT int FMOD5_ChannelGroup_SetVolume(void *c, float v) { HV(c, K_CG); FL(a, v); say("ChannelGroup_SetVolume(%s, %s)", nm(c), a); ((obj *)c)->volume = v; return OK; }
EXPORT int FMOD5_ChannelGroup_GetVolume(void *c, float *v) { HV(c, K_CG); *v = ((obj *)c)->volume; return OK; }
EXPORT int FMOD5_ChannelGroup_SetPitch(void *c, float v) { HV(c, K_CG); FL(a, v); say("ChannelGroup_SetPitch(%s, %s)", nm(c), a); return OK; }
EXPORT int FMOD5_ChannelGroup_SetPaused(void *c, int p) { HV(c, K_CG); say("ChannelGroup_SetPaused(%s, %d)", nm(c), p); return OK; }
EXPORT int FMOD5_ChannelGroup_GetPaused(void *c, int *p) { HV(c, K_CG); *p = 0; return OK; }
EXPORT int FMOD5_ChannelGroup_SetMute(void *c, int p) { HV(c, K_CG); say("ChannelGroup_SetMute(%s, %d)", nm(c), p); return OK; }
EXPORT int FMOD5_ChannelGroup_GetMute(void *c, int *p) { HV(c, K_CG); *p = 0; return OK; }
EXPORT int FMOD5_ChannelGroup_Stop(void *c) { HV(c, K_CG); return OK; }
EXPORT int FMOD5_ChannelGroup_Set3DMinMaxDistance(void *c, float lo, float hi)
{ HV(c, K_CG); FL(a, lo); FL(b, hi); say("ChannelGroup_Set3DMinMaxDistance(%s, %s, %s)", nm(c), a, b); return OK; }

// ---- Studio: system
EXPORT int FMOD_Studio_ParseID(const char *id, uint32_t *guid) { if (guid) memset(guid, 0, 16); say("ParseID(\"%s\")", id ? id : "null"); return id && *id ? OK : ERR_INVALID_PARAM; }
EXPORT int FMOD_Studio_System_Create(void **sys, unsigned version) { *sys = make(K_SYS, "studio"); say("Studio_System_Create(0x%08x)", version); return OK; }
EXPORT int FMOD_Studio_System_IsValid(void *s) { return valid(s, K_SYS); }
EXPORT int FMOD_Studio_System_SetAdvancedSettings(void *s, void *a) { HV(s, K_SYS); say("Studio_System_SetAdvancedSettings(cbSize %d)", a ? *(int *)a : -1); return OK; }
EXPORT int FMOD_Studio_System_GetAdvancedSettings(void *s, void *a) { HV(s, K_SYS); (void)a; return OK; }
EXPORT int FMOD_Studio_System_Initialize(void *s, int maxch, unsigned sflags, unsigned flags, void *extra)
{
    HV(s, K_SYS); (void)extra;
    say("Studio_System_Initialize(%d, 0x%x, 0x%x)", maxch, sflags, flags);
    java_check();
    return OK;
}
EXPORT int FMOD_Studio_System_Release(void *s) { HV(s, K_SYS); say("Studio_System_Release"); return OK; }
EXPORT int FMOD_Studio_System_Update(void *s)
{
    HV(s, K_SYS);
    static int n;
    if (++n == 1 || n == 100 || n == 1000) say("Studio_System_Update (%d calls)", n);
    return OK;
}
EXPORT int FMOD_Studio_System_FlushCommands(void *s) { HV(s, K_SYS); return OK; }
EXPORT int FMOD_Studio_System_FlushSampleLoading(void *s) { HV(s, K_SYS); say("Studio_System_FlushSampleLoading"); return OK; }
EXPORT int FMOD_Studio_System_GetCoreSystem(void *s, void **core) { HV(s, K_SYS); *core = make(K_CORE, "core"); return OK; }
static int lookup(void *s, int kind, const char *what, const char *path, void **out)
{
    HV(s, K_SYS);
    if (!path || !*path) { *out = NULL; return ERR_EVENT_NOTFOUND; }
    *out = make(kind, path);
    say("Studio_System_Get%s(%s)", what, path);
    return OK;
}
EXPORT int FMOD_Studio_System_GetEvent(void *s, const char *p, void **o) { return lookup(s, K_DESC, "Event", p, o); }
EXPORT int FMOD_Studio_System_GetBus(void *s, const char *p, void **o) { return lookup(s, K_BUS, "Bus", p, o); }
EXPORT int FMOD_Studio_System_GetVCA(void *s, const char *p, void **o) { return lookup(s, K_VCA, "VCA", p, o); }
EXPORT int FMOD_Studio_System_GetBank(void *s, const char *p, void **o) { return lookup(s, K_BANK, "Bank", p, o); }
static int lookup_id(void *s, int kind, const char *what, const uint32_t *g, void **out)
{
    HV(s, K_SYS);
    obj *o = g ? by_guid(kind, g) : NULL;
    if (!o) {                                   // a GUID the game knows from its assets: an object for it
        char p[64];
        snprintf(p, sizeof p, "{%08x-%08x-%08x-%08x}", g ? g[0] : 0, g ? g[1] : 0, g ? g[2] : 0, g ? g[3] : 0);
        o = make(kind, p);
        if (g) memcpy(o->guid, g, 16);
    }
    *out = o;
    say("Studio_System_Get%sByID(%s)", what, o->path);
    return OK;
}
EXPORT int FMOD_Studio_System_GetEventByID(void *s, const uint32_t *g, void **o) { return lookup_id(s, K_DESC, "Event", g, o); }
EXPORT int FMOD_Studio_System_GetBusByID(void *s, const uint32_t *g, void **o) { return lookup_id(s, K_BUS, "Bus", g, o); }
EXPORT int FMOD_Studio_System_GetVCAByID(void *s, const uint32_t *g, void **o) { return lookup_id(s, K_VCA, "VCA", g, o); }
EXPORT int FMOD_Studio_System_GetBankByID(void *s, const uint32_t *g, void **o) { return lookup_id(s, K_BANK, "Bank", g, o); }
EXPORT int FMOD_Studio_System_LookupID(void *s, const char *path, uint32_t *g)
{
    HV(s, K_SYS);
    obj *o = make(K_DESC, path);
    memcpy(g, o->guid, 16);
    say("Studio_System_LookupID(%s)", path);
    return OK;
}
EXPORT int FMOD_Studio_System_LookupPath(void *s, const uint32_t *g, char *path, int size, int *got)
{
    HV(s, K_SYS);
    obj *o = by_guid(K_DESC, g);
    const char *p = o ? o->path : "";
    int n = (int)strlen(p) + 1;
    if (path && size > 0) { strncpy(path, p, size); path[size - 1] = 0; }
    if (got) *got = n;
    return o ? OK : ERR_EVENT_NOTFOUND;
}
EXPORT int FMOD_Studio_System_GetNumListeners(void *s, int *n) { HV(s, K_SYS); *n = 1; return OK; }
EXPORT int FMOD_Studio_System_SetNumListeners(void *s, int n) { HV(s, K_SYS); say("Studio_System_SetNumListeners(%d)", n); return OK; }
EXPORT int FMOD_Studio_System_GetListenerAttributes(void *s, int l, void *attr, void *pos) { HV(s, K_SYS); (void)l; if (attr) memset(attr, 0, 48); if (pos) memset(pos, 0, 12); return OK; }
EXPORT int FMOD_Studio_System_SetListenerAttributes(void *s, int l, const float *attr, const float *pos)
{
    HV(s, K_SYS);
    static int n;
    if (++n <= 2 && attr) { FL(x, attr[0]); FL(y, attr[1]); FL(z, attr[2]); say("Studio_System_SetListenerAttributes(%d, position %s %s %s%s)", l, x, y, z, pos ? ", attenuation position" : ""); }
    return OK;
}
EXPORT int FMOD_Studio_System_GetListenerWeight(void *s, int l, float *w) { HV(s, K_SYS); (void)l; *w = 1; return OK; }
EXPORT int FMOD_Studio_System_SetListenerWeight(void *s, int l, float w) { HV(s, K_SYS); FL(a, w); say("Studio_System_SetListenerWeight(%d, %s)", l, a); return OK; }
EXPORT int FMOD_Studio_System_LoadBankFile(void *s, const char *file, unsigned flags, void **bank)
{
    HV(s, K_SYS);
    int fd = open(file, 0);
    if (fd < 0) { say("Studio_System_LoadBankFile(%s, 0x%x): no such file -> ERR_FILE_NOTFOUND", file, flags); *bank = NULL; return ERR_FILE_NOTFOUND; }
    char head[4] = { 0 };
    long n = read(fd, head, 4);
    long size = lseek(fd, 0, 2);
    close(fd);
    say("Studio_System_LoadBankFile(%s, 0x%x): %ld bytes, starts \"%.4s\"", file, flags, size, n == 4 ? head : "");
    obj *o = make(K_BANK, file);
    o->state = LOADED;
    *bank = o;
    return OK;
}
EXPORT int FMOD_Studio_System_LoadBankMemory(void *s, const char *buf, int len, int mode, unsigned flags, void **bank)
{
    HV(s, K_SYS);
    say("Studio_System_LoadBankMemory(%d bytes, mode %d, 0x%x): starts \"%.4s\"", len, mode, flags, buf && len >= 4 ? buf : "");
    obj *o = make(K_BANK, "memory");
    o->state = LOADED;
    *bank = o;
    return OK;
}
EXPORT int FMOD_Studio_System_UnloadAll(void *s) { HV(s, K_SYS); say("Studio_System_UnloadAll"); return OK; }
EXPORT int FMOD_Studio_System_GetBankCount(void *s, int *n) { HV(s, K_SYS); int k = 0; for (obj *o = g_all; o; o = o->next) k += o->kind == K_BANK; *n = k; return OK; }
EXPORT int FMOD_Studio_System_GetBankList(void *s, void **arr, int cap, int *count)
{
    HV(s, K_SYS);
    int k = 0;
    for (obj *o = g_all; o; o = o->next) if (o->kind == K_BANK && k < cap) arr[k++] = o;
    if (count) *count = k;
    return OK;
}
EXPORT int FMOD_Studio_System_GetParameterByName(void *s, const char *n, float *v, float *f) { HV(s, K_SYS); (void)n; if (v) *v = 0; if (f) *f = 0; return OK; }
EXPORT int FMOD_Studio_System_SetParameterByName(void *s, const char *n, float v, int ign) { HV(s, K_SYS); FL(a, v); say("Studio_System_SetParameterByName(%s, %s, %d)", n, a, ign); return OK; }
EXPORT int FMOD_Studio_System_GetParameterByID(void *s, uint32_t d1, uint32_t d2, float *v, float *f) { HV(s, K_SYS); (void)d1; (void)d2; if (v) *v = 0; if (f) *f = 0; return OK; }
EXPORT int FMOD_Studio_System_SetParameterByID(void *s, uint32_t d1, uint32_t d2, float v, int ign) { HV(s, K_SYS); FL(a, v); say("Studio_System_SetParameterByID(%08x:%08x, %s, %d)", d1, d2, a, ign); return OK; }
EXPORT int FMOD_Studio_System_GetParameterDescriptionByName(void *s, const char *n, void *d) { HV(s, K_SYS); (void)n; (void)d; return ERR_EVENT_NOTFOUND; }
EXPORT int FMOD_Studio_System_SetCallback(void *s, void *cb, unsigned mask) { HV(s, K_SYS); (void)cb; say("Studio_System_SetCallback(0x%x)", mask); return OK; }
EXPORT int FMOD_Studio_System_SetUserData(void *s, void *u) { HV(s, K_SYS); ((obj *)s)->user = u; return OK; }
EXPORT int FMOD_Studio_System_GetUserData(void *s, void **u) { HV(s, K_SYS); *u = ((obj *)s)->user; return OK; }
EXPORT int FMOD_Studio_System_GetCPUUsage(void *s, void *a, void *b) { HV(s, K_SYS); if (a) memset(a, 0, 4); if (b) memset(b, 0, 20); return OK; }
EXPORT int FMOD_Studio_System_GetBufferUsage(void *s, void *u) { HV(s, K_SYS); if (u) memset(u, 0, 24); return OK; }
EXPORT int FMOD_Studio_System_ResetBufferUsage(void *s) { HV(s, K_SYS); return OK; }
EXPORT int FMOD_Studio_System_GetMemoryUsage(void *s, void *u) { HV(s, K_SYS); if (u) memset(u, 0, 12); return OK; }

// ---- Studio: banks
EXPORT int FMOD_Studio_Bank_IsValid(void *b) { return valid(b, K_BANK); }
EXPORT int FMOD_Studio_Bank_GetID(void *b, uint32_t *g) { HV(b, K_BANK); memcpy(g, ((obj *)b)->guid, 16); return OK; }
static int get_path(void *p, char *out, int size, int *got)
{
    obj *o = p;
    int n = (int)strlen(o->path) + 1;
    if (out && size > 0) { strncpy(out, o->path, size); out[size - 1] = 0; }
    if (got) *got = n;
    return OK;
}
EXPORT int FMOD_Studio_Bank_GetPath(void *b, char *p, int n, int *g) { HV(b, K_BANK); return get_path(b, p, n, g); }
EXPORT int FMOD_Studio_Bank_Unload(void *b) { HV(b, K_BANK); say("Bank_Unload(%s)", nm(b)); ((obj *)b)->state = 1; return OK; }
EXPORT int FMOD_Studio_Bank_LoadSampleData(void *b) { HV(b, K_BANK); say("Bank_LoadSampleData(%s)", nm(b)); return OK; }
EXPORT int FMOD_Studio_Bank_UnloadSampleData(void *b) { HV(b, K_BANK); say("Bank_UnloadSampleData(%s)", nm(b)); return OK; }
EXPORT int FMOD_Studio_Bank_GetLoadingState(void *b, int *st) { HV(b, K_BANK); *st = ((obj *)b)->state; return OK; }
EXPORT int FMOD_Studio_Bank_GetSampleLoadingState(void *b, int *st) { HV(b, K_BANK); *st = LOADED; return OK; }
EXPORT int FMOD_Studio_Bank_GetEventCount(void *b, int *n) { HV(b, K_BANK); *n = 0; return OK; }
EXPORT int FMOD_Studio_Bank_GetEventList(void *b, void **a, int cap, int *n) { HV(b, K_BANK); (void)a; (void)cap; if (n) *n = 0; return OK; }
EXPORT int FMOD_Studio_Bank_GetBusCount(void *b, int *n) { HV(b, K_BANK); *n = 0; return OK; }
EXPORT int FMOD_Studio_Bank_GetBusList(void *b, void **a, int cap, int *n) { HV(b, K_BANK); (void)a; (void)cap; if (n) *n = 0; return OK; }
EXPORT int FMOD_Studio_Bank_GetVCACount(void *b, int *n) { HV(b, K_BANK); *n = 0; return OK; }
EXPORT int FMOD_Studio_Bank_GetVCAList(void *b, void **a, int cap, int *n) { HV(b, K_BANK); (void)a; (void)cap; if (n) *n = 0; return OK; }
EXPORT int FMOD_Studio_Bank_GetStringCount(void *b, int *n) { HV(b, K_BANK); *n = 0; return OK; }
EXPORT int FMOD_Studio_Bank_GetUserData(void *b, void **u) { HV(b, K_BANK); *u = ((obj *)b)->user; return OK; }
EXPORT int FMOD_Studio_Bank_SetUserData(void *b, void *u) { HV(b, K_BANK); ((obj *)b)->user = u; return OK; }

// ---- Studio: event descriptions
EXPORT int FMOD_Studio_EventDescription_IsValid(void *d) { return valid(d, K_DESC); }
EXPORT int FMOD_Studio_EventDescription_GetID(void *d, uint32_t *g) { HV(d, K_DESC); memcpy(g, ((obj *)d)->guid, 16); return OK; }
EXPORT int FMOD_Studio_EventDescription_GetPath(void *d, char *p, int n, int *g) { HV(d, K_DESC); return get_path(d, p, n, g); }
EXPORT int FMOD_Studio_EventDescription_GetParameterDescriptionCount(void *d, int *n) { HV(d, K_DESC); *n = 0; return OK; }
EXPORT int FMOD_Studio_EventDescription_GetParameterDescriptionByIndex(void *d, int i, void *o) { HV(d, K_DESC); (void)i; (void)o; return ERR_INVALID_PARAM; }
EXPORT int FMOD_Studio_EventDescription_GetParameterDescriptionByName(void *d, const char *n, void *o) { HV(d, K_DESC); (void)o; say("EventDescription_GetParameterDescriptionByName(%s, %s)", nm(d), n); return ERR_EVENT_NOTFOUND; }
EXPORT int FMOD_Studio_EventDescription_GetParameterDescriptionByID(void *d, uint32_t a, uint32_t b, void *o) { HV(d, K_DESC); (void)a; (void)b; (void)o; return ERR_EVENT_NOTFOUND; }
EXPORT int FMOD_Studio_EventDescription_GetUserPropertyCount(void *d, int *n) { HV(d, K_DESC); *n = 0; return OK; }
EXPORT int FMOD_Studio_EventDescription_GetLength(void *d, int *n) { HV(d, K_DESC); *n = 1000; return OK; }
EXPORT int FMOD_Studio_EventDescription_GetMinMaxDistance(void *d, float *lo, float *hi) { HV(d, K_DESC); if (lo) *lo = 1; if (hi) *hi = 20; return OK; }
EXPORT int FMOD_Studio_EventDescription_GetSoundSize(void *d, float *s) { HV(d, K_DESC); *s = 0; return OK; }
#define DFLAG(n, v) EXPORT int FMOD_Studio_EventDescription_##n(void *d, int *b) { HV(d, K_DESC); *b = v; return OK; }
DFLAG(IsSnapshot, 0) DFLAG(IsOneshot, 1) DFLAG(IsStream, 0) DFLAG(Is3D, 0) DFLAG(IsDopplerEnabled, 0) DFLAG(HasSustainPoint, 0)
DFLAG(HasCue, 0)
EXPORT int FMOD_Studio_EventDescription_CreateInstance(void *d, void **i)
{
    HV(d, K_DESC);
    obj *o = make(K_INST, ((obj *)d)->path);
    o->desc = d;
    *i = o;
    say("EventDescription_CreateInstance(%s)", nm(d));
    return OK;
}
EXPORT int FMOD_Studio_EventDescription_GetInstanceCount(void *d, int *n) { HV(d, K_DESC); *n = 0; return OK; }
EXPORT int FMOD_Studio_EventDescription_GetInstanceList(void *d, void **a, int cap, int *n) { HV(d, K_DESC); (void)a; (void)cap; if (n) *n = 0; return OK; }
EXPORT int FMOD_Studio_EventDescription_LoadSampleData(void *d) { HV(d, K_DESC); say("EventDescription_LoadSampleData(%s)", nm(d)); return OK; }
EXPORT int FMOD_Studio_EventDescription_UnloadSampleData(void *d) { HV(d, K_DESC); return OK; }
EXPORT int FMOD_Studio_EventDescription_GetSampleLoadingState(void *d, int *st) { HV(d, K_DESC); *st = LOADED; return OK; }
EXPORT int FMOD_Studio_EventDescription_ReleaseAllInstances(void *d) { HV(d, K_DESC); return OK; }
EXPORT int FMOD_Studio_EventDescription_SetCallback(void *d, void *cb, unsigned m) { HV(d, K_DESC); (void)cb; (void)m; return OK; }
EXPORT int FMOD_Studio_EventDescription_GetUserData(void *d, void **u) { HV(d, K_DESC); *u = ((obj *)d)->user; return OK; }
EXPORT int FMOD_Studio_EventDescription_SetUserData(void *d, void *u) { HV(d, K_DESC); ((obj *)d)->user = u; return OK; }

// ---- Studio: event instances
#define I_(i) HV(i, K_INST); obj *o = i; (void)o
EXPORT int FMOD_Studio_EventInstance_IsValid(void *i) { return valid(i, K_INST); }
EXPORT int FMOD_Studio_EventInstance_GetDescription(void *i, void **d) { I_(i); *d = o->desc; return OK; }
EXPORT int FMOD_Studio_EventInstance_GetVolume(void *i, float *v, float *f) { I_(i); if (v) *v = o->volume; if (f) *f = o->volume; return OK; }
EXPORT int FMOD_Studio_EventInstance_SetVolume(void *i, float v) { I_(i); FL(a, v); say("EventInstance_SetVolume(%s, %s)", nm(i), a); o->volume = v; return OK; }
EXPORT int FMOD_Studio_EventInstance_GetPitch(void *i, float *v, float *f) { I_(i); if (v) *v = o->pitch; if (f) *f = o->pitch; return OK; }
EXPORT int FMOD_Studio_EventInstance_SetPitch(void *i, float v) { I_(i); FL(a, v); say("EventInstance_SetPitch(%s, %s)", nm(i), a); o->pitch = v; return OK; }
EXPORT int FMOD_Studio_EventInstance_Get3DAttributes(void *i, void *a) { I_(i); if (a) memset(a, 0, 48); return OK; }
EXPORT int FMOD_Studio_EventInstance_Set3DAttributes(void *i, const float *a)
{
    I_(i);
    static int n;
    if (++n <= 3 && a) { FL(x, a[0]); FL(y, a[1]); FL(z, a[2]); say("EventInstance_Set3DAttributes(%s, position %s %s %s)", nm(i), x, y, z); }
    return OK;
}
EXPORT int FMOD_Studio_EventInstance_GetListenerMask(void *i, unsigned *m) { I_(i); *m = 1; return OK; }
EXPORT int FMOD_Studio_EventInstance_SetListenerMask(void *i, unsigned m) { I_(i); (void)m; return OK; }
EXPORT int FMOD_Studio_EventInstance_GetProperty(void *i, int p, float *v) { I_(i); (void)p; *v = -1; return OK; }
EXPORT int FMOD_Studio_EventInstance_SetProperty(void *i, int p, float v) { I_(i); FL(a, v); say("EventInstance_SetProperty(%s, %d, %s)", nm(i), p, a); return OK; }
EXPORT int FMOD_Studio_EventInstance_GetReverbLevel(void *i, int idx, float *v) { I_(i); (void)idx; *v = 0; return OK; }
EXPORT int FMOD_Studio_EventInstance_SetReverbLevel(void *i, int idx, float v) { I_(i); FL(a, v); say("EventInstance_SetReverbLevel(%s, %d, %s)", nm(i), idx, a); return OK; }
EXPORT int FMOD_Studio_EventInstance_GetPaused(void *i, int *p) { I_(i); *p = 0; return OK; }
EXPORT int FMOD_Studio_EventInstance_SetPaused(void *i, int p) { I_(i); say("EventInstance_SetPaused(%s, %d)", nm(i), p); return OK; }
EXPORT int FMOD_Studio_EventInstance_Start(void *i) { I_(i); o->state = PLAYING; say("EventInstance_Start(%s)", nm(i)); return OK; }
EXPORT int FMOD_Studio_EventInstance_Stop(void *i, int mode) { I_(i); o->state = STOPPED; say("EventInstance_Stop(%s, %d)", nm(i), mode); return OK; }
EXPORT int FMOD_Studio_EventInstance_GetTimelinePosition(void *i, int *p) { I_(i); *p = 0; return OK; }
EXPORT int FMOD_Studio_EventInstance_SetTimelinePosition(void *i, int p) { I_(i); say("EventInstance_SetTimelinePosition(%s, %d)", nm(i), p); return OK; }
EXPORT int FMOD_Studio_EventInstance_GetPlaybackState(void *i, int *s) { I_(i); *s = o->state; return OK; }
EXPORT int FMOD_Studio_EventInstance_GetChannelGroup(void *i, void **c) { I_(i); *c = make(K_CG, o->path); return OK; }
EXPORT int FMOD_Studio_EventInstance_GetMinMaxDistance(void *i, float *lo, float *hi) { I_(i); if (lo) *lo = 1; if (hi) *hi = 20; return OK; }
EXPORT int FMOD_Studio_EventInstance_Release(void *i) { I_(i); say("EventInstance_Release(%s)", nm(i)); return OK; }
EXPORT int FMOD_Studio_EventInstance_IsVirtual(void *i, int *v) { I_(i); *v = 0; return OK; }
EXPORT int FMOD_Studio_EventInstance_GetParameterByName(void *i, const char *n, float *v, float *f) { I_(i); (void)n; if (v) *v = 0; if (f) *f = 0; return OK; }
EXPORT int FMOD_Studio_EventInstance_SetParameterByName(void *i, const char *n, float v, int ign) { I_(i); FL(a, v); say("EventInstance_SetParameterByName(%s, %s, %s, %d)", nm(i), n, a, ign); return OK; }
EXPORT int FMOD_Studio_EventInstance_SetParameterByNameWithLabel(void *i, const char *n, const char *l, int ign) { I_(i); say("EventInstance_SetParameterByNameWithLabel(%s, %s, %s, %d)", nm(i), n, l, ign); return OK; }
EXPORT int FMOD_Studio_EventInstance_GetParameterByID(void *i, uint32_t a, uint32_t b, float *v, float *f) { I_(i); (void)a; (void)b; if (v) *v = 0; if (f) *f = 0; return OK; }
EXPORT int FMOD_Studio_EventInstance_SetParameterByID(void *i, uint32_t a, uint32_t b, float v, int ign) { I_(i); FL(x, v); say("EventInstance_SetParameterByID(%s, %08x:%08x, %s, %d)", nm(i), a, b, x, ign); return OK; }
EXPORT int FMOD_Studio_EventInstance_SetParameterByIDWithLabel(void *i, uint32_t a, uint32_t b, const char *l, int ign) { I_(i); say("EventInstance_SetParameterByIDWithLabel(%s, %08x:%08x, %s, %d)", nm(i), a, b, l, ign); return OK; }
EXPORT int FMOD_Studio_EventInstance_SetParametersByIDs(void *i, const uint32_t *ids, const float *v, int n, int ign) { I_(i); (void)ids; (void)v; say("EventInstance_SetParametersByIDs(%s, %d, %d)", nm(i), n, ign); return OK; }
EXPORT int FMOD_Studio_EventInstance_KeyOff(void *i) { I_(i); say("EventInstance_KeyOff(%s)", nm(i)); return OK; }
EXPORT int FMOD_Studio_EventInstance_SetCallback(void *i, void *cb, unsigned m) { I_(i); (void)cb; say("EventInstance_SetCallback(%s, 0x%x)", nm(i), m); return OK; }
EXPORT int FMOD_Studio_EventInstance_GetUserData(void *i, void **u) { I_(i); *u = o->user; return OK; }
EXPORT int FMOD_Studio_EventInstance_SetUserData(void *i, void *u) { I_(i); o->user = u; return OK; }
EXPORT int FMOD_Studio_EventInstance_GetCPUUsage(void *i, unsigned *a, unsigned *b) { I_(i); if (a) *a = 0; if (b) *b = 0; return OK; }
EXPORT int FMOD_Studio_EventInstance_GetMemoryUsage(void *i, void *u) { I_(i); if (u) memset(u, 0, 12); return OK; }

// ---- Studio: buses and VCAs
#define BV(K, P, KIND)                                                                                       \
    EXPORT int FMOD_Studio_##K##_IsValid(void *b) { return valid(b, KIND); }                                 \
    EXPORT int FMOD_Studio_##K##_GetID(void *b, uint32_t *g) { HV(b, KIND); memcpy(g, ((obj *)b)->guid, 16); return OK; } \
    EXPORT int FMOD_Studio_##K##_GetPath(void *b, char *p, int n, int *g) { HV(b, KIND); return get_path(b, p, n, g); } \
    EXPORT int FMOD_Studio_##K##_GetVolume(void *b, float *v, float *f) { HV(b, KIND); if (v) *v = ((obj *)b)->volume; if (f) *f = ((obj *)b)->volume; return OK; } \
    EXPORT int FMOD_Studio_##K##_SetVolume(void *b, float v) { HV(b, KIND); FL(a, v); say(P "_SetVolume(%s, %s)", nm(b), a); ((obj *)b)->volume = v; return OK; }
BV(Bus, "Bus", K_BUS)
BV(VCA, "VCA", K_VCA)
EXPORT int FMOD_Studio_Bus_GetPaused(void *b, int *p) { HV(b, K_BUS); *p = 0; return OK; }
EXPORT int FMOD_Studio_Bus_SetPaused(void *b, int p) { HV(b, K_BUS); say("Bus_SetPaused(%s, %d)", nm(b), p); return OK; }
EXPORT int FMOD_Studio_Bus_GetMute(void *b, int *p) { HV(b, K_BUS); *p = 0; return OK; }
EXPORT int FMOD_Studio_Bus_SetMute(void *b, int p) { HV(b, K_BUS); say("Bus_SetMute(%s, %d)", nm(b), p); return OK; }
EXPORT int FMOD_Studio_Bus_StopAllEvents(void *b, int m) { HV(b, K_BUS); say("Bus_StopAllEvents(%s, %d)", nm(b), m); return OK; }
EXPORT int FMOD_Studio_Bus_LockChannelGroup(void *b) { HV(b, K_BUS); return OK; }
EXPORT int FMOD_Studio_Bus_UnlockChannelGroup(void *b) { HV(b, K_BUS); return OK; }
EXPORT int FMOD_Studio_Bus_GetChannelGroup(void *b, void **c) { HV(b, K_BUS); *c = make(K_CG, ((obj *)b)->path); return OK; }
EXPORT int FMOD_Studio_Bus_GetCPUUsage(void *b, unsigned *a, unsigned *c) { HV(b, K_BUS); if (a) *a = 0; if (c) *c = 0; return OK; }
EXPORT int FMOD_Studio_Bus_GetMemoryUsage(void *b, void *u) { HV(b, K_BUS); if (u) memset(u, 0, 12); return OK; }

// ---- for the unit test: an object's volume as stored (its bits, so that no float crosses the ABI)
EXPORT uint32_t fmod_double_volume_bits(void *p) { uint32_t b = 0; if (valid(p, 0)) memcpy(&b, &((obj *)p)->volume, 4); return b; }
