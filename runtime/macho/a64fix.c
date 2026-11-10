// a64fix.c - see a64fix.h.
#include "a64fix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

static const char *const g_names[A64_NKIND] = {
    "LSE ld<op>/st<op>/swp", "LSE cas", "LSE casp", "RCPC ldapr", "RCPC2 ldapur/stlur", "dot product",
    "SHA3 eor3", "SHA3 bcax/rax1/xar", "SHA512", "half-precision FP", "frint32/64", "fjcvtzs",
    "flag manipulation", "sqrdmlah/sqrdmlsh", "fmlal/fmlsl", "crc32", "aes/pmull", "sha1/sha2",
    "complex number", "pointer authentication", "axflag/xaflag", "sb", "PSTATE field" };

const char *a64fix_kind_name(int kind) { return kind >= 0 && kind < A64_NKIND ? g_names[kind] : "?"; }

int a64fix_kind(uint32_t i)
{
    if ((i & 0x3F200C00) == 0x38200000) {                 // atomic memory operations
        uint32_t o3 = (i >> 15) & 1, opc = (i >> 12) & 7;
        if (o3 && opc == 4) return (i & 0x00DF0000) == 0x009F0000 ? A64_LDAPR : -1;
        if (o3 && opc != 0) return -1;
        return A64_LSE;
    }
    if ((i & 0x3FA07C00) == 0x08A07C00) return A64_CAS;
    if ((i & 0xBFA07C00) == 0x08207C00) return A64_CASP;
    if ((i & 0x3F200C00) == 0x19000000) return A64_RCPC2;
    if ((i & 0x9FE0FC00) == 0x0E809400 || (i & 0x9FC0F400) == 0x0F80E000) return A64_DOT;      // vectors, one element
    if ((i & 0xFFE08000) == 0xCE000000) return A64_EOR3;
    if ((i & 0xFFE08000) == 0xCE200000 || (i & 0xFFE0FC00) == 0xCE608C00 || (i & 0xFFE00000) == 0xCE800000) return A64_SHA3;
    if ((i & 0xFFE0B000) == 0xCE608000 || (i & 0xFFFFF000) == 0xCEC08000) return A64_SHA512;
    if ((i & 0xFF200000) == 0x1EE00000) return (i & 0xFFFE7C00) == 0x1EE24000 ? -1 : A64_FP16;   // fcvt from half: ARMv8.0
    if ((i & 0x9F60C400) == 0x0E400400) return A64_FP16;
    if ((i & 0xFF3CFC00) == 0x1E284000 || (i & 0xBFBCFC00) == 0x0E21E800) return A64_FRINT;
    if ((i & 0xFFFFFC00) == 0x1E7E0000) return A64_JSCVT;
    if (i == 0xD500401F || (i & 0xFFE07C10) == 0xBA000400 || (i & 0xFFFFBC1F) == 0x3A00080D) return A64_FLAGM;
    if (i == 0xD500405F || i == 0xD500403F) return A64_FLAGM2;
    if (i == 0xD50330FF) return A64_SB;
    if ((i & 0xFFF8F01F) == 0xD500401F) return A64_PSTATE;
    if ((i & 0xAF20F400) == 0x2E008400 || (i & 0xAF00D400) == 0x2F00D000) return A64_RDM;      // same size, by element
    if ((i & 0xBFA0F400) == 0x0E20EC00 || (i & 0xBFA0F400) == 0x2E20CC00) return A64_FHM;
    if ((i & 0xBF20E400) == 0x2E00C400 || (i & 0xBF20EC00) == 0x2E00E400 || (i & 0xBF009400) == 0x2F001000) return A64_FCMA;
    if ((i & 0x7FE0E000) == 0x1AC04000) return A64_CRC;
    if ((i & 0xFFFFEC00) == 0x4E284800 || (i & 0xBF20FC00) == 0x0E20E000) return A64_AES;
    if ((i & 0xFF208C00) == 0x5E000000 || (i & 0xFFFFEC00) == 0x5E280800 ||
        (i & 0xFFE0BC00) == 0x5E004000 || (i & 0xFFFFFC00) == 0x5E282800) return A64_SHA12;
    if ((i & 0xFFFF0000) == 0xDAC10000 || (i & 0xFFE0FC00) == 0x9AC03000 || (i & 0xFE1FF800) == 0xD61F0800 ||
        (i & 0xFF200400) == 0xF8200400) return A64_PAUTH;                                     // pac*/aut*, pacga, braa/retaa, ldraa
    return -1;
}

