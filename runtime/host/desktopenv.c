// Desktop answers for code written for the desktop player, where Unity's Android player gives
// something a desktop never does (docs/runtime.md, "What a desktop game sees"):
//
//   the command line  The Android player's Mono gets no main arguments: libunity passes none to
//                     mono_unity_runtime_set_main_args (2017.x's Mono) / mono_runtime_set_main_args,
//                     and Mono keeps none for zero, so Environment.GetCommandLineArgs() is null and
//                     Environment.CommandLine throws ArgumentNullException (a desktop game that looks
//                     for its switches stops there). A desktop player passes its argv. The host gives
//                     Mono the same shape: the program's path, then Unity's own switches
//                     (UA_UNITY_ARGS, and what the host adds to them), set as soon as libunity
//                     looks the setter up (Unity's Mono takes them once: libunity's own call, with
//                     none, is then left out).
//   Screen.resolutions  empty on the Android player (Unity lists display modes from Android 6's
//                     Display.Mode, and these players report an older Android); a desktop player
//                     lists the display's modes and always has the current one. When Unity's list is
//                     empty the host answers with one mode: the window's size at the display's
//                     refresh rate (the size Screen.width / height report), as a desktop with one
//                     mode would. Its icall is wrapped as libunity registers it
//                     (mono_add_internal_call, as framerate.c does).
//   the temp folder  Path.GetTempPath(): Mono's is $TMPDIR, else /tmp; IL2CPP's (libil2cpp's
//                     os/Posix/Path.cpp) $TMPDIR / $TMP / $TEMP, else Android's /data/local/tmp, which a
//                     handheld does not have: Enter the Gungeon's saves (written through
//                     Path.GetTempFileName) failed on its IL2CPP build. The host sets TMPDIR to /tmp when
//                     none of the three is set, Mono's own answer, so both runtimes write where Mono does.
// Mono players (the 32-bit route; libunity's mono_* imports). IL2CPP keeps an empty argument list
// rather than none, so its CommandLine is "" and nothing throws. Screen.resolutions also on the
// 64-bit player (IL2CPP, or Unity's Mono behind the bridge of runtime/mono64): the same answer
// through the il2cpp_* functions libunity looks up, its icall wrapped as il2cpp_add_internal_call
// registers it (Enter the Gungeon's options code throws on an empty list there).
#define _GNU_SOURCE
#include "desktopenv.h"

#include <dlfcn.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../loader/so_loader.h"

int so_dladdr(const void *addr, Dl_info *info);   /* loader: Android modules first, then the host */

#define LOG(...) fprintf(stderr, "[host] " __VA_ARGS__)

static char *g_argv[64];
static int g_argc;
static int g_width, g_height, g_hz = 60;

// ---------------------------------------------------------------------------------- command line

typedef void (*set_args_fn)(int argc, const char *argv[]);
static set_args_fn r_set_args;
static int g_args_set;

static void set_args(int argc, const char *const *given)
{
    const char *v[128];
    int n = 0;
    if (g_argc) v[n++] = g_argv[0];
    if (argc > 0 && given)                                  // libunity's own arguments, after the path
        for (int i = 0; i < argc && n < 127; i++) v[n++] = given[i];
    else
        for (int i = 1; i < g_argc && n < 127; i++) v[n++] = g_argv[i];
    v[n] = NULL;
    r_set_args(n, v);
    if (!g_args_set++) LOG("the game's command line: %s and %d argument(s) (Environment.GetCommandLineArgs)\n", v[0] ? v[0] : "?", n - 1);
}

// Unity's 2017.x Mono sets them once only (it asserts that none were set before): the host sets
// them as soon as it has the setter, and libunity's own call (with none, on the Android player) is
// left out; if the host has not set them yet, libunity's arguments follow the program's path
static void w_set_args(int argc, const char *argv[])
{
    if (g_args_set) {
        LOG("libunity sets Mono's main arguments (%d): already set\n", argc);
        return;
    }
    set_args(argc, argv);
}

void desktopenv_init(const char *unity_args, int width, int height, int refresh_hz)
{
    if (!getenv("TMPDIR") && !getenv("TMP") && !getenv("TEMP") && access("/tmp", W_OK) == 0)
        setenv("TMPDIR", "/tmp", 0);
    static char exe[PATH_MAX];
    ssize_t l = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[l > 0 ? l : 0] = 0;
    if (!exe[0]) snprintf(exe, sizeof exe, "ua_host");
    g_argc = 0;
    g_argv[g_argc++] = exe;
    if (unity_args && *unity_args) {
        char *b = strdup(unity_args), *save;
        for (char *t = strtok_r(b, " \t", &save); t && g_argc < 63; t = strtok_r(NULL, " \t", &save)) g_argv[g_argc++] = t;
    }
    g_argv[g_argc] = NULL;
    g_width = width; g_height = height;
    if (refresh_hz > 0) g_hz = refresh_hz;
    if (r_set_args && !g_args_set) set_args(0, NULL);     // (libunity may never call it)
}

// ---------------------------------------------------------------------------------- Screen.resolutions

