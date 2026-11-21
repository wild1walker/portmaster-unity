/*
 * relayout.c - the relayout core (relayout.h): AArch64 function copies with their pc-relative
 * fields fixed, islands for the fields that do not reach, and unwind info for the copies.
 * Pure: works on an rl_image (the library's segments at their vaddrs), used by so_loader.c at load
 * and by runtime/host/relayprof.c offline.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relayout.h"

/* ---------------------------------------------------------------- the image */

static uint32_t rd32(const rl_image *im, uint64_t v) { uint32_t x; memcpy(&x, im->mem + v, 4); return x; }
static uint64_t rd64(const rl_image *im, uint64_t v) { uint64_t x; memcpy(&x, im->mem + v, 8); return x; }
static int in_img(const rl_image *im, uint64_t v, uint64_t n) { return v <= im->size && n <= im->size - v; }

int rl_image_init(rl_image *im, const uint8_t *mem, uint64_t run, uint64_t size,
                  const Elf64_Phdr *ph, int phnum, char *err, size_t errn)
{
    memset(im, 0, sizeof *im);
    im->mem = mem; im->run = run; im->size = size;
    for (int i = 0; i < phnum; i++) {
        const Elf64_Phdr *p = &ph[i];
        if (p->p_type == PT_LOAD && (p->p_flags & PF_X)) {
            if (im->text_hi) { snprintf(err, errn, "more than one executable segment"); return -1; }
            im->text_lo = p->p_vaddr; im->text_hi = p->p_vaddr + p->p_memsz;
        } else if (p->p_type == PT_GNU_EH_FRAME) {
            im->hdr = p->p_vaddr;
        } else if (p->p_type == PT_NOTE && in_img(im, p->p_vaddr, p->p_memsz)) {
            uint64_t q = p->p_vaddr, e = p->p_vaddr + p->p_memsz;
            while (q + 12 <= e) {
                uint32_t nn = rd32(im, q), dn = rd32(im, q + 4), ty = rd32(im, q + 8);
                uint64_t name = q + 12, desc = name + ((nn + 3) & ~3u);
                if (desc + dn > e) break;
                if (ty == NT_GNU_BUILD_ID && nn == 4 && !memcmp(im->mem + name, "GNU", 4) && dn <= sizeof im->buildid) {
                    memcpy(im->buildid, im->mem + desc, dn);
                    im->buildid_len = (int)dn;
                }
                q = desc + ((dn + 3) & ~3u);
            }
        }
    }
    if (!im->text_hi || !in_img(im, im->text_lo, im->text_hi - im->text_lo)) { snprintf(err, errn, "no executable segment in the image"); return -1; }
    if (!im->hdr || !in_img(im, im->hdr, 12)) { snprintf(err, errn, "no .eh_frame_hdr (PT_GNU_EH_FRAME)"); return -1; }
    const uint8_t *h = im->mem + im->hdr;
    /* version 1, eh_frame_ptr any 4-byte form, fde_count udata4, table datarel sdata4 */
    if (h[0] != 1 || (h[1] & 0x0f) != 0x0b || h[2] != 0x03 || h[3] != 0x3b) {
        snprintf(err, errn, ".eh_frame_hdr encodings %02x %02x %02x %02x (want 1 x b 03 3b)", h[0], h[1], h[2], h[3]);
        return -1;
    }
    im->nfde = rd32(im, im->hdr + 8);
    im->table = im->hdr + 12;
    if (!in_img(im, im->table, (uint64_t)im->nfde * 8)) { snprintf(err, errn, ".eh_frame_hdr table outside the image"); return -1; }
    return 0;
}

void rl_buildid_hex(const rl_image *im, char *out, size_t n)
{
    size_t k = 0;
    if (n) out[0] = 0;
    for (int i = 0; i < im->buildid_len && k + 3 <= n; i++) k += (size_t)snprintf(out + k, n - k, "%02x", im->buildid[i]);
}

/* ---------------------------------------------------------------- DWARF pointers, CIE, FDE */

