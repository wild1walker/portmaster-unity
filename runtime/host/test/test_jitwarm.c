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
//   cctor    the static constructor rule: a method whose class, or a class whose static fields its IL names, has a
//            static constructor that has not run waits (until the probe says it ran) and is then compiled ahead;
//            a body with an opcode outside the standard is never compiled ahead, nor a static constructor itself
//   noprobe  no probe for the runtime: only methods touching no class with a static constructor
//   nodefer  UA_JIT_WARM_DEFER=0: every listed method compiled at once (static constructors still not)
//   writer   the list's lines queued for the writer thread through a queue of 64 bytes (the compiling thread
//            waits for room again and again): after jitwarm_flush the file holds every line, in order, the same
//            bytes as the old write from the compiling thread
//   writer0  UA_JIT_WARM_WRITER=0: the same file, written by the compiling thread
//   inline   a runtime whose inliner runs an AggressiveInlining callee's class constructor (JITWARM_INLINE_CCTORS):
//            a call to such a callee of a class not initialised waits too, a plain callee's does not
//   seed     the list and the setup's seed share one array: 5,000 entries of data/jitwarm.txt, then 15,000 of
//            data/jitwarm-seed.txt (Enter the Gungeon's sizes: until 2026-11-22 the seed's entries were written
//            past the array's end, a SIGSEGV at every launch after the first): all read, the first and the last of
//            each compiled ahead
//   torn     the list's last line cut short (a power cut while it was written): not an entry, the whole lines
//            before it compiled ahead, the file written again without it, a new entry on a line of its own
//   garbage  a damaged list (binary bytes, NULs, a line of 5,000 bytes, a token that is not hex, an empty line, a
//            cut "# mvid" line): none of it is an entry or asked of Mono, the whole entries between are compiled
//            ahead, the file is written again with those only
//   garbageseed the same bytes as the setup's seed: left out, nothing crashes, the list's own entries compiled
//   missing  a list naming a method that is not there any more: a token Mono does not find, a token whose method
//            has another name: neither compiled, the entries after them are
#include "../jitwarm.h"
#include <stdint.h>
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
static int g_compiled[3][101], g_lookups, g_bad_lookups, g_gone = -1;
typedef void (*done_cb)(void *, void *, int);
static done_cb g_done;
static void l_install(void *p, void *s) { (void)p; (void)s; }
static void l_jit(void *b, void *e) { (void)b; g_done = (done_cb)e; }
static void l_events(int e) { (void)e; }
static char *full_name(void *m, int sig)
{
    (void)sig; char *b = malloc(32); long v = (long)m;
    if (v % 1000 == 37) snprintf(b, 32, "C%ld:.cctor ()", v / 1000); else snprintf(b, 32, "C%ld:M%ld ()", v / 1000, v % 1000);
    return b;
}
// a class: image * 100 + row / 10 (the methods of rows 0-9 are class 100's, 10-19 class 101's, ...)
static void *get_class(void *m) { long v = (long)m; return (void *)(v / 1000 * 100 + v % 1000 / 10); }
static void *class_image(void *k) { return (void *)((long)k / 100); }
// the static constructor rule's Mono: classes 101 - 105 of image 1 have static constructors, 103 and 105 ran
static int g_has_cctor[300], g_ran[300], g_headers, g_freed;
static void *class_method(void *k, const char *name, int n) { (void)n; return !strcmp(name, ".cctor") && g_has_cctor[(long)k] ? k : NULL; }
static int probe(void *k) { return g_ran[(long)k]; }
// IL bodies by row (image 1); a field token's row is its class (0x04000000 | class), a call token a method's
#define LDSFLD(k) 0x7e, (k) & 0xff, 0, 0, 4
#define STSFLD(k) 0x80, (k) & 0xff, 0, 0, 4
static const unsigned char il3[] = { LDSFLD(102), 0x26, 0x2a };                       // reads class 102's static
static const unsigned char il4[] = { 0x1b, 0x26, 0x2a };                               // ldc.i4.5 pop ret
static const unsigned char il5[] = { 0x45, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,        // switch (2 targets)
                                     LDSFLD(105), STSFLD(104), 0x23, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f, 0x26,   // ldc.r8 1.0, pop
                                     0x38, 0, 0, 0, 0, 0xfe, 0x0c, 0, 0, 0x26, 0x2a };  // br, ldloc 0 (0xfe 0x0c), pop, ret
