// Mono compiles a method the first time it runs, in that frame: on the RG34XX-SP's A53 about 1-2 ms
// a method, and a game's first slash, first hit or first death compiles tens to hundreds of them at
// once (Hollow Knight, King's Pass: 50-220 ms of 115-330 ms hitches; ~1 s of compiling in a 50 s
// walk, 80 % of it in methods that are not generic instances).
//   UA_JIT_WARM=1   (off by default; a port turns it on) the methods the game has had compiled are
//                   listed in data/jitwarm.txt beside the game ("<assembly> <token> <method>", as each
//                   first compiles), and at the next launch compiled ahead, from frame 60 on, in the
//                   order the game first needed them: on a thread of its own (UA_JIT_WARM_THREAD, below),
//                   or with UA_JIT_WARM_THREAD=0 on UnityMain between frames, at most UA_JIT_WARM_MS
//                   (default 4) ms a frame. After them, data/jitwarm-seed.txt, a port's setup's list made
//                   from the player's own assemblies (`pmu jitwarm-seed`: a first fight's code compiled
//                   ahead before the game ever ran it; "*" for a name: the token and the build name the
//                   method; entries of another build are left out). A method whose name is not the one listed (the
//                   game changed) is left out; generic instances are not listed (a token names their
//                   definition). Logged: "[jitwarm] <n> listed", "[jitwarm] <n> compiled ahead in
//                   <ms> ms (<n> already compiled, <n> not found)".
// Unity 2017.x's old Mono (2.6) has no jit-done callback of the new profiler API: there the methods are listed
// through its legacy profiler (mono_profiler_install_jit_compile, as UA_JIT_LOG reads them; Mono 2.6 has one
// profiler, so with UA_JIT_LOG on as well nothing new is listed). There it takes UA_JIT_WARM=2: the launchers'
// default 1 predates it, and a port turns it on.
// Each assembly's entries are tied to the assembly they were listed from: "# mvid <assembly> <id>" lines in the
// list say whose the entries after them are, the id being the module version id (Mono's mono_image_get_guid: a new
// build's) and the counts of its type and method tables (an assembly patcher such as Mono.Cecil keeps the module
// version id, but a patch that adds or removes methods moves their tokens). At start, the entries of an assembly whose loaded module version id is another (a
// port update, the setup run again, a patch) are dropped and the file rewritten without them; a list without such
// lines (an older ua_host's) is dropped whole. A token is also checked against the assembly's method table before
// Mono looks it up (a token past it would raise BadImageFormatException inside Mono's lookup): such an entry is
// skipped and logged. The list only ever holds what this game itself compiled on this handheld.
// What the game does is the same: the methods it would compile, compiled earlier on the same thread.
// Static constructors: compiling runs managed code of its own. Mono runs a class's static constructor
// (.cctor) when it compiles one of the class's methods, or is asked for one already compiled (Unity
// 2017.4's Mono 2.6: mono_jit_compile_method_inner ends with mono_runtime_class_init_full for the method's
// class, and the lookup of a compiled one calls it too; Unity 6's Mono the same), and while it compiles a body,
// those of the beforefieldinit classes whose static fields the body reads or writes (the static field address
// patch, run_cctors); Unity 6's Mono also runs the class constructor of a callee marked AggressiveInlining that
// it inlines. Run by the warm-up, a static constructor would run earlier than the game runs it (at its first
// use of the class), or in a session that never uses the class. So a listed method is compiled ahead only
// when compiling it can run none: its class, the class of every static field its IL names (ldsfld,
// ldsflda, stsfld) and, where the runtime inlines that way, of every AggressiveInlining method it calls,
// either has no .cctor or has run it already (the runtime's probe: Mono's own MonoVTable flag, see
// jitwarm_set_probe). Until then the method waits and is tried again every 30 frames, so it is compiled ahead
// once the game itself has used those classes; a static constructor itself is never compiled ahead, and a body
// the walk cannot read (an opcode it does not know) is left to the game. The warm-up thus runs no managed
// code: every static constructor runs where the game makes it run, as without the warm-up. Logged:
// "[jitwarm] ... <n> waiting for their classes' static constructors", then "[jitwarm] <n> more compiled
// ahead once their classes' static constructors had run". UA_JIT_WARM_CHECK=1 (a measurement, Mono 2.6's
// legacy profiler only) counts the static constructors that run inside a warm-up compile ("[jitwarm] check:
// ..."; 0 is the rule), =2 also logs every static constructor run with its frame; UA_JIT_WARM_DEFER=0 (a
// measurement) compiles every listed method at once again, as before 2026-11-13.
// The list's new lines are written by a thread of their own (ua_jitwrite): the compiling thread (UnityMain, for
// the game's code) formats each line and queues it in memory, the writer appends them to data/jitwarm.txt in
// the same order and flushes, so a slow card never stalls a frame for it (a write to the card can block for
// hundreds of ms while it is busy). The queue is bounded (UA_JIT_WARM_QUEUE bytes, 256 KB): when it is full the
// compiling thread waits for the writer, a line is never dropped. jitwarm_flush() waits until every queued line is
// in the file (ua_host calls it at its end, after SIGTERM or SIGINT too, and when Unity's shutdown crashes;
// a crash writes what is queued from the crash handler). The file has the same bytes in the same order as with
// the old write of each line from the compiling thread (UA_JIT_WARM_WRITER=0, kept as the fallback when the
// thread cannot be made).
// A list that was cut short or damaged cannot crash the next launch, nor lose more than the damaged lines. Each
// batch of whole lines goes to the file in one write() on a descriptor opened O_APPEND, never through a stdio
// buffer that would split a line over two writes, so a kill (the exit hotkey's SIGKILL, an out-of-memory kill)
// leaves whole lines. What a power cut or a full card can still leave is taken at the next read: a line with no
// newline at its end (the file's torn last line), with a NUL or another control byte in it, longer than 2047
// bytes, or not "<assembly> <1-8 hex digits> <name>", is not an entry; data/jitwarm.txt is then written again
// without such lines to data/jitwarm.txt.tmp, synced and renamed over the list (as when another build's entries
// are dropped), and if that fails a newline is put before the first new line so it never joins a torn one. Logged:
// "[jitwarm] <n> lines of data/jitwarm.txt were not entries (cut short or damaged): dropped". The same lines of
// the setup's seed are only left out. An entry that parses but names nothing here (a token past the method table,
// a method Mono does not find, another name) is skipped when the warm-up reaches it, as before.
// The two lists share one array: until 2026-11-22 the seed's read began with its own capacity count at zero, so
// with entries of data/jitwarm.txt already in the array it never grew it again and wrote the seed's entries past
// its end (Enter the Gungeon's 64-bit release candidate: 5,118 own entries in an array of 8,192, then 15,232
// seed entries: SIGSEGV in the list's read at every launch after the first; a port without a seed, or a first
// launch, never reached it).
#define _GNU_SOURCE
#include "jitwarm.h"
#include "hitches.h"
#include "../loader/so_loader.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#define LIST "data/jitwarm.txt"
// the setup's list (the port's setup writes it from the player's own assemblies; never shipped): read after LIST
#define SEED "data/jitwarm-seed.txt"

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
static const char *(*m_image_guid)(void *image);
static int (*m_table_rows)(void *image, int table);
#define TABLE_METHOD 6                       // (MONO_TABLE_METHOD: MethodDef, token type 0x06)

