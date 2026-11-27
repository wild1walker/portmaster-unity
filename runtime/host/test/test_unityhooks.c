// unityhooks.c's unit test: the jump, the trampoline and the row check, on a stand-in for
// AdjustThreadingModeForAPI written here (the same calling convention and a prologue of the shape
// the rows have: ARM state on 32-bit ARM, A64 on AArch64), copied into a page of its own as
// libunity's text is: the hooked function's results, the original's through the trampoline, and
// that a row whose hash does not match the bytes is not taken. On AArch64 also entries with
// pc-relative instructions (branches, a call, adr, adrp): the same results through the trampoline,
// which rewrites them; and the two kinds of entry it refuses, left as they were.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../unityhooks.c"

// the loader's accessors and the other hook files' entries (unityhooks_on_load, unityhooks_frame: not called here)
void unitypool_on_load(uintptr_t base, size_t size) { (void)base; (void)size; }
void unityverts_on_load(uintptr_t base, size_t size) { (void)base; (void)size; }
void unitymath_on_load(uintptr_t base, size_t size) { (void)base; (void)size; }
void unitybeh_on_load(uintptr_t base, size_t size) { (void)base; (void)size; }
void unitybeh_frame(void) {}
void unityscripts_on_load(uintptr_t base, size_t size) { (void)base; (void)size; }
void unityscripts_frame(void) {}
void unitylayers_on_load(uintptr_t base, size_t size) { (void)base; (void)size; }
void fmodasync_on_load(uintptr_t base, size_t size) { (void)base; (void)size; }
void nativebeh_on_load(uintptr_t base, size_t size) { (void)base; (void)size; }
void nativebeh_frame(void) {}
void enginerw_on_load_match(uintptr_t base, size_t size) { (void)base; (void)size; }
void enginerw_on_load_patch(uintptr_t base, size_t size) { (void)base; (void)size; }
void enginerw_frame(void) {}
const char *so_name(so_module *m) { (void)m; return NULL; }
uintptr_t so_base(so_module *m) { (void)m; return 0; }
size_t so_size(so_module *m) { (void)m; return 0; }

// stand-in (fake_adjust.S): mode 3..6 on renderer 11 -> 2, anything else -> mode
#if defined(__arm__)
#define FAKE_ISA ISA_ARM
#elif defined(__aarch64__)
#define FAKE_ISA ISA_A64
#else
#error "ARM or AArch64 only"
#endif
extern char fake_adjust[], fake_adjust_end[];

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#if defined(__arm__)
// Thumb (fmodasync.c's validates): the stand-in at a word-aligned entry and at one that is not, each hooked; the
// hook counts and calls the original through its trampoline
extern char fake_validate[], fake_validate_end[], fake_armcond[], fake_armcond_end[];
static int (*g_cond_orig)(int);
static int g_cond_calls;
static int cond_hook(int x) { g_cond_calls++; return g_cond_orig(x); }
static int (*g_val_orig[2])(void *, void **);
static int g_val_calls[2];
static int val_hook0(void *h, void **o) { g_val_calls[0]++; return g_val_orig[0](h, o); }
static int val_hook1(void *h, void **o) { g_val_calls[1]++; return g_val_orig[1](h, o); }
static void test_thumb(uint8_t *lib, const size_t *at)
{
    // ARM: a b<cond> among the moved instructions (at[2])
    int (*g)(int) = (int (*)(int))(uintptr_t)(lib + at[2]);
    CHECK(g(0) == 7 && g(5) == 6, "armcond: the stand-in itself");
    CHECK(unityhooks_hook((uintptr_t)(lib + at[2]), ISA_ARM, (void *)cond_hook, (void **)&g_cond_orig) == 0, "armcond: hook failed");
    CHECK(g(0) == 7 && g(5) == 6 && g(-1) == 0, "armcond: through the trampoline %d %d", g(0), g(5));
    CHECK(g_cond_calls == 3, "armcond: %d of 3 calls through the hook", g_cond_calls);
    int (*const hook[2])(void *, void **) = { val_hook0, val_hook1 };
    for (int k = 0; k < 2; k++) {
        int (*f)(void *, void **) = (int (*)(void *, void **))(uintptr_t)(lib + at[k] + 1);
        void *out = NULL, *h = &out;
        CHECK(f(NULL, &out) == 0x24 && f(h, NULL) == 0x25 && f(h, &out) == 0 && out == h, "thumb %d: the stand-in itself", k);
        CHECK(unityhooks_hook((uintptr_t)(lib + at[k]), ISA_THUMB, (void *)hook[k], (void **)&g_val_orig[k]) == 0,
              "thumb %d: hook failed", k);
        CHECK(((uintptr_t)g_val_orig[k] & 1) == 1, "thumb %d: the trampoline is not a Thumb address", k);
        out = NULL;
        CHECK(f(NULL, &out) == 0x24, "thumb %d: null handle", k);
        CHECK(f(h, NULL) == 0x25, "thumb %d: null out", k);
        CHECK(f(h, &out) == 0 && out == h, "thumb %d: the handle", k);
        CHECK(g_val_calls[k] == 3, "thumb %d: %d of 3 calls went through the hook", k, g_val_calls[k]);
        // a replaced function: the entry is only a jump (unityhooks_jump), here to the other hook's original
        uint16_t j[5];
        CHECK(thumb_jump(j, (uintptr_t)(lib + at[k]), 0) == (at[k] & 2 ? 10u : 8u), "thumb %d: the jump's length", k);
    }
}
#endif

