// unitypool.c: Unity's pool of GLES buffers (BufferManagerGLES) in lists that cost O(1)
// (UA_GLES_POOL=1; off by default).
//
// Unity's OpenGL ES device renames a buffer whenever it would write one the GPU may still read: every
// constant buffer a draw changes and every chunk of dynamic geometry gets another GL buffer from a
// pool (BufferManagerGLES), and the old one goes back to it. ReleaseBuffer keeps a buffer the GPU may
// still read on a pending list and puts a free one in a std::multimap per buffer usage, keyed by
// size; AcquireBuffer takes the smallest free buffer that fits (lower_bound) or makes a new one; each
// frame UpdateLiveSetFromPending walks the whole pending list for the buffers whose frame the GPU has
// finished, and every tenth frame PruneFreeBuffers deletes the free ones left unused too long. Hollow
// Knight in Greenpath on the RG34XX-SP: ~500 renames a frame and 1400 - 1800 buffers in the pool, so
// each lookup and each insert walks ~10 tree nodes that are not in the cache: about 10 % of the
// render thread (AcquireBuffer alone 4.8 %, most of it in one load of a node's key).
//
// This keeps the same pool, under the same rules, in other structures: the free buffers of each usage
// by size (a sorted array of the sizes that have free buffers, a FIFO of buffers for each), the pending
// ones in bins by the frame the GPU must finish first. Taking and giving back a buffer is a binary
// search over a few dozen sizes and a list operation; a frame looks only at the buffers whose frame
// was finished. Unity's code keeps every decision: when to rename (its own checks, before it calls
// the pool); which free buffer fits (AcquireBuffer's rule: the exact size for the usages Unity marks,
// else the smallest from the larger of the size and the device's smallest buffer up to 1.5 times the
// size; the oldest of a size, as in its multimap); when a buffer is free (drawn in a frame the GPU
// finished, or never drawn); which free buffers are deleted (min(size / 6000, 50) plus the frames
// since the buffer was drawn reaching 59). What is rare goes to Unity's own functions, with the
// buffers handed back to its structures first (the original ReleaseBuffer puts each where it would
// have been): a buffer Unity clears when it reuses it (its AcquireBuffer clears it), the ones a prune
// deletes (its PruneFreeBuffers deletes them), Clear, InvalidateAll (a lost context) and the
// manager's destructor. A new buffer is always Unity's (its AcquireBuffer, finding none). The GL
// calls are the same ones: the picture is the same.
//
// The functions are found as unityhooks.c finds its own (tools/re/unityhooks.py: offsets and hashes of
// the code that was read); the structure offsets in g_layouts were read from that same code. Logged:
// "[unitypool]" at the start and every 600 frames (buffers taken, how many from the pool, free,
// waiting for the GPU, deleted).
#include "unitypool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unityhooks.h"

// what one player's pool code reads, by hand from the rows below
struct layout {
    const char *unity;
    unsigned db_size, db_owner, db_usage, db_drawn;   // DataBufferGLES: size, its GfxBuffer, usage, frame last drawn in
    unsigned m_frame, m_done;                         // BufferManagerGLES: the frame, the last one the GPU finished
    unsigned caps_min;                                // GraphicsCaps: the smallest buffer most usages get
    uint32_t exact;                                   // the usages reused only at their exact size (bits)
    unsigned usages;
    uint32_t k_size, k_cap, k_age;                    // PruneFreeBuffers' floats (bits): min(size * k_size, k_cap) + age >= k_age
};
static const struct layout g_layouts[] = {
    // AcquireBuffer: usage <= 8 && (0x112 >> usage) & 1 -> exact size; else key max(caps[0x470], size),
    // limit max(caps[0x470], 3 * size / 2). ReleaseBuffer and UpdateLiveSetFromPending: pending while
    // drawn <= frame && done < drawn. PruneFreeBuffers: 11 usages, 1/6000, 50, 59.
    { "6000.0.61f1 armeabi-v7a", 0x8, 0xc, 0x10, 0x24, 0x0, 0xa4, 0x470, 0x112, 11, 0x392ec33e, 0x42480000, 0x426c0000 },
};
enum { F_ACQUIRE, F_RELEASE, F_LIVE, F_PRUNE, F_CLEAR, F_INVALIDATE, F_DTOR, F_CAPS, F_N };
static const char *const g_func[F_N] = {
    "BufferManagerGLES::AcquireBuffer", "BufferManagerGLES::ReleaseBuffer",
    "BufferManagerGLES::UpdateLiveSetFromPending", "BufferManagerGLES::PruneFreeBuffers",
    "BufferManagerGLES::Clear", "BufferManagerGLES::InvalidateAll", "BufferManagerGLES::~BufferManagerGLES",
    "GetGraphicsCaps",
};
static const struct uh_row g_rows[] = {
    { "6000.0.61f1 armeabi-v7a", "BufferManagerGLES::AcquireBuffer", 0xb25f14, 0x1dc, 'a', 0x02282da5ddc26fb7ull },
    { "6000.0.61f1 armeabi-v7a", "BufferManagerGLES::ReleaseBuffer", 0xb256d0, 0x108, 'a', 0x9257caca5a3e9b9full },
    { "6000.0.61f1 armeabi-v7a", "BufferManagerGLES::UpdateLiveSetFromPending", 0xb26284, 0x12c, 'a', 0x95e1ef6e46250dc5ull },
    { "6000.0.61f1 armeabi-v7a", "BufferManagerGLES::PruneFreeBuffers", 0xb263b0, 0x1e4, 'a', 0xcc74cefef26c54aaull },
    { "6000.0.61f1 armeabi-v7a", "BufferManagerGLES::Clear", 0xb25d90, 0x184, 'a', 0x57cb448e156a8a00ull },
    { "6000.0.61f1 armeabi-v7a", "BufferManagerGLES::InvalidateAll", 0xb26670, 0x114, 'a', 0xa68615524fed3633ull },
    { "6000.0.61f1 armeabi-v7a", "BufferManagerGLES::~BufferManagerGLES", 0xb25ca4, 0xec, 'a', 0xfdaf38fb8faf3ebbull },
    { "6000.0.61f1 armeabi-v7a", "GetGraphicsCaps", 0x4a48b8, 0xc, 't', 0xb47c84086130b794ull },
};