#define PE_OMIT 0xff
static int enc_size(uint8_t enc)
{
    switch (enc & 0x0f) {
    case 0x00: case 0x04: case 0x0c: return 8;
    case 0x02: case 0x0a: return 2;
    case 0x03: case 0x0b: return 4;
    default: return -1;                     /* uleb/sleb: not as a pointer here */
    }
}
static int64_t rd_raw(const rl_image *im, uint64_t v, uint8_t enc)
{
    switch (enc & 0x0f) {
    case 0x00: case 0x04: case 0x0c: return (int64_t)rd64(im, v);
    case 0x02: { uint16_t x; memcpy(&x, im->mem + v, 2); return x; }
    case 0x0a: { int16_t x; memcpy(&x, im->mem + v, 2); return x; }
    case 0x03: return (int64_t)rd32(im, v);
    case 0x0b: return (int32_t)rd32(im, v);
    }
    return 0;
}
static int wr_raw(uint8_t *p, uint8_t enc, int64_t x)
{
    switch (enc & 0x0f) {
    case 0x00: case 0x04: case 0x0c: memcpy(p, &x, 8); return 0;
    case 0x0b: if (x < INT32_MIN || x > INT32_MAX) return -1; { int32_t y = (int32_t)x; memcpy(p, &y, 4); } return 0;
    case 0x03: if (x < 0 || x > (int64_t)UINT32_MAX) return -1; { uint32_t y = (uint32_t)x; memcpy(p, &y, 4); } return 0;
    case 0x0a: if (x < INT16_MIN || x > INT16_MAX) return -1; { int16_t y = (int16_t)x; memcpy(p, &y, 2); } return 0;
    case 0x02: if (x < 0 || x > 0xffff) return -1; { uint16_t y = (uint16_t)x; memcpy(p, &y, 2); } return 0;
    }
    return -1;
}
static uint64_t uleb(const rl_image *im, uint64_t *v, uint64_t end)
{
    uint64_t r = 0; int s = 0;
    while (*v < end) { uint8_t b = im->mem[(*v)++]; if (s < 64) r |= (uint64_t)(b & 0x7f) << s; s += 7; if (!(b & 0x80)) break; }
    return r;
}

typedef struct {
    uint64_t at, len;        /* the CIE (vaddr) and its bytes with the length word */
    uint8_t fde_enc, lsda_enc, per_enc;
    int z;
    uint64_t per_field;      /* vaddr of the personality pointer, 0 if none */
} cie_t;

static int parse_cie(const rl_image *im, uint64_t c, cie_t *ci)
{
    memset(ci, 0, sizeof *ci);
    ci->fde_enc = 0x00; ci->lsda_enc = PE_OMIT; ci->per_enc = PE_OMIT;
    if (!in_img(im, c, 8)) return -1;
    uint32_t len = rd32(im, c);
    if (len == 0 || len == 0xffffffffu || !in_img(im, c, 4 + (uint64_t)len) || rd32(im, c + 4) != 0) return -1;
    uint64_t e = c + 4 + len, p = c + 8;
    ci->at = c; ci->len = 4 + (uint64_t)len;
    uint8_t ver = im->mem[p++];
    const char *aug = (const char *)im->mem + p;
    size_t al = strnlen(aug, e - p);
    if (p + al >= e) return -1;
    p += al + 1;
    if (strstr(aug, "eh")) return -1;
    uleb(im, &p, e); uleb(im, &p, e);       /* code / data alignment */
    if (ver == 1) p++; else uleb(im, &p, e);
    if (aug[0] != 'z') return aug[0] ? -1 : 0;
    ci->z = 1;
    uleb(im, &p, e);                        /* augmentation length */
    for (const char *a = aug + 1; *a; a++) {
        if (p >= e) return -1;
        switch (*a) {
        case 'P': {
            ci->per_enc = im->mem[p++];
            int n = enc_size(ci->per_enc);
            if (n < 0 || p + (uint64_t)n > e) return -1;
            ci->per_field = p; p += (uint64_t)n;
            break;
        }
        case 'L': ci->lsda_enc = im->mem[p++]; break;
        case 'R': ci->fde_enc = im->mem[p++]; break;
        case 'S': case 'B': case 'G': break;
        default: return -1;
        }
    }
    return 0;
}

