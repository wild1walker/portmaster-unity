// macho.c - see macho.h. What is read: thin or universal files' arm64 slice, MH_DYLIB and
// MH_BUNDLE, LC_SEGMENT_64, LC_SYMTAB, LC_DYLD_INFO(_ONLY) or LC_DYLD_CHAINED_FIXUPS with
// LC_DYLD_EXPORTS_TRIE (the plain 64-bit pointer formats), initializers as pointers and as
// offsets. What is refused with a message, so a build made another way never half-loads:
// arm64e's signed pointers, text relocations, encrypted segments, exports that name a
// resolver function. Thread-local
// variables' sections are kept as the file has them: tlv.c gives each thread its copy.
#define _GNU_SOURCE
#include "macho.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define FAT_MAGIC 0xcafebabeu
#define FAT_MAGIC_64 0xcafebabfu
#define MH_MAGIC_64 0xfeedfacfu
#define CPU_TYPE_ARM64 0x0100000cu
#define MH_DYLIB 6
#define MH_BUNDLE 8
#define LC_SEGMENT_64 0x19u
#define LC_SYMTAB 0x2u
#define LC_LOAD_DYLIB 0xcu
#define LC_ID_DYLIB 0xdu
#define LC_LOAD_WEAK_DYLIB 0x80000018u
#define LC_REEXPORT_DYLIB 0x8000001fu
#define LC_LOAD_UPWARD_DYLIB 0x80000023u
#define LC_DYLD_INFO 0x22u
#define LC_DYLD_INFO_ONLY 0x80000022u
#define LC_DATA_IN_CODE 0x29u
#define LC_ENCRYPTION_INFO_64 0x2cu
#define LC_DYLD_EXPORTS_TRIE 0x80000033u
#define LC_DYLD_CHAINED_FIXUPS 0x80000034u
#define S_ZEROFILL 0x1u
#define S_MOD_INIT_FUNC_POINTERS 0x9u
#define S_THREAD_LOCAL_REGULAR 0x11u
#define S_THREAD_LOCAL_INIT_FUNCTION_POINTERS 0x15u
#define S_INIT_FUNC_OFFSETS 0x16u

static int fail(char *err, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (err && n) vsnprintf(err, n, fmt, ap);
    va_end(ap);
    return -1;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint16_t le16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t le64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

// LEB128 inside [*p, end); *bad is set when the stream ends first
static uint64_t uleb(const uint8_t **p, const uint8_t *end, int *bad)
{
    uint64_t v = 0;
    for (int shift = 0; ; shift += 7) {
        if (*p >= end || shift > 63) { *bad = 1; return 0; }
        uint8_t b = *(*p)++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) return v;
    }
}

static int64_t sleb(const uint8_t **p, const uint8_t *end, int *bad)
{
    int64_t v = 0;
    for (int shift = 0; ; ) {
        if (*p >= end || shift > 63) { *bad = 1; return 0; }
        uint8_t b = *(*p)++;
        v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if ((b & 0x40) && shift < 64) v |= -((int64_t)1 << shift);
            return v;
        }
    }
}

// the address of offset off in segment seg, with room for a pointer; NULL: outside the image
static uint8_t *seg_at(struct macho_image *m, unsigned seg, uint64_t off)
{
    if (seg >= (unsigned)m->nseg || off > m->seg[seg].vmsize || m->seg[seg].vmsize - off < 8) return NULL;
    return m->seg[seg].addr + off;
}

static int rebase(struct macho_image *m, const uint8_t *p, size_t n, char *err, size_t errlen)
{
    const uint8_t *end = p + n;
    unsigned seg = 0, type = 1;
    uint64_t off = 0;
    int bad = 0;
#define REBASE_ONE() do { \
        uint8_t *a = seg_at(m, seg, off); \
        if (!a) return fail(err, errlen, "rebase outside the image (segment %u + 0x%llx)", seg, (unsigned long long)off); \
        uint64_t v; memcpy(&v, a, 8); v += (uint64_t)m->slide; memcpy(a, &v, 8); \
        m->nrebased++; off += 8; \
    } while (0)
    while (p < end && !bad) {
        uint8_t op = *p & 0xf0, imm = *p & 0x0f;
        p++;
        switch (op) {
        case 0x00: return 0;
        case 0x10: type = imm; if (type != 1) return fail(err, errlen, "rebase type %u (text relocations are not read)", type); break;
        case 0x20: seg = imm; off = uleb(&p, end, &bad); break;
        case 0x30: off += uleb(&p, end, &bad); break;
        case 0x40: off += (uint64_t)imm * 8; break;
        case 0x50: for (unsigned i = 0; i < imm; i++) REBASE_ONE(); break;
        case 0x60: { uint64_t c = uleb(&p, end, &bad); for (uint64_t i = 0; i < c && !bad; i++) REBASE_ONE(); break; }
        case 0x70: REBASE_ONE(); off += uleb(&p, end, &bad); break;
        case 0x80: {
            uint64_t c = uleb(&p, end, &bad), skip = uleb(&p, end, &bad);
            for (uint64_t i = 0; i < c && !bad; i++) { REBASE_ONE(); off += skip; }
            break;
        }
        default: return fail(err, errlen, "rebase opcode 0x%02x", op);
        }
    }
#undef REBASE_ONE
    return bad ? fail(err, errlen, "the rebase stream is cut") : 0;
}

enum { BIND_NORMAL, BIND_LAZY, BIND_WEAK };

