// unitypool.c's unit test: its hooks over a model of Unity's pool (the originals' rules as read from
// 6000.0.61f1's code, written here: a std::multimap per usage as a list sorted by size, oldest first
// among equal sizes; a pending list; the same prune formula), driven like Unity's GLES device drives
// it (buffers renamed every frame, drawn, released, the GPU two frames behind, a prune every tenth
// frame, a clear and a lost context in the middle), against the same game on the model alone.
// Checked at every step: a buffer taken is free (not in use, not one the GPU may read), of a size the
// rule allows; no buffer is lost or in two places; a pending buffer is free again once its frame is
// done; a prune deletes exactly what the formula says. Run by unityhooks_test.sh.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../unitypool.c"

// (unityhooks.c's, not called here)
int unityhooks_matches(const struct uh_row *r, uintptr_t base, size_t size) { (void)r; (void)base; (void)size; return 0; }
int unityhooks_hook(uintptr_t fn, char isa, void *to, void **orig) { (void)fn; (void)isa; (void)to; (void)orig; return -1; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// ---- the model: DataBufferGLES (40 bytes; size 0x8, owner 0xc, usage 0x10, drawn 0x24) and the manager
// (frame 0x0, done 0xa4), with ids of our own after the fields Unity's code reads
typedef struct { uint8_t f[40]; int id, where; } dbuf;     // where: 0 in use, 1 model free, 2 model pending, 3 deleted
enum { USED, MFREE, MPEND, GONE };
static uint8_t g_mgr[0xb0];
static uint8_t g_caps[0x480];
#define FRAME U32(g_mgr, 0x0)
#define DONE U32(g_mgr, 0xa4)
#define MAXB 20000
static dbuf *g_all[MAXB]; static int g_nall;
static dbuf *m_free[11][MAXB]; static int m_nfree[11];     // sorted by size, oldest first among equals
static dbuf *m_pend[MAXB]; static int m_npend;
static long m_made, m_clears, m_deleted;

static uint32_t SIZE(dbuf *d) { return U32(d->f, 0x8); }
static void m_insert(dbuf *d)
{
    unsigned u = U32(d->f, 0x10);
    int i = m_nfree[u];
    while (i > 0 && SIZE(m_free[u][i - 1]) > SIZE(d)) i--;  // after the equal ones (Unity's insert goes right on equal)
    memmove(&m_free[u][i + 1], &m_free[u][i], (m_nfree[u] - i) * sizeof(dbuf *));
    m_free[u][i] = d; m_nfree[u]++; d->where = MFREE;
}
static void *m_acquire(void *m, unsigned size, unsigned usage, void *gfx, int clear)
{
    (void)m;
    uint32_t min = U32(g_caps, 0x470);
    int exact = usage <= 8 && (0x112 >> usage) & 1;
    uint32_t key = exact ? size : (min <= size ? size : min);
    uint32_t limit = exact ? size : (min < (3 * size) >> 1 ? (3 * size) >> 1 : min);
    for (int i = 0; i < m_nfree[usage]; i++)
        if (SIZE(m_free[usage][i]) >= key) {
            dbuf *d = m_free[usage][i];
            if (SIZE(d) > limit) break;
            memmove(&m_free[usage][i], &m_free[usage][i + 1], (m_nfree[usage] - i - 1) * sizeof(dbuf *));
            m_nfree[usage]--;
            if (clear) m_clears++;
            PTR(d->f, 0xc) = gfx; d->where = USED;
            return d;
        }
    assert(g_nall < MAXB);
    dbuf *d = calloc(1, sizeof *d);
    U32(d->f, 0x8) = key; U32(d->f, 0x10) = usage; PTR(d->f, 0xc) = gfx;
    U32(d->f, 0x24) = 0xffffffffu;                      // (never drawn)
    d->id = g_nall; d->where = USED; g_all[g_nall++] = d; m_made++;
    return d;
}
static int m_pending(dbuf *d) { uint32_t r = U32(d->f, 0x24); return r <= FRAME && DONE < r; }
static void m_release(void *m, void *p)
{
    (void)m;
    dbuf *d = p;
    CHECK(d->where == USED, "buffer %d handed to Unity's pool from %d", d->id, d->where);
    PTR(d->f, 0xc) = NULL;
    if (m_pending(d)) { m_pend[m_npend++] = d; d->where = MPEND; } else m_insert(d);
}
static void m_live(void *m)
{
    (void)m;
    for (int i = 0; i < m_npend; )
        if (!m_pending(m_pend[i])) { dbuf *d = m_pend[i]; m_pend[i] = m_pend[--m_npend]; m_insert(d); } else i++;
}
static int prune_says(dbuf *d)
{
    float s = (float)SIZE(d) * bits(0x392ec33e);
    if (!(s < 50.0f)) s = 50.0f;
    s += (float)(uint32_t)(FRAME - U32(d->f, 0x24));
    return s >= 59.0f && !m_pending(d);
}
static void m_prune(void *m)
{
    (void)m;
    for (int u = 0; u < 11; u++)
        for (int i = 0; i < m_nfree[u]; )
            if (prune_says(m_free[u][i])) {
                m_free[u][i]->where = GONE; m_deleted++;
                memmove(&m_free[u][i], &m_free[u][i + 1], (m_nfree[u] - i - 1) * sizeof(dbuf *)); m_nfree[u]--;
            } else i++;
}
static void m_clear(void *m)
{
    (void)m;
    for (int u = 0; u < 11; u++) { for (int i = 0; i < m_nfree[u]; i++) { m_free[u][i]->where = GONE; m_deleted++; } m_nfree[u] = 0; }
    for (int i = 0; i < m_npend; i++) { m_pend[i]->where = GONE; m_deleted++; }
    m_npend = 0;
}
static char *m_caps(void) { return (char *)g_caps; }

// every buffer in exactly one place; a free one not pending, a pending one pending; sizes sorted, no empty size
static unsigned char g_here[MAXB], g_pend[MAXB];
static void check_lists(const char *when)
{
    memset(g_here, 0, sizeof g_here); memset(g_pend, 0, sizeof g_pend);
    for (unsigned u = 0; u < L->usages; u++)
        for (unsigned i = 0; i < g_free[u].n; i++) {
            CHECK(g_free[u].b[i].head, "%s: an empty size kept", when);
            CHECK(i == 0 || g_free[u].b[i - 1].size < g_free[u].b[i].size, "%s: sizes out of order", when);
            node *last = NULL;
            for (node *x = g_free[u].b[i].head; x; x = x->next) {
                dbuf *d = x->db; g_here[d->id]++; last = x;
                CHECK(SIZE(d) == g_free[u].b[i].size && U32(d->f, 0x10) == u, "%s: buffer %d under another size or usage", when, d->id);
            }
            CHECK(last == g_free[u].b[i].tail, "%s: a size's tail is not its last buffer", when);
        }
    for (int i = 0; i <= BINS; i++)
        for (node *x = i < BINS ? g_bin[i] : g_far; x; x = x->next) { dbuf *d = x->db; g_here[d->id]++; g_pend[d->id] = 1; }
    unsigned long nf = 0, np = 0;
    for (int k = 0; k < g_nall; k++) {
        dbuf *d = g_all[k];
        int n = g_here[k];
        if (d->where != USED) { CHECK(n == 0, "%s: buffer %d both in Unity's pool (%d) and here", when, d->id, d->where); continue; }
        CHECK(n <= 1, "%s: buffer %d here %d times", when, d->id, n);
        if (n == 1 && !g_pend[k]) { nf++; CHECK(!m_pending(d), "%s: buffer %d free while the GPU may read it", when, d->id); }
        if (n == 1 && g_pend[k]) np++;
    }
    CHECK(nf == g_nfree && np == g_npend, "%s: counts %lu/%lu, lists %lu/%lu", when, g_nfree, g_npend, nf, np);
}

// ---- the game: buffers of these sizes renamed every frame (constant buffers of usage 7 at a few fixed
// sizes, geometry chunks of usages 1 (exact) and 3 (1.5x) at sizes that vary), a few cleared on reuse
static unsigned g_seed = 4242;
static unsigned rnd(void) { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; }
typedef struct { unsigned usage, size; int clear; } want;
static want pick(void)
{
    unsigned r = rnd() % 100;
    if (r < 55) { static const unsigned cb[] = { 64, 128, 400, 1024 }; return (want){ 7, cb[rnd() % 4], 0 }; }
    if (r < 75) return (want){ 1, 256u * (1 + rnd() % 40), 0 };
    if (r < 97) return (want){ 3, 512u * (1 + rnd() % 64), 0 };
    return (want){ 9, 4096, 1 };
}

// one game of frames on the given functions; returns the buffers made
static long play(void *(*acq)(void *, unsigned, unsigned, void *, int), void (*rel)(void *, void *),
                 void (*live)(void *), void (*prune)(void *), void (*clear)(void *), void (*inval)(void *),
                 int frames, int hooked)
{
    static dbuf *held[400];
    int nheld = 0;
    long made0 = m_made;
    g_seed = 4242;
    for (int f = 0; f < frames; f++) {
        // the frame: give back last frame's buffers (Unity renames them), take new ones, draw them
        for (int i = 0; i < nheld; i++) rel(g_mgr, held[i]);
        nheld = 0;
        int k = 150 + (int)(rnd() % 250);
        if (f > frames / 2) k = 20 + (int)(rnd() % 30);      // (a quiet stretch: the pool shrinks by pruning)
        for (int i = 0; i < k; i++) {
            want w = pick();
            dbuf *d = acq(g_mgr, w.size, w.usage, (void *)(uintptr_t)(0x1000 + i), w.clear);
            CHECK(d->where == USED, "frame %d: buffer %d taken from Unity's pool state %d", f, d->id, d->where);
            for (int j = 0; j < nheld; j++) CHECK(held[j] != d, "frame %d: buffer %d taken twice", f, d->id);
            CHECK(!m_pending(d), "frame %d: buffer %d taken while the GPU may read it", f, d->id);
            uint32_t min = U32(g_caps, 0x470);
            int exact = w.usage <= 8 && (0x112 >> w.usage) & 1;
            uint32_t key = exact ? w.size : (min <= w.size ? w.size : min);
            uint32_t lim = exact ? w.size : (min < (3 * w.size) >> 1 ? (3 * w.size) >> 1 : min);
            CHECK(SIZE(d) >= key && SIZE(d) <= lim, "frame %d: size %u for %u (usage %u)", f, SIZE(d), w.size, w.usage);
            CHECK(U32(d->f, 0x10) == w.usage, "frame %d: usage %u for %u", f, U32(d->f, 0x10), w.usage);
            CHECK(PTR(d->f, 0xc) == (void *)(uintptr_t)(0x1000 + i), "frame %d: owner not set", f);
            if (rnd() % 10) U32(d->f, 0x24) = FRAME;           // drawn this frame (some never drawn)
            held[nheld++] = d;
        }
        // AdvanceFrame: the GPU two frames behind (sometimes three), the frame on, the pending ones, a prune
        DONE = FRAME >= 2 + (f % 7 == 0) ? FRAME - 2 - (f % 7 == 0) : 0;
        FRAME++;
        live(g_mgr);
        if (FRAME % 10 == 0) prune(g_mgr);
        if (hooked) check_lists("frame");
        if (f == frames / 3) { for (int i = 0; i < nheld; i++) rel(g_mgr, held[i]); nheld = 0; clear(g_mgr); }
        if (f == 2 * frames / 3) { for (int i = 0; i < nheld; i++) rel(g_mgr, held[i]); nheld = 0; inval(g_mgr); }
        if (hooked && (f == frames / 3 || f == 2 * frames / 3))
            CHECK(g_nfree == 0 && g_npend == 0 && m_nfree[7] == 0 && m_npend == 0, "frame %d: not all handed back", f);
    }
    for (int i = 0; i < nheld; i++) rel(g_mgr, held[i]);
    return m_made - made0;
}

static void reset(void)
{
    for (unsigned u = 0; u < L->usages; u++) {             // (unitypool's lists forget the buffers freed below)
        for (unsigned i = 0; i < g_free[u].n; i++)
            for (node *x = g_free[u].b[i].head, *nx; x; x = nx) { nx = x->next; node_put(x); }
        g_free[u].n = 0;
    }
    for (int i = 0; i <= BINS; i++) {
        node **l = i < BINS ? &g_bin[i] : &g_far;
        for (node *x = *l, *nx; x; x = nx) { nx = x->next; node_put(x); }
        *l = NULL;
    }
    g_nfree = g_npend = 0;
    for (int k = 0; k < g_nall; k++) free(g_all[k]);
    g_nall = 0; memset(m_nfree, 0, sizeof m_nfree); m_npend = 0;
    memset(g_mgr, 0, sizeof g_mgr);
    FRAME = 1;
}

int main(void)
{
    if (sizeof(void *) != 4) { printf("unitypool: the layouts read are 32-bit ARM players'; skipped\n"); return 0; }
    L = &g_layouts[0];
    U32(g_caps, 0x470) = 1024;
    o_acquire = m_acquire; o_release = m_release; o_live = m_live; o_prune = m_prune;
    o_clear = m_clear; o_invalidate = m_clear; o_dtor = NULL; o_caps = m_caps;

    // Unity's pool alone, then the same game through unitypool's hooks
    reset();
    long base = play(m_acquire, m_release, m_live, m_prune, m_clear, m_clear, 900, 0);
    long base_deleted = m_deleted;
    reset(); m_deleted = 0; m_clears = 0;
    long made = play(h_acquire, h_release, h_live, h_prune, h_clear, h_invalidate, 900, 1);
    printf("buffers made: Unity's pool %ld, unitypool %ld; deleted %ld / %ld; taken from the pool %.1f%%; cleared by Unity %ld\n",
           base, made, base_deleted, m_deleted, 100.0 - 100.0 * (double)made / (double)(g_taken ? g_taken : 1), m_clears);
    CHECK(made <= base + base / 50 && made >= base - base / 50, "buffers made %ld, Unity's pool %ld", made, base);
    CHECK(m_clears > 0, "no buffer reused with a clear (Unity's AcquireBuffer must do those)");

    // a prune deletes exactly what the formula says: every free buffer left after it fails the formula,
    // and Unity's model got only buffers the formula deletes
    reset(); g_m = NULL;
    for (int i = 0; i < 300; i++) {                     // drawn 100 .. 1 frames before the prune's frame
        dbuf *d = m_acquire(g_mgr, 64u * (1 + i % 50), 7, NULL, 0);
        U32(d->f, 0x24) = 600u + (uint32_t)i / 3;
        h_release(g_mgr, d);
    }
    FRAME = 700; DONE = 699;
    h_live(g_mgr);
    long before = m_deleted;
    h_prune(g_mgr);
    for (unsigned i = 0; i < g_free[7].n; i++)
        for (node *x = g_free[7].b[i].head; x; x = x->next) CHECK(!prune_says(x->db), "a buffer the formula deletes was kept");
    long want_deleted = 0;
    for (int k = 0; k < g_nall; k++) if (g_all[k]->where == GONE) want_deleted++;
    CHECK(m_deleted - before == want_deleted && want_deleted > 0 && m_nfree[7] == 0, "prune: %ld deleted, %d left in Unity's map",
          m_deleted - before, m_nfree[7]);
    check_lists("prune");

    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("unitypool: ok\n");
    return 0;
}
