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

/* ---------------------------------------------------------------- CFI rows (hot/cold pieces) */

/* A piece that starts inside a function gets an FDE of its own: the row the function's CFI program
 * reaches at the piece's start (CFA rule, each register's rule, AArch64's return-address signing
 * state, the remember_state stack), written as instructions against the CIE's initial row, then the
 * program's own instructions inside the piece with their advances measured from the piece. */
#define NREG 96
typedef struct { uint8_t kind; int64_t v; } rrule;          /* kind 0 = as the CIE's initial row */
enum { RK_INIT, RK_OFF, RK_SAME, RK_UNDEF };
typedef struct { uint32_t cfa_reg; int64_t cfa_off; rrule r[NREG]; int ra; } crow;
typedef struct { uint64_t code_align; int64_t data_align; uint64_t init, init_end; } cie_cf;

static int64_t sleb(const rl_image *im, uint64_t *v, uint64_t end)
{
    int64_t r = 0; int s = 0; uint8_t b = 0;
    while (*v < end) { b = im->mem[(*v)++]; if (s < 64) r |= (int64_t)(b & 0x7f) << s; s += 7; if (!(b & 0x80)) break; }
    if (s < 64 && (b & 0x40)) r |= -((int64_t)1 << s);
    return r;
}
static int cie_cf_read(const rl_image *im, const cie_t *ci, cie_cf *cf)
{
    uint64_t p = ci->at + 8, e = ci->at + ci->len;
    uint8_t ver = im->mem[p++];
    const char *aug = (const char *)im->mem + p;
    p += strnlen(aug, e - p) + 1;
    cf->code_align = uleb(im, &p, e);
    cf->data_align = sleb(im, &p, e);
    if (ver == 1) p++; else uleb(im, &p, e);
    if (aug[0] == 'z') { uint64_t al = uleb(im, &p, e); p += al; }
    cf->init = p; cf->init_end = e;
    return 0;
}
/* run instructions [p, e) on *row (loc starting at *loc) up to pc `stop` (relative to the function):
   returns the position of the first instruction that applies at or after stop's next row, or 0 on an
   unknown instruction. *loc ends at the location of the row containing stop. */
static uint64_t cfa_run(const rl_image *im, const cie_cf *cf, const crow *init, crow *row, crow *stack, int *depth,
                        uint64_t p, uint64_t e, uint64_t *loc, uint64_t stop, int *bad)
{
    while (p < e) {
        uint64_t q = p;
        uint8_t op = im->mem[p++];
        uint64_t adv = 0;
        int is_adv = 0;
        switch (op >> 6) {
        case 1: adv = (op & 0x3f) * cf->code_align; is_adv = 1; break;
        case 2: { uint64_t o = uleb(im, &p, e); if ((op & 0x3f) < NREG) { row->r[op & 0x3f].kind = RK_OFF; row->r[op & 0x3f].v = (int64_t)o * cf->data_align; } continue; }
        case 3: if ((op & 0x3f) < NREG) row->r[op & 0x3f] = init->r[op & 0x3f]; continue;
        }
        if (!is_adv) switch (op) {
        case 0x00: continue;
        case 0x02: adv = im->mem[p] * cf->code_align; p += 1; is_adv = 1; break;
        case 0x03: { uint16_t x; memcpy(&x, im->mem + p, 2); p += 2; adv = x * cf->code_align; is_adv = 1; break; }
        case 0x04: { uint32_t x; memcpy(&x, im->mem + p, 4); p += 4; adv = x * cf->code_align; is_adv = 1; break; }
        case 0x05: { uint64_t r = uleb(im, &p, e), o = uleb(im, &p, e); if (r < NREG) { row->r[r].kind = RK_OFF; row->r[r].v = (int64_t)o * cf->data_align; } else *bad = 1; continue; }
        case 0x11: { uint64_t r = uleb(im, &p, e); int64_t o = sleb(im, &p, e); if (r < NREG) { row->r[r].kind = RK_OFF; row->r[r].v = o * cf->data_align; } else *bad = 1; continue; }
        case 0x06: { uint64_t r = uleb(im, &p, e); if (r < NREG) row->r[r] = init->r[r]; else *bad = 1; continue; }
        case 0x07: { uint64_t r = uleb(im, &p, e); if (r < NREG) row->r[r].kind = RK_UNDEF; else *bad = 1; continue; }
        case 0x08: { uint64_t r = uleb(im, &p, e); if (r < NREG) row->r[r].kind = RK_SAME; else *bad = 1; continue; }
        case 0x0a: if (*depth >= 8) { *bad = 1; return 0; } stack[(*depth)++] = *row; continue;
        case 0x0b: { if (!*depth) { *bad = 1; return 0; } int ra = row->ra; *row = stack[--*depth]; row->ra = ra; continue; }
        case 0x0c: row->cfa_reg = (uint32_t)uleb(im, &p, e); row->cfa_off = (int64_t)uleb(im, &p, e); continue;
        case 0x12: row->cfa_reg = (uint32_t)uleb(im, &p, e); row->cfa_off = sleb(im, &p, e) * cf->data_align; continue;
        case 0x0d: row->cfa_reg = (uint32_t)uleb(im, &p, e); continue;
        case 0x0e: row->cfa_off = (int64_t)uleb(im, &p, e); continue;
        case 0x13: row->cfa_off = sleb(im, &p, e) * cf->data_align; continue;
        case 0x2d: row->ra ^= 1; continue;           /* DW_CFA_AARCH64_negate_ra_state */
        default: *bad = 1; return 0;
        }
        if (is_adv) {
            if (*loc + adv > stop) return q;          /* this advance leaves the row that holds stop */
            *loc += adv;
        }
    }
    return e;
}

