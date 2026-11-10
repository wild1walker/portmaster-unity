// macho.c - see macho.h. What is read: thin or universal files' arm64 slice, MH_DYLIB and
// MH_BUNDLE, LC_SEGMENT_64, LC_SYMTAB, LC_DYLD_INFO(_ONLY). What is refused with a message, so a
// build made another way never half-loads: chained fixups, thread-local variables, initializer
// offsets, text relocations, encrypted segments.
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
            last_addr = resolve(name, lib, ctx); last = name; \
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

static void copy16(char *dst, const uint8_t *src) { memcpy(dst, src, 16); dst[16] = 0; }

struct macho_image *macho_load(const char *path, size_t island, macho_resolve_fn resolve, void *ctx,
                               char *err, size_t errlen)
{
    struct macho_image *m = NULL;
    uint8_t *cmds = NULL, *reserve = MAP_FAILED;
    size_t reserve_len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { fail(err, errlen, "%s: %s", path, strerror(errno)); return NULL; }
    struct stat st;
    uint8_t h[4096];
    ssize_t got = pread(fd, h, sizeof h, 0);
    if (fstat(fd, &st) != 0 || got < 32) { fail(err, errlen, "%s: not a Mach-O file", path); goto out; }

    // the arm64 slice of a universal file
    uint64_t slice = 0, slice_size = (uint64_t)st.st_size;
    uint32_t magic = be32(h);
    if (magic == FAT_MAGIC || magic == FAT_MAGIC_64) {
        uint32_t n = be32(h + 4), rec = magic == FAT_MAGIC ? 20 : 32;
        int found = 0;
        for (uint32_t i = 0; i < n && 8 + (uint64_t)(i + 1) * rec <= (uint64_t)got; i++) {
            const uint8_t *a = h + 8 + i * rec;
            if (be32(a) != CPU_TYPE_ARM64) continue;
            slice = magic == FAT_MAGIC ? be32(a + 8) : be64(a + 8);
            slice_size = magic == FAT_MAGIC ? be32(a + 12) : be64(a + 16);
            found = 1;
            break;
        }
        if (!found) { fail(err, errlen, "%s: no arm64 slice (an Intel-only build)", path); goto out; }
        if (slice + slice_size > (uint64_t)st.st_size || pread(fd, h, 32, (off_t)slice) != 32) {
            fail(err, errlen, "%s: the arm64 slice is cut", path); goto out;
        }
    }
    if (le32(h) != MH_MAGIC_64 || le32(h + 4) != CPU_TYPE_ARM64) { fail(err, errlen, "%s: not arm64 Mach-O", path); goto out; }
    uint32_t filetype = le32(h + 12), ncmds = le32(h + 16), sizeofcmds = le32(h + 20);
    if (filetype != MH_DYLIB && filetype != MH_BUNDLE) { fail(err, errlen, "%s: file type %u (a dylib or a bundle is read)", path, filetype); goto out; }
    if (sizeofcmds > (64u << 20) || 32 + (uint64_t)sizeofcmds > slice_size) { fail(err, errlen, "%s: load commands of %u bytes", path, sizeofcmds); goto out; }
    cmds = malloc(sizeofcmds ? sizeofcmds : 1);
    if (!cmds || pread(fd, cmds, sizeofcmds, (off_t)slice + 32) != (ssize_t)sizeofcmds) { fail(err, errlen, "%s: load commands cut", path); goto out; }
    m = calloc(1, sizeof *m);
    if (!m) { fail(err, errlen, "out of memory"); goto out; }
    snprintf(m->path, sizeof m->path, "%s", path);

    // first pass: the segments' span, what the image needs
    struct { uint64_t vmaddr, vmsize, fileoff, filesize; uint32_t initprot; const uint8_t *cmd; } sg[16];
    const char *libs[64];
    int nlibs = 0;
    const uint8_t *dyld_info = NULL, *symtab = NULL, *dice = NULL;
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
                uint32_t fl = le32(sc + 64), ty = fl & 0xff;
                if (ty == S_INIT_FUNC_OFFSETS) { fail(err, errlen, "%s: initializer offsets are not read", path); goto bad; }
                if (m->nsect == 64) { fail(err, errlen, "%s: more than 64 sections", path); goto bad; }
                struct macho_section *x = &m->sect[m->nsect++];
                copy16(x->name, sc); copy16(x->seg, sc + 16);
                x->addr = (uint8_t *)(uintptr_t)le64(sc + 32);      // vmaddr for now
                x->size = le64(sc + 40);
                x->flags = fl;
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
        case LC_DYLD_CHAINED_FIXUPS: case LC_DYLD_EXPORTS_TRIE:
            fail(err, errlen, "%s: chained fixups are not read (a build for macOS 12 or later only)", path); goto bad;
        case LC_ENCRYPTION_INFO_64:
            if (size >= 20 && le32(c + 16)) { fail(err, errlen, "%s: encrypted", path); goto bad; }
            break;
        }
        c += size;
    }
    if (!m->nseg || lo == UINT64_MAX || !dyld_info) { fail(err, errlen, "%s: no segments or no dyld info", path); goto bad; }

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
    static const int kinds[3] = { BIND_NORMAL, BIND_LAZY, BIND_WEAK };
    static const int at[3] = { 16, 32, 24 };
    for (int i = 0; i < 3; i++) {
        off = le32(dyld_info + at[i]); size = le32(dyld_info + at[i] + 4);
        if (!size) continue;
        if (!(p = LINKEDIT(off, size))) { fail(err, errlen, "%s: binds outside __LINKEDIT", path); goto bad; }
        if (bind(m, p, size, kinds[i], libs, nlibs, resolve, ctx, err, errlen) != 0) goto bad;
    }
#undef LINKEDIT
    if (m->nmissing) { fail(err, errlen, "%s: %d imports have no answer", path, m->nmissing); goto bad; }
    free(cmds);
    close(fd);
    return m;
bad:
    if (reserve != MAP_FAILED) munmap(reserve, reserve_len);
    free(m);
    m = NULL;
out:
    free(cmds);
    close(fd);
    return NULL;
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
        if ((m->sect[i].flags & 0xff) != S_MOD_INIT_FUNC_POINTERS) continue;
        init_fn *f = (init_fn *)m->sect[i].addr;
        for (uint64_t k = 0; k < m->sect[i].size / 8; k++) f[k](argc, argv, envp, apple, NULL);
    }
}

void *macho_export(const struct macho_image *m, const char *name)
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
        if (!bad && !(flags & 0x08) && (flags & 0x03) != 0x02 && a < m->vmsize) cb(name, m->header + a, ctx);
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
