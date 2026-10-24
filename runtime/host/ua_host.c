#define _GNU_SOURCE
// ua_host: runs a Unity game with Unity's own Android player (libmain.so, libunity.so,
// libmono.so or libil2cpp.so) natively on an ARM Linux handheld, no Android system and no
// emulator: the armeabi-v7a player in a 32-bit (armhf) build, the arm64-v8a one in an aarch64 build.
//   ../loader   maps the Android libraries; bionic -> glibc through libhybris's hooks
//   ../android  liblog, libandroid (window, looper, sensors), OpenSL ES (SDL2), EGL glue (Mali fbdev)
//   display.c   the EGL window Unity draws into: Mali fbdev, or an SDL2 window (UA_DISPLAY)
//   ../jni      the Java side: JavaVM/JNIEnv and Unity's UnityPlayer activity, in C
// Usage: ua_host <root>   with <root>/lib/{libmain,libunity,libmono}.so, <root>/<name>.apk (the
// game's data, assets/bin/Data), <root>/main.<ver>.<pkg>.obb (or in UA_OBB_DIR), <root>/data/ (saves) and
// <root>/assets/bin/Data/Managed/etc/ (Mono's config, read from the working directory).
#include "../loader/so_loader.h"
#include "../android/android_shims.h"
#include "../android/egl_glue.h"
#include "../jni/unityplayer.h"
void up_media_init(void);                  /* ../jni/up_media.c */
#include "input.h"
#include "frames.h"
#include "framerate.h"
#include "gcbudget.h"
#include "glhooks.h"
#include "hitches.h"
#include "prof.h"
#include "threads.h"
#include "futex.h"
#include "gltrace.h"
#include "glopt.h"
#include "glpass.h"
#include "glrename.h"
#include "glrec.h"
#include "msaafix.h"
#include "arch.h"
#include "display.h"
#include "fbdev.h"
#include "steamstub.h"
#include "plugins.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#if defined(__arm__)
#include <sys/personality.h>
#endif

static char libdir[512];

// android_package.json beside the APK (pmunity android-data --pack, pmunity apk): "package",
// "build_id", "apk", and from an imported Android build "version_code", "version_name", "min_sdk",
// "target_sdk" and the manifest's "meta_data" ({"key": "value", ...}, ApplicationInfo.metaData), and
// "unity_version"
static char pj_package[256], pj_build_id[128], pj_apk[256], pj_version_name[128], pj_source[32], pj_unity[32];
static int pj_version_code, pj_min_sdk, pj_target_sdk;
static char pj_meta[4096];                      // "key=value;key=value"

static void json_str(const char *js, const char *key, char *out, size_t n)
{
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(js, k);
    if (!p || !(p = strchr(p + strlen(k), ':')) || !(p = strchr(p, '"'))) return;
    const char *e = strchr(++p, '"');
    if (e && (size_t)(e - p) < n) { memcpy(out, p, e - p); out[e - p] = 0; }
}

static int json_int(const char *js, const char *key)
{
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(js, k);
    if (!p || !(p = strchr(p + strlen(k), ':'))) return 0;
    while (*++p == ' ') ;
    return atoi(p);
}

// a JSON string at p (p at the opening quote) into out, with \" \\ \/ unescaped; returns the
// character after the closing quote, or NULL
static const char *json_strval(const char *p, char *out, size_t n)
{
    size_t o = 0;
    for (p++; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\' && p[1]) { c = *++p; if (c == 'n') c = '\n'; else if (c == 't') c = '\t'; }
        if (o + 1 < n) out[o++] = c;
    }
    out[o] = 0;
    return *p == '"' ? p + 1 : NULL;
}