// the assemblies whose module version id the list's file has in force for its next entries (under g_lock)
#define MAXIMG 64
static struct { char image[64]; char guid[96]; } g_mvid[MAXIMG];
static int g_nmvid;
static long g_stale, g_badtok;
static const char *image_id(void *img, char *b, size_t n);

// the methods listed (from the file and since): assembly and token, hashed
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *g_seen;
static size_t g_seen_cap, g_seen_n;
static int g_out = -1;                      // the list, for its new lines (O_APPEND)

// the list's writer: lines queued in q (q_n bytes of q_cap), taken whole by the thread (ua_jitwrite) and appended
// to g_out; q_busy while it writes a batch (outside the lock), so a flush waits for that too
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_data = PTHREAD_COND_INITIALIZER, q_room = PTHREAD_COND_INITIALIZER;
static char *q, *q_spare;
static size_t q_n, q_cap;
static volatile int q_busy, q_on;

// the whole of b in the list's file (one write() for a batch of whole lines, see the top)
static void write_all(const char *b, size_t n)
{
    while (n) {
        ssize_t w = write(g_out, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        b += w; n -= (size_t)w;
    }
}

static void *write_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "ua_jitwrite");
    for (;;) {
        pthread_mutex_lock(&q_lock);
        while (!q_n) pthread_cond_wait(&q_data, &q_lock);
        char *b = q; size_t n = q_n;
        q = q_spare; q_spare = b; q_n = 0; q_busy = 1;
        pthread_mutex_unlock(&q_lock);
        write_all(b, n);
        pthread_mutex_lock(&q_lock);
        q_busy = 0;
        pthread_cond_broadcast(&q_room);
        pthread_mutex_unlock(&q_lock);
    }
    return NULL;
}

// one line of the list: queued for the writer (waiting while the queue is full), or written here without it
static void list_line(const char *line)
{
    size_t n = strlen(line);
    if (!q_on || n > q_cap) {
        if (q_on) jitwarm_flush();      // (a line longer than the queue: after what is queued, in order)
        write_all(line, n);
        return;
    }
    pthread_mutex_lock(&q_lock);
    while (q_n + n > q_cap) pthread_cond_wait(&q_room, &q_lock);
    memcpy(q + q_n, line, n);
    q_n += n;
    pthread_cond_signal(&q_data);
    pthread_mutex_unlock(&q_lock);
}

void jitwarm_flush(void)
{
    if (!q_on) return;
    pthread_mutex_lock(&q_lock);
    while (q_n || q_busy) pthread_cond_wait(&q_room, &q_lock);
    pthread_mutex_unlock(&q_lock);
}

void jitwarm_flush_crash(void)
{
    // from a signal handler: no waiting on a lock that a stopped thread may hold. Let a batch being written
    // finish (up to a second), then write what is queued straight to the file, after it.
    if (!q_on || g_out < 0) return;
    for (int i = 0; i < 1000 && q_busy; i++) usleep(1000);
    if (pthread_mutex_trylock(&q_lock) != 0) return;
    if (q_n && !q_busy) {
        write_all(q, q_n);
        q_n = 0;
    }
    pthread_mutex_unlock(&q_lock);
}

