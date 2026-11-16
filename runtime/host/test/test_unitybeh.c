// unitybeh.c's unit test. UA_BEH_PREFETCH: the prefetch walk over a model of Unity's behaviour list (an iterator
// whose root node links to the nodes still to run; each node's next at +4 and behaviour at +8; a behaviour's
// GameObject at +0x1c, a MonoBehaviour's handle kind at +0x14 and managed object at +0x18) reads only live memory:
// every node and object is put at the end of a page followed by an inaccessible one, so a read past what Unity's own
// code reads faults. Lists of 0 to 6 behaviours, MonoBehaviours mixed with native behaviours of the smallest size
// (a Component: 0x20 bytes), handles of each kind, objects whose GameObject or managed object point nowhere (a
// prefetch of any address is a hint). The hook returns what Unity's Next returned and leaves the list as Next left
// it: a model of Next's node surgery run with and without the hook. UA_BEH_EMPTY: which IL bodies count as empty,
// and Unity's CallUpdateMethod is left out only for a started script whose method is empty and whose class is
// initialized (stand-ins for Mono's method header, class and vtable). Run by unityhooks_test.sh.
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../unitybeh.c"

int unityhooks_matches(const struct uh_row *r, uintptr_t base, size_t size) { (void)r; (void)base; (void)size; return 0; }
int unityhooks_hook(uintptr_t fn, char isa, void *to, void **orig) { (void)fn; (void)isa; (void)to; (void)orig; return -1; }
// Mono's library (hitches.c) and the loader's dlsym: not reached here (the test sets the Mono stand-ins itself)
void *mono_runtime_lib(void) { return NULL; }
void *so_dlsym(void *handle, const char *symbol) { (void)handle; (void)symbol; return NULL; }

#if UINTPTR_MAX > 0xffffffffu
int main(void) { printf("SKIPPED unitybeh: the ARMv7 player's layout (32-bit only)\n"); return 0; }
#else
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// n bytes at the end of a fresh page that is followed by an inaccessible page
static void *guarded(size_t n)
{
    long pg = sysconf(_SC_PAGESIZE);
    uint8_t *p = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); _exit(2); }
    mprotect(p + pg, pg, PROT_NONE);
    memset(p, 0xA5, pg);
    return p + pg - n;
}

typedef struct node { struct node *prev, *next; void *data; } node;
// Unity's iterator: root {prev, next}, the list it puts nodes back into, the current node
typedef struct { node root; node *list; node *cur; } iter;

static const uintptr_t VT_MB = 0x1000;          // "MonoBehaviour's vtable" (never read through)

// a behaviour: MonoBehaviour (0xa0 bytes, its Update node at +0x54) or a native one (0x20 bytes, node elsewhere)
static node *make_behaviour(int mono, int kind, uintptr_t go, uintptr_t target)
{
    if (mono) {
        uint8_t *o = guarded(0xa0);
        memset(o, 0, 0xa0);
        memcpy(o, &VT_MB, 4);
        uint32_t k = (uint32_t)kind; memcpy(o + 0x14, &k, 4);
        uint32_t t = (uint32_t)target; memcpy(o + 0x18, &t, 4);
        uint32_t g = (uint32_t)go; memcpy(o + 0x1c, &g, 4);
        node *n = (node *)(o + 0x54);
        n->data = o;
        return n;
    }
    uint8_t *o = guarded(0x20);
    memset(o, 0, 0x20);
    uint32_t vt = 0x2000; memcpy(o, &vt, 4);
    uint32_t g = (uint32_t)go; memcpy(o + 0x1c, &g, 4);
    node *n = guarded(sizeof(node));
    n->data = o;
    return n;
}

