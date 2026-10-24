// unityhooks.c's unit test: the jump, the trampoline and the row check, on a stand-in for
// AdjustThreadingModeForAPI written here (the same calling convention and a prologue of the shape
// the rows have: ARM state on 32-bit ARM, A64 on AArch64), copied into a page of its own as
// libunity's text is: the hooked function's results, the original's through the trampoline, and
// that a row whose hash does not match the bytes is not taken.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../unityhooks.c"

// the loader's accessors (unityhooks_on_load, not called here)
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

int main(void)
{
    long pg = sysconf(_SC_PAGESIZE);
    size_t n = (size_t)(fake_adjust_end - fake_adjust);
    // "libunity": a page of code at some offset, read-only and executable as the loader leaves it
    uint8_t *lib = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(lib != MAP_FAILED);
    const uint32_t off = (uint32_t)pg - 8;      // the 8 / 16 bytes the jump takes cross a page boundary
    memcpy(lib + off, fake_adjust, n);
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
    CHECK(hook((uintptr_t)(lib + off), FAKE_ISA, (void *)adjust_hook, (void **)&g_adjust_orig) == 0, "hook failed");
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
    printf("%s: %s\n", FAKE_ISA == ISA_ARM ? "arm" : "a64", fails ? "FAILED" : "ok");
    return fails != 0;
}