// "meta_data": {"k": "v", "k2": null, ...} -> "k=v;..." (values that are not strings are skipped)
static void json_meta(const char *js, char *out, size_t n)
{
    const char *p = strstr(js, "\"meta_data\"");
    if (!p || !(p = strchr(p, '{'))) return;
    size_t o = 0;
    out[0] = 0;
    for (p++; *p && *p != '}';) {
        char k[256], v[1024];
        while (*p && *p != '"' && *p != '}') p++;
        if (*p != '"' || !(p = json_strval(p, k, sizeof k))) break;
        while (*p == ' ' || *p == ':') p++;
        if (*p == '"') {
            if (!(p = json_strval(p, v, sizeof v))) break;
            if (!strchr(k, ';') && !strchr(v, ';') && o + strlen(k) + strlen(v) + 3 < n)
                o += snprintf(out + o, n - o, "%s%s=%s", o ? ";" : "", k, v);
        } else {
            while (*p && *p != ',' && *p != '}') p++;
        }
    }
}

static void read_package_json(const char *root)
{
    char path[600], js[16384];
    snprintf(path, sizeof path, "%s/android_package.json", root);
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(js, 1, sizeof js - 1, f);
    fclose(f);
    js[n] = 0;
    json_str(js, "package", pj_package, sizeof pj_package);
    json_str(js, "build_id", pj_build_id, sizeof pj_build_id);
    json_str(js, "apk", pj_apk, sizeof pj_apk);
    if (strchr(pj_apk, '/')) pj_apk[0] = 0;          // an older file with a path in it: not used
    json_str(js, "version_name", pj_version_name, sizeof pj_version_name);
    pj_version_code = json_int(js, "version_code");
    pj_min_sdk = json_int(js, "min_sdk");
    pj_target_sdk = json_int(js, "target_sdk");
    json_meta(js, pj_meta, sizeof pj_meta);
    json_str(js, "source", pj_source, sizeof pj_source);
    json_str(js, "unity_version", pj_unity, sizeof pj_unity);
}

// Build.VERSION.SDK_INT. Converted desktop data: 4.4 (API 19) as the 32-bit ports were made with
// (Cuphead's 2017.4 player among them), 5.1 (API 22) for 64-bit players (64-bit Android starts at
// API 21), or the data's min_sdk where android-data wrote one (22 for a game with compute kernels:
// Unity's Android players create OpenGL ES 3.1+ contexts, and so run compute shaders and GLES 3.1
// programs, only from Android 5.0, API 21); the loader's ro.build.version.sdk says the same. An
// imported Android build (pmunity apk): its targetSdkVersion, the Android its makers tested it on,
// but at most SDK_CAP and at least its minSdkVersion (a phone it installs on has that). UA_SDK_INT
// overrides.
#if defined(__aarch64__)
#define BASE_SDK 22
#else
#define BASE_SDK 19
#endif
#define SDK_CAP 33
static int reported_sdk(void)
{
    if (getenv("UA_SDK_INT")) return atoi(getenv("UA_SDK_INT"));
    if (strcmp(pj_source, "apk")) return pj_min_sdk > BASE_SDK && pj_min_sdk <= SDK_CAP ? pj_min_sdk : BASE_SDK;
    int t = pj_target_sdk > 0 ? pj_target_sdk : pj_min_sdk;
    if (t > SDK_CAP) t = SDK_CAP;
    if (t < pj_min_sdk) t = pj_min_sdk;
    return t > BASE_SDK ? t : BASE_SDK;
}

// Unity 2017.1 - 2017.3 players with a quality level that uses MSAA draw every frame into the
// window and present a framebuffer of their own that nothing drew into (msaafix.c). The fix is on
// for them (android_package.json's unity_version), off for every other player and for packages that
// do not name their version; UA_MSAA_FIX=0|1 overrides.
static int msaa_fix(void)
{
    const char *e = getenv("UA_MSAA_FIX");
    if (e && *e) return atoi(e) != 0;
    int major = 0, minor = 0;
    return sscanf(pj_unity, "%d.%d", &major, &minor) == 2 && major == 2017 && minor <= 3;
}

static int only_apk(const char *root, char *out, size_t n)
{
    DIR *d = opendir(root);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".apk")) { if (!found++) snprintf(out, n, "%s/%s", root, e->d_name); }
    }
    closedir(d);
    return found == 1;
}