void a64fix_init(struct a64fix *f, uint8_t *island, size_t island_bytes)
{
    memset(f, 0, sizeof *f);
    f->island = island;
    f->island_bytes = island_bytes;
    unsigned long h = getauxval(AT_HWCAP), h2 = getauxval(AT_HWCAP2);
    static const struct { int kind; int two; unsigned long bit; } caps[] = {
        { A64_LSE, 0, 1ul << 8 }, { A64_CAS, 0, 1ul << 8 }, { A64_CASP, 0, 1ul << 8 }, { A64_LDAPR, 0, 1ul << 15 },
        { A64_RCPC2, 0, 1ul << 26 }, { A64_DOT, 0, 1ul << 20 }, { A64_EOR3, 0, 1ul << 17 }, { A64_SHA3, 0, 1ul << 17 },
        { A64_SHA512, 0, 1ul << 21 }, { A64_FP16, 0, 1ul << 9 }, { A64_FRINT, 1, 1ul << 8 }, { A64_JSCVT, 0, 1ul << 13 },
        { A64_FLAGM, 0, 1ul << 27 }, { A64_RDM, 0, 1ul << 12 }, { A64_FHM, 0, 1ul << 23 }, { A64_CRC, 0, 1ul << 7 },
        { A64_AES, 0, 1ul << 4 }, { A64_SHA12, 0, 1ul << 6 }, { A64_FCMA, 0, 1ul << 14 }, { A64_PAUTH, 0, 1ul << 30 },
        { A64_FLAGM2, 1, 1ul << 7 }, { A64_SB, 0, 1ul << 29 }, { A64_PSTATE, 0, 1ul << 24 }, { A64_PSTATE, 0, 1ul << 28 } };
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++)
        if (!((caps[i].two ? h2 : h) & caps[i].bit)) f->lacks |= 1u << caps[i].kind;
    const char *e = getenv("UA_A64FIX");
    if (e && strcmp(e, "all") == 0) f->lacks |= 1u << A64_LSE | 1u << A64_CAS | 1u << A64_LDAPR | 1u << A64_EOR3 | 1u << A64_DOT;
    else if (e && strcmp(e, "off") == 0) f->lacks = 0;
}

// ---- the island's code --------------------------------------------------------------------
#define FRAME 176                // past the 128-byte red zone macOS leaf code uses below sp
struct emit { uint32_t *p, *end; };
static void put(struct emit *e, uint32_t w) { if (e->p < e->end) *e->p = w; e->p++; }

static uint32_t a_stp_pre(unsigned a, unsigned b) { return 0xA9800000u | ((uint32_t)(-FRAME / 8) & 0x7f) << 15 | b << 10 | 31u << 5 | a; }
static uint32_t a_ldp_post(unsigned a, unsigned b) { return 0xA8C00000u | (uint32_t)(FRAME / 8) << 15 | b << 10 | 31u << 5 | a; }
static uint32_t a_stp(unsigned a, unsigned b, unsigned off) { return 0xA9000000u | (off / 8) << 15 | b << 10 | 31u << 5 | a; }
static uint32_t a_ldp(unsigned a, unsigned b, unsigned off) { return 0xA9400000u | (off / 8) << 15 | b << 10 | 31u << 5 | a; }
static uint32_t a_ldaxr(unsigned size, unsigned t, unsigned n) { return 0x085FFC00u | size << 30 | n << 5 | t; }
static uint32_t a_stlxr(unsigned size, unsigned s, unsigned t, unsigned n) { return 0x0800FC00u | size << 30 | s << 16 | n << 5 | t; }
static uint32_t a_mov(unsigned sf, unsigned d, unsigned m) { return 0x2A0003E0u | sf << 31 | m << 16 | d; }
static uint32_t a_cbnz(unsigned sf, unsigned t, int words) { return 0x35000000u | sf << 31 | ((uint32_t)words & 0x7ffff) << 5 | t; }
static uint32_t a_b(const uint32_t *from, const uint32_t *to) { return 0x14000000u | ((uint32_t)(to - from) & 0x3ffffff); }

static int reach(const uint32_t *from, const uint32_t *to)
{
    ptrdiff_t d = to - from;
    return d >= -(1 << 25) && d < (1 << 25);
}

// scratch registers: none of the instruction's own
static void scratch(unsigned *r, int n, unsigned a, unsigned b, unsigned c)
{
    static const unsigned pool[] = { 9, 10, 11, 12, 13, 14, 15, 8, 16, 17 };
    int k = 0;
    for (size_t i = 0; i < sizeof pool / sizeof pool[0] && k < n; i++)
        if (pool[i] != a && pool[i] != b && pool[i] != c) r[k++] = pool[i];
}