typedef struct {
    uint64_t at, len;        /* the FDE (vaddr), bytes with the length word */
    uint64_t cie;
    uint64_t start, size;    /* the function (vaddr) */
    uint64_t lsda_field;     /* 0: none */
} fde_t;

static int pcrel_ok(uint8_t enc) { return enc != PE_OMIT && ((enc & 0x70) == 0x10) && enc_size(enc) > 0; }

static int parse_fde(const rl_image *im, uint64_t f, fde_t *fd, cie_t *ci)
{
    memset(fd, 0, sizeof *fd);
    if (!in_img(im, f, 8)) return -1;
    uint32_t len = rd32(im, f), cp = rd32(im, f + 4);
    if (len == 0 || len == 0xffffffffu || cp == 0 || !in_img(im, f, 4 + (uint64_t)len) || cp > f + 4) return -1;
    fd->at = f; fd->len = 4 + (uint64_t)len;
    fd->cie = f + 4 - cp;
    if (parse_cie(im, fd->cie, ci) < 0) return -1;
    if (!pcrel_ok(ci->fde_enc)) return -1;  /* pc_begin pcrel: the only form a PIC library has */
    int n = enc_size(ci->fde_enc);
    uint64_t p = f + 8, e = f + 4 + len;
    if (p + 2 * (uint64_t)n > e) return -1;
    fd->start = p + (uint64_t)rd_raw(im, p, ci->fde_enc);
    fd->size = (uint64_t)rd_raw(im, p + n, ci->fde_enc & 0x0f);
    p += 2 * (uint64_t)n;
    if (ci->z) {
        uint64_t q = p, al = uleb(im, &q, e);
        if (q + al > e) return -1;
        if (ci->lsda_enc != PE_OMIT) {
            int m = enc_size(ci->lsda_enc);
            if (m < 0 || (uint64_t)m > al) return -1;
            fd->lsda_field = q;
        }
    }
    return 0;
}

int rl_fde_find(const rl_image *im, uint64_t pc, uint64_t *start, uint64_t *size, uint64_t *fde)
{
    uint32_t lo = 0, hi = im->nfde;
    while (lo < hi) {                       /* the last entry whose initial location is <= pc */
        uint32_t mid = lo + (hi - lo) / 2;
        uint64_t loc = im->hdr + (uint64_t)(int64_t)(int32_t)rd32(im, im->table + (uint64_t)mid * 8);
        if (loc <= pc) lo = mid + 1; else hi = mid;
    }
    if (!lo) return -1;
    uint64_t f = im->hdr + (uint64_t)(int64_t)(int32_t)rd32(im, im->table + (uint64_t)(lo - 1) * 8 + 4);
    fde_t fd; cie_t ci;
    if (parse_fde(im, f, &fd, &ci) < 0 || pc < fd.start || pc >= fd.start + fd.size) return -1;
    if (start) *start = fd.start;
    if (size) *size = fd.size;
    if (fde) *fde = f;
    return 0;
}

/* ---------------------------------------------------------------- A64 pc-relative forms */

enum { K_NONE, K_B, K_BL, K_BCOND, K_CB, K_TB, K_ADR, K_ADRP, K_LIT };

static int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