static int bind(struct macho_image *m, const uint8_t *p, size_t n, int kind, const char **libs, int nlibs,
                macho_resolve_fn resolve, void *ctx, char *err, size_t errlen)
{
    const uint8_t *end = p + n;
    unsigned seg = 0, type = 1, flags = 0;
    int64_t ordinal = 0, addend = 0;
    uint64_t off = 0;
    const char *name = NULL;
    int bad = 0;
    // one lookup per name run: the streams bind a symbol's every use in a row
    const char *last = NULL;
    void *last_addr = NULL;
#define BIND_ONE() do { \
        uint8_t *a = seg_at(m, seg, off); \
        if (!a || !name) return fail(err, errlen, "bind outside the image (segment %u + 0x%llx)", seg, (unsigned long long)off); \
        if (type != 1) return fail(err, errlen, "bind type %u for %s (text relocations are not read)", type, name); \
        if (name != last) { \
            const char *lib = kind == BIND_WEAK ? "" : ordinal >= 1 && ordinal <= nlibs ? libs[ordinal - 1] : ""; \
            last_addr = kind != BIND_WEAK && ordinal == 0 ? macho_export(m, name) : NULL;   /* the image's own */ \
            if (!last_addr) last_addr = resolve(name, lib, ctx); \
            last = name; \
            if (!last_addr && kind != BIND_WEAK) { \
                if (flags & 1) m->nbound++; \
                else { m->nmissing++; if (m->nmissing <= 40) fprintf(stderr, "[macho] %s: no %s (%s)\n", m->path, name, lib); } \
            } else if (last_addr) m->nbound++; \
        } \
        if (last_addr) { uint64_t v = (uint64_t)(uintptr_t)last_addr + (uint64_t)addend; memcpy(a, &v, 8); } \
        else if (kind != BIND_WEAK) { uint64_t v = 0; memcpy(a, &v, 8); } \
        off += 8; \
    } while (0)
    while (p < end && !bad) {
        uint8_t op = *p & 0xf0, imm = *p & 0x0f;
        p++;
        switch (op) {
        case 0x00: if (kind != BIND_LAZY) return 0; break;       // lazy: one entry ends, the next follows
        case 0x10: ordinal = imm; break;
        case 0x20: ordinal = (int64_t)uleb(&p, end, &bad); break;
        case 0x30: ordinal = imm ? (int8_t)(0xf0 | imm) : 0; break;   // self, main executable, flat lookup
        case 0x40: {
            flags = imm;
            name = (const char *)p;
            const uint8_t *z = memchr(p, 0, (size_t)(end - p));
            if (!z) { bad = 1; break; }
            p = z + 1;
            break;
        }
        case 0x50: type = imm; break;
        case 0x60: addend = sleb(&p, end, &bad); break;
        case 0x70: seg = imm; off = uleb(&p, end, &bad); break;
        case 0x80: off += uleb(&p, end, &bad); break;
        case 0x90: BIND_ONE(); break;
        case 0xa0: BIND_ONE(); off += uleb(&p, end, &bad); break;
        case 0xb0: BIND_ONE(); off += (uint64_t)imm * 8; break;
        case 0xc0: {
            uint64_t c = uleb(&p, end, &bad), skip = uleb(&p, end, &bad);
            for (uint64_t i = 0; i < c && !bad; i++) { BIND_ONE(); off += skip; }
            break;
        }
        default: return fail(err, errlen, "bind opcode 0x%02x", op);
        }
    }
#undef BIND_ONE
    return bad ? fail(err, errlen, "a bind stream is cut") : 0;
}

// ---- chained fixups: what the linker writes for macOS 12 and later ------------------------
// Every pointer the image wants set is a link of a chain that runs through the data itself:
// the table gives each page's first link, a link says how far the next one is (in steps of 4
// bytes) and what it is, a rebase with its target or a bind with an index into the import
// table. One import is one name, however many links use it.

struct macho_weak_use {
    uint8_t    *at;
    const char *name;            // in __LINKEDIT
    uint64_t    addend;
    uint8_t     unbound;         // no definition was found at load (the pointer is 0)
    uint8_t     optional;        // a weak import: 0 is an answer
};

struct chain_import {
    void       *addr;
    const char *name;
    uint64_t    addend;
    uint8_t     coalesced, optional;
};

#define CHAIN_PTR_64 2u              // a rebase's target is the address the file was linked for
#define CHAIN_PTR_64_OFFSET 6u       // ... the offset from the header