// r[0] addr, r[1] loaded, r[2] new or compare, r[3] status, r[4] nzcv, r[5] spare
static void prologue(struct emit *e, const unsigned *r, unsigned rn, unsigned *addr)
{
    put(e, a_stp_pre(r[0], r[1]));
    put(e, a_stp(r[2], r[3], 16));
    put(e, a_stp(r[4], r[5], 32));
    *addr = rn;
    if (rn == 31) { put(e, 0x91000000u | FRAME << 10 | 31u << 5 | r[0]); *addr = r[0]; }   // add addr, sp, #FRAME
}

static void epilogue(struct emit *e, const unsigned *r, const uint32_t *back)
{
    put(e, a_ldp(r[4], r[5], 32));
    put(e, a_ldp(r[2], r[3], 16));
    put(e, a_ldp_post(r[0], r[1]));
    put(e, a_b(e->p, back));
}

// ld<op>/swp: loaded = [Rn]; [Rn] = loaded <op> Rs; Rt = loaded
static void emit_lse(struct emit *e, uint32_t i, const uint32_t *back)
{
    unsigned size = i >> 30, rs = (i >> 16) & 31, o3 = (i >> 15) & 1, opc = (i >> 12) & 7, rn = (i >> 5) & 31, rt = i & 31;
    unsigned sf = size == 3, r[6], addr;
    scratch(r, 6, rs, rt, rn);
    prologue(e, r, rn, &addr);
    if (!o3 && opc >= 4) put(e, 0xD53B4200u | r[4]);                 // mrs nzcv
    uint32_t *loop = e->p;
    put(e, a_ldaxr(size, r[1], addr));
    unsigned newv = r[2];
    if (o3) newv = rs;                                               // swp
    else if (opc == 0) put(e, 0x0B000000u | sf << 31 | rs << 16 | r[1] << 5 | r[2]);   // add
    else if (opc == 1) put(e, 0x0A200000u | sf << 31 | rs << 16 | r[1] << 5 | r[2]);   // bic
    else if (opc == 2) put(e, 0x4A000000u | sf << 31 | rs << 16 | r[1] << 5 | r[2]);   // eor
    else if (opc == 3) put(e, 0x2A000000u | sf << 31 | rs << 16 | r[1] << 5 | r[2]);   // orr
    else {
        // smax gt, smin lt, umax hi, umin lo: new = loaded <cond> Rs ? loaded : Rs
        static const unsigned cond[4] = { 0xC, 0xB, 0x8, 0x3 };
        unsigned x = r[1], y = rs;
        if (size < 2) {
            uint32_t ext = opc < 6 ? (size ? 0x13003C00u : 0x13001C00u)     // sxth, sxtb
                                   : (size ? 0x12003C00u : 0x12001C00u);    // and #0xffff, #0xff
            if (opc < 6) { put(e, ext | r[1] << 5 | r[2]); x = r[2]; }
            put(e, ext | rs << 5 | r[3]);
            y = r[3];
        }
        put(e, 0x6B00001Fu | sf << 31 | y << 16 | x << 5);                          // cmp x, y
        put(e, 0x1A800000u | sf << 31 | rs << 16 | cond[opc - 4] << 12 | r[1] << 5 | r[2]);   // csel
    }
    put(e, a_stlxr(size, r[3], newv, addr));
    put(e, a_cbnz(0, r[3], (int)(loop - e->p)));
    if (!o3 && opc >= 4) put(e, 0xD51B4200u | r[4]);                 // msr nzcv
    if (rt != 31) put(e, a_mov(sf, rt, r[1]));
    epilogue(e, r, back);
}

// cas: loaded = [Rn]; if (loaded == Rs) [Rn] = Rt; Rs = loaded
static void emit_cas(struct emit *e, uint32_t i, const uint32_t *back)
{
    unsigned size = i >> 30, rs = (i >> 16) & 31, rn = (i >> 5) & 31, rt = i & 31;
    unsigned sf = size == 3, r[6], addr;
    scratch(r, 6, rs, rt, rn);
    prologue(e, r, rn, &addr);
    uint32_t *loop = e->p;
    put(e, a_ldaxr(size, r[1], addr));
    unsigned want = rs;
    if (size < 2) { put(e, (size ? 0x12003C00u : 0x12001C00u) | rs << 5 | r[2]); want = r[2]; }
    put(e, 0x4A000000u | sf << 31 | want << 16 | r[1] << 5 | r[2]);  // eor: 0 when equal
    put(e, a_cbnz(sf, r[2], 3));
    put(e, a_stlxr(size, r[3], rt, addr));
    put(e, a_cbnz(0, r[3], (int)(loop - e->p)));
    if (rs != 31) put(e, a_mov(sf, rs, r[1]));
    epilogue(e, r, back);
}