static void mkdirs(const char *path)
{
    char p[512];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

// System.loadLibrary(name) / UnityPlayer.loadLibrary(name): lib<name>.so from the library folder,
// as Android maps a bare name (the Java side refuses names with a '/'), so libunity's probes for
// "libmono.so" or "libc.so.6" fail as on a phone and Unity dlopens those itself; System.load(path):
// that file (IL2CPP players load libil2cpp.so so). A library that was not loaded yet gets its
// JNI_OnLoad called, as Android's VM does (dlsym on the handle: the library, then its dependencies);
// one already loaded natively (libmain, libunity, a dependency) does not get it a second time.
// Returns 1 when loaded, else 0.
static int load_library(void *user, const char *name)
{
    (void)user;
    char path[PATH_MAX];
    if (name[0] == '/') snprintf(path, sizeof path, "%s", name);
    else snprintf(path, sizeof path, "%s/lib%s.so", libdir, name);
    int had = so_find(path) != NULL;
    void *h = so_dlopen(path, 0);
    fprintf(stderr, "[host] loadLibrary(%s): %s\n", name, h ? had ? "ok (already loaded)" : "ok" : so_dlerror());
    if (!h) return 0;
    jint (*onload)(JavaVM *, void *) = had ? NULL : (jint (*)(JavaVM *, void *))so_dlsym(h, "JNI_OnLoad");
    if (onload) fprintf(stderr, "[host] %s JNI_OnLoad -> 0x%x\n", name, onload(fj_vm(), NULL));
    return 1;
}

// crash report: signal, fault address and the module + offset (+ nearest symbol) of pc, lr and
// of return-address-looking words on the stack (Mono installs its own handler later for managed
// code and calls this one for faults outside it)
#include <dlfcn.h>
int so_dladdr(const void *addr, Dl_info *info);   /* loader: Android modules first, then the host */
static void where(const char *what, uintptr_t a)
{
    Dl_info di;
    if (a && so_dladdr((void *)a, &di) && di.dli_fname)
        fprintf(stderr, "[crash]   %-4s %0*lx  %s+0x%lx  %s\n", what, ADDR_HEX, (unsigned long)a, di.dli_fname,
                (unsigned long)(a - (uintptr_t)di.dli_fbase), di.dli_sname ? di.dli_sname : "");
    else
        fprintf(stderr, "[crash]   %-4s %0*lx\n", what, ADDR_HEX, (unsigned long)a);
}
static void on_crash(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    // after a quit, Unity's nativeDone tears the engine down and its JNI wrapper then touches freed
    // state (an Android app's process is killed right there): the end of shutdown, not a crash
    if (up_is_finishing())
    {
        static const char msg[] = "[host] Unity shut down\n";
        (void)!write(2, msg, sizeof msg - 1);
        fbdev_restore();
        _exit(0);
    }
    fprintf(stderr, "[crash] signal %d (code %d) at address %p, thread %ld\n", sig, si->si_code, si->si_addr, (long)syscall(SYS_gettid));
    where("pc", UC_PC(uc));
    where("lr", UC_LR(uc));
    uintptr_t *sp = (uintptr_t *)UC_SP(uc);
    int shown = 0;
    for (int i = 0; i < 8192 / (int)sizeof sp[0] && shown < 24; i++)     // the 8 KB above sp
    {
        Dl_info di;
        uintptr_t v = sp[i];
        if (code_aligned(v))
            if (so_dladdr((void *)v, &di) && di.dli_fname && strstr(di.dli_fname, ".so") && di.dli_fbase && v - (uintptr_t)di.dli_fbase > 0x1000)
            { char t[16]; snprintf(t, sizeof t, "s%d", i); where(t, v); shown++; }
    }
    fbdev_restore();                         // the framebuffer's geometry, if UA_FB_BUFFERS changed it
    signal(sig, SIG_DFL);
    raise(sig);
}

// UA_MONO_TRACE=1: Mono lookups made by Unity that find nothing (a class, method or field the
// engine expects in the managed assemblies), with the caller's offset in libunity.so
static so_module *g_unity;
static void caller(const char *what, void *ra)
{
    Dl_info di;
    if (so_dladdr(ra, &di) && di.dli_fname)
        fprintf(stderr, "[mono] %s from %s+0x%lx\n", what, strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname,
                (unsigned long)((uintptr_t)ra - (uintptr_t)di.dli_fbase));
    else
        fprintf(stderr, "[mono] %s from %p\n", what, ra);
}
static void *(*r_class_from_name)(void *, const char *, const char *);
static void *(*r_method_from_name)(void *, const char *, int);
static void *(*r_field_from_name)(void *, const char *);
static void *(*r_array_class_get)(void *, unsigned);
static const char *(*r_class_get_name)(void *);
static void *w_class_from_name(void *img, const char *ns, const char *name)
{
    void *c = r_class_from_name(img, ns, name);
    if (!c) { char b[300]; snprintf(b, sizeof b, "class %s%s%s not found", ns ? ns : "", ns && *ns ? "." : "", name); caller(b, __builtin_return_address(0)); }
    return c;
}
static void *w_method_from_name(void *k, const char *name, int n)
{
    void *m = r_method_from_name(k, name, n);
    if (!m) { char b[300]; snprintf(b, sizeof b, "method %s.%s(%d args) not found", k && r_class_get_name ? r_class_get_name(k) : "?", name, n); caller(b, __builtin_return_address(0)); }
    return m;
}
static void *w_field_from_name(void *k, const char *name)
{
    void *f = r_field_from_name(k, name);
    if (!f) { char b[300]; snprintf(b, sizeof b, "field %s.%s not found", k && r_class_get_name ? r_class_get_name(k) : "?", name); caller(b, __builtin_return_address(0)); }
    return f;
}
static void *w_array_class_get(void *k, unsigned rank)
{
    if (!k) caller("mono_array_class_get(NULL element class)", __builtin_return_address(0));
    return r_array_class_get(k, rank);
}
static void *(*r_array_new)(void *, void *, uintptr_t);
static void *w_array_new(void *dom, void *k, uintptr_t n)
{
    if (!k) caller("mono_array_new(NULL element class)", __builtin_return_address(0));
    return r_array_new(dom, k, n);
}
// UA_MONO_OPTIONS="--trace=T:Foo ...": Mono runtime options when Unity looks up the Mono API
// (before it initializes the JIT): --trace=<spec> goes to mono_jit_set_trace_options (e.g.
// N:Namespace,T:Class,M:Class:Method; Unity's mono_jit_parse_options lacks it), the rest to
// mono_jit_parse_options
static void mono_options(void *some_mono_fn)
{
    static int done;
    const char *o = getenv("UA_MONO_OPTIONS");
    if (done || !o || !*o) return;
    done = 1;
    Dl_info di;
    void *h = so_dladdr(some_mono_fn, &di) && di.dli_fname ? so_dlopen(di.dli_fname, 0) : NULL;
    void (*parse)(int, char **) = h ? (void (*)(int, char **))so_dlsym(h, "mono_jit_parse_options") : NULL;
    int (*trace)(const char *) = h ? (int (*)(const char *))so_dlsym(h, "mono_jit_set_trace_options") : NULL;
    static char buf[2048];
    static char *argv[64];
    int argc = 0;
    snprintf(buf, sizeof buf, "%s", o);
    for (char *t = strtok(buf, " "); t && argc < 63; t = strtok(NULL, " "))
    {
        if (!strncmp(t, "--trace=", 8))
            fprintf(stderr, "[host] mono trace %s: %s\n", t + 8, trace && trace(t + 8) ? "on" : "FAILED");
        else argv[argc++] = t;
    }
    argv[argc] = NULL;
    if (argc && parse) { fprintf(stderr, "[host] mono options: %s\n", o); parse(argc, argv); }
}
static int g_mono_trace;
static void *dlsym_hook(const char *sym, void *p)
{
    p = plugins_dlsym_hook(sym, p);            // FMOD's file paths (plugins.c)
    p = framerate_dlsym_hook(sym, p);          // UA_FRAME_RATE (framerate.c)
    p = gcbudget_dlsym_hook(sym, p);           // UA_GC_BUDGET (gcbudget.c)
    if (!strncmp(sym, "mono_", 5)) mono_options(p);
    if (!g_mono_trace) return p;
    if (!strcmp(sym, "mono_array_new")) { r_array_new = p; return (void *)w_array_new; }
    if (!strcmp(sym, "mono_class_get_name")) r_class_get_name = p;
    if (!strcmp(sym, "mono_class_from_name")) { r_class_from_name = p; return (void *)w_class_from_name; }
    if (!strcmp(sym, "mono_class_get_method_from_name")) { r_method_from_name = p; return (void *)w_method_from_name; }
    if (!strcmp(sym, "mono_class_get_field_from_name")) { r_field_from_name = p; return (void *)w_field_from_name; }
    if (!strcmp(sym, "mono_array_class_get")) { r_array_class_get = p; return (void *)w_array_class_get; }
    return p;
}

// what glopt.c's GL wrappers call: msaafix.c's wrapper of that function, else gltrace.c's, else
// glhooks.c's (NULL: the driver's)
static const so_shim *g_msaa_shims, *g_hook_shims;
static void *find_shim(const so_shim *t, const char *name)
{
    for (; t && t->name; t++) if (!strcmp(t->name, name)) return t->fn;
    return NULL;
}
static void *after_glopt(const char *name)
{
    void *p = find_shim(g_msaa_shims, name);
    if (!p) p = gltrace_wrapper(name);
    return p ? p : find_shim(g_hook_shims, name);
}
// what glpass.c's GL wrappers call: glopt.c's wrapper of that function, else after_glopt's
static const so_shim *g_glopt_shims, *g_pass_shims;
static void *after_glpass(const char *name)
{
    void *p = find_shim(g_glopt_shims, name);
    return p ? p : after_glopt(name);
}
// what glrename.c's GL wrappers call: glpass.c's wrapper, else after_glpass's
static void *after_glrename(const char *name)
{
    void *p = find_shim(g_pass_shims, name);
    return p ? p : after_glpass(name);
}
// what glrec.c's wrappers call: glopt.c's wrapper of that name and the layers under it, in their
// order (glopt.c's float-taking ones with the host's calling convention), else NULL (the driver's)
static void *after_glrec(const char *name)
{
    void *h = glopt_hard(name);
    if (h) return h;
    void *p = find_shim(g_glopt_shims, name);
    if (!p) p = after_glopt(name);
    if (p) return p;
    if (!strncmp(name, "egl", 3)) {
        if ((p = find_shim(display_egl_shims(), name))) return p;
        if ((p = find_shim(egl_shims, name))) return p;
    }
    return NULL;
}

// UnityMain, after each frame
static void on_frame(void *user) { framerate_frame(user); gcbudget_frame(user); prof_main_frame(); }

static volatile int quitting;
static void on_quit(void *user) { (void)user; quitting = 1; }
static void on_signal(int s) { (void)s; quitting = 1; }

#if defined(__arm__)
// A 32-bit player on a 64-bit ARM kernel runs with the PER_LINUX32 personality, as Android's 32-bit
// app processes do: /proc/cpuinfo then shows the 32-bit view (neon, vfpv3, ...) and uname "armv8l".
// Without it the kernel prints the AArch64 format, and Unity 2019.4+'s CPU detection takes the
// player for ARM64; its FMOD then crashes registering its audio output. UA_LINUX32=0: leave it.
static void linux32_personality(void)
{
    struct utsname u;
    const char *e = getenv("UA_LINUX32");
    if ((e && !strcmp(e, "0")) || uname(&u) != 0 || strcmp(u.machine, "aarch64") != 0) return;
    int p = personality(0xffffffff);
    if (p != -1 && (p & PER_MASK) != PER_LINUX32 && personality((p & ~PER_MASK) | PER_LINUX32) != -1)
        fprintf(stderr, "[host] 64-bit kernel: PER_LINUX32 personality (32-bit /proc/cpuinfo, as on Android)\n");
}
#endif

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
#if defined(__arm__)
    linux32_personality();
#endif
    // Mono's Boehm collector starts with a 64 MB heap instead of growing from ~15 MB: in a fight it
    // collects about half as often (each collection is a 140-280 ms frame on the handheld) for ~7 MB
    // more resident memory. GC_INITIAL_HEAP_SIZE set by the caller wins (UA_GC_HEAP=0: Boehm's default).
    if (!getenv("GC_INITIAL_HEAP_SIZE")) {
        const char *h = getenv("UA_GC_HEAP");
        if (!h || strcmp(h, "0") != 0) setenv("GC_INITIAL_HEAP_SIZE", h && *h ? h : "67108864", 1);
    }
    // the root is also the working directory: Mono reads its config relative to it
    // (assets/bin/Data/Managed/etc/mono/config, outside the APK)
    static char root[PATH_MAX];                     // (realpath needs PATH_MAX; fortified builds check it)
    if (!realpath(argc > 1 ? argv[1] : ".", root) || chdir(root) != 0)
    { fprintf(stderr, "[host] %s: %s\n", argc > 1 ? argv[1] : ".", strerror(errno)); return 1; }
    // package, build id and APK name: the environment, else <root>/android_package.json (written by
    // the data tool next to the APK), else the only .apk in <root>
    read_package_json(root);
    const char *pkg = getenv("UA_PACKAGE") ? getenv("UA_PACKAGE") : pj_package[0] ? pj_package : "com.pmunity.game";
    // an imported Android build (pmunity apk) keeps its own frame pacing setting: with Optimized Frame
    // Pacing on, Unity 2019.3+ needs Swappy running (disabled, it presents nothing or stops after a few
    // frames), so Swappy's off switch (the loader's swappy.disable property) is cleared for it;
    // converted desktop data has the setting off and never starts Swappy (android/README.md)
    if (!strcmp(pj_source, "apk") && !getenv("SO_PROP_swappy_disable")) setenv("SO_PROP_swappy_disable", "0", 1);
    char apk[600], obb[600], data[600], files[600], cache[600], ext[600];
    if (getenv("UA_LIBDIR")) snprintf(libdir, sizeof libdir, "%s", getenv("UA_LIBDIR"));   // e.g. a Development player
    else snprintf(libdir, sizeof libdir, "%s/lib", root);
    if (getenv("UA_APK_NAME")) snprintf(apk, sizeof apk, "%s/%s.apk", root, getenv("UA_APK_NAME"));
    else if (pj_apk[0]) snprintf(apk, sizeof apk, "%s/%s", root, pj_apk);
    else if (!only_apk(root, apk, sizeof apk)) {
        fprintf(stderr, "[host] no APK: set UA_APK_NAME, or keep android_package.json or a single .apk in %s\n", root);
        return 1;
    }
    // OBBs beside the APK by default (the data tool's zip/ layout): with an OBB mounted,
    // Application.dataPath is the OBB, and games look for files at dataPath + "/.." (e.g. a GOG
    // build's DLC .info files), i.e. next to it
    snprintf(obb, sizeof obb, "%s", getenv("UA_OBB_DIR") ? getenv("UA_OBB_DIR") : root);
    snprintf(data, sizeof data, "%s/data", root);
    snprintf(files, sizeof files, "%s/files", data);
    snprintf(cache, sizeof cache, "%s/cache", data);
    snprintf(ext, sizeof ext, "%s/external", data);
    mkdirs(files); mkdirs(cache); mkdirs(ext);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGFPE, &sa, NULL);

    // Build.VERSION.SDK_INT (and ro.build.version.sdk, the libandroid functions present, AConfiguration)
    const int sdk_int = reported_sdk();
    android_shims_set_api_level(sdk_int);       // before the loader's first lookup

    // symbol resolution: ours win over libhybris's hooks, which win over the host
    so_set_library_path(libdir);
    g_mono_trace = getenv("UA_MONO_TRACE") != NULL;
    so_set_dlsym_hook(dlsym_hook);
    hitches_init();                            // UA_HITCH_TRACE
    prof_init(root);                           // UA_PROF
    threads_init();                            // UA_AFFINITY, UA_NICE
    so_add_shims(futex_table());               // UA_FUTEX_STATS, UA_FUTEX_SPIN: Baselib's futex calls
    // GL wrappers, each calling the next: the player -> glrec.c (UA_GL_REC: it calls glopt.c's layer
    // next, so glrename.c and glpass.c are left out while it is on) -> glrename.c -> glpass.c ->
    // glopt.c -> msaafix.c -> gltrace.c -> glhooks.c -> the driver
    const so_shim *trace = gltrace_table();    // UA_GL_TRACE
    g_hook_shims = glhooks_table();            // UA_SHADER_DUMP/OVERRIDE, UA_SWAP_INTERVAL
    int msaa = msaa_fix();                     // UA_MSAA_FIX: also a single-sampled window
    g_msaa_shims = msaafix_table(msaa, gltrace_wrapper);
    g_glopt_shims = glopt_table(after_glopt);  // UA_GL_DSCLEAR
    g_pass_shims = glpass_table(after_glpass); // UA_GL_SAMECLEAR
    const so_shim *rename = glrename_table(after_glrename);   // UA_GL_RENAME
    const so_shim *rec = glrec_table(after_glrec);            // UA_GL_REC: above all of them
    if (rec[0].name && (rename[0].name || g_pass_shims[0].name))
        fprintf(stderr, "[host] UA_GL_REC is on: UA_GL_RENAME and UA_GL_SAMECLEAR are left out\n");
    so_add_shims(rec);
    so_add_shims(rename);
    so_add_shims(g_pass_shims);
    so_add_shims(g_glopt_shims);
    so_add_shims(g_msaa_shims);
    so_add_shims(trace);
    if (msaa) {
        egl_glue_set_single_sample_window(1);
        if (getenv("UA_DISPLAY_SAMPLES") && atoi(getenv("UA_DISPLAY_SAMPLES")) > 0) {
            fprintf(stderr, "[host] UA_DISPLAY_SAMPLES ignored: Unity %s needs a single-sampled window (UA_MSAA_FIX)\n", pj_unity);
            unsetenv("UA_DISPLAY_SAMPLES");
        }
    }
    so_add_shims(g_hook_shims);
    // the window: Mali fbdev (nothing to open), or an SDL2 window whose EGL objects Unity gets
    if (display_init(pkg) != 0) { fprintf(stderr, "[host] no display (UA_DISPLAY)\n"); return 1; }
    so_add_shims(display_egl_shims());         // NULL for fbdev
    so_add_shims(egl_shims);
    so_add_shims(android_shims);
    so_add_shims(bionic_compat_table());
    so_register_fake_lib("liblog.so", android_log_shims);
    so_register_fake_lib("libandroid.so", android_ndk_shims);
    so_register_fake_lib("libOpenSLES.so", android_sles_shims);
    // libmediandk: 2017.4-2021.3 players dlopen it and decode video through Java when that fails
    // (../jni/up_media.c); 2022.3 links it (the shims in ../android/media_ndk.c)
    so_map_library("libmediandk.so", NULL);
    up_media_init();                            // the media core's decoders (../media)
    so_map_library("libvulkan.so", NULL);       // GLES only
    steamstub_register(libdir);                 // Steamworks without a Steam client (steamstub.c)
    plugins_init(root, libdir);                 // the port's plugins.map: ARM builds, stubs (plugins.c)

    char path[600];
    snprintf(path, sizeof path, "%s/libmain.so", libdir);
    so_module *m = so_load(path, NULL);
    if (!m) { fprintf(stderr, "[host] %s: %s\n", path, so_error()); return 1; }
    so_run_init(m);

    struct up_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.package_name = pkg;
    cfg.apk_path = apk;
    cfg.native_lib_dir = libdir;
    cfg.data_dir = data;
    cfg.files_dir = files;
    cfg.cache_dir = cache;
    cfg.external_files_dir = ext;
    cfg.external_cache_dir = cache;
    cfg.obb_dir = obb;
    cfg.external_storage = data;
    // PackageInfo.versionCode names the OBBs the player looks for (main.<code>.<package>.obb)
    cfg.version_code = pj_version_code > 0 ? pj_version_code : 1;
    cfg.version_name = pj_version_name[0] ? pj_version_name : "1.0";
    cfg.command_line = getenv("UA_UNITY_ARGS");
    // ApplicationInfo.metaData: UA_META="key=value;key=value" (e.g. unity.build-id=..., which ties
    // the APK to its OBB)
    static const char *meta[32];
    static char metabuf[4096];
    int nm = 0;
    if (getenv("UA_META"))
        snprintf(metabuf, sizeof metabuf, "%s", getenv("UA_META"));
    else if (pj_meta[0])                        // an imported build's manifest meta-data
        snprintf(metabuf, sizeof metabuf, "%s", pj_meta);
    else if (pj_build_id[0])
        snprintf(metabuf, sizeof metabuf, "unity.build-id=%s", pj_build_id);
    for (char *t = strtok(metabuf, ";"); t && nm < 31; t = strtok(NULL, ";")) meta[nm++] = t;
    meta[nm] = NULL;
    cfg.meta_data = meta;
    ANativeWindow *win = android_window_get();
    cfg.width = ANativeWindow_getWidth(win);
    cfg.height = ANativeWindow_getHeight(win);
    cfg.density_dpi = 160;
    cfg.refresh_rate = display_refresh_rate() > 0 ? (float)display_refresh_rate() : 60.f;
    android_vsync_set_refresh_rate(cfg.refresh_rate);   // the NDK's and Java's Choreographer: one clock
    // 2021.1+ Android players pace at targetFrameRate only (-1: 30 fps); converted desktop data gets
    // the desktop's vsync pacing back (framerate.c). An imported Android build keeps its own.
    framerate_init(strcmp(pj_source, "apk") && atoi(pj_unity) > 0 ? "desktop" : "android", cfg.refresh_rate);
    gcbudget_init();
    cfg.on_frame = on_frame;
    cfg.sdk_int = sdk_int;
    static const char *const releases[] = { "4.4.4", "4.4W", "5.0", "5.1.1", "6.0.1", "7.0", "7.1.2", "8.0.0", "8.1.0", "9",
                                            "10", "11", "12", "12", "13", "14", "15" };
    cfg.os_release = cfg.sdk_int >= 19 && cfg.sdk_int <= 35 ? releases[cfg.sdk_int - 19] : "4.4.4";
    if (cfg.sdk_int != BASE_SDK && !getenv("SO_PROP_ro_build_version_sdk")) {
        char v[16];
        snprintf(v, sizeof v, "%d", cfg.sdk_int);
        setenv("SO_PROP_ro_build_version_sdk", v, 1);
        setenv("SO_PROP_ro_build_version_release", cfg.os_release, 1);
    }
    cfg.cpu_count = 4;
    cfg.total_mem = 973LL << 20;
    // what AudioManager reports to FMOD as the output's native rate and buffer size
    // (UA_AUDIO_RATE / UA_AUDIO_FRAMES: A/B of the mixer's cost)
    cfg.audio_sample_rate = getenv("UA_AUDIO_RATE") ? atoi(getenv("UA_AUDIO_RATE")) : 48000;
    cfg.audio_frames_per_buffer = getenv("UA_AUDIO_FRAMES") ? atoi(getenv("UA_AUDIO_FRAMES")) : 512;
    cfg.libmain_onload = (up_onload_fn)so_sym(m, "JNI_OnLoad");
    cfg.native_window = win;
    msaafix_set_size(cfg.width, cfg.height);
    cfg.on_load_library = load_library;
    cfg.on_quit = on_quit;
    input_init(cfg.width, cfg.height);
    frames_init(root, cfg.width, cfg.height);
    input_set_root(root);
    fprintf(stderr, "[host] starting Unity: apk %s, window %dx%d (%s), API %d\n", apk, cfg.width, cfg.height,
            display_backend(), cfg.sdk_int);
    if (up_start(&cfg) != 0) { fprintf(stderr, "[host] up_start failed\n"); return 1; }
    input_started();

    // this thread is Android's UI thread
    while (!quitting && !display_quit_requested() && up_running())
    {
        up_poll();
        input_poll();
        usleep(4000);
    }
    // Unity installed its own crash handler after ours: ours first again for the shutdown (see on_crash)
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    up_quit();      // also when the game asked to quit (Application.Quit: nativeRender returned false)
    up_wait();
    fprintf(stderr, "[host] Unity stopped\n");
    // Unity has saved and shut down; the libraries' exit handlers (Mono's thread cleanup, static
    // destructors) have nothing left to do but can block on threads that are gone
    fbdev_restore();
    fflush(NULL);
    _exit(0);
}