typedef void (*add_icall_fn)(const char *name, const void *fn);
static add_icall_fn r_mono_add;
static void *(*r_get_resolutions)(void);
static void *(*m_object_get_class)(void *);
static void *(*m_class_get_element_class)(void *);
static void *(*m_domain_get)(void);
static void *(*m_array_new)(void *, void *, uintptr_t);
static int (*m_array_element_size)(void *);
static uintptr_t (*m_array_length)(void *);
static char *(*m_array_addr_with_size)(void *, int, uintptr_t);
static void *(*m_image_loaded)(const char *);
static void *(*m_class_from_name)(void *, const char *, const char *);
static void *(*m_array_class_get)(void *, uint32_t);
static void *(*g_sym)(const char *);      // the Mono library's functions (tests set their own)

static int mono_api(void)
{
    if (m_array_new) return 1;
    if (!g_sym) return 0;
#define M(v, n) (*(void **)&v = g_sym(n))
    M(m_object_get_class, "mono_object_get_class"); M(m_class_get_element_class, "mono_class_get_element_class");
    M(m_domain_get, "mono_domain_get"); M(m_array_element_size, "mono_array_element_size");
    M(m_array_length, "mono_array_length"); M(m_array_addr_with_size, "mono_array_addr_with_size");
    M(m_image_loaded, "mono_image_loaded"); M(m_class_from_name, "mono_class_from_name");
    M(m_array_class_get, "mono_array_class_get");
    M(m_array_new, "mono_array_new");
#undef M
    if (!m_object_get_class || !m_class_get_element_class || !m_domain_get || !m_array_element_size ||
        !m_array_addr_with_size || !m_array_new) { m_array_new = NULL; return 0; }
    return 1;
}

// an array's length: mono_array_length, which Unity's 2017.x Mono has only as a macro: MonoArray's
// max_length, after the object header (vtable, sync) and the bounds pointer
static uintptr_t array_length(void *a)
{
    return m_array_length ? m_array_length(a) : *(uintptr_t *)((char *)a + 3 * sizeof(void *));
}

// UnityEngine.Resolution: width, height, then the refresh rate: an int (to 2022.1), or
// RefreshRate { uint numerator, denominator } (2022.2+), by the element's size
static void *w_get_resolutions(void)
{
    void *arr = r_get_resolutions();
    static int told;
    if (!mono_api()) {
        if (!told++) LOG("Screen.resolutions: Mono's array functions not found: Unity's list as it is\n");
        return arr;
    }
    if (arr && array_length(arr) > 0) {
        if (!told++) LOG("Screen.resolutions: Unity's list (%u modes)\n", (unsigned)array_length(arr));
        return arr;
    }
    if (g_width <= 0 || g_height <= 0) return arr;
    void *ec = NULL, *ac = NULL;
    if (arr) { ac = m_object_get_class(arr); ec = ac ? m_class_get_element_class(ac) : NULL; }
    else if (m_image_loaded && m_class_from_name) {
        static const char *const images[] = { "UnityEngine.CoreModule", "UnityEngine" };
        for (unsigned i = 0; i < 2 && !ec; i++) { void *im = m_image_loaded(images[i]); ec = im ? m_class_from_name(im, "UnityEngine", "Resolution") : NULL; }
        ac = ec && m_array_class_get ? m_array_class_get(ec, 1) : NULL;
    }
    if (!ec || !ac) return arr;
    void *n = m_array_new(m_domain_get(), ec, 1);
    int sz = m_array_element_size(ac);
    if (!n || sz < 12) return arr;
    int32_t v[4] = { g_width, g_height, g_hz, sz >= 16 ? 1 : 0 };
    memcpy(m_array_addr_with_size(n, sz, 0), v, sz >= 16 ? 16 : 12);
    static int logged;
    if (!logged++) LOG("Screen.resolutions: none from Unity, the game gets %dx%d @ %d Hz (as a desktop with one mode)\n", g_width, g_height, g_hz);
    return n;
}

static void w_mono_add(const char *name, const void *fn)
{
    if (name && fn && !strcmp(name, "UnityEngine.Screen::get_resolutions")) {
        r_get_resolutions = (void *(*)(void))fn;
        fn = (const void *)w_get_resolutions;
    }
    r_mono_add(name, fn);
}

// IL2CPP's API (libunity looks every one up by name: the hook keeps what it finds); the arrays have
// Mono's layout (header, bounds, max_length, the elements at four pointers)
static add_icall_fn r_il2cpp_add;
static void *(*r_il2_get_resolutions)(void);
static void *(*i_object_get_class)(void *), *(*i_class_get_element_class)(void *), *(*i_domain_get)(void),
    *(*i_array_new)(void *, uintptr_t), *(*i_domain_assembly_open)(void *, const char *),
    *(*i_assembly_get_image)(void *), *(*i_class_from_name)(void *, const char *, const char *),
    *(*i_array_class_get)(void *, uint32_t);
static uint32_t (*i_array_length)(void *);
static int (*i_array_element_size)(void *);

