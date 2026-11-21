// test_relayout: so_relayout (relayout.c) on a synthetic AArch64 library (syn_rl_asm.S, syn_rl.c) whose
// functions use every pc-relative instruction class, exceptions and outline atomics. Each function runs
// before the relayout and after it with the same arguments: the results must be the same, the functions
// the list names must run in their copies (the original entry a branch into the region), the copies'
// out-of-reach branches go through islands, BL between copies goes copy to copy, a function changed in
// memory or one with data among its code stays, a forced unwind through the copies runs the same
// cleanups and a backtrace sees the same frames, so_module_at / so_dladdr / so_relayout_orig name a copy
// as its original, and the outline-atomic helpers sent to their LL/SC loops give the same results.
// Usage: test_relayout <dir with libsyn_rl.so>
#define _GNU_SOURCE
#include "../so_loader.h"
#include "../relayout.h"
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static int fails;

typedef long (*fn1)(long);
typedef long (*fn2)(long, long);

static const char *in_list[] = {
    "rl_leaf", "rl_t_bl", "rl_t_tail", "rl_t_bcond", "rl_t_cbz", "rl_t_cbnz", "rl_t_tbz", "rl_t_adr", "rl_t_lit",
    "rl_t_adrp", "rl_t_jt", "rl_t_loop", "rl_t_hooked", "rl_t_self", "rl_t_incode", "rl_t_simdlit",
    "rl_ldadd4_relax", "rl_cas1_acq_rel", "rl_t_atomic",
    "rl_c_cleanup", "rl_c_raise", "rl_c_inner", "rl_c_outer", "rl_c_bt", "rl_c_switch", "rl_t_formb", "rl_t_vcall",
};
#define NL (int)(sizeof in_list / sizeof *in_list)
static const char *stays[] = { "rl_t_hooked", "rl_t_incode", "rl_t_simdlit" };

#define NR 64
static void run_all(so_module *m, long *r)
{
    int k = 0;
    long a1[] = { 0, 1, 4, 5, 6, 9, 16, 24, 31, 100 };
    for (unsigned i = 0; i < sizeof a1 / sizeof *a1; i++) {
        r[k++] = ((fn1)so_sym(m, "rl_t_bcond"))(a1[i]) + 10 * ((fn1)so_sym(m, "rl_t_cbz"))(a1[i]) +
                 100 * ((fn1)so_sym(m, "rl_t_cbnz"))(a1[i]) + 1000 * ((fn1)so_sym(m, "rl_t_tbz"))(a1[i]) +
                 10000 * ((fn1)so_sym(m, "rl_t_jt"))(a1[i]) + 100000 * ((fn1)so_sym(m, "rl_c_switch"))(a1[i] % 10);
    }
    r[k++] = ((fn1)so_sym(m, "rl_t_bl"))(41);
    r[k++] = ((fn1)so_sym(m, "rl_t_tail"))(41);
    r[k++] = ((fn1)so_sym(m, "rl_t_adr"))(0);
    r[k++] = ((fn1)so_sym(m, "rl_t_lit"))(0);
    r[k++] = ((fn1)so_sym(m, "rl_t_adrp"))(0);
    r[k++] = ((fn2)so_sym(m, "rl_t_loop"))(10, 5);
    r[k++] = ((fn1)so_sym(m, "rl_t_hooked"))(0);
    r[k++] = ((fn1)so_sym(m, "rl_t_self"))(0);
    r[k++] = ((fn1)so_sym(m, "rl_t_incode"))(0);
    r[k++] = ((fn1)so_sym(m, "rl_t_simdlit"))(0);
    r[k++] = ((fn1)so_sym(m, "rl_c_outer"))(3);
    r[k++] = ((fn1)so_sym(m, "rl_c_outer"))(0);
    r[k++] = ((fn1)so_sym(m, "rl_c_bt"))(4);
    static int cell[2];
    cell[0] = 5; cell[1] = 0;
    r[k++] = ((fn2)so_sym(m, "rl_t_atomic"))(7, (long)cell);
    r[k++] = cell[0] + 100 * cell[1];
    r[k++] = ((fn2)so_sym(m, "rl_t_atomic"))(1, (long)cell);   /* the byte is 9 now: the swap fails */
    r[k++] = cell[0] + 100 * cell[1];
    for (long i = 0; i < 3; i++) r[k++] = ((fn2)so_sym(m, "rl_t_vcall"))(i, 5 + i);
    r[k++] = ((fn1)so_sym(m, "rl_t_formb"))(0);
    while (k < NR) r[k++] = 0;
}

