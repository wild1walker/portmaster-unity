// Mono compiles a method the first time it runs, in that frame: on the RG34XX-SP's A53 about 1-2 ms
// a method, and a game's first slash, first hit or first death compiles tens to hundreds of them at
// once (Hollow Knight, King's Pass: 50-220 ms of 115-330 ms hitches; ~1 s of compiling in a 50 s
// walk, 80 % of it in methods that are not generic instances).
//   UA_JIT_WARM=1   (off by default; a port turns it on) the methods the game has had compiled are
//                   listed in data/jitwarm.txt beside the game ("<assembly> <token> <method>", as each
//                   first compiles), and at the next launch compiled ahead on UnityMain between
//                   frames, from frame 60 on, at most UA_JIT_WARM_MS (default 4) ms a frame, in the
//                   order the game first needed them. A method whose name is not the one listed (the
//                   game changed) is left out; generic instances are not listed (a token names their
//                   definition). Logged: "[jitwarm] <n> listed", "[jitwarm] <n> compiled ahead in
//                   <ms> ms (<n> already compiled, <n> not found)".
// Unity 2017.x's old Mono (2.6) has no jit-done callback of the new profiler API: there the methods are listed
// through its legacy profiler (mono_profiler_install_jit_compile, as UA_JIT_LOG reads them; Mono 2.6 has one
// profiler, so with UA_JIT_LOG on as well nothing new is listed). There it takes UA_JIT_WARM=2: the launchers'
// default 1 predates it, and a port turns it on once it has checked its game's static field initializers
// (below).
// What the game does is the same: the methods it would compile, compiled earlier on the same thread.
// One thing moves with them: Mono runs the static field initializers of a class without its own
// static constructor (beforefieldinit) when it compiles a method that uses its static fields, so
// those run earlier too. That is why it is a port's choice: a game whose field initializers read
// other objects (FindObjectOfType, a singleton's instance) could cache something else; Hollow
// Knight's are constants, collections and paths (checked 2026-11-04).
#include "jitwarm.h"
#include "hitches.h"
#include "../loader/so_loader.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LIST "data/jitwarm.txt"

typedef void *(*create_fn)(void *);
typedef void (*done_cb)(void *prof, void *method, void *jinfo);
typedef void (*set_done_fn)(void *handle, done_cb cb);

static int g_on, g_installed;
static double g_budget = 0.004;
static long g_frame;
static char *(*m_full_name)(void *method, int signature);
static void (*m_free)(void *);
static void *(*m_get_class)(void *method), *(*m_class_image)(void *klass), *(*m_image_loaded)(const char *name);
static const char *(*m_image_name)(void *image);
static unsigned (*m_token)(void *method);
static void *(*m_get_method)(void *image, unsigned token, void *klass);
static void *(*m_compile)(void *method);

// the methods listed (from the file and since): assembly and token, hashed
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *g_seen;
static size_t g_seen_cap, g_seen_n;
static FILE *g_out;

// the warm-up's list, read at start
struct entry { char image[64]; unsigned token; char *name; };
static struct entry *g_list;
static size_t g_list_n, g_next;
static long g_done, g_had, g_missing;
static double g_spent;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static uint64_t key(const char *image, unsigned token)
{
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = image; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ULL;
    h = (h ^ token) * 1099511628211ULL;
    return h ? h : 1;
}

// adds the key; 1 when it was not there (under g_lock)
static int seen_add(uint64_t k)
{
    if ((g_seen_n + 1) * 2 > g_seen_cap) {
        size_t cap = g_seen_cap ? g_seen_cap * 2 : 1 << 14;
        uint64_t *t = calloc(cap, sizeof *t);
        if (!t) return 0;
        for (size_t i = 0; i < g_seen_cap; i++)
            if (g_seen[i]) { size_t j = g_seen[i] & (cap - 1); while (t[j]) j = (j + 1) & (cap - 1); t[j] = g_seen[i]; }
        free(g_seen); g_seen = t; g_seen_cap = cap;
    }
    size_t j = k & (g_seen_cap - 1);
    while (g_seen[j]) { if (g_seen[j] == k) return 0; j = (j + 1) & (g_seen_cap - 1); }
    g_seen[j] = k; g_seen_n++;
    return 1;
}

// a generic instance: a generic type's ("List`1<int>:Add") or a generic method's ("C:M<int>"); the
// compiler's own names ("C/<Start>d__5:MoveNext", "C:<Awake>b__3_0") are not
static int generic_instance(const char *name)
{
    if (strchr(name, '`')) return 1;
    const char *m = strrchr(name, ':');
    if (!m) return 0;
    m++;
    const char *lt = strchr(m, '<');
    return lt && lt != m;
}

static void on_begin(void *prof, void *method) { (void)prof; (void)method; }   // (Mono 2.6's legacy API calls both)

static void on_done(void *prof, void *method, void *jinfo)
{
    (void)prof; (void)jinfo;
    unsigned tok = m_token(method);
    if (!tok || !g_out) return;
    void *img = m_class_image(m_get_class(method));
    const char *in = img ? m_image_name(img) : NULL;
    if (!in || strlen(in) >= sizeof g_list[0].image || strchr(in, ' ')) return;
    char *n = m_full_name(method, 1);
    if (!n) return;
    if (!generic_instance(n) && !strstr(n, "(wrapper")) {
        pthread_mutex_lock(&g_lock);
        if (seen_add(key(in, tok))) { fprintf(g_out, "%s %08x %s\n", in, tok, n); fflush(g_out); }
        pthread_mutex_unlock(&g_lock);
    }
    if (m_free) m_free(n);
}

