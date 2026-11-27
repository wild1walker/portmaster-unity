// pack_relocs [--relr] [--aps2] [--android-relr] [--truncate] in.so out.so
//
// Rewrites the dynamic relocations of a shared library the way newer Android toolchains emit them,
// for the loader's tests (GNU ld 2.34 writes neither; lld does with --pack-dyn-relocs=android+relr).
// ELF32 ARM (REL) or ELF64 AArch64 (RELA), little-endian. Built and run on the build machine by
// build.sh (it was test/pack_relocs.py).
//
//   --relr          relative relocations -> DT_RELR (their addends written in place)
//   --aps2          the rest of DT_REL / DT_RELA -> Android's packed "APS2" table
//                   (DT_ANDROID_REL / DT_ANDROID_RELA)
//   --android-relr  RELR under Android's pre-standard tags (DT_ANDROID_RELR...)
//   --truncate      cut the packed table short: a broken library
//
// The new tables go where the old one was (they are smaller); the dynamic section's spare DT_NULL
// slots take the new tags. The APS2 groups use every flag combination bionic decodes: by offset
// step + info, by info only, ungrouped; addends per relocation, per group, or none (reset to 0).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DT_RELA_ = 7, DT_RELASZ_ = 8, DT_RELAENT_ = 9, DT_REL_ = 17, DT_RELSZ_ = 18, DT_RELENT_ = 19,
    DT_RELRSZ_ = 35, DT_RELR_ = 36, DT_RELRENT_ = 37, DT_RELACOUNT_ = 0x6ffffff9, DT_RELCOUNT_ = 0x6ffffffa,
    DT_ANDROID_REL_ = 0x6000000f, DT_ANDROID_RELSZ_ = 0x60000010, DT_ANDROID_RELA_ = 0x60000011,
    DT_ANDROID_RELASZ_ = 0x60000012, DT_ANDROID_RELR_ = 0x6fffe000, DT_ANDROID_RELRSZ_ = 0x6fffe001,
    DT_ANDROID_RELRENT_ = 0x6fffe003,
};
enum { BY_INFO = 1, BY_OFFSET_DELTA = 2, BY_ADDEND = 4, HAS_ADDEND = 8 };

typedef struct { uint64_t off, info; int64_t add; } Rel;
typedef struct { uint8_t *b; size_t n, cap; } Buf;

static void die(const char *m) { fprintf(stderr, "pack_relocs: %s\n", m); exit(1); }

static void put(Buf *o, uint8_t c)
{
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 256;
        o->b = realloc(o->b, o->cap);
        if (!o->b) die("out of memory");
    }
    o->b[o->n++] = c;
}

