// Mono's JIT compilations timed, to tell first-use compiling from the rest of a long frame:
//   UA_JIT_LOG=1        each frame of 100 ms or more that compiled: "[jit] <n> methods, <ms> ms of a
//                       <ms> ms frame at <s> s" (the [hitch] lines' clock); every compile of at least
//                       UA_JIT_LOG_MS (default 5) named: "[jit] <ms> ms: <method>"; every 10 s with any
//                       compiling, the totals: "[jit] <n> methods, <ms> ms so far"
//   UA_JIT_LOG=2        also every compile: "[jitc] <s> <ms> <assembly> <token> <method>" (seconds
//                       since the first frame, its time, the metadata token: what a warm-up could compile)
// The profiler's JIT begin/done events, installed at the first frame; compiles on any thread count
// (the frame's are mostly UnityMain's). Measurement only: nothing the game does changes.
#include "jitlog.h"
#include "frames.h"
#include "hitches.h"
#include "../loader/so_loader.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef void *(*create_fn)(void *);
typedef void (*begin_cb)(void *prof, void *method);
typedef void (*done_cb)(void *prof, void *method, void *jinfo);
typedef void (*set_begin_fn)(void *handle, begin_cb cb);
typedef void (*set_done_fn)(void *handle, done_cb cb);

static int g_on, g_installed;
static double g_name_ms = 5;
static char *(*m_full_name)(void *method, int signature);
static void *(*m_get_class)(void *method), *(*m_class_image)(void *klass);
static const char *(*m_image_name)(void *image);
static unsigned (*m_token)(void *method);
static void (*m_free)(void *);
static atomic_llong g_ns, g_count;          // compile time and compiles, all threads, so far
static long long f_ns, f_count;             // at the last frame
static double f_last, g_last_total;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

// a compile can start another (a class's static constructor run while compiling): a stack a thread
#define DEPTH 16
static __thread double t_begin[DEPTH];
static __thread int t_depth;

static void on_begin(void *prof, void *method)
{
    (void)prof; (void)method;
    if (t_depth < DEPTH) t_begin[t_depth] = now();
    t_depth++;
}

static void on_done(void *prof, void *method, void *jinfo)
{
    (void)prof; (void)jinfo;
    if (t_depth <= 0) return;
    t_depth--;
    if (t_depth >= DEPTH) return;
    double dt = now() - t_begin[t_depth];
    atomic_fetch_add(&g_ns, (long long)(dt * 1e9));
    atomic_fetch_add(&g_count, 1);
    if (g_on >= 2 && m_full_name && m_get_class && m_class_image && m_image_name && m_token) {
        char *n = m_full_name(method, 1);
        void *img = m_class_image(m_get_class(method));
        double t0 = frames_t_start();
        fprintf(stderr, "[jitc] %.2f %.2f %s %08x %s\n", t0 > 0 ? now() - t0 : 0.0, dt * 1000,
                img ? m_image_name(img) : "?", m_token(method), n ? n : "?");
        if (n && m_free) m_free(n);
    } else if (dt * 1000 >= g_name_ms && m_full_name) {
        char *n = m_full_name(method, 1);
        fprintf(stderr, "[jit] %.1f ms: %s\n", dt * 1000, n ? n : "?");
        if (n && m_free) m_free(n);
    }
}

void jitlog_init(void)
{
    const char *e = getenv("UA_JIT_LOG");
    g_on = e && *e && *e != '0' ? (atoi(e) >= 2 ? 2 : 1) : 0;
    if ((e = getenv("UA_JIT_LOG_MS")) && atof(e) > 0) g_name_ms = atof(e);
}

// Mono's functions: the loaded runtime's (tests set their own)
static void *(*g_sym)(const char *name);
static void *runtime_sym(const char *name) { void *h = mono_runtime_lib(); return h ? so_dlsym(h, name) : NULL; }
void jitlog_set_resolver(void *(*sym)(const char *name)) { g_sym = sym; }
long long jitlog_compiles(void) { return atomic_load(&g_count); }

static void install(void)
{
    g_installed = 1;
    void *(*sym)(const char *) = g_sym ? g_sym : runtime_sym;
    create_fn create = (create_fn)sym("mono_profiler_create");
    set_begin_fn sb = (set_begin_fn)sym("mono_profiler_set_jit_begin_callback");
    set_done_fn sd = (set_done_fn)sym("mono_profiler_set_jit_done_callback");
    // Unity 2017.x's old Mono (2.6): the single profiler of the legacy API (mono_profiler_install, the JIT
    // compile events); its end callback's third argument is the result, unused here
    void (*l_install)(void *, void *) = (void (*)(void *, void *))sym("mono_profiler_install");
    void (*l_jit)(void *, void *) = (void (*)(void *, void *))sym("mono_profiler_install_jit_compile");
    void (*l_events)(int) = (void (*)(int))sym("mono_profiler_set_events");
    if ((!sb || !sd) && l_install && l_jit && l_events) {
        static int prof_token;
        m_full_name = (char *(*)(void *, int))sym("mono_method_full_name");
        m_free = (void (*)(void *))sym("g_free");
        m_get_class = (void *(*)(void *))sym("mono_method_get_class");
        m_class_image = (void *(*)(void *))sym("mono_class_get_image");
        m_image_name = (const char *(*)(void *))sym("mono_image_get_name");
        m_token = (unsigned (*)(void *))sym("mono_method_get_token");
        l_install(&prof_token, NULL);
        l_jit((void *)on_begin, (void *)on_done);
        l_events(1 << 4);                       // MONO_PROFILE_JIT_COMPILATION
        fprintf(stderr, "[jit] compiles timed (UA_JIT_LOG, Mono's legacy profiler; named from %.0f ms)\n", g_name_ms);
        return;
    }
    if (!create || !sb || !sd) { fprintf(stderr, "[jit] no Mono profiler API: UA_JIT_LOG off\n"); g_on = 0; return; }
    m_full_name = (char *(*)(void *, int))sym("mono_method_full_name");
    m_free = (void (*)(void *))sym("mono_free");
    m_get_class = (void *(*)(void *))sym("mono_method_get_class");
    m_class_image = (void *(*)(void *))sym("mono_class_get_image");
    m_image_name = (const char *(*)(void *))sym("mono_image_get_name");
    m_token = (unsigned (*)(void *))sym("mono_method_get_token");
    void *handle = create(NULL);
    sb(handle, on_begin);
    sd(handle, on_done);
    fprintf(stderr, "[jit] compiles timed (UA_JIT_LOG; named from %.0f ms)\n", g_name_ms);
}

void jitlog_frame(void)
{
    if (!g_on) return;
    if (!g_installed) { install(); f_last = now(); return; }
    double t = now(), dt = t - f_last;
    long long ns = atomic_load(&g_ns), count = atomic_load(&g_count);
    if (dt >= 0.100 && count > f_count) {
        double t0 = frames_t_start();
        fprintf(stderr, "[jit] %lld methods, %.1f ms of a %.1f ms frame at %.1f s\n", count - f_count,
                (ns - f_ns) / 1e6, dt * 1000, t0 > 0 ? t - t0 : 0.0);
    }
    if (t - g_last_total >= 10 && count > 0) {
        static long long shown = -1;
        if (count != shown) fprintf(stderr, "[jit] %lld methods, %.1f ms so far\n", count, ns / 1e6);
        shown = count;
        g_last_total = t;
    }
    f_ns = ns; f_count = count; f_last = t;
}
