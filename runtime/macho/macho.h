// macho.h - a loader for 64-bit ARM Mach-O images (a macOS build's dylibs and bundles) on Linux:
// the arm64 slice's segments mapped from the file, rebased and bound (the opcode streams of
// LC_DYLD_INFO: lazy pointers are bound at load, so dyld_stub_binder is never entered), exports
// and symbols looked up, initializers run. docs/macho-route.md.
#ifndef UA_MACHO_H
#define UA_MACHO_H

#include <stddef.h>
#include <stdint.h>

struct macho_section {
    char     seg[17], name[17];
    uint8_t *addr;
    uint64_t size;
    uint32_t flags;
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
    uint8_t  *header;            // the mach_header_64 in memory
    struct macho_segment seg[16];
    int       nseg;
    struct macho_section sect[64];
    int       nsect;
    const uint8_t *linkedit;     // the file's __LINKEDIT, mapped read-only
    uint64_t  linkedit_fileoff;
    const uint8_t *exports;      // the export trie
    uint32_t  exports_size;
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
// message in err when the file is not one this loader reads completely.
struct macho_image *macho_load(const char *path, size_t island, macho_resolve_fn resolve, void *ctx,
                               char *err, size_t errlen);
// Segment protections as the image asks (after the code was patched); 0 or -1 with errno.
int   macho_protect(struct macho_image *m);
void  macho_run_initializers(struct macho_image *m, int argc, char **argv, char **envp);
void *macho_export(const struct macho_image *m, const char *name);    // "_il2cpp_init"
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