static int classify(uint32_t w, int64_t *off)
{
    if ((w & 0x7c000000u) == 0x14000000u) { *off = sext(w & 0x3ffffffu, 26) * 4; return (w >> 31) ? K_BL : K_B; }
    if ((w & 0xff000000u) == 0x54000000u) { *off = sext((w >> 5) & 0x7ffffu, 19) * 4; return K_BCOND; }
    if ((w & 0x7e000000u) == 0x34000000u) { *off = sext((w >> 5) & 0x7ffffu, 19) * 4; return K_CB; }
    if ((w & 0x7e000000u) == 0x36000000u) { *off = sext((w >> 5) & 0x3fffu, 14) * 4; return K_TB; }
    if ((w & 0x9f000000u) == 0x10000000u) { *off = sext((((w >> 5) & 0x7ffffu) << 2) | ((w >> 29) & 3), 21); return K_ADR; }
    if ((w & 0x9f000000u) == 0x90000000u) { *off = sext((((w >> 5) & 0x7ffffu) << 2) | ((w >> 29) & 3), 21) * 4096; return K_ADRP; }
    if ((w & 0x3b000000u) == 0x18000000u) { *off = sext((w >> 5) & 0x7ffffu, 19) * 4; return K_LIT; }
    return K_NONE;
}
static int put26(uint32_t *w, int64_t d)
{
    if ((d & 3) || d < -(1LL << 27) || d >= (1LL << 27)) return -1;
    *w = (*w & 0xfc000000u) | (uint32_t)((d >> 2) & 0x3ffffff);
    return 0;
}
static int put19(uint32_t *w, int64_t d)
{
    if ((d & 3) || d < -(1LL << 20) || d >= (1LL << 20)) return -1;
    *w = (*w & ~(0x7ffffu << 5)) | (uint32_t)(((d >> 2) & 0x7ffff) << 5);
    return 0;
}
static int put14(uint32_t *w, int64_t d)
{
    if ((d & 3) || d < -(1LL << 15) || d >= (1LL << 15)) return -1;
    *w = (*w & ~(0x3fffu << 5)) | (uint32_t)(((d >> 2) & 0x3fff) << 5);
    return 0;
}
static int put21(uint32_t *w, int64_t d)    /* ADR bytes / ADRP pages */
{
    if (d < -(1LL << 20) || d >= (1LL << 20)) return -1;
    *w = (*w & 0x9f00001fu) | (uint32_t)((d & 3) << 29) | (uint32_t)(((d >> 2) & 0x7ffff) << 5);
    return 0;
}
#define A64_B    0x14000000u
#define A64_ADRP 0x90000000u
#define A64_NOP  0xd503201fu
#define A64_BTIC 0xd503245fu

static int lit_gp(uint32_t w) { return !((w >> 26) & 1) && (w >> 30) != 3; }   /* LDR W/X, LDRSW */
static int lit_prfm(uint32_t w) { return !((w >> 26) & 1) && (w >> 30) == 3; }

/* bytes of islands the instruction may need (0 if none), or -1: it cannot be moved */
static int island_need(int k, uint32_t w)
{
    switch (k) {
    case K_BCOND: case K_CB: case K_TB: return 4;               /* b target */
    case K_ADR: return 12;                                       /* adrp; add; b back */
    case K_LIT: return lit_gp(w) ? 16 : lit_prfm(w) ? 0 : -1;    /* adrp; [add;] ldr; b back */
    default: return 0;
    }
}

static const char *scan(const rl_image *im, uint64_t start, uint64_t size, uint32_t *isl)
{
    if ((start & 3) || (size & 3) || !size) return "not whole instructions";
    if (start < im->text_lo || start + size > im->text_hi) return "outside the executable segment";
    if (size > (512u << 10)) return "over 512 KB";
    uint32_t need = 0;
    int tb_out = 0;
    for (uint64_t pc = start; pc < start + size; pc += 4) {
        uint32_t w = rd32(im, pc);
        int64_t off;
        int k = classify(w, &off);
        if (k == K_NONE || k == K_ADRP || k == K_B || k == K_BL) continue;
        uint64_t t = pc + (uint64_t)off;
        if (t >= start && t < start + size) {
            if (k == K_LIT) return "a literal load from inside the function (data among the code)";
            continue;
        }
        int n = island_need(k, w);
        if (n < 0) return "a SIMD/FP literal load from outside the function";
        need += (uint32_t)n;
        if (k == K_TB) tb_out = 1;
    }
    if (tb_out && size + need > (30u << 10)) return "TBZ/TBNZ to another function from a function over 30 KB (its island would not reach)";
    if (size + need > (900u << 10)) return "too large for its islands to reach";
    if (isl) *isl = need;
    return NULL;
}

