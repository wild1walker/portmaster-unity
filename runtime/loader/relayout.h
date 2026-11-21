/*
 * relayout.h - copies of an AArch64 library's hot functions, laid out together in the order a
 * profile gives, made at load (UA_RELAYOUT; docs/runtime.md, docs/arm64-routes.md route 2).
 *
 * The core here works on an image of the library (its PT_LOAD segments at their vaddrs) and knows
 * nothing of the loader, so the loader (so_relayout below, at load) and the profile tool
 * (runtime/host/relayprof.c, offline on the donor's file) make the same decisions:
 *
 *   rl_image_init   the image's executable range, build id and .eh_frame_hdr search table
 *   rl_fde_find     a function's extent: the FDE covering an address (every function the
 *                   compiler emitted unwind info for; the copies move whole functions only)
 *   rl_check        why a function cannot move (NULL: it can)
 *   rl_plan_make    the layout: each function's place in the region, its islands, its FDE
 *   rl_plan_emit    the copies with their pc-relative fields fixed, the islands, a .eh_frame and
 *                   .eh_frame_hdr of their own for the region
 *   rl_atomics      the NDK's outline-atomic helpers sent straight to their LL/SC loops
 *
 * Only the fields of pc-relative instructions change in a copy (B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ,
 * ADR, ADRP, LDR/LDRSW/PRFM literal); every other word is the original's. A branch to a function
 * that was copied too goes to its copy; any other target, and every address an ADR/ADRP/literal
 * forms, stays the original's (so a function's address is the same everywhere).
 */
#ifndef RELAYOUT_H
#define RELAYOUT_H

#include <elf.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *mem;        /* vaddr v is at mem + v */
    uint64_t run;              /* the address vaddr 0 has where the code runs (the loader: mem) */
    uint64_t size;             /* bytes from vaddr 0 */
    uint64_t text_lo, text_hi; /* the executable segment */
    uint64_t hdr;              /* .eh_frame_hdr (vaddr) */
    uint64_t table;            /* its search table (vaddr), nfde pairs of datarel sdata4 */
    uint32_t nfde;
    uint8_t buildid[64];
    int buildid_len;
} rl_image;

int rl_image_init(rl_image *im, const uint8_t *mem, uint64_t run, uint64_t size,
                  const Elf64_Phdr *ph, int phnum, char *err, size_t errn);
/* "8936b587..." */
void rl_buildid_hex(const rl_image *im, char *out, size_t n);
/* the function whose FDE covers vaddr pc: 0 and its start / size / FDE vaddr; -1 none */
int rl_fde_find(const rl_image *im, uint64_t pc, uint64_t *start, uint64_t *size, uint64_t *fde);
/* why the function [start, start + size) cannot be moved, NULL if it can */
const char *rl_check(const rl_image *im, uint64_t start, uint64_t size);

typedef struct {
    uint64_t start, size;      /* vaddr, bytes */
    uint64_t fde;              /* its FDE (vaddr) */
    uint64_t at;               /* its copy: offset in the region */
    uint32_t isl;              /* bytes of islands reserved after the copy */
} rl_fn;

typedef struct {
    const rl_image *im;
    rl_fn *fn;                 /* in layout order (at ascending) */
    int n;
    int *by_start;             /* indices into fn, start ascending */
    uint64_t code_size;        /* copies and islands */
    uint64_t eh_off, eh_size;  /* .eh_frame of the copies */
    uint64_t hdr_off, hdr_size;/* .eh_frame_hdr */
    uint64_t total;
    int skipped;               /* functions rl_plan_make left out */
} rl_plan;

/* starts: function start vaddrs in layout order (each must be an FDE's start); those rl_check
 * refuses, unknown or repeated are left out (counted in p->skipped; why(start, reason) told) */
int rl_plan_make(rl_plan *p, const rl_image *im, const uint64_t *starts, int n,
                 void (*why)(uint64_t start, const char *reason, void *ud), void *ud);
void rl_plan_free(rl_plan *p);
/* the region's bytes (p->total) for the address dst_run; -1 if a field does not reach (the region
 * must lie within 128 MB of the whole executable segment: rl_reach) */
int rl_plan_emit(const rl_plan *p, uint8_t *dst, uint64_t dst_run, char *err, size_t errn);
/* 1 if a region of `total` bytes at dst_run reaches every B/BL target of the image and back */
int rl_reach(const rl_image *im, uint64_t dst_run, uint64_t total);
/* the copy of vaddr v (a function copied whole), or 0 */
uint64_t rl_copy_of(const rl_plan *p, uint64_t v, uint64_t dst_run);
/* the original address (run) of an address in the region at dst_run, or 0 (islands: their function's start) */
uint64_t rl_orig_of(const rl_plan *p, uint64_t addr, uint64_t dst_run);

/* the NDK's outline-atomic helpers (compiler-rt's lse.S: [bti c;] adrp x16, F; ldrb w16, [x16, #lo];
 * cbz w16, L) in code[0, len) at run address `run`: their count, and with patch=1 the adrp
 * replaced by `b L` (the LL/SC loop: what runs on a CPU without LSE, where the flag F is 0).
 * flag (optional): the vaddr-independent run address of F the helpers test, all of them the same */
int rl_atomics(uint8_t *code, uint64_t run, uint64_t len, int patch, uint64_t *flag);

#ifdef __cplusplus
}
#endif
#endif
