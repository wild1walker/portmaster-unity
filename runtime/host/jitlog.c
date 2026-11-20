// Mono's JIT compilations timed, to tell first-use compiling from the rest of a long frame:
//   UA_JIT_LOG=1        each frame of 100 ms or more that compiled: "[jit] <n> methods, <ms> ms of a
//                       <ms> ms frame at <s> s" (the [hitch] lines' clock); every compile of at least
//                       UA_JIT_LOG_MS (default 5) named: "[jit] <ms> ms: <method>"; every 10 s with any
//                       compiling, the totals: "[jit] <n> methods, <ms> ms so far"
//   UA_JIT_LOG=2        also every compile: "[jitc] <s> <ms> <assembly> <token> <method>" (seconds
//                       since the first frame, its time, the metadata token: what a warm-up could compile)
// The profiler's JIT begin/done events, installed at the first frame; compiles on any thread count
// (the frame's are mostly UnityMain's). Measurement only: nothing the game does changes.
//
// The game's managed allocations counted (the profiler's GC allocation event):
//   UA_ALLOC_LOG=<s>    every <s> s: "[alloc] UnityMain <n> objects, <KB> KB a frame; other threads <n> a
//                       second; <class> <n> ..." (UnityMain's most allocated classes a frame). Every object the
//                       runtime makes (new, boxing, arrays, strings, delegates; from managed code and from the
//                       engine's calls) passes through the event. Only with Boehm (no managed allocator: Unity's
//                       Mono, the Mono route's 64-bit Mono) is turning the event on free of side effects; a
//                       runtime that has managed allocators and has started already refuses it (logged).
//                       Unity 2017.x's Mono 5.0 (the Mono route) through its legacy profiler (MONO_PROFILE_ALLOCATIONS);
//                       not Mono 2.6 (2017.x's 32-bit players: its legacy event misses code compiled before it is on).
#include "jitlog.h"
#include "frames.h"
#include "hitches.h"
#include "../loader/so_loader.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
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

// ---- UA_ALLOC_LOG ----
static double g_alloc_every, g_alloc_next;
static __thread int t_main;                 // set on UnityMain at its first frame
static long long a_main, a_main_bytes, a_frames, a_main_total;
static atomic_llong a_other;
static void *(*m_obj_class)(void *obj);
static unsigned (*m_obj_size)(void *obj);
static const char *(*m_class_name)(void *klass), *(*m_class_ns)(void *klass);
#define ACLS 512                            // UnityMain's classes, open addressing on the class pointer
static struct { void *k; long long n, b; } a_cls[ACLS];

static void on_alloc(void *prof, void *obj)
{
    (void)prof;
    if (!t_main) { atomic_fetch_add_explicit(&a_other, 1, memory_order_relaxed); return; }
    unsigned sz = m_obj_size ? m_obj_size(obj) : 0;
    a_main++; a_main_total++; a_main_bytes += sz;
    void *k = m_obj_class ? m_obj_class(obj) : NULL;
    unsigned h = (unsigned)(((uintptr_t)k >> 3) * 2654435761u) % ACLS;
    for (int i = 0; i < 16; i++, h = (h + 1) % ACLS)
        if (a_cls[h].k == k || !a_cls[h].k) { a_cls[h].k = k; a_cls[h].n++; a_cls[h].b += sz; return; }
}

static void on_alloc_legacy(void *prof, void *obj, void *klass) { (void)klass; on_alloc(prof, obj); }