// eor3 Vd, Vn, Vm, Va: the operand written last must not be Vd
static void emit_eor3(struct emit *e, uint32_t i, const uint32_t *back)
{
    unsigned v[3] = { (i >> 5) & 31, (i >> 16) & 31, (i >> 10) & 31 }, d = i & 31;
    int last = -1;
    for (int k = 2; k >= 0 && last < 0; k--) if (v[k] != d) last = k;
    if (last >= 0) {                                                 // all three Vd: d ^ d ^ d = d
        unsigned a = v[(last + 1) % 3], b = v[(last + 2) % 3];
        put(e, 0x6E201C00u | b << 16 | a << 5 | d);
        put(e, 0x6E201C00u | v[last] << 16 | d << 5 | d);
    }
    put(e, a_b(e->p, back));
}

// sdot/udot Vd, Vn, Vm (or one four-byte element of Vm for every lane): each 32-bit lane of Vd
// gains the sum of its four byte products
static void emit_dot(struct emit *e, uint32_t i, const uint32_t *back)
{
    unsigned q = (i >> 30) & 1, u = (i >> 29) & 1, elem = (i >> 24) & 1, rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
    unsigned t[3], k = 0;
    for (unsigned r = 16; k < 3; r++) if (r != rd && r != rn && r != rm) t[k++] = r;
    put(e, 0xAD800000u | ((uint32_t)(-FRAME / 16) & 0x7f) << 15 | t[1] << 10 | 31u << 5 | t[0]);   // stp q, q, [sp, #-FRAME]!
    put(e, 0x3D800000u | 2u << 10 | 31u << 5 | t[2]);                                              // str q, [sp, #32]
    unsigned m = rm;
    if (elem) {
        unsigned idx = ((i >> 11) & 1) << 1 | ((i >> 21) & 1);
        put(e, 0x4E000400u | (idx << 3 | 4) << 16 | rm << 5 | t[2]);     // dup .4s, Vm.s[idx]
        m = t[2];
    }
    uint32_t mull = u ? 0x2E20C000u : 0x0E20C000u, addlp = u ? 0x6E602800u : 0x4E602800u;
    put(e, mull | m << 16 | rn << 5 | t[0]);                             // the low eight bytes' products
    put(e, addlp | t[0] << 5 | t[0]);                                    // by twos
    if (q) {
        put(e, mull | 1u << 30 | m << 16 | rn << 5 | t[1]);              // the high eight
        put(e, addlp | t[1] << 5 | t[1]);
    }
    put(e, 0x4EA0BC00u | (q ? t[1] : t[0]) << 16 | t[0] << 5 | t[0]);    // addp: by fours
    put(e, 0x0EA08400u | q << 30 | t[0] << 16 | rd << 5 | rd);           // a 64-bit Vd: its top half cleared
    put(e, 0x3DC00000u | 2u << 10 | 31u << 5 | t[2]);
    put(e, 0xACC00000u | (uint32_t)(FRAME / 16) << 15 | t[1] << 10 | 31u << 5 | t[0]);
    put(e, a_b(e->p, back));
}

int a64fix_code(struct a64fix *f, uint8_t *code, size_t size, const uint8_t *const *skip, int nskip)
{
    uint32_t *w = (uint32_t *)code, *end = w + size / 4;
    int s = 0;
    for (; w < end; w++) {
        while (s < nskip && (const uint8_t *)w >= skip[2 * s + 1]) s++;
        if (s < nskip && (const uint8_t *)w >= skip[2 * s]) continue;
        uint32_t i = *w;
        int k = a64fix_kind(i);
        if (k < 0) continue;
        f->found[k]++;
        if (!(f->lacks & 1u << k)) continue;
        if (k == A64_LDAPR) { *w = 0x08DFFC00u | (i & 0xC0000000u) | (i & 0x3ff); f->rewritten++; continue; }
        if (k != A64_LSE && k != A64_CAS && k != A64_EOR3 && k != A64_DOT) {
            if (!f->refused++) snprintf(f->refused_what, sizeof f->refused_what, "%s (0x%08x at +0x%zx)", g_names[k], i, (size_t)((uint8_t *)w - code));
            continue;
        }
        struct emit e = { (uint32_t *)(f->island + f->island_used), (uint32_t *)(f->island + f->island_bytes) };
        uint32_t *start = e.p;
        if (!f->island || !reach(w, start) || !reach(start + 40, w + 1)) return -1;
        if (k == A64_LSE) emit_lse(&e, i, w + 1);
        else if (k == A64_CAS) emit_cas(&e, i, w + 1);
        else if (k == A64_DOT) emit_dot(&e, i, w + 1);
        else emit_eor3(&e, i, w + 1);
        if (e.p > e.end) return -1;
        f->island_used += (size_t)((uint8_t *)e.p - (uint8_t *)start);
        __builtin___clear_cache((char *)start, (char *)e.p);
        *w = a_b(w, start);
        f->rewritten++;
    }
    return 0;
}
