// gcbudget.c's UA_GC_GROW (runtime/host/test/plugins_test.sh) on a stand-in for Mono 2.6's collector calls:
//   grow       the heap grows inside a long frame (no frame for 0.7 s, a load) with less available than the setting
//              (UA_GC_GROW far above any MemAvailable): one full collection, attached to Mono and detached again
//   playing    the same growth while frames come every 10 ms: no collection
//   collected  the heap grew right after a collection of Mono's own (its count moved): no collection
//   plenty     more available than the setting (UA_GC_GROW=1): no collection
//   off        no UA_GC_GROW: no thread, nothing asked of Mono
#include "../gcbudget.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile int64_t g_heap = 64LL << 20;
static volatile int g_count, g_collects, g_attached, g_detached, g_max_gen_asked, g_lookups;
static int64_t heap(void) { return g_heap; }
static int64_t used(void) { return g_heap - (10LL << 20); }
static int count(int gen) { (void)gen; return g_count; }
static void collect(int gen) { if (gen == 0) g_max_gen_asked = 1; g_collects++; g_count++; }
static int max_gen(void) { return 0; }
static void *root(void) { return (void *)0x10; }
static void *attach(void *d) { (void)d; g_attached++; return (void *)0x20; }
static void detach(void *t) { (void)t; g_detached++; }
void *mono_runtime_lib(void) { return (void *)1; }
void *so_dlopen(const char *name, int flags) { (void)flags; return !strcmp(name, "libmono.so") ? (void *)1 : NULL; }
void *so_dlsym(void *h, const char *n)
{
    (void)h; g_lookups++;
    static const struct { const char *n; void *f; } t[] = {
        { "mono_gc_get_heap_size", (void *)heap }, { "mono_gc_get_used_size", (void *)used },
        { "mono_gc_collection_count", (void *)count }, { "mono_gc_collect", (void *)collect },
        { "mono_gc_max_generation", (void *)max_gen }, { "mono_get_root_domain", (void *)root },
        { "mono_thread_attach", (void *)attach }, { "mono_thread_detach", (void *)detach } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(t[i].n, n)) return t[i].f;
    return NULL;
}
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *sc = argc > 1 ? argv[1] : "grow";
    if (!strcmp(sc, "plenty")) setenv("UA_GC_GROW", "1", 1);
    else if (strcmp(sc, "off")) setenv("UA_GC_GROW", "100000000", 1);
    gcbudget_init();
    gcbudget_frame(NULL);                       // the first frame: the thread starts
    if (!strcmp(sc, "playing")) {
        for (int i = 0; i < 100; i++) { if (i == 50) g_heap += 8LL << 20; gcbudget_frame(NULL); usleep(10000); }
        usleep(100000);
        CHECK(g_collects == 0, "playing: %d collections", g_collects);
    } else {
        usleep(700000);                         // a long frame (a load)
        if (!strcmp(sc, "collected")) g_count++;  // Mono's own collection first, then the growth
        g_heap += 8LL << 20;
        usleep(300000);
        int want = !strcmp(sc, "grow") ? 1 : 0;
        CHECK(g_collects == want, "%s: %d collections, want %d", sc, g_collects, want);
        if (want) {
            CHECK(g_max_gen_asked, "grow: not a full collection");
            CHECK(g_attached == 1 && g_detached == 1, "grow: attached %d, detached %d", g_attached, g_detached);
        }
        if (!strcmp(sc, "off")) CHECK(g_lookups == 0, "off: %d lookups", g_lookups);
    }
    printf("gcgrow %s: %s\n", sc, fails ? "FAILED" : "ok");
    return fails != 0;
}