static const struct layout *L;
#define U32(p, off) (*(uint32_t *)((char *)(p) + (off)))
#define PTR(p, off) (*(void **)((char *)(p) + (off)))

// Unity's functions (the originals, through unityhooks' trampolines)
static void *(*o_acquire)(void *m, unsigned size, unsigned usage, void *gfx, int clear);
static void (*o_release)(void *m, void *db);
static void (*o_live)(void *m), (*o_prune)(void *m), (*o_clear)(void *m), (*o_invalidate)(void *m);
static void *(*o_dtor)(void *m);
static char *(*o_caps)(void);

static void *g_m;                   // the manager these lists belong to (Unity makes one)
static unsigned long g_taken, g_new, g_cleared, g_pruned, g_nfree, g_npend;

// ---- list nodes, from blocks that are never given back (a few thousand at most)
typedef struct node { void *db; struct node *next; } node;
static node *g_spare;
static node *node_new(void *db)
{
    if (!g_spare) {
        node *c = malloc(256 * sizeof *c);
        if (!c) return NULL;
        for (int i = 0; i < 256; i++) { c[i].next = g_spare; g_spare = &c[i]; }
    }
    node *n = g_spare;
    g_spare = n->next;
    n->db = db; n->next = NULL;
    return n;
}
static void node_put(node *n) { n->next = g_spare; g_spare = n; }

// ---- the free buffers: per usage, the sizes that have some (sorted), each with its buffers oldest first
typedef struct { uint32_t size; node *head, *tail; } bucket;
typedef struct { bucket *b; unsigned n, cap; } sizes;
static sizes g_free[16];

static unsigned lower(const sizes *s, uint32_t key)
{
    unsigned lo = 0, hi = s->n;
    while (lo < hi) { unsigned mid = (lo + hi) / 2; if (s->b[mid].size < key) lo = mid + 1; else hi = mid; }
    return lo;
}
static int free_put(void *db)
{
    unsigned u = U32(db, L->db_usage);
    if (u >= L->usages) return 0;
    sizes *s = &g_free[u];
    uint32_t size = U32(db, L->db_size);
    unsigned i = lower(s, size);
    if (i == s->n || s->b[i].size != size) {
        if (s->n == s->cap) {
            unsigned c = s->cap ? s->cap * 2 : 32;
            bucket *t = realloc(s->b, c * sizeof *t);
            if (!t) return 0;
            s->b = t; s->cap = c;
        }
        memmove(&s->b[i + 1], &s->b[i], (s->n - i) * sizeof *s->b);
        s->b[i] = (bucket){ size, NULL, NULL };
        s->n++;
    }
    node *n = node_new(db);
    bucket *b = &s->b[i];
    if (!n) {
        if (!b->head) { memmove(b, b + 1, (s->n - i - 1) * sizeof *b); s->n--; }
        return 0;
    }
    if (b->tail) b->tail->next = n; else b->head = n;
    b->tail = n;
    g_nfree++;
    return 1;
}
// the oldest free buffer of the smallest size >= key, when that size is <= limit
static void *free_take(unsigned u, uint32_t key, uint32_t limit)
{
    sizes *s = &g_free[u];
    unsigned i = lower(s, key);
    if (i == s->n || s->b[i].size > limit) return NULL;
    bucket *b = &s->b[i];
    node *n = b->head;
    if (!(b->head = n->next)) { b->tail = NULL; memmove(b, b + 1, (s->n - i - 1) * sizeof *b); s->n--; }
    void *db = n->db;
    node_put(n);
    g_nfree--;
    return db;
}