static void writer_start(void)
{
    const char *w = getenv("UA_JIT_WARM_WRITER");
    if (w && !strcmp(w, "0")) return;
    const char *qs = getenv("UA_JIT_WARM_QUEUE");
    long cap = qs && *qs ? atol(qs) : 256 * 1024;
    if (cap < 64) cap = 64;
    q_cap = (size_t)cap;
    q = malloc(q_cap); q_spare = malloc(q_cap);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (!q || !q_spare || pthread_create(&t, &a, write_thread, NULL)) {
        free(q); free(q_spare); q = q_spare = NULL;
        fprintf(stderr, "[jitwarm] no writer thread: the list's lines written by the compiling thread\n");
    } else q_on = 1;
    pthread_attr_destroy(&a);
}

// the warm-up's list, read at start
struct entry { char image[64]; unsigned token; char *name; short idx; };   // idx: its id in g_fid (-1: checked)
// the ids the file names for assemblies not loaded when it is read: checked when the warm-up reaches their entries
static struct { char image[64]; char guid[96]; } g_fid[MAXIMG];
static int g_nfid;
static struct entry *g_list;
static size_t g_list_n, g_list_cap, g_next;     // (g_list_cap: of both files' entries, the list's then the seed's)
static long g_torn;                              // lines of the list's file that were not entries
static long g_done, g_had, g_missing;
static double g_spent;

// static constructors (see the top): what the walk asks of Mono, the runtime's probe, the methods waiting
static void *(*m_class_method)(void *klass, const char *name, int nparams);    // mono_class_get_method_from_name
static void *(*m_get_header)(void *method);
static const unsigned char *(*m_header_code)(void *header, uint32_t *size, uint32_t *max_stack);
static void (*m_free_mh)(void *header);
static void *(*m_field_from_token)(void *image, uint32_t token, void **klass, void *context);
static void *(*m_field_parent)(void *field);
static int (*m_method_flags)(void *method, uint32_t *iflags);                  // mono_method_get_flags
static const char *(*m_method_name)(void *method);
void *(*jitwarm_class_vtable)(void *domain, void *klass);                      // for the probes
void *(*jitwarm_domain_get)(void);
static int (*g_probe)(void *klass);
static unsigned g_probe_flags;
static int g_defer = 1, g_check;
struct waiting { void *m, *k; };
static struct waiting *g_wait;
static size_t g_wait_n, g_wait_cap;
static size_t g_seed_n; static long g_seed_stale;
static int g_thread = 1;
static void *g_domain;
static void *(*m_thread_attach)(void *domain);
static volatile int g_started;

static long g_later, g_later_said, g_waited, g_unread, g_cctors_skipped, g_check_runs;
static __thread int t_compiling;                                                 // (UnityMain, in a warm-up compile)
static int g_reported;

void jitwarm_set_probe(int (*ran)(void *klass), unsigned flags) { g_probe = ran; g_probe_flags = flags; }

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

// ---------------------------------------------------------------- static constructors (see the top)
// the size of an IL instruction's operand (ECMA-335 partition III; op: the opcode, 0x1xx for 0xfe-prefixed
// ones), -1 for an opcode not in the standard
static int operand_size(unsigned op, const unsigned char *p, size_t left)
{
    if (op < 0x100) {
        if (op <= 0x0d || (op >= 0x14 && op <= 0x1e) || op == 0x25 || op == 0x26 || op == 0x2a || (op >= 0x46 && op <= 0x6e) ||
            op == 0x76 || op == 0x7a || (op >= 0x82 && op <= 0x8b) || op == 0x8e || (op >= 0x90 && op <= 0xa2) ||
            (op >= 0xb3 && op <= 0xba) || op == 0xc3 || (op >= 0xd1 && op <= 0xdc) || op == 0xdf || op == 0xe0) return 0;
        if ((op >= 0x0e && op <= 0x13) || op == 0x1f || (op >= 0x2b && op <= 0x37) || op == 0xde) return 1;
        if (op == 0x21 || op == 0x23) return 8;
        if (op == 0x45) {                                    // switch: a count, then that many targets
            if (left < 4) return -1;
            uint32_t n = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
            return n > (left - 4) / 4 ? -1 : 4 + 4 * (int)n;
        }
        if (op == 0x20 || op == 0x22 || (op >= 0x27 && op <= 0x29) || (op >= 0x38 && op <= 0x44) || (op >= 0x6f && op <= 0x75) ||
            op == 0x79 || (op >= 0x7b && op <= 0x81) || op == 0x8c || op == 0x8d || op == 0x8f || (op >= 0xa3 && op <= 0xa5) ||
            op == 0xc2 || op == 0xc6 || op == 0xd0 || op == 0xdd) return 4;
        return -1;
    }
    op &= 0xff;
    if (op <= 0x05 || op == 0x0f || op == 0x11 || op == 0x13 || op == 0x14 || op == 0x17 || op == 0x18 || op == 0x1a ||
        op == 0x1d || op == 0x1e) return 0;
    if (op == 0x12 || op == 0x19) return 1;
    if (op >= 0x09 && op <= 0x0e) return 2;
    if (op == 0x06 || op == 0x07 || op == 0x15 || op == 0x16 || op == 0x1c) return 4;
    return -1;
}

