// macho.h - a loader for 64-bit ARM Mach-O images (a macOS build's dylibs and bundles) on Linux:
// the arm64 slice's segments mapped from the file, rebased and bound (the opcode streams of
// LC_DYLD_INFO: lazy pointers are bound at load, so dyld_stub_binder is never entered; or the
// chained fixups of a build for macOS 12 and later), exports and symbols looked up,
// initializers run. docs/macho-route.md.
#ifndef UA_MACHO_H
#define UA_MACHO_H

#include <stddef.h>
#include <stdint.h>

struct macho_section {
    char     seg[17], name[17];
    uint8_t *addr;
    uint64_t size;
    uint32_t flags;
    uint32_t align;              // log2 of the alignment the section asks for
};

struct macho_segment {
    char     name[17];
    uint8_t *addr;
    uint64_t vmsize, filesize, fileoff;
    int      prot;               // PROT_* as the image asks
};

struct macho_image {
    char      path[1024];
    char      install_name[256]; // LC_ID_DYLIB's last component ("libc++.1.dylib"; "" for a bundle)
    uint8_t  *base;              // where the image's lowest vmaddr is mapped
    uint64_t  vmaddr;            // that lowest vmaddr (0 for a dylib)
    uint64_t  vmsize;
    intptr_t  slide;             // base - vmaddr
    uint8_t  *island;            // island_bytes of rwx memory right before the image (NULL: none)
    size_t    island_bytes;
    uint8_t  *map;               // all that was mapped for it: the island and the image
    size_t    map_len;
    uint8_t  *header;            // the mach_header_64 in memory
    uint32_t  flags;             // the header's (MACHO_WEAK_DEFINES, MACHO_BINDS_TO_WEAK, ...)
    struct macho_segment seg[16];
    int       nseg;
    struct macho_section sect[64];
    int       nsect;
    const uint8_t *linkedit;     // the file's __LINKEDIT, mapped read-only
    uint64_t  linkedit_fileoff;
    const uint8_t *exports;      // the export trie
    uint32_t  exports_size;
    const uint8_t *weak;         // the weak-bind stream (macho_bind_weak)
    uint32_t  weak_size;
    struct macho_weak_use *weak_uses;   // the same of an image with chained fixups: each use of
    uint32_t  nweak_uses;               // a coalesced name
    const uint8_t *symtab;       // nlist_64 entries
    uint32_t  nsyms;
    const char *strtab;
    uint32_t  strsize;
    const uint8_t *dice;         // LC_DATA_IN_CODE entries
    uint32_t  ndice;
    uint32_t *sorted;            // defined symbols' indices by address (macho_symbolize)
    uint32_t  nsorted;
    int       nbound, nmissing, nrebased;
};

// An import's address: name as the image spells it (with the leading underscore), lib the
// dylib it names ("libSystem.B.dylib"; "" for a flat or weak lookup). NULL: not there (the load
// fails unless the import is weak, which then binds to 0).
typedef void *(*macho_resolve_fn)(const char *name, const char *lib, void *ctx);

// Loads path's arm64 slice (a universal or a thin file). island: bytes of writable, executable
// memory to leave right before the image, within a branch's reach of its code. NULL with a
// message in err when the file is not one this loader reads completely. The image's own binds
// are made (each import from the dylib it names); the names several images may define and all
// must agree on are left for macho_bind_weak (an image with chained fixups has such a name
// bound to its own definition or to what resolve(name, "", ctx) gives until then, and one with
// neither stays 0: macho_bind_weak is what reports it).
struct macho_image *macho_load(const char *path, size_t island, macho_resolve_fn resolve, void *ctx,
                               char *err, size_t errlen);
// Gives an image's memory back. Only for one that never ran: no thread may be in its code or
// hold an address inside it.
void  macho_unload(struct macho_image *m);
// 1 when path is a regular file that starts as a Mach-O or a universal file does (whether
// macho_load reads it is another matter), else 0.
int   macho_probe(const char *path);
// The dylibs the file's arm64 slice names, before anything is mapped: cb(name, weak, ctx) with
// the name as the load command spells it ("@rpath/libfoo.dylib", "/usr/lib/libSystem.B.dylib");
// weak: the image runs without it. 0, or -1 with a message for a file macho_load would not read.
int   macho_needed(const char *path, void (*cb)(const char *name, int weak, void *ctx), void *ctx,
                   char *err, size_t errlen);
// The coalesced names (C++ inline functions and typeinfo, operator new and delete), once every
// image that is to run together is loaded and before macho_protect: each use the image lists is
// bound to resolve(name, "", ctx), the one definition all the images share; NULL leaves the
// image with its own. 0, or -1 with a message (a stream this loader does not read; a name
// that has no definition anywhere and is not optional).
#define MACHO_WEAK_DEFINES   0x8000u       // header flags: the image defines such names,
#define MACHO_BINDS_TO_WEAK  0x10000u      // uses them
int   macho_bind_weak(struct macho_image *m, macho_resolve_fn resolve, void *ctx, char *err, size_t errlen);
// Segment protections as the image asks (after the code was patched); 0 or -1 with errno.
int   macho_protect(struct macho_image *m);
void  macho_run_initializers(struct macho_image *m, int argc, char **argv, char **envp);
void *macho_export(const struct macho_image *m, const char *name);    // "_il2cpp_init"
// The same, with the export's flags: MACHO_EXPORT_WEAK is a definition another image's may replace.
#define MACHO_EXPORT_WEAK 0x04u
void *macho_export_flags(const struct macho_image *m, const char *name, unsigned *flags);
void *macho_symbol(const struct macho_image *m, const char *name);    // LC_SYMTAB: locals too
const struct macho_section *macho_section(const struct macho_image *m, const char *seg, const char *sect);
int   macho_contains(const struct macho_image *m, const void *p);
// The function a code address is in: its name (in the image's string table) and the offset.
const char *macho_symbolize(struct macho_image *m, const void *pc, uint64_t *offset);
// The sections that hold instructions (S_ATTR_PURE_INSTRUCTIONS, S_ATTR_SOME_INSTRUCTIONS), i = 0..:
// NULL past the last.
const struct macho_section *macho_code_section(const struct macho_image *m, int i);
// The data inside the code (jump tables), as LC_DATA_IN_CODE lists it: up to max (start, end)
// pairs in address order. The count of ranges (more than max: none is sorted, ask again with
// room for them); -1: a range outside the image's segments.
long  macho_data_in_code(const struct macho_image *m, const uint8_t **pairs, long max);
// Every export of the image: cb(name, address, ctx).
void  macho_each_export(const struct macho_image *m, void (*cb)(const char *, void *, void *), void *ctx);

#endif