// UA_BEH_EMPTY's stand-ins: methods 0x100 (IL: ret), 0x200 (nop nop ret), 0x300 (ldarg.0 pop ret), 0x400 (no IL);
// their class's vtable byte +0x1b bit 1 (initialized) set for all but 0x200's at first
static uint8_t vt_init[0x40], vt_noinit[0x40];
static const uint8_t il_ret[] = { 0x2a }, il_nops[] = { 0x00, 0x00, 0x2a }, il_work[] = { 0x02, 0x26, 0x2a };
static void *f_get_header(uintptr_t m) { return m == 0x400 ? NULL : (void *)m; }   // (0x500: as 0x100, synchronized)
static const uint8_t *f_get_code(void *h, uint32_t *size, uint32_t *ms)
{
    (void)ms;
    uintptr_t m = (uintptr_t)h;
    const uint8_t *c = m == 0x100 || m == 0x500 ? il_ret : m == 0x200 ? il_nops : il_work;
    *size = m == 0x100 || m == 0x500 ? 1 : 3;
    return c;
}
static uintptr_t f_get_class(uintptr_t m) { return m; }
static uint32_t f_get_flags(uintptr_t m, uint32_t *iflags) { *iflags = m == 0x500 ? 0x0020 : 0; return 0; }
static uintptr_t f_domain(void) { return 1; }
static uintptr_t f_vtable(uintptr_t d, uintptr_t k) { (void)d; return k == 0x200 ? (uintptr_t)vt_noinit : (uintptr_t)vt_init; }
static int orig_calls;
static void f_cum(uintptr_t mb, int idx) { (void)mb; (void)idx; orig_calls++; }

// Next as Unity's does it (the model): the root's first node, unlinked, put back at the end of the list
static void *model_next(void *v)
{
    iter *it = v;
    node *n = it->root.next;
    if (n == &it->root) { it->cur = NULL; return NULL; }
    it->cur = n;
    if (n->prev) { n->prev->next = n->next; n->next->prev = n->prev; n->prev = n->next = NULL; }
    node *l = it->list;
    if (n == l) return n;
    n->prev = l->prev; n->next = l; l->prev->next = n; l->prev = n;
    return n;
}

static void build(iter *it, node *list, node **ns, int count)
{
    list->prev = list->next = list;
    it->root.prev = it->root.next = &it->root;
    it->list = list; it->cur = NULL;
    node *last = &it->root;
    for (int i = 0; i < count; i++) {
        ns[i]->prev = last; ns[i]->next = &it->root; last->next = ns[i]; it->root.prev = ns[i]; last = ns[i];
    }
}