// compiling against the class can run no static constructor: it has none, or the runtime's probe says it ran.
// A class once found so stays so (a static constructor runs once): those are kept in a set (UnityMain only)
static void **g_okset;
static size_t g_okset_n, g_okset_cap;
static int ok_known(void *k)
{
    if (!g_okset_cap) return 0;
    size_t j = ((uintptr_t)k >> 3) * 2654435761u & (g_okset_cap - 1);
    while (g_okset[j]) { if (g_okset[j] == k) return 1; j = (j + 1) & (g_okset_cap - 1); }
    return 0;
}
static void ok_add(void *k)
{
    if ((g_okset_n + 1) * 2 > g_okset_cap) {
        size_t cap = g_okset_cap ? g_okset_cap * 2 : 1024;
        void **t = calloc(cap, sizeof *t);
        if (!t) return;
        for (size_t i = 0; i < g_okset_cap; i++)
            if (g_okset[i]) { size_t j = ((uintptr_t)g_okset[i] >> 3) * 2654435761u & (cap - 1); while (t[j]) j = (j + 1) & (cap - 1); t[j] = g_okset[i]; }
        free(g_okset); g_okset = t; g_okset_cap = cap;
    }
    size_t j = ((uintptr_t)k >> 3) * 2654435761u & (g_okset_cap - 1);
    while (g_okset[j]) j = (j + 1) & (g_okset_cap - 1);
    g_okset[j] = k; g_okset_n++;
}
static int class_ok(void *k)
{
    if (!k) return 0;
    if (ok_known(k)) return 1;
    if (m_class_method(k, ".cctor", 0) && !(g_probe && g_probe(k) == 1)) return 0;
    ok_add(k);
    return 1;
}

// whether compiling the method can run no static constructor (1), can run one that has not run yet (0, *wait:
// its class), or its body cannot be read (-1)
static int warm_ok(void *m, void **wait)
{
    void *k = m_get_class(m);
    if (!class_ok(k)) { *wait = k; return 0; }
    void *h = m_get_header(m);
    if (!h) return 1;                                        // no IL (an internal call, abstract): only its class
    uint32_t size = 0, maxs = 0;
    const unsigned char *il = m_header_code(h, &size, &maxs);
    void *img = m_class_image(k);
    int r = il ? 1 : -1;
    for (size_t i = 0; il && i < size && r == 1; ) {
        unsigned op = il[i++];
        if (op == 0xfe) { if (i >= size) { r = -1; break; } op = 0x100 | il[i++]; }
        int n = operand_size(op, il + i, size - i);
        if (n < 0 || i + (size_t)n > size) { r = -1; break; }
        int fld = op == 0x7e || op == 0x7f || op == 0x80;   // ldsfld, ldsflda, stsfld
        int call = op == 0x28 || op == 0x6f || op == 0x73;  // call, callvirt, newobj
        if (fld || (call && (g_probe_flags & JITWARM_INLINE_CCTORS))) {
            uint32_t tok = il[i] | il[i + 1] << 8 | il[i + 2] << 16 | (uint32_t)il[i + 3] << 24;
            void *c = NULL;
            if (fld) {
                void *fk = NULL, *f = m_field_from_token(img, tok, &fk, NULL);
                c = f ? m_field_parent(f) : NULL;
                if (!c) r = -1;
            } else {
                uint32_t iflags = 0;
                void *t = m_get_method(img, tok, NULL);
                if (!t) r = -1;
                else if ((m_method_flags(t, &iflags), iflags & 0x0100)) c = m_get_class(t);   // AggressiveInlining
            }
            if (c && !class_ok(c)) { *wait = c; r = 0; }
        }
        i += (size_t)n;
    }
    if (m_free_mh) m_free_mh(h);
    return r;
}

static void wait_add(void *m, void *k)
{
    if (g_wait_n == g_wait_cap) {
        size_t cap = g_wait_cap ? g_wait_cap * 2 : 1024;
        struct waiting *w = realloc(g_wait, cap * sizeof *w);
        if (!w) return;                                      // (not compiled ahead: as if not listed)
        g_wait = w; g_wait_cap = cap;
    }
    g_wait[g_wait_n].m = m; g_wait[g_wait_n].k = k; g_wait_n++;
    g_waited++;
}

// the warm-up's compile (the check's counter knows it is one)
static void compile_ahead(void *m)
{
    t_compiling = 1;
    m_compile(m);
    t_compiling = 0;
}

#if defined(__arm__)
// Unity 2017.4's armeabi-v7a libmono (Mono 2.6): mono_runtime_class_init_full returns at once when bit 1 of the
// vtable's byte 0x1b (MonoVTable.initialized) is set, the first thing it tests; its bytes up to there (FNV-1a,
// as monoinvoke.c checks this build) and mono_runtime_class_init's place are checked before the probe reads it
static int ran_2017_4(void *klass)
{
    void *vt = jitwarm_class_vtable(jitwarm_domain_get(), klass);   // (made if not yet: no managed code runs)
    return vt && (((const unsigned char *)vt)[0x1b] & 2);
}
static int probe_2017_4(void *h)
{
    void *ci = so_dlsym(h, "mono_runtime_class_init");
    uintptr_t bias = 0;
    if (!ci || !so_module_at(ci, &bias) || (uintptr_t)ci != bias + 0x1f0784) return 0;
    const unsigned char *p = (const unsigned char *)bias + 0x1f07a8;
    uint32_t x = 0x811c9dc5u;
    for (int i = 0; i < 0x3c; i++) x = (x ^ p[i]) * 0x01000193u;
    return x == 0x6c9a9ab2u;
}
#endif

