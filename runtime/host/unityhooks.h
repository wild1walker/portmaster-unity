// unityhooks.c: opt-in changes to functions inside libunity.so, found by Unity's symbol names and
// checked against the bytes they were read from (UA_GFX_JOBS=legacy)
#ifndef UNITYHOOKS_H
#define UNITYHOOKS_H
#include "../loader/so_loader.h"
// the loader's load hook (so_set_load_hook): patches libunity.so once it is relocated
void unityhooks_on_load(so_module *m);
// 1 when UA_GFX_JOBS=legacy (and UA_UNITY_HOOKS is not 0)
int unityhooks_gfx_jobs(void);
// UnityMain, after each nativeRender: UA_PRESENT_SYNC's setting, once
void unityhooks_frame(void);

// For the other hook files (unitypool.c): a function as tools/re/unityhooks.py read it in one player
#include <stddef.h>
#include <stdint.h>
struct uh_row {
    const char *unity;      // the player it was read from (for the log)
    const char *func;       // the function (demangled name, for the log)
    uint32_t off, size;     // in libunity.so (ELF address = file offset of the text here)
    char isa;               // 'a' ARM, 't' Thumb (a function only called), 'x' A64
    uint64_t fnv;           // FNV-1a 64 of the function's size bytes
};
// 1 when the loaded library (base, size) has exactly the row's bytes at its offset
int unityhooks_matches(const struct uh_row *r, uintptr_t base, size_t size);
// the entry of fn (ARM or A64) becomes a jump to `to`; *orig gets a callable original. 0 on success
int unityhooks_hook(uintptr_t fn, char isa, void *to, void **orig);
#endif