static int chained(struct macho_image *m, const uint8_t *b, size_t n, const char **libs, int nlibs,
                   macho_resolve_fn resolve, void *ctx, char *err, size_t errlen)
{
    if (n < 28) return fail(err, errlen, "%s: chained fixups: the header is cut", m->path);
    uint32_t starts = le32(b + 4), imports = le32(b + 8), symbols = le32(b + 12), count = le32(b + 16), format = le32(b + 20);
    if (le32(b) != 0) return fail(err, errlen, "%s: chained fixups of version %u", m->path, le32(b));
    if (le32(b + 24) != 0) return fail(err, errlen, "%s: chained fixups: compressed import names", m->path);
    size_t each = format == 1 ? 4 : format == 2 ? 8 : format == 3 ? 16 : 0;
    if (!each) return fail(err, errlen, "%s: chained fixups: import format %u", m->path, format);
    if (imports > n || (uint64_t)count * each > n - imports || symbols > n || starts > n || n - starts < 4)
        return fail(err, errlen, "%s: chained fixups: a table outside the data", m->path);
    if (count > 0xffffff) return fail(err, errlen, "%s: chained fixups: %u imports", m->path, count);
    struct chain_import *imp = calloc(count ? count : 1, sizeof *imp);
    if (!imp) return fail(err, errlen, "out of memory");
    int r = -1;
    uint32_t uses = 0;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = b + imports + (size_t)i * each;
        int64_t ordinal;
        uint64_t name_off;
        struct chain_import *x = &imp[i];
        if (format == 3) {
            uint64_t v = le64(e);
            ordinal = v & 0xffff; x->optional = v >> 16 & 1; name_off = v >> 32; x->addend = le64(e + 8);
            if (ordinal > 0xfff0) ordinal -= 0x10000;           // the top values are the special ones, -1 to -3
        } else {
            uint32_t v = le32(e);
            ordinal = v & 0xff; x->optional = v >> 8 & 1; name_off = v >> 9;
            if (ordinal > 0xf0) ordinal -= 0x100;
            if (format == 2) x->addend = (uint64_t)(int64_t)(int32_t)le32(e + 4);
        }
        if (name_off >= n - symbols || !memchr(b + symbols + name_off, 0, n - symbols - name_off)) {
            fail(err, errlen, "%s: chained fixups: the name of import %u", m->path, i); goto out;
        }
        x->name = (const char *)b + symbols + name_off;
        const char *lib = "";
        if (ordinal >= 1 && ordinal <= nlibs) lib = libs[ordinal - 1];
        else if (ordinal == -3) x->coalesced = 1;
        else if (ordinal > 0 || ordinal < -3) { fail(err, errlen, "%s: %s is asked of library %lld of %d", m->path, x->name, (long long)ordinal, nlibs); goto out; }
        if (ordinal == 0 || ordinal == -3) x->addr = macho_export(m, x->name);      // the image's own
        if (!x->addr) x->addr = resolve(x->name, lib, ctx);
        if (x->addr || x->optional) m->nbound++;
        else if (!x->coalesced) {            // a coalesced name's definition may be in an image that loads after this one
            m->nmissing++;
            if (m->nmissing <= 40) fprintf(stderr, "[macho] %s: no %s (%s)\n", m->path, x->name, lib);
        }
    }

    const uint8_t *st = b + starts;
    size_t room = n - starts;
    uint32_t nseg = le32(st);
    if ((uint64_t)nseg * 4 > room - 4) { fail(err, errlen, "%s: chained fixups: %u segments", m->path, nseg); goto out; }
    // two passes over the chains: how many uses of coalesced names there are, then the fixups
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1 && uses) {
            m->weak_uses = calloc(uses, sizeof *m->weak_uses);
            if (!m->weak_uses) { fail(err, errlen, "out of memory"); goto out; }
        }
        for (uint32_t k = 0; k < nseg; k++) {
            uint32_t o = le32(st + 4 + 4 * (size_t)k);
            if (!o) continue;                                   // a segment with nothing to fix
            if (o > room || room - o < 22) { fail(err, errlen, "%s: chained fixups: segment %u's table", m->path, k); goto out; }
            const uint8_t *s = st + o;
            uint32_t size = le32(s);
            unsigned page_size = le16(s + 4), pf = le16(s + 6), pages = le16(s + 20);
            uint64_t seg_off = le64(s + 8);
            if (size < 22 || size > room - o || 22 + (uint64_t)pages * 2 > size || !page_size) { fail(err, errlen, "%s: chained fixups: segment %u's table", m->path, k); goto out; }
            if (pf != CHAIN_PTR_64 && pf != CHAIN_PTR_64_OFFSET) {
                fail(err, errlen, "%s: chained fixups in pointer format %u (the plain 64-bit ones, 2 and 6, are read; arm64e's signed pointers are not)", m->path, pf); goto out;
            }
            struct macho_segment *sg = NULL;
            for (int j = 0; j < m->nseg; j++)
                if (m->seg[j].vmsize && m->seg[j].addr != m->linkedit && m->seg[j].addr == m->header + seg_off) sg = &m->seg[j];
            if (!sg) { fail(err, errlen, "%s: chained fixups: no segment at offset 0x%llx", m->path, (unsigned long long)seg_off); goto out; }
            for (unsigned p = 0; p < pages; p++) {
                unsigned first = le16(s + 22 + 2 * (size_t)p);
                if (first == 0xffff) continue;                  // a page with nothing to fix
                if (first & 0x8000) { fail(err, errlen, "%s: chained fixups: several chains in a page of %s (a 32-bit form)", m->path, sg->name); goto out; }
                uint64_t off = (uint64_t)p * page_size + first;
                for (;;) {
                    if (off > sg->vmsize || sg->vmsize - off < 8) { fail(err, errlen, "%s: chained fixups: a chain leaves %s at 0x%llx", m->path, sg->name, (unsigned long long)off); goto out; }
                    uint8_t *at = sg->addr + off;
                    uint64_t raw = le64(at), v;
                    unsigned next = raw >> 51 & 0xfff;
                    if (raw >> 63) {
                        uint32_t ord = raw & 0xffffff;
                        if (ord >= count) { fail(err, errlen, "%s: chained fixups: import %u of %u in %s", m->path, ord, count, sg->name); goto out; }
                        const struct chain_import *x = &imp[ord];
                        uint64_t addend = x->addend + (raw >> 24 & 0xff);
                        v = x->addr ? (uint64_t)(uintptr_t)x->addr + addend : 0;
                        if (x->coalesced) {
                            if (pass == 1) {
                                struct macho_weak_use *u = &m->weak_uses[m->nweak_uses++];
                                u->at = at; u->name = x->name; u->addend = addend;
                                u->unbound = !x->addr; u->optional = x->optional;
                            } else uses++;
                        }
                    } else {
                        uint64_t target = raw & 0xfffffffffULL, high8 = raw >> 36 & 0xff;
                        v = (pf == CHAIN_PTR_64_OFFSET ? (uint64_t)(uintptr_t)m->header + target : target + (uint64_t)m->slide) | high8 << 56;
                        if (pass == 1) m->nrebased++;
                    }
                    if (pass == 1) memcpy(at, &v, 8);
                    if (!next) break;
                    off += (uint64_t)next * 4;
                }
            }
        }
    }
    r = 0;
out:
    free(imp);
    return r;
}

static void copy16(char *dst, const uint8_t *src) { memcpy(dst, src, 16); dst[16] = 0; }