// UA_JIT_WARM_CHECK (Mono 2.6's legacy profiler): the static constructors run, those inside a warm-up compile counted
static void on_invoke(void *prof, void *method)
{
    (void)prof;
    const char *n = m_method_name(method);
    if (!n || strcmp(n, ".cctor")) return;
    char *f = m_full_name(method, 0);
    if (t_compiling) {
        g_check_runs++;
        fprintf(stderr, "[jitwarm] check: a static constructor ran inside a warm-up compile: %s\n", f ? f : "?");
    } else if (g_check > 1) fprintf(stderr, "[jitwarm] cctor f%ld %s\n", g_frame, f ? f : "?");
    if (f && m_free) m_free(f);
}
static void on_invoke_end(void *prof, void *method) { (void)prof; (void)method; }

static void on_begin(void *prof, void *method) { (void)prof; (void)method; }   // (Mono 2.6's legacy API calls both)

static void on_done(void *prof, void *method, void *jinfo)
{
    (void)prof; (void)jinfo;
    unsigned tok = m_token(method);
    if (!tok || g_out < 0) return;
    void *img = m_class_image(m_get_class(method));
    const char *in = img ? m_image_name(img) : NULL;
    if (!in || strlen(in) >= sizeof g_list[0].image || strchr(in, ' ')) return;
    char *n = m_full_name(method, 1);
    if (!n) return;
    if (!generic_instance(n) && !strstr(n, "(wrapper") && !strstr(n, ":.cctor (")) {   // (a .cctor runs once: nothing to warm)
        char gb[96];
        const char *guid = image_id(img, gb, sizeof gb);
        pthread_mutex_lock(&g_lock);
        if (guid && *guid && strlen(guid) < sizeof g_mvid[0].guid && seen_add(key(in, tok))) {
            int k = 0;
            while (k < g_nmvid && strcmp(g_mvid[k].image, in)) k++;
            if (k == g_nmvid || strcmp(g_mvid[k].guid, guid)) {   // whose the entries after this line are
                if (k == g_nmvid && g_nmvid < MAXIMG) g_nmvid++;
                if (k < MAXIMG) { snprintf(g_mvid[k].image, sizeof g_mvid[k].image, "%s", in); snprintf(g_mvid[k].guid, sizeof g_mvid[k].guid, "%s", guid); }
                char ml[256];
                snprintf(ml, sizeof ml, "# mvid %s %s\n", in, guid);
                list_line(ml);
            }
            size_t ll = strlen(in) + strlen(n) + 16;
            char lb[512], *l = ll <= sizeof lb ? lb : malloc(ll);
            if (l) { snprintf(l, ll <= sizeof lb ? sizeof lb : ll, "%s %08x %s\n", in, tok, n); list_line(l); if (l != lb) free(l); }
        }
        pthread_mutex_unlock(&g_lock);
    }
    if (m_free) m_free(n);
}

void jitwarm_init(void)
{
    const char *e = getenv("UA_JIT_WARM");
    g_on = e && *e && *e != '0' ? (atoi(e) >= 2 ? 2 : 1) : 0;
    if ((e = getenv("UA_JIT_WARM_MS")) && atof(e) >= 0) g_budget = atof(e) / 1000;
    if ((e = getenv("UA_JIT_WARM_DEFER")) && !strcmp(e, "0")) g_defer = 0;
    if ((e = getenv("UA_JIT_WARM_CHECK")) && *e && *e != '0') g_check = atoi(e) >= 2 ? 2 : 1;
    // (the thread is the default where it was measured: Unity 2017.4's Mono 2.6, UA_JIT_WARM=2; Unity 6's Mono keeps
    // UnityMain unless UA_JIT_WARM_THREAD=1)
    if ((e = getenv("UA_JIT_WARM_THREAD")) && *e) g_thread = strcmp(e, "0") != 0;
    else g_thread = g_on == 2;
}

// an assembly's id for the list: its module version id and its type and method counts ("" when Mono cannot say)
static const char *image_id(void *img, char *b, size_t n)
{
    const char *g = img && m_image_guid ? m_image_guid(img) : NULL;
    if (!g || !*g || strchr(g, ' ')) return "";
    snprintf(b, n, "%s/%d/%d", g, m_table_rows(img, 2), m_table_rows(img, TABLE_METHOD));   // (2: TypeDef)
    return b;
}
static const char *loaded_guid(const char *image)
{
    static char b[96];
    return image_id(m_image_loaded(image), b, sizeof b);
}

// a line of a list's file as an entry: "<assembly> <token, 1-8 hex digits> <name>"; 0 when it is not one
static int parse_entry(char *line, char *image, size_t image_n, unsigned *tok, char **name)
{
    char *sp = strchr(line, ' ');
    size_t il = sp ? (size_t)(sp - line) : 0;
    if (!il || il >= image_n || line[0] == '#') return 0;
    char *t = sp + 1, *e = t;
    while (isxdigit((unsigned char)*e)) e++;
    if (e == t || e - t > 8 || *e != ' ' || !e[1]) return 0;
    memcpy(image, line, il); image[il] = 0;
    *tok = (unsigned)strtoul(t, NULL, 16);
    *name = e + 1;
    return 1;
}

