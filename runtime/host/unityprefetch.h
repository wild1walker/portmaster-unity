// unityprefetch.c: PRFM ahead of libunity's pointer walks on the 64-bit player (UA_PREFETCH=1 | beh,sprite)
#ifndef UNITYPREFETCH_H
#define UNITYPREFETCH_H
#include <stddef.h>
#include <stdint.h>
// libunity.so once relocated (unityhooks_on_load), before any of its code runs: the call sites UA_PREFETCH names
void unityprefetch_on_load(uintptr_t base, size_t size);
// for the test: UA_PREFETCH's value names the stub; the `bl` from pc to target (0: out of range); a stub's jump
// to its callee; a page within branch range of [lo, hi)
int unityprefetch_wanted(const char *v, const char *name);
uint32_t unityprefetch_bl(uint64_t pc, uint64_t target);
void unityprefetch_tail(uint32_t *at, uint64_t target);
void *unityprefetch_near(uintptr_t lo, uintptr_t hi, size_t len);
#endif