// A file's arm64 slice (the whole of a thin file): where it lies, its header's flags, its load
// commands. The file stays open.
struct slice {
    int      fd;
    uint64_t off, size;
    uint32_t flags, ncmds, sizeofcmds;
    uint8_t *cmds;
};

static int open_slice(const char *path, struct slice *s, char *err, size_t errlen)
{
    memset(s, 0, sizeof *s);
    s->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (s->fd < 0) return fail(err, errlen, "%s: %s", path, strerror(errno));
    struct stat st;
    uint8_t h[4096];
    ssize_t got = pread(s->fd, h, sizeof h, 0);
    if (fstat(s->fd, &st) != 0 || got < 32) { fail(err, errlen, "%s: not a Mach-O file", path); goto bad; }

    // the arm64 slice of a universal file
    s->size = (uint64_t)st.st_size;
    uint32_t magic = be32(h);
    if (magic == FAT_MAGIC || magic == FAT_MAGIC_64) {
        uint32_t n = be32(h + 4), rec = magic == FAT_MAGIC ? 20 : 32;
        int found = 0;
        for (uint32_t i = 0; i < n && 8 + (uint64_t)(i + 1) * rec <= (uint64_t)got; i++) {
            const uint8_t *a = h + 8 + i * rec;
            if (be32(a) != CPU_TYPE_ARM64) continue;
            s->off = magic == FAT_MAGIC ? be32(a + 8) : be64(a + 8);
            s->size = magic == FAT_MAGIC ? be32(a + 12) : be64(a + 16);
            found = 1;
            break;
        }
        if (!found) { fail(err, errlen, "%s: no arm64 slice (an Intel-only build)", path); goto bad; }
        if (s->off + s->size > (uint64_t)st.st_size || pread(s->fd, h, 32, (off_t)s->off) != 32) {
            fail(err, errlen, "%s: the arm64 slice is cut", path); goto bad;
        }
    }
    if (le32(h) != MH_MAGIC_64 || le32(h + 4) != CPU_TYPE_ARM64) { fail(err, errlen, "%s: not arm64 Mach-O", path); goto bad; }
    uint32_t filetype = le32(h + 12);
    s->ncmds = le32(h + 16);
    s->sizeofcmds = le32(h + 20);
    s->flags = le32(h + 24);
    if (filetype != MH_DYLIB && filetype != MH_BUNDLE) { fail(err, errlen, "%s: file type %u (a dylib or a bundle is read)", path, filetype); goto bad; }
    if (s->sizeofcmds > (64u << 20) || 32 + (uint64_t)s->sizeofcmds > s->size) { fail(err, errlen, "%s: load commands of %u bytes", path, s->sizeofcmds); goto bad; }
    s->cmds = malloc(s->sizeofcmds ? s->sizeofcmds : 1);
    if (!s->cmds || pread(s->fd, s->cmds, s->sizeofcmds, (off_t)s->off + 32) != (ssize_t)s->sizeofcmds) { fail(err, errlen, "%s: load commands cut", path); goto bad; }
    return 0;
bad:
    free(s->cmds);
    close(s->fd);
    return -1;
}

int macho_probe(const char *path)
{
    uint8_t h[4];
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int is = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && pread(fd, h, 4, 0) == 4 &&
             (le32(h) == MH_MAGIC_64 || be32(h) == FAT_MAGIC || be32(h) == FAT_MAGIC_64);
    close(fd);
    return is;
}

int macho_needed(const char *path, void (*cb)(const char *name, int weak, void *ctx), void *ctx, char *err, size_t errlen)
{
    struct slice sl;
    if (open_slice(path, &sl, err, errlen) != 0) return -1;
    int r = 0;
    const uint8_t *c = sl.cmds, *end = sl.cmds + sl.sizeofcmds;
    for (uint32_t i = 0; i < sl.ncmds; i++) {
        if ((size_t)(end - c) < 8) { r = fail(err, errlen, "%s: load command %u cut", path, i); break; }
        uint32_t cmd = le32(c), size = le32(c + 4);
        if (size < 8 || size > (size_t)(end - c)) { r = fail(err, errlen, "%s: load command %u of %u bytes", path, i, size); break; }
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB || cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) {
            uint32_t o = size >= 12 ? le32(c + 8) : size;
            if (o >= size || !memchr(c + o, 0, size - o)) { r = fail(err, errlen, "%s: dylib command", path); break; }
            cb((const char *)c + o, cmd == LC_LOAD_WEAK_DYLIB, ctx);
        }
        c += size;
    }
    free(sl.cmds);
    close(sl.fd);
    return r;
}