static void read_list(const char *path)
{
    int own = !strcmp(path, LIST);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char *line = NULL; size_t line_cap = 0; ssize_t len;
    long bad = 0;
    // the module version id each assembly's entries were listed under, as the file goes (a file's later
    // lines can name a newer build)
    struct { char image[64]; char guid[96]; int ok; } cur[MAXIMG];
    int ncur = 0;
    while ((len = getline(&line, &line_cap, f)) > 0) {
        char image[64], guid[96]; unsigned tok = 0; char *name = NULL;
        // a whole line only: its newline there (the file's last line cut short has none), no NUL or other control
        // byte in it, not longer than a list's line gets
        int whole = line[len - 1] == '\n' && len < 2048;
        for (ssize_t i = 0; whole && i < len - 1; i++) if ((unsigned char)line[i] < 0x20 && line[i] != '\t') whole = 0;
        if (!whole) { bad++; continue; }
        line[len - 1] = 0;
        if (sscanf(line, "# mvid %63s %95s", image, guid) == 2) {
            int k = 0;
            while (k < ncur && strcmp(cur[k].image, image)) k++;
            if (k == ncur) { if (ncur == MAXIMG) continue; ncur++; }
            snprintf(cur[k].image, sizeof cur[k].image, "%s", image);
            snprintf(cur[k].guid, sizeof cur[k].guid, "%s", guid);
            // an assembly loaded later (on first use): its entries kept, their id checked when the warm-up reaches them
            cur[k].ok = m_image_loaded(image) ? (strcmp(guid, loaded_guid(image)) ? 0 : 1) : 2;
            continue;
        }
        if (line[0] == '#' && strncmp(line, "# mvid", 6)) continue;      // (a comment)
        if (!parse_entry(line, image, sizeof image, &tok, &name)) { bad++; continue; }
        int k = 0;
        while (k < ncur && strcmp(cur[k].image, image)) k++;
        if (k == ncur || !cur[k].ok) { g_stale++; continue; }     // another build's, or no module version id
        short idx = -1;
        if (cur[k].ok == 2) {
            int j = 0;
            while (j < g_nfid && (strcmp(g_fid[j].image, image) || strcmp(g_fid[j].guid, cur[k].guid))) j++;
            if (j == g_nfid) { if (g_nfid == MAXIMG) continue; memcpy(g_fid[j].image, cur[k].image, sizeof g_fid[j].image); memcpy(g_fid[j].guid, cur[k].guid, sizeof g_fid[j].guid); g_nfid++; }
            idx = (short)j;
        }
        if (!seen_add(key(image, tok))) continue;
        if (g_list_n >= g_list_cap) {
            size_t cap = g_list_cap ? g_list_cap * 2 : 4096;
            struct entry *l = realloc(g_list, cap * sizeof *l);
            if (!l) break;
            g_list = l; g_list_cap = cap;
        }
        char *nm = strdup(name);
        if (!nm) break;
        struct entry *x = &g_list[g_list_n++];
        snprintf(x->image, sizeof x->image, "%s", image);
        x->token = tok;
        x->name = nm;
        x->idx = idx;
    }
    free(line);
    fclose(f);
    if (!own) {                               // (the setup's seed list: entries of another build are only left out)
        if (bad) fprintf(stderr, "[jitwarm] %ld lines of %s were not entries (cut short or damaged): left out\n", bad, path);
        return;
    }
    g_torn = bad;
    if (!g_stale && !g_torn) {
        // the file's assemblies, as they stand: new entries of these follow on without a new line
        for (int k = 0; k < ncur && g_nmvid < MAXIMG; k++)
            if (cur[k].ok) { memcpy(g_mvid[g_nmvid].image, cur[k].image, sizeof g_mvid[0].image);
                             memcpy(g_mvid[g_nmvid].guid, cur[k].guid, sizeof g_mvid[0].guid); g_nmvid++; }
        return;
    }
    // some lines were not entries (cut short or damaged), or some entries were another build's: the file written
    // again with the kept ones only (and their assemblies' module version ids), so the next launch does not read
    // them again
    if (g_torn) fprintf(stderr, "[jitwarm] %ld lines of %s were not entries (cut short or damaged): dropped\n", g_torn, LIST);
    if (g_stale) fprintf(stderr, "[jitwarm] %ld listed methods were another build's (an assembly changed since): dropped\n", g_stale);
    FILE *o = fopen(LIST ".tmp", "w");
    if (!o) return;
    for (size_t i = 0; i < g_list_n; i++) {
        struct entry *x = &g_list[i];
        int k = 0;
        while (k < g_nmvid && strcmp(g_mvid[k].image, x->image)) k++;
        if (k == g_nmvid && g_nmvid < MAXIMG) {
            snprintf(g_mvid[k].image, sizeof g_mvid[k].image, "%s", x->image);
            snprintf(g_mvid[k].guid, sizeof g_mvid[k].guid, "%s", x->idx >= 0 ? g_fid[x->idx].guid : loaded_guid(x->image));
            g_nmvid++;
            fprintf(o, "# mvid %s %s\n", x->image, g_mvid[k].guid);
        }
        fprintf(o, "%s %08x %s\n", x->image, x->token, x->name);
    }
    // (synced before the rename: the list is either the old file or the whole new one, after a power cut too)
    int ok = fflush(o) == 0 && !ferror(o) && fsync(fileno(o)) == 0;
    if (fclose(o) != 0) ok = 0;
    if (!ok || rename(LIST ".tmp", LIST) != 0) remove(LIST ".tmp");
}

