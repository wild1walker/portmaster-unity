// jitlog.c's check (runtime/host/test/plugins_test.sh): UA_JIT_LOG installs its compile callbacks through Mono's
// profiler API, the current one (mono_profiler_create + set_jit_*_callback) or, on Unity 2017.x's Mono 2.6, the
// legacy one (mono_profiler_install + install_jit_compile + set_events(JIT_COMPILATION)), and counts the compiles
// the callbacks report; without either API it stays off. Stand-ins for Mono; the loader and frames.c stubbed.
#include "../jitlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double frames_t_start(void) { return 0; }
void *mono_runtime_lib(void) { return NULL; }
void *so_dlsym(void *h, const char *s) { (void)h; (void)s; return NULL; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef void (*begin_cb)(void *, void *);
typedef void (*done_cb)(void *, void *, void *);
static begin_cb g_begin;
static done_cb g_done;
static int g_events = -1, g_installed;

// the legacy API
static void l_install(void *prof, void *shutdown) { (void)shutdown; g_installed = prof != NULL; }
static void l_jit(void *begin, void *end) { g_begin = (begin_cb)begin; g_done = (done_cb)end; }
static void l_events(int e) { g_events = e; }
static void *legacy_sym(const char *n)
{
    if (!strcmp(n, "mono_profiler_install")) return (void *)l_install;
    if (!strcmp(n, "mono_profiler_install_jit_compile")) return (void *)l_jit;
    if (!strcmp(n, "mono_profiler_set_events")) return (void *)l_events;
    return NULL;
}
static void *none_sym(const char *n) { (void)n; return NULL; }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *mode = argc > 1 ? argv[1] : "none";
    setenv("UA_JIT_LOG", "1", 1);
    jitlog_set_resolver(strcmp(mode, "legacy") ? none_sym : legacy_sym);
    jitlog_init();
    jitlog_frame();                         // installs at the first frame
    if (strcmp(mode, "legacy")) {
        jitlog_frame();
        CHECK(jitlog_compiles() == 0 && !g_begin, "no API: nothing installed");
    } else {
        CHECK(g_installed && g_begin && g_done && g_events == (1 << 4), "legacy API: installed, JIT compilation events (%d)", g_events);
        if (g_begin && g_done) {
            g_begin(NULL, (void *)1); g_done(NULL, (void *)1, (void *)0);
            g_begin(NULL, (void *)2); g_begin(NULL, (void *)3); g_done(NULL, (void *)3, (void *)0); g_done(NULL, (void *)2, (void *)0);
        }
        CHECK(jitlog_compiles() == 3, "three compiles, one inside another: %lld", jitlog_compiles());
        jitlog_frame();
    }
    printf("jitlog %s: %s (%d failures)\n", mode, fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