static void alloc_install(void *(*sym)(const char *))
{
    m_obj_class = (void *(*)(void *))sym("mono_object_get_class");
    m_obj_size = (unsigned (*)(void *))sym("mono_object_get_size");
    m_class_name = (const char *(*)(void *))sym("mono_class_get_name");
    m_class_ns = (const char *(*)(void *))sym("mono_class_get_namespace");
    void *(*create)(void *) = (void *(*)(void *))sym("mono_profiler_create");
    int (*enable)(void) = (int (*)(void))sym("mono_profiler_enable_allocations");
    void (*set_cb)(void *, void *) = (void (*)(void *, void *))sym("mono_profiler_set_gc_allocation_callback");
    // the legacy API (Unity 2017.x's Mono 5.0, the Mono route's 2017.4 runtime): its event reaches every allocation
    // there; Mono 2.6 (2017.x's 32-bit players) chooses an allocation function without the event when a method is
    // compiled (and in its AOT images), so code compiled before the first frame would go uncounted
    void (*l_install)(void *, void *) = (void (*)(void *, void *))sym("mono_profiler_install");
    void (*l_alloc)(void *) = (void (*)(void *))sym("mono_profiler_install_allocation");
    void (*l_events)(int) = (void (*)(int))sym("mono_profiler_set_events");
    char *(*build_info)(void) = (char *(*)(void))sym("mono_get_runtime_build_info");
    char *bi = build_info ? build_info() : NULL;
    int major = bi ? atoi(bi) : 0;
    t_main = 1;
    if (create && enable && set_cb) {
        if (!enable()) { fprintf(stderr, "[alloc] the runtime refuses allocation events this late: UA_ALLOC_LOG off\n"); g_alloc_every = 0; return; }
        set_cb(create(NULL), (void *)on_alloc);
    } else if (l_install && l_alloc && l_events && major >= 5) {
        static int prof_token;
        l_install(&prof_token, NULL);
        l_alloc((void *)on_alloc_legacy);
        l_events(1 << 7 | (g_on ? 1 << 4 : 0));     // MONO_PROFILE_ALLOCATIONS (and UA_JIT_LOG's JIT events)
    } else {
        fprintf(stderr, "[alloc] no allocation event for this runtime (%s; Mono 2.6's legacy one misses compiled code): UA_ALLOC_LOG off\n", bi ? bi : "?");
        g_alloc_every = 0; return;
    }
    fprintf(stderr, "[alloc] managed allocations counted (UA_ALLOC_LOG, every %g s)\n", g_alloc_every);
}

static void alloc_frame(void)
{
    double t = now();
    a_frames++;
    if (t < g_alloc_next) return;
    long long other = atomic_exchange(&a_other, 0);
    if (g_alloc_next > 0 && a_frames > 0) {
        double secs = g_alloc_every + (t - g_alloc_next);
        char line[1024] = ""; int o = 0;
        for (int top = 0; top < 8 && o < 900; top++) {
            int bi = -1;
            for (int i = 0; i < ACLS; i++) if (a_cls[i].k && a_cls[i].n > 0 && (bi < 0 || a_cls[i].n > a_cls[bi].n)) bi = i;
            if (bi < 0) break;
            const char *ns = m_class_ns ? m_class_ns(a_cls[bi].k) : "", *n = m_class_name ? m_class_name(a_cls[bi].k) : "?";
            o += snprintf(line + o, sizeof line - o, " %s%s%s %.1f (%lld B)", ns && *ns ? ns : "", ns && *ns ? "." : "", n ? n : "?",
                          (double)a_cls[bi].n / a_frames, a_cls[bi].b / (a_cls[bi].n ? a_cls[bi].n : 1));
            a_cls[bi].n = -a_cls[bi].n;     // shown (the table is cleared below)
        }
        fprintf(stderr, "[alloc] UnityMain %.1f objects, %.2f KB a frame (%lld frames); other threads %.0f a second;%s\n",
                (double)a_main / a_frames, a_main_bytes / 1024.0 / a_frames, a_frames,
                other / secs, line);
    }
    memset(a_cls, 0, sizeof a_cls);
    a_main = a_main_bytes = a_frames = 0;
    g_alloc_next = t + g_alloc_every;
}

void jitlog_init(void)
{
    const char *e = getenv("UA_ALLOC_LOG");
    if (e && atof(e) > 0) g_alloc_every = atof(e);
    e = getenv("UA_JIT_LOG");
    g_on = e && *e && *e != '0' ? (atoi(e) >= 2 ? 2 : 1) : 0;
    if ((e = getenv("UA_JIT_LOG_MS")) && atof(e) > 0) g_name_ms = atof(e);
}

// Mono's functions: the loaded runtime's (tests set their own)
static void *(*g_sym)(const char *name);
static void *runtime_sym(const char *name) { void *h = mono_runtime_lib(); return h ? so_dlsym(h, name) : NULL; }
void jitlog_set_resolver(void *(*sym)(const char *name)) { g_sym = sym; }
long long jitlog_compiles(void) { return atomic_load(&g_count); }
long long jitlog_allocs(void) { return a_main_total; }

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
    if (g_alloc_every > 0) {
        static int a_installed;
        if (!a_installed) { a_installed = 1; alloc_install(g_sym ? g_sym : runtime_sym); }
        else alloc_frame();
    }
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