struct macho_image *macho_load(const char *path, size_t island, macho_resolve_fn resolve, void *ctx,
                               char *err, size_t errlen)
{
    struct macho_image *m = NULL;
    uint8_t *reserve = MAP_FAILED;
    size_t reserve_len = 0;
    struct slice sl;
    if (open_slice(path, &sl, err, errlen) != 0) return NULL;
    int fd = sl.fd;
    uint8_t *cmds = sl.cmds;
    uint64_t slice = sl.off, slice_size = sl.size;
    uint32_t ncmds = sl.ncmds, sizeofcmds = sl.sizeofcmds;
    m = calloc(1, sizeof *m);
    if (!m) { fail(err, errlen, "out of memory"); goto out; }
    snprintf(m->path, sizeof m->path, "%s", path);
    m->flags = sl.flags;

    // first pass: the segments' span, what the image needs
    struct { uint64_t vmaddr, vmsize, fileoff, filesize; uint32_t initprot; const uint8_t *cmd; } sg[16];
    const char *libs[64];
    int nlibs = 0;
    const uint8_t *dyld_info = NULL, *symtab = NULL, *dice = NULL, *fixups = NULL, *trie = NULL;
    uint64_t lo = UINT64_MAX, hi = 0;
    const uint8_t *c = cmds;
    for (uint32_t i = 0; i < ncmds; i++) {
        if ((size_t)(cmds + sizeofcmds - c) < 8) { fail(err, errlen, "%s: load command %u cut", path, i); goto bad; }
        uint32_t cmd = le32(c), size = le32(c + 4);
        if (size < 8 || size > (size_t)(cmds + sizeofcmds - c)) { fail(err, errlen, "%s: load command %u of %u bytes", path, i, size); goto bad; }
        switch (cmd) {
        case LC_SEGMENT_64: {
            if (size < 72 || m->nseg == 16) { fail(err, errlen, "%s: segment command", path); goto bad; }
            int k = m->nseg++;
            sg[k].vmaddr = le64(c + 24); sg[k].vmsize = le64(c + 32);
            sg[k].fileoff = le64(c + 40); sg[k].filesize = le64(c + 48);
            sg[k].initprot = le32(c + 60); sg[k].cmd = c;
            copy16(m->seg[k].name, c + 8);
            if (sg[k].filesize > sg[k].vmsize || sg[k].fileoff + sg[k].filesize > slice_size) { fail(err, errlen, "%s: segment %s is cut", path, m->seg[k].name); goto bad; }
            if (sg[k].vmsize) {
                if (sg[k].vmaddr < lo) lo = sg[k].vmaddr;
                if (sg[k].vmaddr + sg[k].vmsize > hi) hi = sg[k].vmaddr + sg[k].vmsize;
            }
            uint32_t nsects = le32(c + 64);
            if (72 + (uint64_t)nsects * 80 > size) { fail(err, errlen, "%s: sections of %s", path, m->seg[k].name); goto bad; }
            for (uint32_t s = 0; s < nsects; s++) {
                const uint8_t *sc = c + 72 + s * 80;
                uint32_t fl = le32(sc + 64);
                if (m->nsect == 64) { fail(err, errlen, "%s: more than 64 sections", path); goto bad; }
                struct macho_section *x = &m->sect[m->nsect++];
                copy16(x->name, sc); copy16(x->seg, sc + 16);
                x->addr = (uint8_t *)(uintptr_t)le64(sc + 32);      // vmaddr for now
                x->size = le64(sc + 40);
                x->align = le32(sc + 52);
                x->flags = fl;
                if (x->align > 30) { fail(err, errlen, "%s: section %s aligned to 2^%u", path, x->name, x->align); goto bad; }
                if (le64(sc + 32) < sg[k].vmaddr || le64(sc + 32) + x->size > sg[k].vmaddr + sg[k].vmsize) { fail(err, errlen, "%s: section %s outside its segment", path, x->name); goto bad; }
            }
            break;
        }
        case LC_SYMTAB: if (size >= 24) symtab = c; break;
        case LC_DATA_IN_CODE: if (size >= 16) dice = c; break;
        case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY: if (size >= 48) dyld_info = c; break;
        case LC_ID_DYLIB: {
            uint32_t o = le32(c + 8);
            if (o >= size || !memchr(c + o, 0, size - o)) { fail(err, errlen, "%s: dylib id command", path); goto bad; }
            const char *nm = (const char *)c + o, *sl = strrchr(nm, '/');
            snprintf(m->install_name, sizeof m->install_name, "%s", sl ? sl + 1 : nm);
            break;
        }
        case LC_LOAD_DYLIB: case LC_LOAD_WEAK_DYLIB: case LC_REEXPORT_DYLIB: case LC_LOAD_UPWARD_DYLIB: {
            uint32_t o = le32(c + 8);
            if (o >= size || !memchr(c + o, 0, size - o) || nlibs == 64) { fail(err, errlen, "%s: dylib command", path); goto bad; }
            const char *nm = (const char *)c + o, *sl = strrchr(nm, '/');
            libs[nlibs++] = sl ? sl + 1 : nm;
            break;
        }
        case LC_DYLD_CHAINED_FIXUPS: if (size >= 16) fixups = c; break;
        case LC_DYLD_EXPORTS_TRIE: if (size >= 16) trie = c; break;
        case LC_ENCRYPTION_INFO_64:
            if (size >= 20 && le32(c + 16)) { fail(err, errlen, "%s: encrypted", path); goto bad; }
            break;
        }
        c += size;
    }
    if (!m->nseg || lo == UINT64_MAX || !dyld_info == !fixups) {
        fail(err, errlen, "%s: %s", path, dyld_info && fixups ? "dyld info and chained fixups both" : "no segments, or neither dyld info nor chained fixups"); goto bad;
    }

    // the image's address range, the island before it
    long page = sysconf(_SC_PAGESIZE);
    size_t isl = (island + 0xffff) & ~(size_t)0xffff;
    uint64_t span = (hi - lo + (uint64_t)page - 1) & ~((uint64_t)page - 1);
    reserve_len = isl + span;
    reserve = mmap(NULL, reserve_len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reserve == MAP_FAILED) { fail(err, errlen, "%s: no %zu MiB of address space", path, reserve_len >> 20); goto bad; }
    m->base = reserve + isl;
    m->vmaddr = lo;
    m->vmsize = hi - lo;
    m->slide = (intptr_t)m->base - (intptr_t)lo;
    if (isl) {
        if (mprotect(reserve, isl, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) { fail(err, errlen, "%s: island: %s", path, strerror(errno)); goto bad; }
        m->island = reserve;
        m->island_bytes = isl;
    }
    for (int k = 0; k < m->nseg; k++) {
        struct macho_segment *s = &m->seg[k];
        s->addr = m->base + (sg[k].vmaddr - lo);
        s->vmsize = sg[k].vmsize;
        s->filesize = sg[k].filesize;
        s->fileoff = sg[k].fileoff;
        s->prot = (sg[k].initprot & 1 ? PROT_READ : 0) | (sg[k].initprot & 2 ? PROT_WRITE : 0) | (sg[k].initprot & 4 ? PROT_EXEC : 0);
        if (!s->vmsize) continue;
        int linkedit = strcmp(s->name, "__LINKEDIT") == 0;
        int prot = linkedit ? PROT_READ : PROT_READ | PROT_WRITE;
        uint64_t fo = slice + sg[k].fileoff;
        uint64_t vmlen = (s->vmsize + (uint64_t)page - 1) & ~((uint64_t)page - 1);
        if ((uintptr_t)s->addr % (uintptr_t)page) { fail(err, errlen, "%s: segment %s is not on a %ld-byte page", path, s->name, page); goto bad; }
        if (s->filesize && fo % (uint64_t)page == 0) {
            // the file's pages themselves: untouched ones stay shared with the page cache
            if (mmap(s->addr, s->filesize, prot, MAP_PRIVATE | MAP_FIXED, fd, (off_t)fo) == MAP_FAILED) { fail(err, errlen, "%s: map %s: %s", path, s->name, strerror(errno)); goto bad; }
            uint64_t filled = (s->filesize + (uint64_t)page - 1) & ~((uint64_t)page - 1);
            if (filled > vmlen) filled = vmlen;
            if (vmlen > filled && mmap(s->addr + filled, vmlen - filled, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) { fail(err, errlen, "%s: zero-fill of %s: %s", path, s->name, strerror(errno)); goto bad; }
            if (!linkedit && s->filesize % (uint64_t)page && s->vmsize > s->filesize) {
                uint64_t z = filled - s->filesize;
                if (z > s->vmsize - s->filesize) z = s->vmsize - s->filesize;
                memset(s->addr + s->filesize, 0, z);          // the file goes on past the segment's end
            }
        } else {
            if (mmap(s->addr, vmlen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) { fail(err, errlen, "%s: memory for %s: %s", path, s->name, strerror(errno)); goto bad; }
            for (uint64_t done = 0; done < s->filesize; ) {
                ssize_t r = pread(fd, s->addr + done, s->filesize - done, (off_t)(fo + done));
                if (r <= 0) { fail(err, errlen, "%s: read of %s: %s", path, s->name, r ? strerror(errno) : "cut"); goto bad; }
                done += (uint64_t)r;
            }
            if (linkedit) mprotect(s->addr, vmlen, PROT_READ);
        }
        if (linkedit) { m->linkedit = s->addr; m->linkedit_fileoff = sg[k].fileoff; }
        if (sg[k].fileoff == 0 && s->filesize >= 32) m->header = s->addr;
    }
    for (int i = 0; i < m->nsect; i++) m->sect[i].addr = (uint8_t *)((intptr_t)(uintptr_t)m->sect[i].addr + m->slide);
    if (!m->linkedit || !m->header) { fail(err, errlen, "%s: no __LINKEDIT or no header segment", path); goto bad; }

    // link-edit data: [off, off+size) of the file must lie in __LINKEDIT
    struct macho_segment *le = NULL;
    for (int k = 0; k < m->nseg; k++) if (m->seg[k].addr == m->linkedit) le = &m->seg[k];
#define LINKEDIT(off, size) ((uint64_t)(off) >= m->linkedit_fileoff && (uint64_t)(off) + (uint64_t)(size) <= m->linkedit_fileoff + le->filesize \
        ? m->linkedit + ((uint64_t)(off) - m->linkedit_fileoff) : NULL)
    if (symtab) {
        m->nsyms = le32(symtab + 12);
        m->strsize = le32(symtab + 20);
        m->symtab = LINKEDIT(le32(symtab + 8), (uint64_t)m->nsyms * 16);
        m->strtab = (const char *)LINKEDIT(le32(symtab + 16), m->strsize);
        if (!m->symtab || !m->strtab || !m->strsize || m->strtab[m->strsize - 1] != 0) { m->symtab = NULL; m->strtab = NULL; m->nsyms = 0; }
    }
    if (dice && le32(dice + 12) >= 8) {
        m->ndice = le32(dice + 12) / 8;
        if (!(m->dice = LINKEDIT(le32(dice + 8), (uint64_t)m->ndice * 8))) { fail(err, errlen, "%s: data-in-code list outside __LINKEDIT", path); goto bad; }
    }
    const uint8_t *p;
    uint32_t off, size;
    if (fixups) {
        if (trie && (size = le32(trie + 12)) != 0) {
            if (!(m->exports = LINKEDIT(le32(trie + 8), size))) { fail(err, errlen, "%s: exports outside __LINKEDIT", path); goto bad; }
            m->exports_size = size;
        }
        off = le32(fixups + 8); size = le32(fixups + 12);
        if (size) {
            if (!(p = LINKEDIT(off, size))) { fail(err, errlen, "%s: chained fixups outside __LINKEDIT", path); goto bad; }
            if (chained(m, p, size, libs, nlibs, resolve, ctx, err, errlen) != 0) goto bad;
        }
        goto bound;
    }
    off = le32(dyld_info + 40); size = le32(dyld_info + 44);
    if (size) {
        if (!(m->exports = LINKEDIT(off, size))) { fail(err, errlen, "%s: exports outside __LINKEDIT", path); goto bad; }
        m->exports_size = size;
    }
    off = le32(dyld_info + 8); size = le32(dyld_info + 12);
    if (size) {
        if (!(p = LINKEDIT(off, size))) { fail(err, errlen, "%s: rebases outside __LINKEDIT", path); goto bad; }
        if (rebase(m, p, size, err, errlen) != 0) goto bad;
    }
    static const int kinds[2] = { BIND_NORMAL, BIND_LAZY };
    static const int at[2] = { 16, 32 };
    for (int i = 0; i < 2; i++) {
        off = le32(dyld_info + at[i]); size = le32(dyld_info + at[i] + 4);
        if (!size) continue;
        if (!(p = LINKEDIT(off, size))) { fail(err, errlen, "%s: binds outside __LINKEDIT", path); goto bad; }
        if (bind(m, p, size, kinds[i], libs, nlibs, resolve, ctx, err, errlen) != 0) goto bad;
    }
    off = le32(dyld_info + 24); size = le32(dyld_info + 28);
    if (size) {
        if (!(m->weak = LINKEDIT(off, size))) { fail(err, errlen, "%s: weak binds outside __LINKEDIT", path); goto bad; }
        m->weak_size = size;
    }
bound:
#undef LINKEDIT
    if (m->nmissing) { fail(err, errlen, "%s: %d imports have no answer", path, m->nmissing); goto bad; }
    m->map = reserve;
    m->map_len = reserve_len;
    free(cmds);
    close(fd);
    return m;
bad:
    if (reserve != MAP_FAILED) munmap(reserve, reserve_len);
    free(m->weak_uses);
    free(m);
    m = NULL;
out:
    free(cmds);
    close(fd);
    return NULL;
}

void macho_unload(struct macho_image *m)
{
    munmap(m->map, m->map_len);
    free(m->sorted);
    free(m->weak_uses);
    free(m);
}

int macho_bind_weak(struct macho_image *m, macho_resolve_fn resolve, void *ctx, char *err, size_t errlen)
{
    if (m->weak_size && bind(m, m->weak, m->weak_size, BIND_WEAK, NULL, 0, resolve, ctx, err, errlen) != 0) return -1;
    int missing = 0;
    const char *first = "";
    for (uint32_t i = 0; i < m->nweak_uses; i++) {
        const struct macho_weak_use *u = &m->weak_uses[i];
        void *p = resolve(u->name, "", ctx);
        if (p) { uint64_t v = (uint64_t)(uintptr_t)p + u->addend; memcpy(u->at, &v, 8); }
        else if (u->unbound && !u->optional && !missing++) first = u->name;
    }
    return missing ? fail(err, errlen, "%s: %d uses of names with no definition in any image (%s)", m->path, missing, first) : 0;
}

int macho_protect(struct macho_image *m)
{
    long page = sysconf(_SC_PAGESIZE);
    for (int k = 0; k < m->nseg; k++) {
        struct macho_segment *s = &m->seg[k];
        if (!s->vmsize || s->addr == m->linkedit) continue;
        uint64_t len = (s->vmsize + (uint64_t)page - 1) & ~((uint64_t)page - 1);
        if (mprotect(s->addr, len, s->prot) != 0) return -1;
        if (s->prot & PROT_EXEC) __builtin___clear_cache((char *)s->addr, (char *)s->addr + len);
    }
    return 0;
}

void macho_run_initializers(struct macho_image *m, int argc, char **argv, char **envp)
{
    typedef void (*init_fn)(int, char **, char **, char **, void *);
    static char *apple[] = { NULL };
    for (int i = 0; i < m->nsect; i++) {
        unsigned type = m->sect[i].flags & 0xff;
        if (type == S_MOD_INIT_FUNC_POINTERS) {
            init_fn *f = (init_fn *)m->sect[i].addr;
            for (uint64_t k = 0; k < m->sect[i].size / 8; k++) f[k](argc, argv, envp, apple, NULL);
        } else if (type == S_INIT_FUNC_OFFSETS) {       // the same as offsets from the header (macOS 12 and later)
            for (uint64_t k = 0; k < m->sect[i].size / 4; k++)
                ((init_fn)(m->header + le32(m->sect[i].addr + 4 * k)))(argc, argv, envp, apple, NULL);
        }
    }
}

void *macho_export(const struct macho_image *m, const char *name)
{
    return macho_export_flags(m, name, NULL);
}

void *macho_export_flags(const struct macho_image *m, const char *name, unsigned *xflags)
{
    const uint8_t *start = m->exports, *end = start + m->exports_size, *p = start;
    const char *s = name;
    int bad = 0;
    if (!start) return NULL;
    for (int depth = 0; depth < 4096; depth++) {
        uint64_t term = uleb(&p, end, &bad);
        if (bad || term > (uint64_t)(end - p)) return NULL;
        if (!*s) {
            if (!term) return NULL;
            uint64_t flags = uleb(&p, end, &bad);
            if (bad || (flags & 0x08)) return NULL;          // a re-export: another image's
            uint64_t a = uleb(&p, end, &bad);
            if (bad || (flags & 0x03) == 0x02 || a >= m->vmsize) return NULL;    // an absolute value: no address
            if (flags & 0x10) {                              // a stub and the function that picks its target
                fprintf(stderr, "[macho] %s: %s is exported through a resolver function, which is not read\n", m->path, name);
                return NULL;
            }
            if (xflags) *xflags = (unsigned)flags;
            return m->header + a;
        }
        p += term;
        if (p >= end) return NULL;
        unsigned nchild = *p++;
        const uint8_t *next = NULL;
        for (unsigned i = 0; i < nchild && !next; i++) {
            const char *t = s;
            while (p < end && *p && *p == (uint8_t)*t) { p++; t++; }
            int match = p < end && !*p;
            while (p < end && *p) p++;
            if (p >= end) return NULL;
            p++;
            uint64_t child = uleb(&p, end, &bad);
            if (bad || child >= m->exports_size) return NULL;
            if (match) { next = start + child; s = t; }
        }
        if (!next) return NULL;
        p = next;
    }
    return NULL;
}

static void walk_exports(const struct macho_image *m, const uint8_t *p, char *name, size_t len, int depth,
                         void (*cb)(const char *, void *, void *), void *ctx)
{
    const uint8_t *start = m->exports, *end = start + m->exports_size;
    int bad = 0;
    if (depth > 512 || p >= end) return;
    uint64_t term = uleb(&p, end, &bad);
    if (bad || term > (uint64_t)(end - p)) return;
    if (term) {
        const uint8_t *q = p;
        uint64_t flags = uleb(&q, end, &bad);
        uint64_t a = uleb(&q, end, &bad);
        if (!bad && !(flags & 0x18) && (flags & 0x03) != 0x02 && a < m->vmsize) cb(name, m->header + a, ctx);
    }
    p += term;
    if (p >= end) return;
    unsigned nchild = *p++;
    for (unsigned i = 0; i < nchild; i++) {
        size_t n = len;
        while (p < end && *p && n + 1 < 1024) name[n++] = (char)*p++;
        if (p >= end || *p) return;
        name[n] = 0;
        p++;
        uint64_t child = uleb(&p, end, &bad);
        if (bad || child >= m->exports_size) return;
        walk_exports(m, start + child, name, n, depth + 1, cb, ctx);
        name[len] = 0;
    }
}

void macho_each_export(const struct macho_image *m, void (*cb)(const char *, void *, void *), void *ctx)
{
    char name[1024] = "";
    if (m->exports) walk_exports(m, m->exports, name, 0, 0, cb, ctx);
}

// nlist_64: n_strx u32, n_type u8, n_sect u8, n_desc u16, n_value u64. A defined symbol: not a
// debugger entry (N_STAB 0xe0), type N_SECT (0x0e of the N_TYPE bits).
static int sym_defined(const uint8_t *nl) { return !(nl[4] & 0xe0) && (nl[4] & 0x0e) == 0x0e; }

void *macho_symbol(const struct macho_image *m, const char *name)
{
    for (uint32_t i = 0; i < m->nsyms; i++) {
        const uint8_t *nl = m->symtab + (size_t)i * 16;
        uint32_t strx = le32(nl);
        if (!sym_defined(nl) || strx >= m->strsize || strcmp(m->strtab + strx, name) != 0) continue;
        uint64_t v = le64(nl + 8);
        if (v < m->vmaddr || v - m->vmaddr >= m->vmsize) return NULL;
        return (void *)(uintptr_t)(v + (uint64_t)m->slide);
    }
    return NULL;
}

const struct macho_section *macho_section(const struct macho_image *m, const char *seg, const char *sect)
{
    for (int i = 0; i < m->nsect; i++)
        if (strcmp(m->sect[i].seg, seg) == 0 && strcmp(m->sect[i].name, sect) == 0) return &m->sect[i];
    return NULL;
}

int macho_contains(const struct macho_image *m, const void *p)
{
    return (const uint8_t *)p >= m->base && (const uint8_t *)p < m->base + m->vmsize;
}

static const uint8_t *g_sort_symtab;
static int by_value(const void *a, const void *b)
{
    uint64_t x = le64(g_sort_symtab + (size_t)*(const uint32_t *)a * 16 + 8);
    uint64_t y = le64(g_sort_symtab + (size_t)*(const uint32_t *)b * 16 + 8);
    return x < y ? -1 : x > y;
}

const char *macho_symbolize(struct macho_image *m, const void *pc, uint64_t *offset)
{
    if (!macho_contains(m, pc) || !m->nsyms) return NULL;
    if (!m->sorted) {
        uint32_t *s = malloc((size_t)m->nsyms * 4), n = 0;
        if (!s) return NULL;
        for (uint32_t i = 0; i < m->nsyms; i++)
            if (sym_defined(m->symtab + (size_t)i * 16) && le32(m->symtab + (size_t)i * 16) < m->strsize) s[n++] = i;
        g_sort_symtab = m->symtab;
        qsort(s, n, 4, by_value);
        m->sorted = s;
        m->nsorted = n;
    }
    uint64_t v = (uint64_t)((intptr_t)(uintptr_t)pc - m->slide);
    uint32_t lo = 0, hi = m->nsorted;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (le64(m->symtab + (size_t)m->sorted[mid] * 16 + 8) <= v) lo = mid + 1; else hi = mid;
    }
    if (!lo) return NULL;
    const uint8_t *nl = m->symtab + (size_t)m->sorted[lo - 1] * 16;
    if (offset) *offset = v - le64(nl + 8);
    return m->strtab + le32(nl);
}

static int by_start(const void *a, const void *b)
{
    const uint8_t *x = *(const uint8_t *const *)a, *y = *(const uint8_t *const *)b;
    return x < y ? -1 : x > y;
}

long macho_data_in_code(const struct macho_image *m, const uint8_t **pairs, long max)
{
    long n = 0;
    for (uint32_t i = 0; i < m->ndice; i++) {
        const uint8_t *d = m->dice + (size_t)i * 8;
        uint64_t off = le32(d), len = (uint64_t)d[4] | (uint64_t)d[5] << 8;
        const struct macho_segment *s = NULL;
        for (int k = 0; k < m->nseg && !s; k++)
            if (m->seg[k].filesize && off >= m->seg[k].fileoff && off + len <= m->seg[k].fileoff + m->seg[k].filesize) s = &m->seg[k];
        if (!s) return -1;
        if (!len) continue;
        if (n < max) { pairs[2 * n] = s->addr + (off - s->fileoff); pairs[2 * n + 1] = pairs[2 * n] + len; }
        n++;
    }
    if (n <= max) qsort(pairs, (size_t)n, 2 * sizeof *pairs, by_start);
    return n;
}

const struct macho_section *macho_code_section(const struct macho_image *m, int i)
{
    for (int k = 0; k < m->nsect; k++)
        if ((m->sect[k].flags & 0x80000400u) && (m->sect[k].flags & 0xff) != S_ZEROFILL && m->sect[k].size && i-- == 0) return &m->sect[k];
    return NULL;
}