void jitwarm_init(void)
{
    const char *e = getenv("UA_JIT_WARM");
    g_on = e && *e && *e != '0' ? (atoi(e) >= 2 ? 2 : 1) : 0;
    if ((e = getenv("UA_JIT_WARM_MS")) && atof(e) >= 0) g_budget = atof(e) / 1000;
}

static void read_list(void)
{
    FILE *f = fopen(LIST, "r");
    if (!f) return;
    char line[2048];
    size_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        char image[64]; unsigned tok; int at = 0;
        if (sscanf(line, "%63s %x %n", image, &tok, &at) != 2 || !at) continue;
        char *name = line + at;
        name[strcspn(name, "\n")] = 0;
        if (!seen_add(key(image, tok))) continue;
        if (g_list_n == cap) {
            cap = cap ? cap * 2 : 4096;
            struct entry *l = realloc(g_list, cap * sizeof *l);
            if (!l) break;
            g_list = l;
        }
        struct entry *x = &g_list[g_list_n++];
        snprintf(x->image, sizeof x->image, "%s", image);
        x->token = tok;
        x->name = strdup(name);
    }
    fclose(f);
}

static void install(void)
{
    g_installed = 1;
    void *h = mono_runtime_lib();
    create_fn create = h ? (create_fn)so_dlsym(h, "mono_profiler_create") : NULL;
    set_done_fn sd = h ? (set_done_fn)so_dlsym(h, "mono_profiler_set_jit_done_callback") : NULL;
    if (h) {
        m_full_name = (char *(*)(void *, int))so_dlsym(h, "mono_method_full_name");
        m_free = (void (*)(void *))so_dlsym(h, "mono_free");
        m_get_class = (void *(*)(void *))so_dlsym(h, "mono_method_get_class");
        m_class_image = (void *(*)(void *))so_dlsym(h, "mono_class_get_image");
        m_image_name = (const char *(*)(void *))so_dlsym(h, "mono_image_get_name");
        m_image_loaded = (void *(*)(const char *))so_dlsym(h, "mono_image_loaded");
        m_token = (unsigned (*)(void *))so_dlsym(h, "mono_method_get_token");
        m_get_method = (void *(*)(void *, unsigned, void *))so_dlsym(h, "mono_get_method");
        m_compile = (void *(*)(void *))so_dlsym(h, "mono_compile_method");
    }
    // Unity 2017.x's Mono 2.6: the legacy profiler's JIT events (its end callback's third argument is
    // the result, which on_done does not read)
    void (*l_install)(void *, void *) = h ? (void (*)(void *, void *))so_dlsym(h, "mono_profiler_install") : NULL;
    void (*l_jit)(void *, void *) = h ? (void (*)(void *, void *))so_dlsym(h, "mono_profiler_install_jit_compile") : NULL;
    void (*l_events)(int) = h ? (void (*)(int))so_dlsym(h, "mono_profiler_set_events") : NULL;
    int legacy = (!create || !sd) && l_install && l_jit && l_events;
    if (legacy && g_on < 2) {
        fprintf(stderr, "[jitwarm] Mono 2.6: off (UA_JIT_WARM=2 turns it on once a port has checked its static field initializers)\n");
        g_on = 0;
        return;
    }
    if (legacy && !m_free && h) m_free = (void (*)(void *))so_dlsym(h, "g_free");
    if ((!legacy && (!create || !sd)) || !m_full_name || !m_get_class || !m_class_image || !m_image_name || !m_image_loaded ||
        !m_token || !m_get_method || !m_compile) {
        fprintf(stderr, "[jitwarm] not a Mono player (or its API is missing): off\n");
        g_on = 0;
        return;
    }
    read_list();
    const char *jl = getenv("UA_JIT_LOG");
    if (legacy && jl && *jl && *jl != '0')
        fprintf(stderr, "[jitwarm] UA_JIT_LOG holds Mono 2.6's one profiler: compiled ahead, nothing new listed\n");
    else {
        g_out = fopen(LIST, "a");
        if (!g_out) fprintf(stderr, "[jitwarm] cannot write %s: nothing new is listed\n", LIST);
        if (legacy) {
            static int prof_token;
            l_install(&prof_token, NULL);
            l_jit((void *)on_begin, (void *)on_done);
            l_events(1 << 4);                   // MONO_PROFILE_JIT_COMPILATION
        } else sd(create(NULL), on_done);
    }
    fprintf(stderr, "[jitwarm] %zu listed (%s), compiled ahead from frame 60 at most %.1f ms a frame\n", g_list_n, LIST,
            g_budget * 1000);
}

void jitwarm_frame(void)
{
    if (!g_on) return;
    if (!g_installed) { install(); return; }
    if (++g_frame < 60 || g_next >= g_list_n) return;
    double t0 = now(), end = t0 + g_budget;
    while (g_next < g_list_n && now() < end) {
        struct entry *x = &g_list[g_next];
        void *img = m_image_loaded(x->image);  // (Unity loads the game's assemblies before its first frame)
        void *m = img ? m_get_method(img, x->token, NULL) : NULL;
        char *n = m ? m_full_name(m, 1) : NULL;
        if (n && !strcmp(n, x->name)) {
            double c = now();
            m_compile(m);
            if (now() - c < 0.0002) g_had++; else g_done++;
        } else g_missing++;
        if (n && m_free) m_free(n);
        free(x->name); x->name = NULL;
        g_next++;
    }
    g_spent += now() - t0;
    if (g_next >= g_list_n)
        fprintf(stderr, "[jitwarm] %ld compiled ahead in %.0f ms (%ld already compiled, %ld not found)\n", g_done,
                g_spent * 1000, g_had, g_missing);
}