static const unsigned char ilret[] = { 0x2a };
static const unsigned char il35[] = { LDSFLD(105), 0x26, 0x2a };
static const unsigned char il45[] = { 0xf0, 0x00, 0x2a };                              // not an opcode of the standard
static const unsigned char il6[] = { 0x28, 26, 0, 0, 6, 0x2a };                        // call M26 (AggressiveInlining, class 102)
static const unsigned char il7[] = { 0x28, 16, 0, 0, 6, 0x2a };                        // call M16 (plain, class 101)
struct hdr { const unsigned char *il; uint32_t n; };
static struct hdr H[100];
static void *get_header(void *m) { long v = (long)m; if (v / 1000 != 1 || !H[v % 1000].il) return NULL; g_headers++; return &H[v % 1000]; }
static const unsigned char *header_code(void *h, uint32_t *n, uint32_t *ms) { *n = ((struct hdr *)h)->n; *ms = 8; return ((struct hdr *)h)->il; }
static void free_mh(void *h) { (void)h; g_freed++; }
static void *field_from_token(void *img, uint32_t tok, void **k, void *ctx) { (void)ctx; *k = NULL; return (tok >> 24) == 4 ? (void *)((long)img * 100000 + (tok & 0xffffff)) : NULL; }
static void *field_parent(void *f) { return (void *)((long)f % 100000); }
static int method_flags(void *m, uint32_t *iflags) { *iflags = (long)m % 1000 == 26 ? 0x0100 : 0; return 0; }
static const char *method_name(void *m) { (void)m; return "M"; }
static void *class_vtable(void *d, void *k) { (void)d; return k; }
static void *domain_get(void) { return (void *)1; }
const char *so_module_at(const void *a, uintptr_t *b) { (void)a; (void)b; return NULL; }
static const char *image_name(void *img) { return (long)img == 1 ? "Assembly-CSharp" : "Other"; }
static int g_other_loaded = 1;
static void *image_loaded(const char *n) { return (void *)(!strcmp(n, "Assembly-CSharp") ? 1L : !strcmp(n, "Other") && g_other_loaded ? 2L : 0L); }
static unsigned token(void *m) { return 0x06000000u | (unsigned)((long)m % 1000); }
static void *get_method(void *img, unsigned tok, void *k)
{
    (void)k; g_lookups++;
    if ((tok & 0xffffff) == (unsigned)g_gone) return NULL;                         // (a method Mono does not find)
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
        { "mono_image_get_table_rows", (void *)table_rows }, { "mono_class_get_method_from_name", (void *)class_method },
        { "mono_method_get_header", (void *)get_header }, { "mono_method_header_get_code", (void *)header_code },
        { "mono_metadata_free_mh", (void *)free_mh }, { "mono_field_from_token", (void *)field_from_token },
        { "mono_field_get_parent", (void *)field_parent }, { "mono_method_get_flags", (void *)method_flags },
        { "mono_method_get_name", (void *)method_name }, { "mono_class_vtable", (void *)class_vtable },
        { "mono_domain_get", (void *)domain_get } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(t[i].n, n)) return t[i].f;
    return NULL;
}
static void frames(int n) { for (int i = 0; i < n; i++) jitwarm_frame(); }
static void put_bytes(const char *path, const char *b, size_t n) { FILE *f = fopen(path, "w"); fwrite(b, 1, n, f); fclose(f); }
static char *slurp(size_t *n) { jitwarm_flush(); FILE *f = fopen("data/jitwarm.txt", "r"); char *b = calloc(1, 1 << 20); *n = f ? fread(b, 1, (1 << 20) - 1, f) : 0; if (f) fclose(f); return b; }
// a damaged list: whole entries (M3, M7, M9) between lines that are none
static size_t garbage(char *b)
{
    size_t n = 0;
    n += sprintf(b + n, "# mvid Assembly-CSharp AAAA-1111/10/100\nAssembly-CSharp 06000003 C1:M3 ()\n");
    memcpy(b + n, "Assembly-CSharp 06000004 C1:\0M4 ()\n", 34); n += 34;                // a NUL in a line
    memcpy(b + n, "\x01\x02\xff\xfe\x00\x7f\x1b[0m\n", 11); n += 11;                   // binary bytes
    n += sprintf(b + n, "Assembly-CSharp 06000005 ");                                   // a line of 5,000 bytes
    memset(b + n, 'x', 5000); n += 5000; b[n++] = '\n';
    n += sprintf(b + n, "Assembly-CSharp zzzz C1:M6 ()\n\nAssembly-CSharp 06000008\n# mvid\nAssembly-CSharp 0600000800 C1:M8 ()\n");
    n += sprintf(b + n, "Assembly-CSharp 06000007 C1:M7 ()\n 06000002 C1:M2 ()\nAssembly-CSharp 06000009 C1:M9 ()\n");
    n += sprintf(b + n, "Assembly-CSharp 0600000a C1:M1");                              // and the last line cut short
    return n;
}
static void put(const char *text) { FILE *f = fopen("data/jitwarm.txt", "w"); fputs(text, f); fclose(f); }
static int count(const char *what) { jitwarm_flush(); FILE *f = fopen("data/jitwarm.txt", "r"); if (!f) return -1; char l[256]; int n = 0; while (fgets(l, sizeof l, f)) if (strstr(l, what)) n++; fclose(f); return n; }

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
    if (!strcmp(sc, "seed")) {
        // tokens past the 100 methods of the test's assemblies are skipped when the warm-up reaches them: the
        // entries still fill the array, and the first and last of each file are real methods
        FILE *f = fopen("data/jitwarm.txt", "w");
        fprintf(f, "# mvid Assembly-CSharp AAAA-1111/10/100\nAssembly-CSharp 06000003 C1:M3 ()\n");
        for (int i = 0; i < 4998; i++) fprintf(f, "Assembly-CSharp %08x C1:M%d ()\n", 0x06001000 + i, 0x1000 + i);
        fprintf(f, "Assembly-CSharp 06000007 C1:M7 ()\n");
        fclose(f);
        f = fopen("data/jitwarm-seed.txt", "w");
        fprintf(f, "# mvid Other BBBB-2222/10/100\nOther 06000005 *\n");
        for (int i = 0; i < 14998; i++) fprintf(f, "Other %08x *\n", 0x06001000 + i);
        fprintf(f, "Other 06000009 *\nAssembly-CSharp 06000003 *\n");               // (the last: the list's own already)
        fclose(f);
    }
    if (!strcmp(sc, "torn"))
        put("# mvid Assembly-CSharp AAAA-1111/10/100\nAssembly-CSharp 06000003 C1:M3 ()\nAssembly-CSharp 06000007 C1:M7 ()\n"
            "Assembly-CSharp 0600");
    if (!strcmp(sc, "garbage") || !strcmp(sc, "garbageseed")) {
        char *b = malloc(16384); size_t n = garbage(b);
        if (!strcmp(sc, "garbage")) put_bytes("data/jitwarm.txt", b, n);
        else { put_bytes("data/jitwarm-seed.txt", b, n); put("# mvid Assembly-CSharp AAAA-1111/10/100\nAssembly-CSharp 0600000b C1:M11 ()\n"); }
        free(b);
    }
    if (!strcmp(sc, "missing")) {
        put("# mvid Assembly-CSharp AAAA-1111/10/100\nAssembly-CSharp 06000003 C1:M3 ()\nAssembly-CSharp 06000004 C1:Renamed ()\n"
            "Assembly-CSharp 06000006 C1:M6 ()\nAssembly-CSharp 06000007 C1:M7 ()\n");
        g_gone = 6;
    }
    int rule = !strcmp(sc, "cctor") || !strcmp(sc, "noprobe") || !strcmp(sc, "nodefer") || !strcmp(sc, "inline");
    if (rule) {
        put("# mvid Assembly-CSharp AAAA-1111/10/100\nAssembly-CSharp 06000003 C1:M3 ()\nAssembly-CSharp 06000004 C1:M4 ()\n"
            "Assembly-CSharp 06000005 C1:M5 ()\nAssembly-CSharp 0600000f C1:M15 ()\nAssembly-CSharp 06000019 C1:M25 ()\n"
            "Assembly-CSharp 06000023 C1:M35 ()\nAssembly-CSharp 0600002d C1:M45 ()\nAssembly-CSharp 06000025 C1:.cctor ()\n"
            "Assembly-CSharp 06000006 C1:M6 ()\nAssembly-CSharp 06000007 C1:M7 ()\n");
        g_has_cctor[101] = g_has_cctor[102] = g_has_cctor[103] = g_has_cctor[105] = 1;   // (104: none)
        g_ran[103] = g_ran[105] = 1;
        H[3] = (struct hdr){ il3, sizeof il3 }; H[4] = (struct hdr){ il4, sizeof il4 }; H[5] = (struct hdr){ il5, sizeof il5 };
        H[15] = (struct hdr){ ilret, 1 }; H[25] = (struct hdr){ ilret, 1 }; H[35] = (struct hdr){ il35, sizeof il35 };
        H[45] = (struct hdr){ il45, sizeof il45 }; H[37] = (struct hdr){ ilret, 1 };
        H[6] = (struct hdr){ il6, sizeof il6 }; H[7] = (struct hdr){ il7, sizeof il7 };
        if (!strcmp(sc, "cctor") || !strcmp(sc, "nodefer")) jitwarm_set_probe(probe, 0);
        if (!strcmp(sc, "inline")) jitwarm_set_probe(probe, JITWARM_INLINE_CCTORS);
        if (!strcmp(sc, "nodefer")) setenv("UA_JIT_WARM_DEFER", "0", 1);
    }
    if (!strcmp(sc, "writer")) setenv("UA_JIT_WARM_QUEUE", "64", 1);
    if (!strcmp(sc, "writer0")) setenv("UA_JIT_WARM_WRITER", "0", 1);
    jitwarm_init();
    frames(1);                                                   // install: the list read
    if (!strcmp(sc, "late")) g_other_loaded = 1;
    if (!strcmp(sc, "latestale")) { g_other_loaded = 1; g_guid[2] = "BBBB-3333"; }
    frames(70);                                                  // from frame 60: compiled ahead
    if (!strcmp(sc, "writer") || !strcmp(sc, "writer0")) {
        char *want = malloc(65536); size_t wn = 0;
        wn += sprintf(want + wn, "# mvid Assembly-CSharp AAAA-1111/10/100\n");
        for (long r = 1; r < 100; r++) {
            if (r == 37) continue;
            if (r == 1) { g_done(NULL, (void *)(1000L + r), 0); wn += sprintf(want + wn, "Assembly-CSharp %08lx C1:M%ld ()\n", 0x06000000L + r, r);
                          g_done(NULL, (void *)(2000L + r), 0); wn += sprintf(want + wn, "# mvid Other BBBB-2222/10/100\nOther %08lx C2:M%ld ()\n", 0x06000000L + r, r); continue; }
            g_done(NULL, (void *)(1000L + r), 0); wn += sprintf(want + wn, "Assembly-CSharp %08lx C1:M%ld ()\n", 0x06000000L + r, r);
            g_done(NULL, (void *)(2000L + r), 0); wn += sprintf(want + wn, "Other %08lx C2:M%ld ()\n", 0x06000000L + r, r);
        }
        jitwarm_flush();
        FILE *f = fopen("data/jitwarm.txt", "r"); char *got = calloc(1, 65536); size_t gn = f ? fread(got, 1, 65535, f) : 0; if (f) fclose(f);
        CHECK(gn == wn && !memcmp(got, want, wn), "%s: the file holds every line in order, the same bytes (%zu of %zu bytes)", sc, gn, wn);
        free(want); free(got);
    } else if (!strcmp(sc, "record")) {
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
    } else if (!strcmp(sc, "seed")) {
        CHECK(g_compiled[1][3] == 1 && g_compiled[1][7] == 1, "the list's first and last entries compiled ahead once (%d %d)", g_compiled[1][3], g_compiled[1][7]);
        CHECK(g_compiled[2][5] == 1 && g_compiled[2][9] == 1, "the seed's first and last entries compiled ahead once (%d %d)", g_compiled[2][5], g_compiled[2][9]);
        CHECK(g_lookups == 4 && g_bad_lookups == 0, "the 19,996 entries between read and skipped, none asked of Mono (%d lookups, %d bad)", g_lookups, g_bad_lookups);
        CHECK(count("Assembly-CSharp 0600") == 5000 && count("Other") == 0, "the list's file as it was");
    } else if (!strcmp(sc, "torn")) {
        CHECK(g_compiled[1][3] && g_compiled[1][7] && g_lookups == 2 && g_bad_lookups == 0, "the whole lines compiled ahead, the cut one not an entry (%d lookups)", g_lookups);
        g_done(NULL, (void *)1008L, 0);
        size_t n; char *got = slurp(&n);
        const char *want = "# mvid Assembly-CSharp AAAA-1111/10/100\nAssembly-CSharp 06000003 C1:M3 ()\nAssembly-CSharp 06000007 C1:M7 ()\n"
                           "Assembly-CSharp 06000008 C1:M8 ()\n";
        CHECK(n == strlen(want) && !memcmp(got, want, n), "the file written again without the cut line, the new entry on its own line: %s", got);
        CHECK(access("data/jitwarm.txt.tmp", F_OK) != 0, "no .tmp left");
        free(got);
    } else if (!strcmp(sc, "garbage")) {
        int *c = g_compiled[1];
        CHECK(c[3] && c[7] && c[9] && g_lookups == 3 && g_bad_lookups == 0, "only the whole entries compiled ahead (%d lookups, %d bad)", g_lookups, g_bad_lookups);
        for (int i = 0; i <= 100; i++) CHECK(c[i] == (i == 3 || i == 7 || i == 9), "M%d compiled %d times", i, c[i]);
        size_t n; char *got = slurp(&n);
        const char *want = "# mvid Assembly-CSharp AAAA-1111/10/100\nAssembly-CSharp 06000003 C1:M3 ()\nAssembly-CSharp 06000007 C1:M7 ()\n"
                           "Assembly-CSharp 06000009 C1:M9 ()\n";
        CHECK(n == strlen(want) && !memcmp(got, want, n), "the file written again with the whole entries only (%zu bytes)", n);
        free(got);
    } else if (!strcmp(sc, "garbageseed")) {
        int *c = g_compiled[1];
        CHECK(c[11] && c[3] && c[7] && c[9] && g_lookups == 4 && g_bad_lookups == 0, "the list's entry and the seed's whole entries compiled, the rest left out (%d lookups)", g_lookups);
        CHECK(count("Assembly-CSharp 0600") == 1, "the list's file as it was");
    } else if (!strcmp(sc, "missing")) {
        int *c = g_compiled[1];
        CHECK(c[3] && c[7] && !c[4] && !c[6], "a method that is gone and a renamed one not compiled, the others compiled (%d %d %d %d)", c[3], c[7], c[4], c[6]);
    } else if (!strcmp(sc, "cctor")) {
        int *c = g_compiled[1];
        CHECK(c[4] && c[5] && c[35] && c[6] && c[7], "the methods touching no class whose static constructor is still to run compiled ahead");
        CHECK(!c[3] && !c[15] && !c[25], "those touching one (its own class, a static field's) waiting: %d %d %d", c[3], c[15], c[25]);
        CHECK(!c[45] && !c[37], "a body that cannot be read and a static constructor never compiled ahead");
        CHECK(g_headers == g_freed, "every header given back (%d of %d)", g_freed, g_headers);
        g_ran[102] = 1;                                          // the game runs class 102's static constructor
        frames(40);
        CHECK(c[3] && c[25] && !c[15], "after class 102's: its waiting methods compiled, class 101's not");
        g_ran[101] = 1;
        frames(40);
        CHECK(c[15] && !c[45] && !c[37], "after class 101's: its method compiled too, the unreadable one and the .cctor never");
        for (int i = 0; i < 100; i++) CHECK(c[i] <= 1, "M%d compiled %d times", i, c[i]);
    } else if (!strcmp(sc, "noprobe")) {
        int *c = g_compiled[1];
        CHECK(c[4] && c[7] && !c[3] && !c[5] && !c[15] && !c[25] && !c[35] && !c[45] && !c[37],
              "no probe: only the methods touching no class with a static constructor (%d %d %d %d %d)", c[4], c[7], c[3], c[5], c[35]);
    } else if (!strcmp(sc, "nodefer")) {
        int *c = g_compiled[1];
        CHECK(c[3] && c[15] && c[25] && c[45] && c[6] && !c[37], "UA_JIT_WARM_DEFER=0: all at once but the static constructor");
    } else if (!strcmp(sc, "inline")) {
        int *c = g_compiled[1];
        CHECK(!c[6] && c[7], "a call to an AggressiveInlining callee of a class not initialised waits, a plain one does not");
        g_ran[102] = 1;
        frames(40);
        CHECK(c[6], "compiled once the callee's class constructor ran");
    } else { printf("unknown scenario %s\n", sc); return 2; }
    printf("%s %s\n", fails ? "FAIL" : "PASS", sc);
    return fails != 0;
}