// the list opened for its new lines: appended (O_APPEND), after a newline when its last line has none
static void open_out(void)
{
    g_out = open(LIST, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0666);
    if (g_out < 0) return;
    off_t end = lseek(g_out, 0, SEEK_END);
    int r = end > 0 ? open(LIST, O_RDONLY | O_CLOEXEC) : -1;
    if (r >= 0) {
        char c = '\n';
        if (pread(r, &c, 1, end - 1) == 1 && c != '\n') write_all("\n", 1);
        close(r);
    }
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
        m_image_guid = (const char *(*)(void *))so_dlsym(h, "mono_image_get_guid");
        m_table_rows = (int (*)(void *, int))so_dlsym(h, "mono_image_get_table_rows");
        m_class_method = (void *(*)(void *, const char *, int))so_dlsym(h, "mono_class_get_method_from_name");
        m_get_header = (void *(*)(void *))so_dlsym(h, "mono_method_get_header");
        m_header_code = (const unsigned char *(*)(void *, uint32_t *, uint32_t *))so_dlsym(h, "mono_method_header_get_code");
        m_free_mh = (void (*)(void *))so_dlsym(h, "mono_metadata_free_mh");
        m_field_from_token = (void *(*)(void *, uint32_t, void **, void *))so_dlsym(h, "mono_field_from_token");
        m_field_parent = (void *(*)(void *))so_dlsym(h, "mono_field_get_parent");
        m_method_flags = (int (*)(void *, uint32_t *))so_dlsym(h, "mono_method_get_flags");
        m_method_name = (const char *(*)(void *))so_dlsym(h, "mono_method_get_name");
        jitwarm_class_vtable = (void *(*)(void *, void *))so_dlsym(h, "mono_class_vtable");
        jitwarm_domain_get = (void *(*)(void))so_dlsym(h, "mono_domain_get");
        m_thread_attach = (void *(*)(void *))so_dlsym(h, "mono_thread_attach");
    }
    // Unity 2017.x's Mono 2.6: the legacy profiler's JIT events (its end callback's third argument is
    // the result, which on_done does not read)
    void (*l_install)(void *, void *) = h ? (void (*)(void *, void *))so_dlsym(h, "mono_profiler_install") : NULL;
    void (*l_jit)(void *, void *) = h ? (void (*)(void *, void *))so_dlsym(h, "mono_profiler_install_jit_compile") : NULL;
    void (*l_events)(int) = h ? (void (*)(int))so_dlsym(h, "mono_profiler_set_events") : NULL;
    int legacy = (!create || !sd) && l_install && l_jit && l_events;
    if (legacy && g_on < 2) {
        fprintf(stderr, "[jitwarm] Mono 2.6: off (UA_JIT_WARM=2 turns it on)\n");
        g_on = 0;
        return;
    }
    if (legacy && !m_free && h) m_free = (void (*)(void *))so_dlsym(h, "g_free");
    if ((!legacy && (!create || !sd)) || !m_full_name || !m_get_class || !m_class_image || !m_image_name || !m_image_loaded ||
        !m_token || !m_get_method || !m_compile || !m_image_guid || !m_table_rows) {
        fprintf(stderr, "[jitwarm] not a Mono player (or its API is missing): off\n");
        g_on = 0;
        return;
    }
    if (g_defer && (!m_class_method || !m_get_header || !m_header_code || !m_field_from_token || !m_field_parent ||
                    !m_method_flags || !jitwarm_class_vtable || !jitwarm_domain_get)) {
        fprintf(stderr, "[jitwarm] Mono's API for the static constructor rule is missing: off\n");
        g_on = 0;
        return;
    }
#if defined(__arm__)
    if (legacy && !g_probe && probe_2017_4(h)) jitwarm_set_probe(ran_2017_4, 0);
#endif
    read_list(LIST);
    size_t own_n = g_list_n; long own_stale = g_stale;
    read_list(SEED);
    g_seed_n = g_list_n - own_n; g_seed_stale = g_stale - own_stale; g_stale = own_stale;
    const char *jl = getenv("UA_JIT_LOG");
    if (legacy && jl && *jl && *jl != '0')
        fprintf(stderr, "[jitwarm] UA_JIT_LOG holds Mono 2.6's one profiler: compiled ahead, nothing new listed\n");
    else {
        open_out();
        if (g_out < 0) fprintf(stderr, "[jitwarm] cannot write %s: nothing new is listed\n", LIST);
        else { writer_start(); atexit(jitwarm_flush); }
        if (legacy) {
            static int prof_token;
            l_install(&prof_token, NULL);
            l_jit((void *)on_begin, (void *)on_done);
            void (*l_invoke)(void *, void *) = g_check ? (void (*)(void *, void *))so_dlsym(h, "mono_profiler_install_method_invoke") : NULL;
            if (l_invoke && m_method_name) l_invoke((void *)on_invoke, (void *)on_invoke_end);
            l_events(1 << 4 | (l_invoke && m_method_name ? 1 << 16 : 0));   // MONO_PROFILE_JIT_COMPILATION (| METHOD_EVENTS)
            if (g_check && !(l_invoke && m_method_name)) g_check = 0;
        } else sd(create(NULL), on_done);
    }
    if (g_check && !legacy) g_check = 0;
    char how[96];
    if (g_thread) snprintf(how, sizeof how, "on a thread of its own (ua_jitwarm)");
    else snprintf(how, sizeof how, "on UnityMain at most %.1f ms a frame", g_budget * 1000);
    fprintf(stderr, "[jitwarm] %zu listed (%s; %zu from %s, %ld of another build left out), compiled ahead from frame 60 %s; static constructors: %s%s\n",
            g_list_n, LIST, g_seed_n, SEED, g_seed_stale, how, !g_defer ? "not waited for (UA_JIT_WARM_DEFER=0)" :
            g_probe ? "waited for" : "no probe for this runtime: methods touching a class with one are not compiled ahead",
            g_check ? " (UA_JIT_WARM_CHECK: counted)" : "");
}

