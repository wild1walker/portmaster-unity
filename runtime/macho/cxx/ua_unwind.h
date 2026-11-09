// ua_unwind.h - the two hooks the Mach-O route adds to LLVM's libunwind (patched by build-cxx.sh,
// built with UA_MACHO_COMPACT): a macOS image's compact unwind info and __eh_frame for a pc in it,
// and a dl_iterate_phdr that also lists the Android libraries the runtime's own ELF loader maps
// (libunity.so's frames sit between a managed throw and its catch when an icall raises).
#ifndef UA_UNWIND_H
#define UA_UNWIND_H

#include <stddef.h>
#include <stdint.h>

struct dl_phdr_info;

struct ua_unwind_sections {
    uintptr_t dso_base;          // the image's mach_header (compact unwind offsets are from it)
    size_t    text_length;
    uintptr_t dwarf;             // __TEXT,__eh_frame (0: none)
    size_t    dwarf_length;
    uintptr_t compact;           // __TEXT,__unwind_info
    size_t    compact_length;
};

#ifdef __cplusplus
extern "C" {
#endif
// set by the bridge before the first throw; NULL: libunwind's own search only
extern int (*ua_unwind_find_macho)(uintptr_t pc, struct ua_unwind_sections *s);
extern int (*ua_unwind_iterate_phdr)(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data);
#ifdef __cplusplus
}
#endif

#endif