#if defined(__aarch64__)
// the stand-ins with pc-relative entries; each one's hook counts the call and returns the original's result
extern char fake_cond[], fake_cond_end[], fake_call[], fake_call_end[], fake_addr[], fake_addr_end[],
            fake_jump[], fake_jump_end[], fake_literal[], fake_literal_end[], fake_inner[], fake_inner_end[];
enum { R_COND, R_CALL, R_ADDR, R_JUMP, R_LITERAL, R_INNER, R_N };
static uint64_t (*g_reloc_orig[R_N])(uint64_t);
static int g_reloc_calls[R_N];
#define THROUGH(k) static uint64_t through_##k(uint64_t x) { g_reloc_calls[k]++; return g_reloc_orig[k](x); }
THROUGH(R_COND) THROUGH(R_CALL) THROUGH(R_ADDR) THROUGH(R_JUMP) THROUGH(R_LITERAL) THROUGH(R_INNER)

static void test_relocated(uint8_t *lib, const size_t *at)
{
    static uint64_t (*const through[R_N])(uint64_t) = { through_R_COND, through_R_CALL, through_R_ADDR, through_R_JUMP,
                                                        through_R_LITERAL, through_R_INNER };
    static const char *const name[R_N] = { "b.cond", "bl", "adrp, adr, cbz, tbnz", "cbnz, b", "a literal load", "a branch inside" };
    static uint64_t counts[3][4] = { { 0, 0, 0, 5 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0xffffffff80000000ull } };
    const uint64_t in[R_N][6] = {
        { (uint64_t)(uintptr_t)counts[0], (uint64_t)(uintptr_t)counts[1], (uint64_t)(uintptr_t)counts[2] },
        { 0, 1, 7, 0x123456789abcull },
        { 0, 1, 2, 3, 4, 5 },
        { 0, 1, (uint32_t)-5, 100 },
        { 0, 39 },
        { 1, 1000 },
    };
    const int nin[R_N] = { 3, 4, 6, 4, 2, 2 };
    for (int k = 0; k < R_N; k++) {
        uint64_t (*f)(uint64_t) = (uint64_t (*)(uint64_t))(uintptr_t)(lib + at[k]);
        uint64_t before[6];
        for (int i = 0; i < nin[k]; i++) before[i] = f(in[k][i]);
        if (k == R_COND) CHECK(before[0] == 11 && before[1] == 7 && before[2] == 7, "%s: the stand-in itself", name[k]);
        if (k == R_CALL) CHECK(before[2] == 28, "%s: the stand-in itself", name[k]);
        if (k == R_ADDR) {
            uint64_t page = (((uint64_t)(uintptr_t)(lib + at[k])) & ~(uint64_t)0xfff) + 0x1000, word = 0x1122334455667788ull;
            CHECK(before[0] == page && before[2] == word && before[3] == word && before[1] == page + word,
                  "%s: the stand-in itself", name[k]);
        }
        if (k == R_JUMP) CHECK(before[0] == 10 && before[2] == 1 && before[3] == 210, "%s: the stand-in itself", name[k]);
        int r = unityhooks_hook((uintptr_t)(lib + at[k]), ISA_A64, (void *)through[k], (void **)&g_reloc_orig[k]);
        if (k == R_LITERAL || k == R_INNER) {
            CHECK(r != 0, "an entry with %s was hooked", name[k]);
            for (int i = 0; i < nin[k]; i++) CHECK(f(in[k][i]) == before[i], "%s: changed although the hook refused it", name[k]);
            CHECK(g_reloc_calls[k] == 0, "%s: the refused hook was called", name[k]);
            continue;
        }
        CHECK(r == 0, "%s: hook failed", name[k]);
        if (r != 0) continue;
        for (int i = 0; i < nin[k]; i++) {
            uint64_t got = f(in[k][i]);
            CHECK(got == before[i], "%s: input %d gives %#llx through the trampoline, %#llx before", name[k], i,
                  (unsigned long long)got, (unsigned long long)before[i]);
        }
        CHECK(g_reloc_calls[k] == nin[k], "%s: %d of %d calls went through the hook", name[k], g_reloc_calls[k], nin[k]);
    }
}
#endif