static uint64_t rd(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

static void wr(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// signed LEB128 of v taken as a w-byte word (bionic's decoder wraps at the word size)
static void sleb(Buf *o, int64_t v, int w)
{
    if (w == 4) v = (int32_t)(uint32_t)v;
    for (;;) {
        uint8_t b = v & 0x7f;
        v >>= 7;   // (arithmetic: v is signed)
        if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) { put(o, b); return; }
        put(o, b | 0x80);
    }
}

static size_t run(const Rel *r, const int64_t *deltas, size_t n, size_t i, int same_step)
{
    size_t j = i + 1;
    while (j < n && r[j].info == r[i].info && (!same_step || deltas[j] == deltas[i])) j++;
    return j - i;
}

static void aps2(Buf *o, const Rel *r, size_t n, int rela, int w)
{
    int64_t *deltas = malloc((n + 1) * sizeof *deltas);
    size_t (*groups)[3] = malloc((n + 1) * sizeof *groups);   // flags, start, size
    size_t ng = 0;
    uint64_t prev = 0;
    for (size_t i = 0; i < n; i++) { deltas[i] = (int64_t)(r[i].off - prev); prev = r[i].off; }
    for (size_t i = 0; i < n;) {
        size_t a = run(r, deltas, n, i, 1), b;
        if (a >= 3) { groups[ng][0] = BY_INFO | BY_OFFSET_DELTA; groups[ng][1] = i; groups[ng++][2] = a; i += a; continue; }
        b = run(r, deltas, n, i, 0);
        if (b >= 2) { groups[ng][0] = BY_INFO; groups[ng][1] = i; groups[ng++][2] = b; i += b; continue; }
        if (ng && groups[ng - 1][0] == 0 && groups[ng - 1][1] + groups[ng - 1][2] == i) groups[ng - 1][2]++;
        else { groups[ng][0] = 0; groups[ng][1] = i; groups[ng++][2] = 1; }
        i++;
    }
    put(o, 'A'); put(o, 'P'); put(o, 'S'); put(o, '2');
    sleb(o, (int64_t)n, w);
    sleb(o, 0, w);
    int64_t addend = 0;
    for (size_t g = 0; g < ng; g++) {
        size_t flags = groups[g][0], start = groups[g][1], size = groups[g][2];
        int any = 0, same = 1;
        for (size_t k = 0; k < size; k++) {
            if (r[start + k].add) any = 1;
            if (r[start + k].add != r[start].add) same = 0;
        }
        if (rela && any) flags |= HAS_ADDEND | (same ? BY_ADDEND : 0);
        sleb(o, (int64_t)size, w);
        sleb(o, (int64_t)flags, w);
        if (flags & BY_OFFSET_DELTA) sleb(o, deltas[start], w);
        if (flags & BY_INFO) sleb(o, (int64_t)r[start].info, w);
        if (flags & BY_ADDEND) { sleb(o, r[start].add - addend, w); addend = r[start].add; }
        else if (!(flags & HAS_ADDEND)) addend = 0;
        for (size_t k = 0; k < size; k++) {
            if (!(flags & BY_OFFSET_DELTA)) sleb(o, deltas[start + k], w);
            if (!(flags & BY_INFO)) sleb(o, (int64_t)r[start + k].info, w);
            if ((flags & HAS_ADDEND) && !(flags & BY_ADDEND)) { sleb(o, r[start + k].add - addend, w); addend = r[start + k].add; }
        }
    }
    free(deltas);
    free(groups);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

// addresses (even words) and bitmaps of the following 31 / 63 words (odd words)
static size_t relr(uint64_t *out, const uint64_t *offs, size_t n, int w)
{
    uint64_t nbits = 8 * (uint64_t)w - 1;
    size_t m = 0, i = 0;
    while (i < n) {
        out[m++] = offs[i];
        uint64_t where = offs[i] + w;
        i++;
        for (;;) {
            uint64_t bitmap = 0;
            while (i < n) {
                int64_t d = (int64_t)(offs[i] - where);
                if (d < 0 || (uint64_t)d >= nbits * w || d % w) break;
                bitmap |= (uint64_t)1 << (d / w);
                i++;
            }
            if (!bitmap) break;
            out[m++] = bitmap << 1 | 1;
            where += nbits * w;
        }
    }
    return m;
}

typedef struct { uint32_t type; uint64_t off, va, filesz; } Ph;

static long file_offset(const Ph *ph, int nph, uint64_t va, uint64_t size)
{
    for (int i = 0; i < nph; i++)
        if (ph[i].type == 1 && ph[i].va <= va && va + size <= ph[i].va + ph[i].filesz) return (long)(ph[i].off + va - ph[i].va);
    return -1;
}

int main(int argc, char **argv)
{
    int o_relr = 0, o_aps2 = 0, o_arelr = 0, o_trunc = 0, nf = 0;
    const char *files[2] = { 0 };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--relr")) o_relr = 1;
        else if (!strcmp(argv[i], "--aps2")) o_aps2 = 1;
        else if (!strcmp(argv[i], "--android-relr")) o_arelr = 1;
        else if (!strcmp(argv[i], "--truncate")) o_trunc = 1;
        else if (!strncmp(argv[i], "--", 2)) die("unknown option");
        else if (nf < 2) files[nf++] = argv[i];
    }
    if (nf != 2) die("usage: pack_relocs [--relr] [--aps2] [--android-relr] [--truncate] in.so out.so");
    FILE *f = fopen(files[0], "rb");
    if (!f) die("cannot open the input");
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(len);
    if (!data || fread(data, 1, len, f) != (size_t)len) die("cannot read the input");
    fclose(f);
    int is64 = data[4] == 2, w = is64 ? 8 : 4;
    uint16_t machine = (uint16_t)rd(data + 0x12, 2);
    uint64_t relative = machine == 183 ? 1027 : 23;   // R_AARCH64_RELATIVE, R_ARM_RELATIVE
    uint64_t phoff = is64 ? rd(data + 0x20, 8) : rd(data + 0x1c, 4);
    int phentsize = (int)rd(data + (is64 ? 0x36 : 0x2a), 2), phnum = (int)rd(data + (is64 ? 0x38 : 0x2c), 2);
    Ph *ph = calloc(phnum + 1, sizeof *ph);
    for (int i = 0; i < phnum; i++) {
        const uint8_t *p = data + phoff + (uint64_t)i * phentsize;
        ph[i].type = (uint32_t)rd(p, 4);
        if (is64) { ph[i].off = rd(p + 8, 8); ph[i].va = rd(p + 16, 8); ph[i].filesz = rd(p + 32, 8); }
        else { ph[i].off = rd(p + 4, 4); ph[i].va = rd(p + 8, 4); ph[i].filesz = rd(p + 16, 4); }
    }
    int di = -1;
    for (int i = 0; i < phnum; i++) if (ph[i].type == 2) { di = i; break; }
    if (di < 0) die("no dynamic segment");
    uint64_t dynoff = ph[di].off;
    size_t capacity = ph[di].filesz / (2 * w), ndyn = 0;
    int64_t (*dyn)[2] = calloc(capacity + 4, sizeof *dyn);
    for (size_t k = 0; k < capacity; k++) {
        const uint8_t *p = data + dynoff + k * 2 * w;
        int64_t tag = is64 ? (int64_t)rd(p, 8) : (int32_t)(uint32_t)rd(p, 4);
        if (!tag) break;
        dyn[ndyn][0] = tag;
        dyn[ndyn++][1] = (int64_t)rd(p + w, w);
    }
    int rela = 0;
    uint64_t tab_va = 0, tab_sz = 0, have_va = 0;
    for (size_t k = 0; k < ndyn; k++) if (dyn[k][0] == DT_RELA_) rela = 1;
    for (size_t k = 0; k < ndyn; k++) {   // (dict(dyn): the last of a tag counts)
        if (dyn[k][0] == (rela ? DT_RELA_ : DT_REL_)) { tab_va = dyn[k][1]; have_va = 1; }
        if (dyn[k][0] == (rela ? DT_RELASZ_ : DT_RELSZ_)) tab_sz = dyn[k][1];
    }
    if (!have_va) die("no DT_REL or DT_RELA");
    int ent = is64 ? (rela ? 24 : 16) : (rela ? 12 : 8);
    long tab_off = file_offset(ph, phnum, tab_va, tab_sz);
    if (tab_off < 0) die("the relocation table is not in the file");
    size_t nrel = tab_sz / ent, nrest = 0, nrelr = 0;
    Rel *rel = calloc(nrel + 1, sizeof *rel), *rest = calloc(nrel + 1, sizeof *rest);
    uint64_t *relr_off = calloc(nrel + 1, sizeof *relr_off);
    for (size_t k = 0; k < nrel; k++) {
        const uint8_t *p = data + tab_off + k * ent;
        rel[k].off = rd(p, w);
        rel[k].info = rd(p + w, w);
        rel[k].add = rela ? (is64 ? (int64_t)rd(p + 2 * w, 8) : (int32_t)(uint32_t)rd(p + 8, 4)) : 0;
    }
    if (o_relr) {
        for (size_t k = 0; k < nrel; k++) {
            long o = file_offset(ph, phnum, rel[k].off, w);
            if ((rel[k].info & (is64 ? 0xffffffffu : 0xffu)) == relative && rel[k].off % w == 0 && o >= 0) {
                if (rela) wr(data + o, (uint64_t)rel[k].add, w);
                relr_off[nrelr++] = rel[k].off;
            } else rest[nrest++] = rel[k];
        }
    } else {
        memcpy(rest, rel, nrel * sizeof *rel);
        nrest = nrel;
    }
    Buf blob = { 0 }, out = { 0 };
    if (o_aps2 && nrest) aps2(&blob, rest, nrest, rela, w);
    if (o_trunc) blob.n /= 2;
    size_t plain = 0;
    if (!o_aps2) {
        for (size_t k = 0; k < nrest; k++) {
            uint8_t e[24];
            wr(e, rest[k].off, w);
            wr(e + w, rest[k].info, w);
            if (rela) wr(e + 2 * w, (uint64_t)rest[k].add, w);
            for (int i = 0; i < ent; i++) put(&out, e[i]);
        }
        plain = out.n;
    }
    for (size_t i = 0; i < blob.n; i++) put(&out, blob.b[i]);
    while (out.n % w) put(&out, 0);
    qsort(relr_off, nrelr, sizeof *relr_off, cmp_u64);
    uint64_t *rr = calloc(2 * nrelr + 1, sizeof *rr);
    size_t nrr = relr(rr, relr_off, nrelr, w);
    uint64_t relr_va = tab_va + out.n;
    for (size_t k = 0; k < nrr; k++) {
        uint8_t e[8];
        wr(e, rr[k], w);
        for (int i = 0; i < w; i++) put(&out, e[i]);
    }
    if (out.n > tab_sz) {
        fprintf(stderr, "pack_relocs: new tables (%zu bytes) do not fit in %llu\n", out.n, (unsigned long long)tab_sz);
        return 1;
    }
    memset(data + tab_off, 0, tab_sz);
    if (out.n) memcpy(data + tab_off, out.b, out.n);

    int64_t t_tab = rela ? DT_RELA_ : DT_REL_, t_sz = rela ? DT_RELASZ_ : DT_RELSZ_, t_ent = rela ? DT_RELAENT_ : DT_RELENT_;
    int64_t (*nw)[2] = calloc(ndyn + 8, sizeof *nw);
    size_t nn = 0;
    for (size_t k = 0; k < ndyn; k++) {
        int64_t tag = dyn[k][0], val = dyn[k][1];
        if (tag == DT_RELACOUNT_ || tag == DT_RELCOUNT_) continue;
        if (tag == t_tab || tag == t_sz || tag == t_ent) {
            if (!plain) continue;
            if (tag == t_sz) val = (int64_t)plain;
        }
        nw[nn][0] = tag;
        nw[nn++][1] = val;
    }
    if (blob.n) {
        nw[nn][0] = rela ? DT_ANDROID_RELA_ : DT_ANDROID_REL_; nw[nn++][1] = (int64_t)tab_va;
        nw[nn][0] = rela ? DT_ANDROID_RELASZ_ : DT_ANDROID_RELSZ_; nw[nn++][1] = (int64_t)blob.n;
    }
    if (nrr) {
        nw[nn][0] = o_arelr ? DT_ANDROID_RELR_ : DT_RELR_; nw[nn++][1] = (int64_t)relr_va;
        nw[nn][0] = o_arelr ? DT_ANDROID_RELRSZ_ : DT_RELRSZ_; nw[nn++][1] = (int64_t)(nrr * w);
        nw[nn][0] = o_arelr ? DT_ANDROID_RELRENT_ : DT_RELRENT_; nw[nn++][1] = w;
    }
    if (nn >= capacity) die("no spare slots in the dynamic section");
    for (size_t k = 0; k < capacity; k++) {
        uint8_t *p = data + dynoff + k * 2 * w;
        wr(p, k < nn ? (uint64_t)nw[k][0] : 0, w);
        wr(p + w, k < nn ? (uint64_t)nw[k][1] : 0, w);
    }
    f = fopen(files[1], "wb");
    if (!f || fwrite(data, 1, len, f) != (size_t)len || fclose(f)) die("cannot write the output");
    const char *base = strrchr(files[1], '/');
    printf("%s: %zu relocations -> %zu RELR words, %zu APS2 bytes, %zu plain\n", base ? base + 1 : files[1], nrel, nrr, blob.n, plain / ent);
    return 0;
}