int main(void)
{
    // one walk per list length, every mix of kinds
    for (int count = 0; count <= 6; count++) {
        for (int mix = 0; mix < 4; mix++) {
            iter *it = guarded(sizeof(iter));
            node *list = guarded(sizeof(node));
            node *ns[6];
            for (int i = 0; i < count; i++) {
                int mono = (mix >> (i & 1)) & 1;
                ns[i] = make_behaviour(mono, (i % 3) == 0 ? 2 : (i % 3), i == 2 ? 0 : 0x10000 + 0x40 * i, i == 4 ? 0xfffffff0u : 0x20000u + 0x80u * (unsigned)i);
            }
            build(it, list, ns, count);
            // walk as the loop does: Next (the model) then the prefetch, until the list is empty
            int got = 0;
            while (model_next(it)) {
                unitybeh_prefetch((uintptr_t)it, VT_MB);
                CHECK(it->cur == ns[got], "count %d mix %d: step %d returned another node", count, mix, got);
                got++;
            }
            CHECK(got == count, "count %d mix %d: %d of %d behaviours walked", count, mix, got, count);
            unitybeh_prefetch((uintptr_t)it, VT_MB);        // on an empty iterator too
            // UA_BEH_PREFETCH=2 loads: MonoBehaviours with a real GameObject (its cached byte at +0x3f the last one
            // read) and managed object (its header: 12 bytes), every object at the end of its page
            for (int i = 0; i < count; i++) {
                uint8_t *o = ns[i]->data;
                uint32_t vt; memcpy(&vt, o, 4);
                if (vt != (uint32_t)VT_MB) continue;
                uint32_t g = (uint32_t)(uintptr_t)guarded(0x40), m = (uint32_t)(uintptr_t)guarded(12);
                memcpy(o + 0x1c, &g, 4);
                uint32_t kind; memcpy(&kind, o + 0x14, 4);
                if (kind == 2) memcpy(o + 0x18, &m, 4);
            }
            build(it, list, ns, count);
            got = 0;
            while (model_next(it)) { unitybeh_touch((uintptr_t)it, VT_MB); got++; }
            CHECK(got == count, "count %d mix %d: touch walk %d of %d", count, mix, got, count);
            unitybeh_touch((uintptr_t)it, VT_MB);
        }
    }
    // the hook returns Next's own result and leaves the list as Next left it
    {
        iter *a = guarded(sizeof(iter)), *b = guarded(sizeof(iter));
        node *la = guarded(sizeof(node)), *lb = guarded(sizeof(node));
        node *na[5], *nb[5];
        for (int i = 0; i < 5; i++) { na[i] = make_behaviour(i & 1, 2, 0, 0); nb[i] = make_behaviour(i & 1, 2, 0, 0); }
        build(a, la, na, 5);
        build(b, lb, nb, 5);
        o_next = model_next;
        g_vt = VT_MB;
        for (int step = 0; step < 7; step++) {
            void *ra = next_hook(a), *rb = model_next(b);
            int ia = -1, ib = -1;
            for (int i = 0; i < 5; i++) { if (ra == na[i]) ia = i; if (rb == nb[i]) ib = i; }
            CHECK((ra == NULL) == (rb == NULL) && ia == ib, "step %d: the hook returned another node", step);
        }
        // the list order after the walk
        int i = 0;
        node *x = la->next, *y = lb->next;
        for (; x != la && y != lb && i < 10; x = x->next, y = y->next, i++) {
            int ix = -1, iy = -1;
            for (int k = 0; k < 5; k++) { if (x == na[k]) ix = k; if (y == nb[k]) iy = k; }
            CHECK(ix == iy, "the list differs at %d", i);
        }
        CHECK(x == la && y == lb && i == 5, "the list's length differs");
    }
    // the IL test
    CHECK(unitybeh_il_empty(il_ret, 1) && unitybeh_il_empty(il_nops, 3) && !unitybeh_il_empty(il_work, 3), "IL bodies");
    CHECK(!unitybeh_il_empty(NULL, 0) && !unitybeh_il_empty(il_ret, 0), "no IL");
    // UA_BEH_EMPTY: Unity's own CallUpdateMethod is called except for a started script whose method is empty and
    // whose class is initialized
    {
        g_mono = 1; m_get_header = f_get_header; m_header_get_code = f_get_code; m_method_get_class = f_get_class; m_get_flags = f_get_flags;
        m_domain_get = f_domain; m_class_vtable = f_vtable; o_cum = f_cum; g_empty = 1;
        vt_init[0x1b] = 2; vt_noinit[0x1b] = 0;
        uint8_t *mb = guarded(0xa0);
        uintptr_t methods[3] = { 0x100, 0x300, 0 };
        uint32_t mt = (uint32_t)(uintptr_t)methods; memset(mb, 0, 0xa0); memcpy(mb + 0x48, &mt, 4);
        struct { int started, idx; uintptr_t m; int called; } cases[] = {
            { 0, 0, 0x100, 1 },     // Start not run yet: Unity's (it runs Start, then the method)
            { 1, 0, 0x100, 0 },     // started, empty, class initialized: not called
            { 1, 1, 0x300, 1 },     // a method that does something
            { 1, 2, 0, 1 },         // no method: Unity's (it returns)
            { 1, 0, 0x200, 1 },     // empty (nops) but its class not initialized: Unity's (the invoke runs the constructor)
            { 1, 0, 0x400, 1 },     // no IL
            { 1, 0, 0x500, 1 },     // empty but synchronized (the call takes the object's lock)
        };
        for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
            mb[0x99] = (uint8_t)cases[i].started;
            methods[cases[i].idx] = cases[i].m;
            orig_calls = 0;
            cum_hook((uintptr_t)mb, cases[i].idx);
            CHECK(orig_calls == cases[i].called, "case %u: Unity's CallUpdateMethod %s", i, cases[i].called ? "not called" : "called");
        }
        vt_noinit[0x1b] = 2;                // the class initialized since: now left out
        methods[0] = 0x200; orig_calls = 0; mb[0x99] = 1;
        cum_hook((uintptr_t)mb, 0);
        CHECK(orig_calls == 0, "an empty method of a class initialized later was still called");
        g_empty = 0; orig_calls = 0;        // UA_BEH_STATS alone: always Unity's
        cum_hook((uintptr_t)mb, 0);
        CHECK(orig_calls == 1, "with UA_BEH_EMPTY off an empty method was left out");
    }
    printf("%s unitybeh: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
#endif