// ---- the buffers the GPU may still read: in bins by the frame they were drawn in
#define BINS 64
static node *g_bin[BINS], *g_far;   // g_far: drawn BINS or more frames after the last finished one
static uint32_t g_seen;             // the last finished frame whose bin was looked at

// Unity's test (ReleaseBuffer, UpdateLiveSetFromPending, PruneFreeBuffers)
static int pending(void *db, uint32_t frame, uint32_t done)
{
    uint32_t drawn = U32(db, L->db_drawn);
    return drawn <= frame && done < drawn;
}
static int pend_put(void *db, uint32_t done)
{
    node *n = node_new(db);
    if (!n) return 0;
    uint32_t drawn = U32(db, L->db_drawn);
    node **l = drawn - done < BINS ? &g_bin[drawn % BINS] : &g_far;
    n->next = *l; *l = n;
    g_npend++;
    return 1;
}
// each buffer of the list again: still pending -> its bin, else free (Unity's own structures when out of memory)
static void sort_out(void *m, node **l, uint32_t frame, uint32_t done)
{
    node *n = *l;
    *l = NULL;
    while (n) {
        node *next = n->next;
        void *db = n->db;
        node_put(n);
        g_npend--;
        if (!(pending(db, frame, done) ? pend_put(db, done) : free_put(db))) o_release(m, db);
        n = next;
    }
}
static void drain(void *m, uint32_t frame, uint32_t done)
{
    if (done - g_seen >= BINS)                      // (also when done went back)
        for (int i = 0; i < BINS; i++) sort_out(m, &g_bin[i], frame, done);
    else
        for (uint32_t f = g_seen; f != done; ) { f++; sort_out(m, &g_bin[f % BINS], frame, done); }
    if (g_far) sort_out(m, &g_far, frame, done);
    g_seen = done;
}

// every buffer here back to Unity's structures (its ReleaseBuffer puts each in its pending list or its maps)
static void spill(void *m)
{
    for (unsigned u = 0; u < L->usages; u++) {
        sizes *s = &g_free[u];
        for (unsigned i = 0; i < s->n; i++)
            for (node *n = s->b[i].head, *next; n; n = next) { next = n->next; o_release(m, n->db); node_put(n); }
        s->n = 0;
    }
    for (int i = 0; i <= BINS; i++) {
        node **l = i < BINS ? &g_bin[i] : &g_far;
        for (node *n = *l, *next; n; n = next) { next = n->next; o_release(m, n->db); node_put(n); }
        *l = NULL;
    }
    g_nfree = g_npend = 0;
}

static int mine(void *m)
{
    if (!g_m) { g_m = m; g_seen = U32(m, L->m_done); }
    return m == g_m;
}

// ---- the hooks
static void *h_acquire(void *m, unsigned size, unsigned usage, void *gfx, int clear)
{
    if (!mine(m) || usage >= L->usages) return o_acquire(m, size, usage, gfx, clear);
    uint32_t min = U32(o_caps(), L->caps_min), half = (3 * size) >> 1;
    int exact = usage < 32 && (L->exact >> usage) & 1;
    uint32_t key = exact || min <= size ? size : min, limit = exact ? size : min < half ? half : min;
    g_taken++;
    void *db = free_take(usage, key, limit);
    if (!db) { g_new++; return o_acquire(m, size, usage, gfx, clear); }
    if (clear) {                                    // Unity clears it: its AcquireBuffer finds it in its map
        g_cleared++;
        o_release(m, db);
        return o_acquire(m, size, usage, gfx, clear);
    }
    if (PTR(db, L->db_owner) != gfx) PTR(db, L->db_owner) = gfx;
    return db;
}

static void h_release(void *m, void *db)
{
    if (!mine(m)) { o_release(m, db); return; }
    if (PTR(db, L->db_owner)) PTR(db, L->db_owner) = NULL;
    uint32_t done = U32(m, L->m_done);
    if (!(pending(db, U32(m, L->m_frame), done) ? pend_put(db, done) : free_put(db))) o_release(m, db);
}