/* the bytes of an instruction stream that turns row `from` into row `to` */
typedef struct { uint8_t *b; size_t n, cap; } bbuf;
static void bput(bbuf *b, uint8_t x) { if (b->n == b->cap) { b->cap = b->cap ? 2 * b->cap : 256; b->b = realloc(b->b, b->cap); } b->b[b->n++] = x; }
static void bu(bbuf *b, uint64_t v) { do { uint8_t x = v & 0x7f; v >>= 7; bput(b, x | (v ? 0x80 : 0)); } while (v); }
static void bs(bbuf *b, int64_t v) { int more = 1; while (more) { uint8_t x = v & 0x7f; v >>= 7; if ((v == 0 && !(x & 0x40)) || (v == -1 && (x & 0x40))) more = 0; else x |= 0x80; bput(b, x); } }
static int row_diff(bbuf *b, const cie_cf *cf, const crow *init, const crow *from, const crow *to)
{
    if (from->cfa_reg != to->cfa_reg || from->cfa_off != to->cfa_off) {
        if (to->cfa_off < 0) return -1;
        bput(b, 0x0c); bu(b, to->cfa_reg); bu(b, (uint64_t)to->cfa_off);
    }
    for (int r = 0; r < NREG; r++) {
        const rrule *x = &from->r[r], *y = &to->r[r];
        if (x->kind == y->kind && x->v == y->v) continue;
        if (y->kind == init->r[r].kind && y->v == init->r[r].v) { bput(b, 0x06); bu(b, (uint64_t)r); continue; }
        switch (y->kind) {
        case RK_OFF:
            if (!cf->data_align || y->v % cf->data_align) return -1;
            bput(b, 0x11); bu(b, (uint64_t)r); bs(b, y->v / cf->data_align); break;
        case RK_SAME: bput(b, 0x08); bu(b, (uint64_t)r); break;
        case RK_UNDEF: bput(b, 0x07); bu(b, (uint64_t)r); break;
        default: return -1;
        }
    }
    if (from->ra != to->ra) bput(b, 0x2d);
    return 0;
}

