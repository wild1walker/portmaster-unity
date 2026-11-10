// jitwarm.c's list (runtime/host/test/plugins_test.sh): UA_JIT_WARM=2 on a stand-in for Unity 2017.x's Mono 2.6
// (the legacy profiler API), in a folder of its own (data/jitwarm.txt):
//   record   compiles reported by the profiler are listed after an "# mvid <assembly> <id>" line, once per assembly
//   warm     at the next launch the listed methods of the same build are compiled ahead
//   stale    the assembly's module version id changed (a port update, the setup again): its entries are dropped,
//            nothing is looked up, the file is written again without them; the other assembly's are kept
//   late     an assembly loaded after the list is read: its entries kept, its id checked when they come up
//   latestale the same with another id: skipped
//   patched  the same module version id but another method count (a patcher keeps the id): dropped
//   legacy   a list without "# mvid" lines (an older ua_host's): dropped whole
//   badtoken a listed token past the assembly's method table, or not a method's: skipped and logged, never
//            asked of Mono (whose lookup would raise BadImageFormatException)
#include "../jitwarm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

double frames_t_start(void) { return 0; }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// two assemblies of 100 methods each; image ids 1, 2; a method = image * 1000 + row
static const char *g_guid[3] = { "", "AAAA-1111", "BBBB-2222" };
static int g_compiled[3][101], g_lookups, g_bad_lookups;
typedef void (*done_cb)(void *, void *, int);
static done_cb g_done;
static void l_install(void *p, void *s) { (void)p; (void)s; }
static void l_jit(void *b, void *e) { (void)b; g_done = (done_cb)e; }
static void l_events(int e) { (void)e; }
static char *full_name(void *m, int sig) { (void)sig; char *b = malloc(32); long v = (long)m; snprintf(b, 32, "C%ld:M%ld ()", v / 1000, v % 1000); return b; }
static void *get_class(void *m) { return (void *)((long)m / 1000); }
static void *class_image(void *k) { return k; }
static const char *image_name(void *img) { return (long)img == 1 ? "Assembly-CSharp" : "Other"; }
static int g_other_loaded = 1;
static void *image_loaded(const char *n) { return (void *)(!strcmp(n, "Assembly-CSharp") ? 1L : !strcmp(n, "Other") && g_other_loaded ? 2L : 0L); }
static unsigned token(void *m) { return 0x06000000u | (unsigned)((long)m % 1000); }
static void *get_method(void *img, unsigned tok, void *k)
{
    (void)k; g_lookups++;
    unsigned row = tok & 0xffffff;
    if ((tok >> 24) != 6 || !row || row > 100) { g_bad_lookups++; return NULL; }   // (Mono would raise here)
    return (void *)((long)img * 1000 + row);
}
static void *compile(void *m) { long v = (long)m; g_compiled[v / 1000][v % 1000]++; return m; }
static const char *image_guid(void *img) { return g_guid[(long)img]; }
static int g_methods[3] = { 0, 100, 100 };
static int table_rows(void *img, int t) { return t == 6 ? g_methods[(long)img] : t == 2 ? 10 : 0; }
static void free_(void *p) { free(p); }
void *mono_runtime_lib(void) { return (void *)1; }
void *so_dlsym(void *h, const char *n)
{
    (void)h;
    static const struct { const char *n; void *f; } t[] = {
        { "mono_profiler_install", (void *)l_install }, { "mono_profiler_install_jit_compile", (void *)l_jit },
        { "mono_profiler_set_events", (void *)l_events }, { "mono_method_full_name", (void *)full_name },
        { "g_free", (void *)free_ }, { "mono_method_get_class", (void *)get_class }, { "mono_class_get_image", (void *)class_image },
        { "mono_image_get_name", (void *)image_name }, { "mono_image_loaded", (void *)image_loaded },
        { "mono_method_get_token", (void *)token }, { "mono_get_method", (void *)get_method },
        { "mono_compile_method", (void *)compile }, { "mono_image_get_guid", (void *)image_guid },
        { "mono_image_get_table_rows", (void *)table_rows } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(t[i].n, n)) return t[i].f;
    return NULL;
}
static void frames(int n) { for (int i = 0; i < n; i++) jitwarm_frame(); }
static void put(const char *text) { FILE *f = fopen("data/jitwarm.txt", "w"); fputs(text, f); fclose(f); }
static int count(const char *what) { FILE *f = fopen("data/jitwarm.txt", "r"); if (!f) return -1; char l[256]; int n = 0; while (fgets(l, sizeof l, f)) if (strstr(l, what)) n++; fclose(f); return n; }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *sc = argc > 1 ? argv[1] : "record";
    if (argc > 2 && chdir(argv[2]) != 0) return 2;
    mkdir("data", 0755);
    setenv("UA_JIT_WARM", "2", 1); setenv("UA_JIT_WARM_MS", "1000", 1);
    if (!strcmp(sc, "warm") || !strcmp(sc, "stale"))
        put("# mvid Assembly-CSharp AAAA-1111/10/100\nAssembly-CSharp 06000003 C1:M3 ()\nAssembly-CSharp 06000007 C1:M7 ()\n"
            "# mvid Other BBBB-2222/10/100\nOther 06000005 C2:M5 ()\n");
    if (!strcmp(sc, "stale")) g_guid[1] = "AAAA-9999";            // Assembly-CSharp built again
    if (!strcmp(sc, "patched")) {                                // the same id, a method more (an assembly patch)
        put("# mvid Assembly-CSharp AAAA-1111/10/100\nAssembly-CSharp 06000003 C1:M3 ()\n");
        g_methods[1] = 101;
    }
    if (!strcmp(sc, "late") || !strcmp(sc, "latestale")) {   // Other loaded only after the list is read
        put("# mvid Assembly-CSharp AAAA-1111/10/100\nAssembly-CSharp 06000003 C1:M3 ()\n# mvid Other BBBB-2222/10/100\nOther 06000005 C2:M5 ()\n");
        g_other_loaded = 0;
    }
    if (!strcmp(sc, "legacy")) put("Assembly-CSharp 06000003 C1:M3 ()\nOther 06000005 C2:M5 ()\n");
    if (!strcmp(sc, "badtoken"))
        put("# mvid Assembly-CSharp AAAA-1111/10/100\nAssembly-CSharp 06009dab C1:M40363 ()\nAssembly-CSharp 02000004 C1:T4 ()\n"
            "Assembly-CSharp 06000004 C1:M4 ()\n");
    jitwarm_init();
    frames(1);                                                   // install: the list read
    if (!strcmp(sc, "late")) g_other_loaded = 1;
    if (!strcmp(sc, "latestale")) { g_other_loaded = 1; g_guid[2] = "BBBB-3333"; }
    frames(70);                                                  // from frame 60: compiled ahead
    if (!strcmp(sc, "record")) {
        g_done(NULL, (void *)1003L, 0); g_done(NULL, (void *)1004L, 0); g_done(NULL, (void *)2009L, 0); g_done(NULL, (void *)1003L, 0);
        CHECK(count("# mvid Assembly-CSharp AAAA-1111/10/100") == 1 && count("# mvid Other BBBB-2222/10/100") == 1, "one mvid line each");
        CHECK(count("Assembly-CSharp 06000003") == 1 && count("Assembly-CSharp 06000004") == 1 && count("Other 06000009") == 1, "listed once each");
    } else if (!strcmp(sc, "warm")) {
        CHECK(g_compiled[1][3] && g_compiled[1][7] && g_compiled[2][5], "the listed methods compiled ahead");
    } else if (!strcmp(sc, "stale")) {
        CHECK(!g_compiled[1][3] && !g_compiled[1][7] && g_compiled[2][5], "only the unchanged assembly's compiled");
        CHECK(count("Assembly-CSharp 0600") == 0 && count("Other 06000005") == 1 && count("# mvid Other BBBB-2222/10/100") == 1, "the file written again without them");
        g_done(NULL, (void *)1008L, 0);
        CHECK(count("# mvid Assembly-CSharp AAAA-9999/10/100") == 1 && count("Assembly-CSharp 06000008") == 1, "new entries under the new id");
    } else if (!strcmp(sc, "late")) {
        CHECK(g_compiled[1][3] && g_compiled[2][5], "an assembly loaded after the list was read: its entries kept and compiled");
    } else if (!strcmp(sc, "latestale")) {
        CHECK(g_compiled[1][3] && !g_compiled[2][5] && g_lookups == 1, "an assembly loaded later with another id: its entries skipped (%d lookups)", g_lookups);
    } else if (!strcmp(sc, "patched")) {
        CHECK(g_lookups == 0 && !g_compiled[1][3], "a patched assembly's entries dropped");
    } else if (!strcmp(sc, "legacy")) {
        CHECK(g_lookups == 0 && !g_compiled[1][3] && !g_compiled[2][5], "a list without ids dropped");
    } else if (!strcmp(sc, "badtoken")) {
        CHECK(g_bad_lookups == 0, "a bad token never looked up (%d)", g_bad_lookups);
        CHECK(g_compiled[1][4], "the good entry after them compiled");
    } else { printf("unknown scenario %s\n", sc); return 2; }
    printf("%s %s\n", fails ? "FAIL" : "PASS", sc);
    return fails != 0;
}