static void h_live(void *m)
{
    o_live(m);                                      // (its own list: buffers handed to it, normally none)
    if (!mine(m)) return;
    uint32_t frame = U32(m, L->m_frame);
    drain(m, frame, U32(m, L->m_done));
    if (frame % 600 == 0) {
        fprintf(stderr, "[unitypool] %lu buffers taken (%.1f%% from the pool, %lu cleared by Unity), %lu free, "
                        "%lu waiting for the GPU, %lu deleted\n", g_taken,
                g_taken ? 100.0 * (g_taken - g_new) / g_taken : 0.0, g_cleared, g_nfree, g_npend, g_pruned);
        g_taken = g_new = g_cleared = g_pruned = 0;
    }
}

static float bits(uint32_t b) { float f; memcpy(&f, &b, sizeof f); return f; }

static void h_prune(void *m)
{
    if (mine(m)) {                                  // the ones to delete, to Unity's maps; its prune deletes them
        uint32_t frame = U32(m, L->m_frame), done = U32(m, L->m_done);
        float ks = bits(L->k_size), kc = bits(L->k_cap), ka = bits(L->k_age);
        for (unsigned u = 0; u < L->usages; u++) {
            sizes *s = &g_free[u];
            for (unsigned i = 0; i < s->n; ) {
                bucket *b = &s->b[i];
                node **pp = &b->head, *last = NULL;
                while (*pp) {
                    node *n = *pp;
                    void *db = n->db;
                    float score = (float)U32(db, L->db_size) * ks;
                    if (!(score < kc)) score = kc;
                    score += (float)(uint32_t)(frame - U32(db, L->db_drawn));
                    if (score >= ka && !pending(db, frame, done)) {
                        *pp = n->next;
                        node_put(n);
                        g_nfree--; g_pruned++;
                        o_release(m, db);
                    } else { last = n; pp = &n->next; }
                }
                b->tail = last;
                if (!b->head) { memmove(b, b + 1, (s->n - i - 1) * sizeof *b); s->n--; } else i++;
            }
        }
    }
    o_prune(m);
}

static void h_clear(void *m) { if (mine(m)) spill(m); o_clear(m); }
static void h_invalidate(void *m) { if (mine(m)) spill(m); o_invalidate(m); }
static void *h_dtor(void *m)
{
    int me = m == g_m;
    if (me) spill(m);
    void *r = o_dtor(m);                            // (calls Clear: nothing left here)
    if (me) g_m = NULL;
    return r;
}

void unitypool_on_load(uintptr_t base, size_t size)
{
    const char *v = getenv("UA_GLES_POOL");
    if (!v || strcmp(v, "1")) return;
    const struct uh_row *row[F_N];
    for (size_t k = 0; k < sizeof g_layouts / sizeof *g_layouts; k++) {
        int found = 0;
        for (int f = 0; f < F_N; f++) {
            row[f] = NULL;
            for (size_t i = 0; i < sizeof g_rows / sizeof *g_rows && !row[f]; i++)
                if (!strcmp(g_rows[i].unity, g_layouts[k].unity) && !strcmp(g_rows[i].func, g_func[f]) &&
                    unityhooks_matches(&g_rows[i], base, size)) row[f] = &g_rows[i];
            found += row[f] != NULL;
        }
        if (found < F_N) continue;
        L = &g_layouts[k];
        o_caps = (char *(*)(void))(base + row[F_CAPS]->off + (row[F_CAPS]->isa == 't'));
        // ReleaseBuffer last, the one hook that puts buffers in these lists: with any of the others alone
        // the lists stay empty and Unity's pool works as before (a failed hook leaves no buffer here)
        static const int order[F_CAPS] = { F_DTOR, F_CLEAR, F_INVALIDATE, F_PRUNE, F_LIVE, F_ACQUIRE, F_RELEASE };
        void *to[F_CAPS] = { (void *)h_acquire, (void *)h_release, (void *)h_live, (void *)h_prune,
                             (void *)h_clear, (void *)h_invalidate, (void *)h_dtor };
        void **orig[F_CAPS] = { (void **)&o_acquire, (void **)&o_release, (void **)&o_live, (void **)&o_prune,
                                (void **)&o_clear, (void **)&o_invalidate, (void **)&o_dtor };
        for (int k2 = 0; k2 < F_CAPS; k2++) {
            int f = order[k2];
            if (unityhooks_hook(base + row[f]->off, row[f]->isa, to[f], orig[f]) != 0) {
                fprintf(stderr, "[unitypool] %s: hook failed; Unity's own pool is used\n", g_func[f]);
                return;
            }
        }
        fprintf(stderr, "[unitypool] Unity's GLES buffer pool in O(1) lists (UA_GLES_POOL; libunity %s)\n", L->unity);
        return;
    }
    fprintf(stderr, "[unitypool] UA_GLES_POOL: this libunity.so is not one whose buffer pool was read "
                    "(tools/re/unityhooks.py); Unity's own pool is used\n");
}