static void *w_il2_get_resolutions(void)
{
    void *arr = r_il2_get_resolutions();
    static int told;
    if (!i_array_new || !i_array_length || !i_array_element_size || !i_object_get_class || !i_class_get_element_class) {
        if (!told++) LOG("Screen.resolutions: IL2CPP's array functions not found: Unity's list as it is\n");
        return arr;
    }
    if (arr && i_array_length(arr) > 0) {
        if (!told++) LOG("Screen.resolutions: Unity's list (%u modes)\n", (unsigned)i_array_length(arr));
        return arr;
    }
    if (g_width <= 0 || g_height <= 0) return arr;
    void *ec = NULL, *ac = NULL;
    if (arr) { ac = i_object_get_class(arr); ec = ac ? i_class_get_element_class(ac) : NULL; }
    else if (i_domain_get && i_domain_assembly_open && i_assembly_get_image && i_class_from_name && i_array_class_get) {
        static const char *const names[] = { "UnityEngine.CoreModule.dll", "UnityEngine.CoreModule", "UnityEngine.dll", "UnityEngine" };
        for (unsigned i = 0; i < sizeof names / sizeof names[0] && !ec; i++) {
            void *as = i_domain_assembly_open(i_domain_get(), names[i]);
            void *im = as ? i_assembly_get_image(as) : NULL;
            ec = im ? i_class_from_name(im, "UnityEngine", "Resolution") : NULL;
        }
        ac = ec ? i_array_class_get(ec, 1) : NULL;
    }
    if (!ec || !ac) return arr;
    void *n = i_array_new(ec, 1);
    int sz = i_array_element_size(ac);
    if (!n || sz < 12) return arr;
    int32_t v[4] = { g_width, g_height, g_hz, sz >= 16 ? 1 : 0 };
    memcpy((char *)n + 4 * sizeof(void *), v, sz >= 16 ? 16 : 12);
    static int logged;
    if (!logged++) LOG("Screen.resolutions: none from Unity, the game gets %dx%d @ %d Hz (as a desktop with one mode)\n", g_width, g_height, g_hz);
    return n;
}

static void w_il2cpp_add(const char *name, const void *fn)
{
    if (name && fn && !strcmp(name, "UnityEngine.Screen::get_resolutions")) {
        r_il2_get_resolutions = (void *(*)(void))fn;
        fn = (const void *)w_il2_get_resolutions;
    }
    r_il2cpp_add(name, fn);
}

static void *il2cpp_hook(const char *symbol, void *found)
{
    static const struct { const char *n; void **v; } api[] = {
        { "il2cpp_object_get_class", (void **)&i_object_get_class }, { "il2cpp_class_get_element_class", (void **)&i_class_get_element_class },
        { "il2cpp_domain_get", (void **)&i_domain_get }, { "il2cpp_array_new", (void **)&i_array_new },
        { "il2cpp_domain_assembly_open", (void **)&i_domain_assembly_open }, { "il2cpp_assembly_get_image", (void **)&i_assembly_get_image },
        { "il2cpp_class_from_name", (void **)&i_class_from_name }, { "il2cpp_array_class_get", (void **)&i_array_class_get },
        { "il2cpp_array_length", (void **)&i_array_length }, { "il2cpp_array_element_size", (void **)&i_array_element_size } };
    for (unsigned i = 0; i < sizeof api / sizeof api[0]; i++)
        if (!strcmp(symbol, api[i].n)) { *api[i].v = found; return found; }
    if (!strcmp(symbol, "il2cpp_add_internal_call") && found != (void *)w_il2cpp_add) {
        r_il2cpp_add = (add_icall_fn)found;
        return (void *)w_il2cpp_add;
    }
    return found;
}

// ---------------------------------------------------------------------------------- the hook

static void *g_mono;                     // the Mono library's handle (the loader's)
static void *loader_sym(const char *n) { return g_mono ? so_dlsym(g_mono, n) : NULL; }

void desktopenv_set_resolver(void *(*sym)(const char *)) { g_sym = sym; }

void *desktopenv_dlsym_hook(const char *symbol, void *found)
{
    if (found && !strncmp(symbol, "il2cpp_", 7)) return il2cpp_hook(symbol, found);
    if (!found || strncmp(symbol, "mono_", 5)) return found;
    if (!g_mono && !g_sym) {
        Dl_info di;
        if (so_dladdr(found, &di) && di.dli_fname && strstr(di.dli_fname, "mono")) g_mono = so_dlopen(di.dli_fname, 0);
        if (g_mono) g_sym = loader_sym;
    }
    if ((!strcmp(symbol, "mono_unity_runtime_set_main_args") || !strcmp(symbol, "mono_runtime_set_main_args")) &&
        found != (void *)w_set_args) {
        r_set_args = (set_args_fn)found;
        if (g_argc && !g_args_set) set_args(0, NULL);       // now, in case libunity never calls it
        return (void *)w_set_args;
    }
    if (!strcmp(symbol, "mono_add_internal_call") && found != (void *)w_mono_add) {
        r_mono_add = (add_icall_fn)found;
        return (void *)w_mono_add;
    }
    return found;
}