const char *rl_check(const rl_image *im, uint64_t start, uint64_t size)
{
    uint64_t s, z, f;
    if (rl_fde_find(im, start, &s, &z, &f) < 0 || s != start || z != size) return "no FDE with this extent";
    fde_t fd; cie_t ci;
    if (parse_fde(im, f, &fd, &ci) < 0) return "an FDE this tool does not read";
    if (ci.per_field && !pcrel_ok(ci.per_enc & 0x7f)) return "a personality pointer not pc-relative";
    if (fd.lsda_field && !pcrel_ok(ci.lsda_enc & 0x7f)) return "an LSDA pointer not pc-relative";
    return scan(im, start, size, NULL);
}

/* ---------------------------------------------------------------- the plan */

static const rl_plan *g_sort_plan;
static int by_start_cmp(const void *a, const void *b)
{
    uint64_t x = g_sort_plan->fn[*(const int *)a].start, y = g_sort_plan->fn[*(const int *)b].start;
    return x < y ? -1 : x > y;
}
static int u64_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int rl_plan_make(rl_plan *p, const rl_image *im, const uint64_t *starts, int n,
                 void (*why)(uint64_t, const char *, void *), void *ud)
{
    memset(p, 0, sizeof *p);
    p->im = im;
    p->fn = calloc((size_t)n + 1, sizeof *p->fn);
    p->by_start = calloc((size_t)n + 1, sizeof *p->by_start);
    uint64_t *seen = calloc((size_t)n + 1, sizeof *seen);
    if (!p->fn || !p->by_start || !seen) { free(seen); rl_plan_free(p); return -1; }
    int nseen = 0;
    uint64_t at = 0, eh = 0;
    uint64_t *cies = calloc((size_t)n + 1, sizeof *cies);
    int ncie = 0;
    for (int i = 0; i < n; i++) {
        uint64_t s = starts[i], z = 0, f = 0;
        /* repeated: a sorted copy of what was taken, searched */
        int dup = 0;
        if (nseen && bsearch(&s, seen, (size_t)nseen, sizeof *seen, u64_cmp)) dup = 1;
        if (dup) { p->skipped++; if (why) why(s, "listed twice", ud); continue; }
        if (rl_fde_find(im, s, &s, &z, &f) < 0 || s != starts[i]) {
            p->skipped++; if (why) why(starts[i], "not the start of a function with an FDE", ud); continue;
        }
        const char *r = rl_check(im, s, z);
        uint32_t isl = 0;
        if (!r) r = scan(im, s, z, &isl);
        if (r) { p->skipped++; if (why) why(s, r, ud); continue; }
        rl_fn *fn = &p->fn[p->n++];
        fn->start = s; fn->size = z; fn->fde = f; fn->isl = isl;
        uint64_t al = (s & 15) ? 4 : 16;    /* a 16-byte aligned original stays so */
        at = (at + al - 1) & ~(al - 1);
        fn->at = at;
        at += z + isl;
        /* insert into the sorted list of taken starts */
        int k = nseen++;
        while (k > 0 && seen[k - 1] > s) { seen[k] = seen[k - 1]; k--; }
        seen[k] = s;
        fde_t fd; cie_t ci;
        parse_fde(im, f, &fd, &ci);
        eh += fd.len;
        int known = 0;
        for (int c = 0; c < ncie && !known; c++) known = cies[c] == fd.cie;
        if (!known) { cies[ncie++] = fd.cie; eh += ci.len; }
    }
    free(seen); free(cies);
    for (int i = 0; i < p->n; i++) p->by_start[i] = i;
    g_sort_plan = p;
    qsort(p->by_start, (size_t)p->n, sizeof *p->by_start, by_start_cmp);
    p->code_size = at;
    p->eh_off = (at + 7) & ~7ull;
    p->eh_size = eh + 4;                    /* + the terminator */
    p->hdr_off = (p->eh_off + p->eh_size + 3) & ~3ull;
    p->hdr_size = 12 + 8 * (uint64_t)p->n;
    p->total = p->hdr_off + p->hdr_size;
    return 0;
}

void rl_plan_free(rl_plan *p)
{
    free(p->fn); free(p->by_start);
    memset(p, 0, sizeof *p);
}