/* the CFI instructions of the piece [a, b) of the function whose FDE is fd (offsets in the function) */
static int piece_cfi(const rl_image *im, const fde_t *fd, const cie_t *ci, uint64_t a, uint64_t bnd, bbuf *out)
{
    cie_cf cf;
    cie_cf_read(im, ci, &cf);
    if (!cf.code_align) return -1;
    crow init, row, stack[8], dummy[8];
    memset(&init, 0, sizeof init);
    int depth = 0, d0 = 0, bad = 0;
    uint64_t loc = 0;
    crow zero; memset(&zero, 0, sizeof zero);
    cfa_run(im, &cf, &zero, &init, dummy, &d0, cf.init, cf.init_end, &loc, ~0ull, &bad);
    if (bad || d0) return -1;
    for (int r = 0; r < NREG; r++) init.r[r].kind = init.r[r].kind;   /* (the CIE's own rules are the reference) */
    /* the FDE's instructions follow its augmentation data */
    uint64_t p = fd->at + 8 + 2 * (uint64_t)enc_size(ci->fde_enc), e = fd->at + fd->len;
    if (ci->z) { uint64_t al = uleb(im, &p, e); p += al; }
    row = init;
    /* rows are relative to init: copies of it whose rules mean "the CIE's" are the RK_INIT kind */
    crow base = init;
    for (int r = 0; r < NREG; r++) { base.r[r].kind = RK_INIT; base.r[r].v = 0; }
    row = base;
    loc = 0;
    uint64_t q = cfa_run(im, &cf, &base, &row, stack, &depth, p, e, &loc, a, &bad);
    if (bad) return -1;
    /* the remember_state stack, bottom up, then the row at a */
    crow cur = base;
    for (int k = 0; k < depth; k++) {
        if (row_diff(out, &cf, &base, &cur, &stack[k]) < 0) return -1;
        bput(out, 0x0a);
        cur = stack[k];
    }
    if (row_diff(out, &cf, &base, &cur, &row) < 0) return -1;
    /* then the program's instructions inside [a, bnd): advances re-measured from a */
    uint64_t pos = a;                       /* the location the emitted rows have reached */
    while (q && q < e) {
        uint64_t q0 = q;
        uint8_t op = im->mem[q];
        uint64_t adv = 0;
        int is_adv = 1;
        if ((op >> 6) == 1) { adv = (op & 0x3f) * cf.code_align; q++; }
        else if (op == 0x02) { adv = im->mem[q + 1] * cf.code_align; q += 2; }
        else if (op == 0x03) { uint16_t x; memcpy(&x, im->mem + q + 1, 2); adv = x * cf.code_align; q += 3; }
        else if (op == 0x04) { uint32_t x; memcpy(&x, im->mem + q + 1, 4); adv = x * cf.code_align; q += 5; }
        else is_adv = 0;
        if (is_adv) {
            loc += adv;
            if (loc >= bnd) break;
            if (loc > pos) {
                uint64_t d = (loc - pos) / cf.code_align;
                bput(out, 0x04); for (int k = 0; k < 4; k++) bput(out, (uint8_t)(d >> (8 * k)));
                pos = loc;
            }
            continue;
        }
        /* one non-advance instruction: run it on a scratch row to find its length, copy its bytes */
        crow tmp = row; int dd = depth; uint64_t l2 = loc; crow st2[8]; memcpy(st2, stack, sizeof st2);
        uint64_t q1 = q0;
        /* find the end of this single instruction */
        {
            uint8_t o = im->mem[q1++];
            if ((o >> 6) == 2) uleb(im, &q1, e);
            else if ((o >> 6) == 3) { }
            else switch (o) {
            case 0x00: case 0x0a: case 0x0b: case 0x2d: break;
            case 0x05: case 0x0c: uleb(im, &q1, e); uleb(im, &q1, e); break;
            case 0x11: case 0x12: uleb(im, &q1, e); sleb(im, &q1, e); break;
            case 0x06: case 0x07: case 0x08: case 0x0d: case 0x0e: uleb(im, &q1, e); break;
            case 0x13: sleb(im, &q1, e); break;
            default: return -1;
            }
        }
        cfa_run(im, &cf, &base, &tmp, st2, &dd, q0, q1, &l2, ~0ull, &bad);
        if (bad) return -1;
        row = tmp; depth = dd; memcpy(stack, st2, sizeof st2);
        for (uint64_t k = q0; k < q1; k++) bput(out, im->mem[k]);
        q = q1;
    }
    return 0;
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

const char *rl_split_check(const rl_image *im, uint64_t start, uint64_t size)
{
    uint64_t s, z, f;
    if (rl_fde_find(im, start, &s, &z, &f) < 0 || s != start || z != size) return "no FDE";
    fde_t fd; cie_t ci;
    if (parse_fde(im, f, &fd, &ci) < 0) return "FDE";
    if (fd.lsda_field && rd_raw(im, fd.lsda_field, ci.lsda_enc)) return "an LSDA (landing pads are offsets from the function's start)";
    for (uint64_t pc = start; pc < start + size; pc += 4) {
        int64_t off;
        uint32_t w = rd32(im, pc);
        if (classify(w, &off) == K_ADR) { uint64_t t = pc + (uint64_t)off; if (t >= start && t < start + size) return "an ADR inside the function"; }
    }
    bbuf b = { 0 };
    int r = piece_cfi(im, &fd, &ci, size / 2 & ~3ull, size, &b);
    free(b.b);
    return r < 0 ? "CFI this tool does not follow" : NULL;
}

static int falls_through(uint32_t w)
{
    if ((w & 0xfc000000u) == 0x14000000u) return 0;            /* b */
    if ((w & 0xfffffc1fu) == 0xd61f0000u) return 0;            /* br */
    if ((w & 0xfffffc1fu) == 0xd65f0000u) return 0;            /* ret */
    if (w == 0xd65f0bffu || w == 0xd65f0fffu) return 0;        /* retaa, retab */
    if ((w & 0xffe0001fu) == 0xd4200000u) return 0;            /* brk */
    return 1;
}

/* island bytes the piece [s, e) may need: every out-of-piece field a copy might not reach */
static uint32_t piece_isl(const rl_image *im, uint64_t s, uint64_t e)
{
    uint32_t need = 0;
    for (uint64_t pc = s; pc < e; pc += 4) {
        uint32_t w = rd32(im, pc);
        int64_t off;
        int k = classify(w, &off);
        if (k == K_NONE || k == K_ADRP || k == K_B || k == K_BL) continue;
        uint64_t t = pc + (uint64_t)off;
        if (t >= s && t < e) continue;
        int n = island_need(k, w);
        need += n > 0 ? (uint32_t)n : 0;
    }
    return need;
}

int rl_plan_make(rl_plan *p, const rl_image *im, const rl_want *want, int n,
                 void (*why)(uint64_t, const char *, void *), void *ud)
{
    memset(p, 0, sizeof *p);
    p->im = im;
    p->fn = calloc((size_t)n + 1, sizeof *p->fn);
    p->by_start = calloc((size_t)n + 1, sizeof *p->by_start);
    int capp = 2 * n + 16;
    p->pc = calloc((size_t)capp, sizeof *p->pc);
    uint64_t *seen = calloc((size_t)n + 1, sizeof *seen);
    if (!p->fn || !p->by_start || !p->pc || !seen) { free(seen); rl_plan_free(p); return -1; }
    int nseen = 0;
    for (int i = 0; i < n; i++) {
        uint64_t s = want[i].start, z = 0, f = 0;
        if (nseen && bsearch(&s, seen, (size_t)nseen, sizeof *seen, u64_cmp)) { p->skipped++; if (why) why(s, "listed twice", ud); continue; }
        if (rl_fde_find(im, s, &s, &z, &f) < 0 || s != want[i].start) {
            p->skipped++; if (why) why(want[i].start, "not the start of a function with an FDE", ud); continue;
        }
        const char *r = rl_check(im, s, z);
        if (r) { p->skipped++; if (why) why(s, r, ud); continue; }
        int fi = p->n++;
        rl_fn *fn = &p->fn[fi];
        fn->start = s; fn->size = z; fn->fde = f;
        int k = nseen++;
        while (k > 0 && seen[k - 1] > s) { seen[k] = seen[k - 1]; k--; }
        seen[k] = s;
        /* its pieces: the hot ranges the list gives (the first always holds the entry), the rest cold */
        uint32_t cut[2 * RL_MAXHOT + 2]; int nc = 0;
        int split = want[i].nhot > 0 && !rl_split_check(im, s, z);
        if (want[i].nhot > 0 && !split) p->unsplit++;
        if (split) {
            uint32_t prev = 0;
            for (int h = 0; h < want[i].nhot; h++) {
                uint32_t a = want[i].hot[2 * h] & ~3u, b = (want[i].hot[2 * h + 1] + 3) & ~3u;
                if (h == 0) a = 0;
                if (b > z) b = (uint32_t)z;
                if (a < prev || a >= b) { split = 0; break; }
                cut[nc++] = a; cut[nc++] = b;
                prev = b;
            }
        }
        if (!split || !nc) { nc = 0; cut[nc++] = 0; cut[nc++] = (uint32_t)z; }
        /* pieces: [cut0,cut1) hot, [cut1,cut2) cold, ... and a cold tail */
        fn->p0 = p->np;
        uint32_t pos = 0;
        for (int c = 0; c < nc; c += 2) {
            if (cut[c] > pos) { p->pc[p->np++] = (rl_piece){ .fn = fi, .start = s + pos, .size = cut[c] - pos, .hot = 0 }; }
            p->pc[p->np++] = (rl_piece){ .fn = fi, .start = s + cut[c], .size = cut[c + 1] - cut[c], .hot = 1 };
            pos = cut[c + 1];
            if (p->np + 4 > capp) { capp *= 2; p->pc = realloc(p->pc, sizeof *p->pc * (size_t)capp); }
        }
        if (pos < z) p->pc[p->np++] = (rl_piece){ .fn = fi, .start = s + pos, .size = z - pos, .hot = 0 };
        if (p->np + 4 > capp) { capp *= 2; p->pc = realloc(p->pc, sizeof *p->pc * (size_t)capp); }
        fn->npc = p->np - fn->p0;
        if (fn->npc > 1) p->split++;
    }
    free(seen);
    for (int i = 0; i < p->n; i++) p->by_start[i] = i;
    g_sort_plan = p;
    qsort(p->by_start, (size_t)p->n, sizeof *p->by_start, by_start_cmp);
    /* islands, the fall-through branch, CFI of the pieces that start inside a function */
    uint64_t eh = 0;
    uint64_t *cies = calloc((size_t)p->n + 1, sizeof *cies);
    int ncie = 0;
    for (int j = 0; j < p->np; j++) {
        rl_piece *q = &p->pc[j];
        const rl_fn *f = &p->fn[q->fn];
        q->isl = piece_isl(im, q->start, q->start + q->size);
        int last = q->start + q->size == f->start + f->size;
        q->ft = !last && falls_through(rd32(im, q->start + q->size - 4));
        fde_t fd; cie_t ci;
        parse_fde(im, f->fde, &fd, &ci);
        int known = 0;
        for (int c = 0; c < ncie && !known; c++) known = cies[c] == fd.cie;
        if (!known) { cies[ncie++] = fd.cie; eh += ci.len; }
        if (f->npc == 1) { eh += fd.len; continue; }
        bbuf b = { 0 };
        if (piece_cfi(im, &fd, &ci, q->start - f->start, q->start + q->size - f->start, &b) < 0) { free(b.b); free(cies); rl_plan_free(p); return -1; }
        /* header: length, CIE pointer, pc_begin, pc_range (fde_enc), the augmentation as the original's */
        uint64_t hdr = 8 + 2 * (uint64_t)enc_size(ci.fde_enc), augl = 0;
        if (ci.z) { uint64_t a0 = fd.at + hdr, a1 = a0; uint64_t al = uleb(im, &a1, fd.at + fd.len); augl = (a1 - a0) + al; }
        size_t len = hdr + augl + b.n;
        len = (len + 7) & ~(size_t)7;
        q->cfi = b.b; q->ncfi = b.n; q->fde_len = len;
        eh += len;
    }
    free(cies);
    /* layout: the hot pieces in the list's order, then the cold ones */
    uint64_t at = 0;
    for (int pass = 1; pass >= 0; pass--)
        for (int j = 0; j < p->np; j++) {
            rl_piece *q = &p->pc[j];
            if (q->hot != pass) continue;
            uint64_t al = (q->start & 15) ? 4 : 16;
            at = (at + al - 1) & ~(al - 1);
            q->at = at;
            at += q->size + (q->ft ? 4 : 0) + q->isl;
            if (q->start == p->fn[q->fn].start) p->fn[q->fn].at = q->at;
        }
    /* p->order: pieces by `at` */
    p->order = calloc((size_t)p->np + 1, sizeof *p->order);
    for (int j = 0; j < p->np; j++) p->order[j] = j;
    for (int a = 1; a < p->np; a++) { int v = p->order[a], b = a; while (b > 0 && p->pc[p->order[b - 1]].at > p->pc[v].at) { p->order[b] = p->order[b - 1]; b--; } p->order[b] = v; }
    p->code_size = at;
    p->eh_off = (at + 7) & ~7ull;
    p->eh_size = eh + 4;
    p->hdr_off = (p->eh_off + p->eh_size + 3) & ~3ull;
    p->hdr_size = 12 + 8 * (uint64_t)p->np;
    p->total = p->hdr_off + p->hdr_size;
    return 0;
}

void rl_plan_free(rl_plan *p)
{
    if (p->pc) for (int j = 0; j < p->np; j++) free(p->pc[j].cfi);
    free(p->fn); free(p->by_start); free(p->pc); free(p->order);
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
static const rl_piece *piece_at(const rl_plan *p, uint64_t v)
{
    const rl_fn *f = fn_at(p, v);
    if (!f) return NULL;
    for (int j = f->p0; j < f->p0 + f->npc; j++)
        if (v >= p->pc[j].start && v < p->pc[j].start + p->pc[j].size) return &p->pc[j];
    return NULL;
}

uint64_t rl_copy_of(const rl_plan *p, uint64_t v, uint64_t dst_run)
{
    const rl_piece *q = piece_at(p, v);
    return q ? dst_run + q->at + (v - q->start) : 0;
}

uint64_t rl_orig_of(const rl_plan *p, uint64_t addr, uint64_t dst_run)
{
    if (addr < dst_run || addr >= dst_run + p->code_size) return 0;
    uint64_t o = addr - dst_run;
    int lo = 0, hi = p->np;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (p->pc[p->order[mid]].at <= o) lo = mid + 1; else hi = mid;
    }
    if (!lo) return 0;
    const rl_piece *q = &p->pc[p->order[lo - 1]];
    if (o >= q->at + q->size + (q->ft ? 4 : 0) + q->isl) return 0;
    /* (the fall-through branch and the islands: the piece's last instruction) */
    return p->im->run + q->start + (o < q->at + q->size ? o - q->at : q->size - 4);
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
    for (int j = 0; j < p->np; j++) {
        const rl_piece *f = &p->pc[j];
        uint64_t isl = f->at + f->size + (f->ft ? 4 : 0);   /* the next free island byte (region offset) */
        if (f->ft) {                                        /* fall through into the function's next piece */
            uint32_t b = A64_B;
            uint64_t from = dst_run + f->at + f->size, to = rl_copy_of(p, f->start + f->size, dst_run);
            if (!to || put26(&b, (int64_t)(to - from)) < 0) { snprintf(err, errn, "fall-through of %llx", (unsigned long long)f->start); return -1; }
            w32(dst + f->at + f->size, b);
        }
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
            if (tv >= f->start && tv < f->start + f->size) { w32(dst + f->at + o, w); continue; }   /* inside the piece: relative */
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
            uint64_t ia = isl, irun = dst_run + ia;
            if (k == K_LIT && lit_prfm(w)) { w32(dst + f->at + o, A64_NOP); continue; }
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
                    ins[ni++] = 0x91000000u | (lo << 10) | (rdg << 5) | rdg;
                } else {
                    uint32_t opc = w >> 30, sz = opc == 1 ? 8 : 4;
                    uint32_t base = opc == 0 ? 0xb9400000u : opc == 1 ? 0xf9400000u : 0xb9800000u;
                    if (lo % sz) { ins[ni++] = 0x91000000u | (lo << 10) | (rdg << 5) | rdg; lo = 0; }
                    ins[ni++] = base | ((lo / sz) << 10) | (rdg << 5) | rdg;
                }
                uint32_t back = A64_B;
                if (put26(&back, (int64_t)((npc + 4) - (irun + 4ull * ni))) < 0) goto range;
                ins[ni++] = back;
                for (uint32_t jj = 0; jj < ni; jj++) w32(dst + ia + 4 * jj, ins[jj]);
                isl += 4ull * ni;
                uint32_t b = A64_B;
                if (put26(&b, (int64_t)(irun - npc)) < 0) goto range;
                w32(dst + f->at + o, b);
            }
            if (isl > f->at + f->size + (f->ft ? 4 : 0) + f->isl) { snprintf(err, errn, "islands of %llx overflow", (unsigned long long)f->start); return -1; }
            continue;
range:
            snprintf(err, errn, "%llx: a field that does not reach (kind %d)", (unsigned long long)pc, k);
            return -1;
        }
    }
    /* .eh_frame: the CIEs, each once, then each piece's FDE (the original's for a whole function) */
    uint64_t q = p->eh_off;
    uint64_t *cie_from = calloc((size_t)p->n + 1, sizeof *cie_from), *cie_to = calloc((size_t)p->n + 1, sizeof *cie_to);
    int ncie = 0;
    if (!cie_from || !cie_to) { free(cie_from); free(cie_to); snprintf(err, errn, "out of memory"); return -1; }
    uint8_t *hdr = dst + p->hdr_off;
    uint64_t hdr_run = dst_run + p->hdr_off;
    for (int oi = 0; oi < p->np; oi++) {
        const rl_piece *pc = &p->pc[p->order[oi]];
        const rl_fn *f = &p->fn[pc->fn];
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
        uint64_t fq = q, flen;
        if (f->npc == 1) {
            flen = fd.len;
            memcpy(dst + fq, im->mem + fd.at, fd.len);
        } else {
            uint64_t hdrl = 8 + 2 * (uint64_t)enc_size(ci.fde_enc), augl = 0;
            if (ci.z) { uint64_t a0 = fd.at + hdrl, a1 = a0; uint64_t al = uleb(im, &a1, fd.at + fd.len); augl = (a1 - a0) + al; }
            flen = pc->fde_len;
            memcpy(dst + fq, im->mem + fd.at, hdrl + augl);
            uint32_t l = (uint32_t)(flen - 4);
            memcpy(dst + fq, &l, 4);
            if (wr_raw(dst + fq + 8 + enc_size(ci.fde_enc), ci.fde_enc & 0x0f, (int64_t)pc->size) < 0) { snprintf(err, errn, "pc_range"); goto fail; }
            memcpy(dst + fq + hdrl + augl, pc->cfi, pc->ncfi);
            /* (the rest: DW_CFA_nop) */
        }
        uint32_t cp = (uint32_t)((fq + 4) - cq);
        memcpy(dst + fq + 4, &cp, 4);
        if (wr_raw(dst + fq + 8, ci.fde_enc, (int64_t)((dst_run + pc->at) - (dst_run + fq + 8))) < 0) { snprintf(err, errn, "pc_begin"); goto fail; }
        if (fd.lsda_field) {
            int64_t raw = rd_raw(im, fd.lsda_field, ci.lsda_enc);
            if (raw) {
                uint64_t tgt = im->run + fd.lsda_field + (uint64_t)raw, fld = fq + (fd.lsda_field - fd.at);
                if (wr_raw(dst + fld, ci.lsda_enc, (int64_t)(tgt - (dst_run + fld))) < 0) { snprintf(err, errn, "LSDA pointer"); goto fail; }
            }
        }
        q += flen;
        int32_t loc = (int32_t)((int64_t)(dst_run + pc->at) - (int64_t)hdr_run), fa = (int32_t)((int64_t)(dst_run + fq) - (int64_t)hdr_run);
        memcpy(hdr + 12 + 8 * (uint64_t)oi, &loc, 4);
        memcpy(hdr + 16 + 8 * (uint64_t)oi, &fa, 4);
    }
    free(cie_from); free(cie_to);
    if (q + 4 > p->eh_off + p->eh_size) { snprintf(err, errn, "unwind data overflow"); return -1; }
    hdr[0] = 1; hdr[1] = 0x1b; hdr[2] = 0x03; hdr[3] = 0x3b;
    int32_t ep = (int32_t)((int64_t)(dst_run + p->eh_off) - (int64_t)(hdr_run + 4));
    uint32_t cnt = (uint32_t)p->np;
    memcpy(hdr + 4, &ep, 4);
    memcpy(hdr + 8, &cnt, 4);
    return 0;
fail:
    free(cie_from); free(cie_to);
    return -1;
}

/* "<start> <size> [<weight>] [h:<o>-<e>,<o>-<e>...]" (hex): 0 and *w filled; -1 not such a line */
int rl_want_parse(const char *line, rl_want *w)
{
    unsigned long long s, z;
    int used = 0;
    if (line[0] == '#' || sscanf(line, "%llx %llx%n", &s, &z, &used) != 2) return -1;
    memset(w, 0, sizeof *w);
    w->start = s;
    const char *h = strstr(line + used, "h:");
    if (!h) return 0;
    h += 2;
    while (*h && w->nhot < RL_MAXHOT) {
        unsigned a, b; int k = 0;
        if (sscanf(h, "%x-%x%n", &a, &b, &k) != 2) break;
        w->hot[2 * w->nhot] = a; w->hot[2 * w->nhot + 1] = b; w->nhot++;
        h += k;
        if (*h != ',') break;
        h++;
    }
    return 0;
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