int main(void)
{
    long pg = sysconf(_SC_PAGESIZE);
    size_t n = (size_t)(fake_adjust_end - fake_adjust);
    // "libunity": a page of code at some offset, read-only and executable as the loader leaves it
    uint8_t *lib = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(lib != MAP_FAILED);
    const uint32_t off = (uint32_t)pg - 8;      // the 8 / 16 bytes the jump takes cross a page boundary
    memcpy(lib + off, fake_adjust, n);
#if defined(__aarch64__)
    static char *const blob[R_N][2] = { { fake_cond, fake_cond_end }, { fake_call, fake_call_end }, { fake_addr, fake_addr_end },
                                        { fake_jump, fake_jump_end }, { fake_literal, fake_literal_end }, { fake_inner, fake_inner_end } };
    size_t at[R_N], next = (size_t)pg + 0x400;
    for (int k = 0; k < R_N; k++) {
        size_t len = (size_t)(blob[k][1] - blob[k][0]);
        at[k] = next;
        memcpy(lib + next, blob[k][0], len);
        next = (next + len + 31) & ~(size_t)15;
    }
    assert(next <= (size_t)(2 * pg));
#elif defined(__arm__)
    const char *v = (const char *)((uintptr_t)fake_validate & ~(uintptr_t)1);   // (a Thumb symbol's bit 0)
    size_t vlen = (size_t)(fake_validate_end - v), at[3] = { (size_t)pg + 0x400, (size_t)pg + 0x482, (size_t)pg + 0x500 };
    memcpy(lib + at[2], fake_armcond, (size_t)(fake_armcond_end - fake_armcond));
    memcpy(lib + at[0], v, vlen);                  // word-aligned entry: 8 bytes moved
    memcpy(lib + at[1], v, vlen);                  // half-word aligned: 10 (the IT block and the next cmp)
#endif
    mprotect(lib, 2 * pg, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)lib, (char *)lib + 2 * pg);
    int (*f)(int, int, int) = (int (*)(int, int, int))(uintptr_t)(lib + off);
    CHECK(f(3, 11, 0) == 2 && f(4, 11, 0) == 2 && f(3, 17, 0) == 3 && f(2, 11, 0) == 2, "stand-in before the hook");

    // rows: a wrong hash at the right place, the right one, a size past the library
    uint64_t h = fnv1a(lib + off, n);
    struct row rows[] = {
        { "test", "wrong hash", off, (uint32_t)n, FAKE_ISA, h ^ 1 },
        { "test", "too large", off, (uint32_t)(2 * pg), FAKE_ISA, h },
        { "test", "AdjustThreadingModeForAPI", off, (uint32_t)n, FAKE_ISA, h },
    };
    const struct row *r = find_row(rows, 2, (uintptr_t)lib, 2 * pg);
    CHECK(r == NULL, "a row without the library's bytes was taken");
    r = find_row(rows, 3, (uintptr_t)lib, 2 * pg);
    CHECK(r == &rows[2], "the matching row was not found");
    char other = FAKE_ISA == ISA_ARM ? ISA_A64 : ISA_ARM;
    struct row wrong_isa = { "test", "isa", off, (uint32_t)n, other, h };
    CHECK(find_row(&wrong_isa, 1, (uintptr_t)lib, 2 * pg) == NULL, "a row of the other instruction set was taken");

    // the hook: ClientWorkerJobs kept on renderer 11, every other result as the original's
    CHECK(unityhooks_hook((uintptr_t)(lib + off), FAKE_ISA, (void *)adjust_hook, (void **)&g_adjust_orig) == 0, "hook failed");
    CHECK(f(3, 11, 0) == 3, "mode 3 on GLES: %d", f(3, 11, 0));
    CHECK(f(4, 11, 0) == 2, "mode 4 on GLES: %d", f(4, 11, 0));
    CHECK(f(6, 11, 0) == 2, "mode 6 on GLES: %d", f(6, 11, 0));
    CHECK(f(2, 11, 0) == 2, "mode 2 on GLES: %d", f(2, 11, 0));
    CHECK(f(3, 17, 0) == 3, "mode 3 on GL core: %d", f(3, 17, 0));
    CHECK(f(0, 11, 0) == 0, "mode 0 on GLES: %d", f(0, 11, 0));
    CHECK(g_adjust_orig(3, 11, 0) == 2, "the original through the trampoline: %d", g_adjust_orig(3, 11, 0));
    for (int i = 0; i < 100000; i++)
        if (f(i & 7, 11, 0) != ((i & 7) == 3 ? 3 : (i & 7) >= 4 && (i & 7) <= 6 ? 2 : (i & 7))) { CHECK(0, "call %d", i); break; }
    // the page is executable and not writable again
    CHECK(fnv1a(lib + off, n) != h, "the entry was not changed");
#if defined(__aarch64__)
    test_relocated(lib, at);
#elif defined(__arm__)
    test_thumb(lib, at);
#endif
    printf("%s: %s\n", FAKE_ISA == ISA_ARM ? "arm, thumb" : "a64", fails ? "FAILED" : "ok");
    return fails != 0;
}