static const rl_fn *fn_at(const rl_plan *p, uint64_t v)
{
    int lo = 0, hi = p->n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (p->fn[p->by_start[mid]].start <= v) lo = mid + 1; else hi = mid;
    }
    if (!lo) return NULL;
    const rl_fn *f = &p->fn[p->by_start[lo - 1]];
    return v < f->start + f->size ? f : NULL;
}

uint64_t rl_copy_of(const rl_plan *p, uint64_t v, uint64_t dst_run)
{
    const rl_fn *f = fn_at(p, v);
    return f ? dst_run + f->at + (v - f->start) : 0;
}

uint64_t rl_orig_of(const rl_plan *p, uint64_t addr, uint64_t dst_run)
{
    if (addr < dst_run || addr >= dst_run + p->code_size) return 0;
    uint64_t o = addr - dst_run;
    int lo = 0, hi = p->n;                  /* p->fn is in `at` order */
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (p->fn[mid].at <= o) lo = mid + 1; else hi = mid;
    }
    if (!lo) return 0;
    const rl_fn *f = &p->fn[lo - 1];
    if (o >= f->at + f->size + f->isl) return 0;
    return p->im->run + f->start + (o < f->at + f->size ? o - f->at : 0);
}

int rl_reach(const rl_image *im, uint64_t dst_run, uint64_t total)
{
    const uint64_t lim = (1ull << 27) - (1ull << 20);   /* 128 MB less a margin */
    uint64_t tlo = im->run + im->text_lo, thi = im->run + im->text_hi;
    uint64_t lo = dst_run < tlo ? dst_run : tlo, hi = dst_run + total > thi ? dst_run + total : thi;
    return hi - lo < lim;
}

/* ---------------------------------------------------------------- emit */

static void w32(uint8_t *p, uint32_t w) { memcpy(p, &w, 4); }