static uint32_t word(uintptr_t a) { uint32_t w; memcpy(&w, (void *)a, 4); return w; }
static uintptr_t b_target(uintptr_t a)          /* the target of the B at a, or 0 */
{
    uint32_t w = word(a);
    if ((w & 0xfc000000u) != 0x14000000u) return 0;
    return a + (uintptr_t)((int64_t)((uint64_t)(w & 0x3ffffff) << 38) >> 36);
}
static uintptr_t cond_target(uintptr_t a)       /* B.cond / CBZ / CBNZ: the target */
{
    uint32_t w = word(a);
    return a + (uintptr_t)((int64_t)((uint64_t)((w >> 5) & 0x7ffff) << 45) >> 43);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    setvbuf(stdout, NULL, _IONBF, 0);
    char path[512], list[600];
    snprintf(path, sizeof path, "%s/libsyn_rl.so", dir);
    so_set_library_path(dir);
    so_add_shims(bionic_compat_table());         /* dl_iterate_phdr: the loader's (the copies' unwind info) */
    so_module *m = so_load(path, NULL);
    if (!m) { printf("FAIL load %s: %s\n", path, so_error()); return 1; }
    so_run_init(m);
    uintptr_t base = so_base(m);

    long before[NR], after[NR], after2[NR];
    run_all(m, before);
    CHECK(before[15] == 60, "rl_t_loop(10, 5) %ld, want 60", before[15]);

    /* "hook" rl_t_hooked: mov x0, #5 -> mov x0, #6 */
    uintptr_t hk = (uintptr_t)so_sym(m, "rl_t_hooked");
    mprotect((void *)(hk & ~4095ul), 8192, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint32_t mov6 = 0xd28000c0u;
    memcpy((void *)(hk + 4), &mov6, 4);
    mprotect((void *)(hk & ~4095ul), 8192, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)hk, (char *)hk + 8);
    before[16] = 6;

    /* the list: this library's build id, the functions in in_list's order */
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
    rl_image im;
    char err[200], bid[140];
    if (rl_image_init(&im, (const uint8_t *)base, base, so_size(m), (const Elf64_Phdr *)(base + eh->e_phoff), eh->e_phnum, err, sizeof err) < 0) {
        printf("FAIL image: %s\n", err); return 1;
    }
    rl_buildid_hex(&im, bid, sizeof bid);
    CHECK(bid[0], "no build id");
    snprintf(list, sizeof list, "%s/rl_list.txt", dir);
    FILE *f = fopen(list, "w");
    fprintf(f, "ua_relayout 1\nlib libsyn_rl.so\nbuildid %s\n", bid);
    for (int i = 0; i < NL; i++) {
        uintptr_t a = (uintptr_t)so_sym(m, in_list[i]);
        uint64_t s, z;
        CHECK(a && rl_fde_find(&im, a - base, &s, &z, NULL) == 0 && s == a - base, "%s: no FDE", in_list[i]);
        fprintf(f, "%lx %lx 1\n", (unsigned long)(a - base), (unsigned long)z);
    }
    uintptr_t *vt = (uintptr_t *)so_sym(m, "rl_vtab"), va = vt[0], vb = vt[1], vfar = vt[2];
    for (int i = 0; i < 2; i++) {                    /* the table's two local functions */
        uint64_t s, z;
        CHECK(rl_fde_find(&im, vt[i] - base, &s, &z, NULL) == 0, "rl_vt_%c: no FDE", 'a' + i);
        fprintf(f, "%lx %lx 1\n", (unsigned long)(vt[i] - base), (unsigned long)z);
    }
    fclose(f);
    /* a list for another build: refused */
    char wrong[620];
    snprintf(wrong, sizeof wrong, "%s/rl_wrong.txt", dir);
    f = fopen(wrong, "w");
    fprintf(f, "buildid 00\n%lx 4 1\n", (unsigned long)((uintptr_t)so_sym(m, "rl_leaf") - base));
    fclose(f);
    CHECK(so_relayout(m, wrong, 0) < 0, "a list of another build id was used");

    int n = so_relayout(m, list, SO_RELAYOUT_DATA);
    CHECK(n == NL - 3 + 2, "laid out %d of %d + 2 (want all but %d)", n, NL, 3);

    /* each listed function: in the region (its entry a B there), except the three that stay */
    uintptr_t lo = ~0ul, hi = 0;
    for (int i = 0; i < NL; i++) {
        uintptr_t a = (uintptr_t)so_sym(m, in_list[i]), t = b_target(a);
        int stay = 0;
        for (unsigned k = 0; k < sizeof stays / sizeof *stays; k++) stay |= !strcmp(stays[k], in_list[i]);
        if (stay) { CHECK(!t || t - a < 0x100000, "%s moved", in_list[i]); continue; }
        CHECK(t && (t > a + 0x100000 || t < a - 0x100000), "%s: entry not a branch to a copy", in_list[i]);
        if (!t) continue;
        if (t < lo) lo = t;
        if (t > hi) hi = t;
        CHECK(so_relayout_orig(t) == a, "%s: so_relayout_orig(copy) %lx != %lx", in_list[i], (unsigned long)so_relayout_orig(t), (unsigned long)a);
        uintptr_t bias = 0;
        const char *mp = so_module_at((void *)(t + 4), &bias);
        CHECK(mp && strstr(mp, "libsyn_rl.so") && t + 4 - bias == a + 4 - base, "%s: so_module_at(copy)", in_list[i]);
        Dl_info di;
        int (*dla)(const void *, Dl_info *) = (int (*)(const void *, Dl_info *))so_shim_lookup("dladdr");
        if (dla) CHECK(dla((void *)(t + 4), &di) && di.dli_sname && !strcmp(di.dli_sname, in_list[i]),
                       "%s: dladdr(copy) %s", in_list[i], di.dli_sname ? di.dli_sname : "-");
    }
    /* BL between copies: rl_t_bl's first BL goes to rl_leaf's copy */
    uintptr_t cbl = b_target((uintptr_t)so_sym(m, "rl_t_bl")), cleaf = b_target((uintptr_t)so_sym(m, "rl_leaf"));
    int found = 0;
    for (int k = 0; k < 16 && cbl; k++) {
        uint32_t w = word(cbl + 4 * k);
        if ((w & 0xfc000000u) == 0x94000000u) {
            uintptr_t tg = cbl + 4 * k + (uintptr_t)((int64_t)((uint64_t)(w & 0x3ffffff) << 38) >> 36);
            CHECK(tg == cleaf, "rl_t_bl's BL goes to %lx, rl_leaf's copy is %lx", (unsigned long)tg, (unsigned long)cleaf);
            found = 1;
            break;
        }
    }
    CHECK(found, "no BL in rl_t_bl's copy");
    /* the island: rl_t_bcond's b.gt (its second instruction) goes to an island right behind the copy, a B to rl_far_ret1 */
    uintptr_t cb = b_target((uintptr_t)so_sym(m, "rl_t_bcond"));
    if (cb) {
        uintptr_t isl = cond_target(cb + 4);
        CHECK(isl > cb && isl < cb + 64 && b_target(isl) == (uintptr_t)so_sym(m, "rl_far_ret1"), "rl_t_bcond's b.gt: no island to rl_far_ret1");
        uintptr_t cz = cond_target(cb + 8);
        CHECK(cz == cleaf, "rl_t_bcond's cbz goes to %lx, not rl_leaf's copy %lx", (unsigned long)cz, (unsigned long)cleaf);
    }

    /* SO_RELAYOUT_DATA: rl_vt_a's table word is its copy now; rl_vt_b's (formed in code) and rl_far_leaf's (not copied) stay */
    CHECK(vt[0] == b_target(va) && vt[0] != va, "rl_vtab[0] %lx: not rl_vt_a's copy %lx", (unsigned long)vt[0], (unsigned long)b_target(va));
    CHECK(vt[1] == vb, "rl_vtab[1] %lx changed (rl_vt_b's address is formed in code)", (unsigned long)vt[1]);
    CHECK(vt[2] == vfar, "rl_vtab[2] changed");

    run_all(m, after);
    for (int i = 0; i < NR; i++) CHECK(before[i] == after[i], "result %d: %ld before, %ld after the relayout", i, before[i], after[i]);

    /* the outline-atomic helpers (and their copies) straight to LL/SC: the same results (the test bypasses
       so_atomics_llsc's LSE check: this library's flag byte is 0, so the loop is its path on any CPU) */
    int na = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *p = (const Elf64_Phdr *)(base + eh->e_phoff) + i;
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        uintptr_t s = (base + p->p_vaddr) & ~4095ul, e = (base + p->p_vaddr + p->p_memsz + 4095) & ~4095ul;
        mprotect((void *)s, e - s, PROT_READ | PROT_WRITE | PROT_EXEC);
        uint64_t flag = 0;
        int k = rl_atomics((uint8_t *)s, s, e - s, 1, &flag);
        CHECK(!k, "%d helpers found in the original code: their entries branch to the copies", k);
        na += k;
        mprotect((void *)s, e - s, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)s, (char *)e);
    }
    if (lo < hi) {
        uintptr_t s = lo & ~4095ul, e = (hi + 8192) & ~4095ul;
        mprotect((void *)s, e - s, PROT_READ | PROT_WRITE | PROT_EXEC);
        uint64_t flag = 0;
        na += rl_atomics((uint8_t *)s, s, e - s, 1, &flag);
        CHECK(flag == (uintptr_t)so_sym(m, "rl_have_lse"), "the helpers' flag %lx", (unsigned long)flag);
        mprotect((void *)s, e - s, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)s, (char *)e);
    }
    CHECK(na == 2, "%d helpers patched (want the 2 copies)", na);
    run_all(m, after2);
    for (int i = 0; i < NR; i++) CHECK(before[i] == after2[i], "result %d: %ld before, %ld with the helpers patched", i, before[i], after2[i]);

    printf("test_relayout: %d functions laid out, %d helper entries patched, %s\n", n, na, fails ? "FAILED" : "ok");
    printf("  results:");
    for (int i = 0; i < 28; i++) printf(" %ld", after2[i]);
    printf("\n");
    return fails ? 1 : 0;
}
