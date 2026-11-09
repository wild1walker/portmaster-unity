#!/usr/bin/env python3
"""patch-libunwind.py <llvm-project dir>: LLVM 19 libunwind for the Mach-O route (UA_MACHO_COMPACT).

On Linux libunwind reads only DWARF; its compact unwind reader (CompactUnwinder.hpp, the arm64
encodings macOS images carry in __TEXT,__unwind_info) is built for Apple only. With
UA_MACHO_COMPACT it is built here too, and findUnwindSections asks the bridge first
(ua_unwind_find_macho, ua_unwind.h) for a pc in a loaded macOS image, then searches the ELF
libraries through ua_unwind_iterate_phdr (glibc's and the runtime loader's) instead of
dl_iterate_phdr alone. Each edit must match exactly once; a second run finds them applied."""
import sys

root = sys.argv[1] + '/libunwind/src/'


def edit(name, old, new):
    p = root + name
    s = open(p).read()
    if new in s:
        return
    if s.count(old) != 1:
        sys.exit(f'{name}: the text to patch is not there once:\n{old}')
    open(p, 'w').write(s.replace(old, new))


edit('config.h',
     '''  // Assume an ELF system with a dl_iterate_phdr function.
  #define _LIBUNWIND_USE_DL_ITERATE_PHDR 1
''',
     '''  // Assume an ELF system with a dl_iterate_phdr function.
  #define _LIBUNWIND_USE_DL_ITERATE_PHDR 1
  #if defined(UA_MACHO_COMPACT)
    // macOS images loaded by the Mach-O route: their compact unwind info (ua_unwind.h)
    #define _LIBUNWIND_SUPPORT_COMPACT_UNWIND 1
  #endif
''')

edit('AddressSpace.hpp',
     '''inline bool LocalAddressSpace::findUnwindSections(pint_t targetAddr,
                                                  UnwindInfoSections &info) {
#ifdef __APPLE__''',
     '''inline bool LocalAddressSpace::findUnwindSections(pint_t targetAddr,
                                                  UnwindInfoSections &info) {
#if defined(UA_MACHO_COMPACT)
  info.compact_unwind_section = 0;
  info.compact_unwind_section_length = 0;
  {
    struct ua_unwind_sections s;
    if (ua_unwind_find_macho && ua_unwind_find_macho(targetAddr, &s)) {
      info.dso_base = s.dso_base;
      info.text_segment_length = s.text_length;
      info.dwarf_section = s.dwarf;
      info.dwarf_section_length = s.dwarf_length;
      info.dwarf_index_section = 0;
      info.dwarf_index_section_length = 0;
      info.compact_unwind_section = s.compact;
      info.compact_unwind_section_length = s.compact_length;
      return true;
    }
  }
#endif
#ifdef __APPLE__''')

edit('AddressSpace.hpp',
     '''  int found = dl_iterate_phdr(findUnwindSectionsByPhdr, &cb_data);''',
     '''#if defined(UA_MACHO_COMPACT)
  int found = ua_unwind_iterate_phdr
                  ? ua_unwind_iterate_phdr(findUnwindSectionsByPhdr, &cb_data)
                  : dl_iterate_phdr(findUnwindSectionsByPhdr, &cb_data);
#else
  int found = dl_iterate_phdr(findUnwindSectionsByPhdr, &cb_data);
#endif''')

edit('AddressSpace.hpp',
     '''#include "libunwind.h"
#include "config.h"''',
     '''#include "libunwind.h"
#include "config.h"
#if defined(UA_MACHO_COMPACT)
#include "ua_unwind.h"
#endif''')

edit('libunwind.cpp',
     '''#include "config.h"
#include "libunwind_ext.h"''',
     '''#include "config.h"
#include "libunwind_ext.h"
#if defined(UA_MACHO_COMPACT)
#include "ua_unwind.h"
int (*ua_unwind_find_macho)(uintptr_t, struct ua_unwind_sections *) = 0;
int (*ua_unwind_iterate_phdr)(int (*)(struct dl_phdr_info *, size_t, void *), void *) = 0;
#endif''')
print('libunwind patched for UA_MACHO_COMPACT')