int rl_plan_emit(const rl_plan *p, uint8_t *dst, uint64_t dst_run, char *err, size_t errn)
{
    const rl_image *im = p->im;
    memset(dst, 0, p->total);
    if (!rl_reach(im, dst_run, p->total)) { snprintf(err, errn, "the region is not within 128 MB of the code"); return -1; }
    for (int i = 0; i < p->n; i++) {
        const rl_fn *f = &p->fn[i];
        uint64_t isl = f->at + f->size;     /* the next free island byte (region offset) */
        for (uint64_t o = 0; o < f->size; o += 4) {
            uint64_t pc = f->start + o, npc = dst_run + f->at + o;
            uint32_t w = rd32(im, pc), nw = w;
            int64_t off;
            int k = classify(w, &off);
            if (k == K_NONE) { w32(dst + f->at + o, w); continue; }
            if (k == K_ADRP) {
                uint64_t t = im->run + ((pc & ~0xfffull) + (uint64_t)off);
                if (put21(&nw, (int64_t)((t >> 12) - (npc >> 12))) < 0) goto range;
                w32(dst + f->at + o, nw);
                continue;
            }
            uint64_t tv = pc + (uint64_t)off;
            if (tv >= f->start && tv < f->start + f->size) { w32(dst + f->at + o, w); continue; }   /* inside: relative */
            uint64_t t = im->run + tv;
            if (k == K_B || k == K_BL || k == K_BCOND || k == K_CB || k == K_TB) {
                uint64_t c = rl_copy_of(p, tv, dst_run);
                if (c) t = c;
            }
            int64_t d = (int64_t)(t - npc);
            int ok;
            switch (k) {
            case K_B: case K_BL: ok = put26(&nw, d) == 0; if (!ok) goto range; break;
            case K_BCOND: case K_CB: ok = put19(&nw, d) == 0; break;
            case K_TB: ok = put14(&nw, d) == 0; break;
            case K_ADR: ok = put21(&nw, d) == 0; break;
            default: ok = (lit_prfm(w) || lit_gp(w)) && put19(&nw, d) == 0; break;
            }
            if (ok) { w32(dst + f->at + o, nw); continue; }
            /* an island behind the function */
            uint64_t ia = isl, irun = dst_run + ia;
            if (k == K_LIT && lit_prfm(w)) { w32(dst + f->at + o, A64_NOP); continue; }   /* a hint: no effect either way */
            if (k == K_BCOND || k == K_CB || k == K_TB) {
                uint32_t b = A64_B;
                if (put26(&b, (int64_t)(t - irun)) < 0) goto range;
                w32(dst + ia, b); isl += 4;
                int64_t di = (int64_t)(irun - npc);
                if ((k == K_TB ? put14(&nw, di) : put19(&nw, di)) < 0) goto range;
                w32(dst + f->at + o, nw);
            } else {
                uint32_t rdg = w & 31, ins[4], ni = 0;
                uint32_t adrp = A64_ADRP | rdg;
                if (put21(&adrp, (int64_t)((t >> 12) - (irun >> 12))) < 0) goto range;
                ins[ni++] = adrp;
                uint32_t lo = (uint32_t)(t & 0xfff);
                if (k == K_ADR) {
                    ins[ni++] = 0x91000000u | (lo << 10) | (rdg << 5) | rdg;                 /* add xd, xd, #lo */
                } else {
                    uint32_t opc = w >> 30, sz = opc == 1 ? 8 : 4;
                    uint32_t base = opc == 0 ? 0xb9400000u : opc == 1 ? 0xf9400000u : 0xb9800000u;
                    if (lo % sz) { ins[ni++] = 0x91000000u | (lo << 10) | (rdg << 5) | rdg; lo = 0; }
                    ins[ni++] = base | ((lo / sz) << 10) | (rdg << 5) | rdg;                  /* ldr[sw] t, [xt, #lo] */
                }
                uint32_t back = A64_B;
                if (put26(&back, (int64_t)((npc + 4) - (irun + 4ull * ni))) < 0) goto range;
                ins[ni++] = back;
                for (uint32_t j = 0; j < ni; j++) w32(dst + ia + 4 * j, ins[j]);
                isl += 4ull * ni;
                uint32_t b = A64_B;
                if (put26(&b, (int64_t)(irun - npc)) < 0) goto range;
                w32(dst + f->at + o, b);
            }
            if (isl > f->at + f->size + f->isl) { snprintf(err, errn, "islands of %llx overflow", (unsigned long long)f->start); return -1; }
            continue;
range:
            snprintf(err, errn, "%llx: a field that does not reach (kind %d)", (unsigned long long)pc, k);
            return -1;
        }
    }
    /* .eh_frame: the CIEs the copies use, each once, then each copy's FDE */
    uint64_t q = p->eh_off;
    uint64_t *cie_from = calloc((size_t)p->n + 1, sizeof *cie_from), *cie_to = calloc((size_t)p->n + 1, sizeof *cie_to);
    int ncie = 0;
    if (!cie_from || !cie_to) { free(cie_from); free(cie_to); snprintf(err, errn, "out of memory"); return -1; }
    uint8_t *hdr = dst + p->hdr_off;
    uint64_t hdr_run = dst_run + p->hdr_off;
    for (int i = 0; i < p->n; i++) {
        const rl_fn *f = &p->fn[i];
        fde_t fd; cie_t ci;
        if (parse_fde(im, f->fde, &fd, &ci) < 0) { snprintf(err, errn, "FDE of %llx", (unsigned long long)f->start); goto fail; }
        uint64_t cq = 0;
        for (int c = 0; c < ncie; c++) if (cie_from[c] == fd.cie) cq = cie_to[c];
        if (!cq) {
            cq = q;
            memcpy(dst + q, im->mem + ci.at, ci.len);
            if (ci.per_field) {
                int64_t raw = rd_raw(im, ci.per_field, ci.per_enc);
                if (raw) {
                    uint64_t tgt = im->run + ci.per_field + (uint64_t)raw, fld = q + (ci.per_field - ci.at);
                    if (wr_raw(dst + fld, ci.per_enc, (int64_t)(tgt - (dst_run + fld))) < 0) { snprintf(err, errn, "personality pointer"); goto fail; }
                }
            }
            cie_from[ncie] = fd.cie; cie_to[ncie++] = cq;
            q += ci.len;
        }
        uint64_t fq = q;
        memcpy(dst + fq, im->mem + fd.at, fd.len);
        uint32_t cp = (uint32_t)((fq + 4) - cq);
        memcpy(dst + fq + 4, &cp, 4);
        if (wr_raw(dst + fq + 8, ci.fde_enc, (int64_t)((dst_run + f->at) - (dst_run + fq + 8))) < 0) { snprintf(err, errn, "pc_begin"); goto fail; }
        if (fd.lsda_field) {
            int64_t raw = rd_raw(im, fd.lsda_field, ci.lsda_enc);
            if (raw) {
                uint64_t tgt = im->run + fd.lsda_field + (uint64_t)raw, fld = fq + (fd.lsda_field - fd.at);
                if (wr_raw(dst + fld, ci.lsda_enc, (int64_t)(tgt - (dst_run + fld))) < 0) { snprintf(err, errn, "LSDA pointer"); goto fail; }
            }
        }
        q += fd.len;
        /* the search table entry: datarel to the header */
        int32_t loc = (int32_t)((int64_t)(dst_run + f->at) - (int64_t)hdr_run), fa = (int32_t)((int64_t)(dst_run + fq) - (int64_t)hdr_run);
        memcpy(hdr + 12 + 8 * (uint64_t)i, &loc, 4);
        memcpy(hdr + 16 + 8 * (uint64_t)i, &fa, 4);
    }
    free(cie_from); free(cie_to);
    if (q + 4 > p->eh_off + p->eh_size) { snprintf(err, errn, "unwind data overflow"); return -1; }
    /* (the terminator: the zeroed word at q) */
    hdr[0] = 1; hdr[1] = 0x1b; hdr[2] = 0x03; hdr[3] = 0x3b;
    int32_t ep = (int32_t)((int64_t)(dst_run + p->eh_off) - (int64_t)(hdr_run + 4));
    uint32_t cnt = (uint32_t)p->n;
    memcpy(hdr + 4, &ep, 4);
    memcpy(hdr + 8, &cnt, 4);
    return 0;
fail:
    free(cie_from); free(cie_to);
    return -1;
}