// one round of the warm-up until `end`: the list's next methods, then (retry) the ones waiting for a class's static
// constructor
static void warm_step(double end, int retry)
{
    double t0 = now();
    while (g_next < g_list_n && now() < end) {
        struct entry *x = &g_list[g_next];
        void *img = m_image_loaded(x->image);  // (Unity loads the game's assemblies before its first frame)
        if (img && x->idx >= 0 && strcmp(g_fid[x->idx].guid, loaded_guid(x->image))) {
            g_stale++;                          // (an assembly loaded after the list was read, of another build)
            free(x->name); x->name = NULL;
            g_next++;
            continue;
        }
        unsigned row = x->token & 0xffffff;
        if (img && ((x->token >> 24) != TABLE_METHOD || !row || (int)row > m_table_rows(img, TABLE_METHOD))) {
            // (not a method of this assembly: never asked of Mono, whose lookup would raise)
            if (g_badtok++ < 5) fprintf(stderr, "[jitwarm] %s %08x: not a method token of this assembly: skipped\n", x->image, x->token);
            free(x->name); x->name = NULL;
            g_next++;
            continue;
        }
        void *m = img ? m_get_method(img, x->token, NULL) : NULL;
        char *n = m ? m_full_name(m, 1) : NULL;
        if (n && (!strcmp(n, x->name) || !strcmp(x->name, "*"))) {
            void *k = NULL;
            int ok = strstr(n, ":.cctor (") ? 2 : g_defer ? warm_ok(m, &k) : 1;
            if (ok == 1) {
                double c = now();
                compile_ahead(m);
                if (now() - c < 0.0002) g_had++; else g_done++;
            } else if (ok == 0) wait_add(m, k);
            else if (ok == 2) g_cctors_skipped++;
            else g_unread++;
        } else g_missing++;
        if (n && m_free) m_free(n);
        free(x->name); x->name = NULL;
        g_next++;
    }
    // the methods waiting for a class's static constructor: tried again every 30 frames once the list is through
    if (g_next >= g_list_n && g_wait_n && retry)
        for (size_t i = 0; i < g_wait_n && now() < end; ) {
            struct waiting *w = &g_wait[i];
            void *k = NULL;
            int ok = class_ok(w->k) ? warm_ok(w->m, &k) : 0;
            if (ok == 0) { if (k) w->k = k; i++; continue; }
            if (ok == 1) { compile_ahead(w->m); g_later++; } else g_unread++;
            g_wait[i] = g_wait[--g_wait_n];
        }
    g_spent += now() - t0;
    char chk[96] = "";
    if (g_check) snprintf(chk, sizeof chk, "; check: %ld static constructors ran inside them", g_check_runs);
    if (g_next >= g_list_n && !g_reported) {
        g_reported = 1;
        g_later_said = g_later;
        fprintf(stderr, "[jitwarm] %ld compiled ahead in %.0f ms (%ld already compiled, %ld not found, %ld bad tokens; %zu waiting for "
                "their classes' static constructors, %ld static constructors not compiled ahead, %ld bodies not read%s)\n",
                g_done, g_spent * 1000, g_had, g_missing, g_badtok, g_wait_n, g_cctors_skipped, g_unread, chk);
    }
    if (g_reported && g_later != g_later_said && (!g_wait_n || g_frame % 1800 == 0)) {
        g_later_said = g_later;
        fprintf(stderr, "[jitwarm] %ld more compiled ahead once their classes' static constructors had run (%zu still waiting%s)\n",
                g_later, g_wait_n, chk);
    }
}

// UA_JIT_WARM_THREAD (default 1 with UA_JIT_WARM=2, Unity 2017.4's Mono 2.6; 0 with UA_JIT_WARM=1): the warm-up compiles on a thread of its own (ua_jitwarm, attached to the game's
// domain, nice 10), from frame 60, never on UnityMain: a compile ahead never adds to a frame the game draws. The
// rule is the same (a method is compiled only when compiling it can run no static constructor), so every static
// constructor still runs on the game's own thread, at the game's own first use. =0: on UnityMain between frames, at
// most UA_JIT_WARM_MS a frame, as before 2026-11-16.

static void *warm_thread(void *a)
{
    (void)a;
    m_thread_attach(g_domain);
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    long round = 0;
    for (;;) {
        int retry = ++round % 50 == 0;     // (the waiting ones every ~0.5 s once the list is through)
        if (g_next >= g_list_n && !g_wait_n) break;
        warm_step(now() + 0.004, retry);
        usleep(g_next >= g_list_n ? 10000 : 500);
    }
    return NULL;
}

void jitwarm_frame(void)
{
    if (!g_on) return;
    if (!g_installed) { install(); return; }
    ++g_frame;
    if (g_thread) {
        if (g_frame == 59) g_domain = jitwarm_domain_get ? jitwarm_domain_get() : NULL;
        if (g_frame >= 60 && !g_started) {
            g_started = 1;
            pthread_t t;
            if (!g_domain || !m_thread_attach || pthread_create(&t, NULL, warm_thread, NULL)) {
                fprintf(stderr, "[jitwarm] no thread of its own: compiled ahead on UnityMain\n");
                g_thread = 0;
            } else { pthread_setname_np(t, "ua_jitwarm"); pthread_detach(t); }
        }
        if (g_thread) return;
    }
    if (g_frame < 60 || (g_next >= g_list_n && (!g_wait_n || g_frame % 30))) return;
    warm_step(now() + g_budget, g_frame % 30 == 0);
}