/* ---------------------------------------------------------------- outline atomics */

int rl_atomics(uint8_t *code, uint64_t run, uint64_t len, int patch, uint64_t *flag)
{
    int n = 0;
    uint64_t want = 0;
    for (uint64_t o = 0; o + 12 <= len; o += 4) {
        uint32_t a, b, c;
        memcpy(&a, code + o, 4); memcpy(&b, code + o + 4, 4); memcpy(&c, code + o + 8, 4);
        if ((a & 0x9f00001fu) != 0x90000010u) continue;         /* adrp x16, F */
        if ((b & 0xffc003ffu) != 0x39400210u) continue;         /* ldrb w16, [x16, #lo] */
        if ((c & 0xff00001fu) != 0x34000010u) continue;         /* cbz w16, L */
        if (o >= 4) {                                           /* a function's start: after bti c or a return */
            uint32_t prev; memcpy(&prev, code + o - 4, 4);
            if (prev != A64_BTIC && prev != 0xd65f03c0u /* ret */ && prev != 0) continue;
        }
        int64_t off;
        classify(a, &off);
        uint64_t f = ((run + o) & ~0xfffull) + (uint64_t)off + ((b >> 10) & 0xfff);
        int64_t lo; classify(c, &lo);
        if (lo <= 4 || lo > 64) continue;
        uint64_t l = o + 8 + (uint64_t)lo;
        if (l + 4 > len) continue;
        /* the loop's first instruction writes x16 whole (mov w16/x16, wN/xN or uxtb/uxth w16, w0):
           skipping the adrp/ldrb leaves the same registers */
        uint32_t first; memcpy(&first, code + l, 4);
        if ((first & 0x7fe0ffffu) != 0x2a0003f0u && (first & 0xffff83ffu) != 0x53000010u) continue;
        if (want && f != want) continue;
        want = f;
        n++;
        if (patch) {
            uint32_t br = A64_B;
            put26(&br, (int64_t)(l - o));
            memcpy(code + o, &br, 4);
        }
        o += 8;
    }
    if (flag) *flag = want;
    return n;
}
