// unitybeh.c: the scripts' update loops with the next scripts' memory fetched ahead (UA_BEH_PREFETCH=1), without
// started scripts' empty callbacks (UA_BEH_EMPTY=1), counted (UA_BEH_STATS=<s>)
#ifndef UNITYBEH_H
#define UNITYBEH_H
#include <stddef.h>
#include <stdint.h>
// libunity.so once relocated (unityhooks_on_load): the hooks the switches ask for
void unitybeh_on_load(uintptr_t base, size_t size);
// UnityMain, after each frame (unityhooks_frame): UA_BEH_STATS's lines
void unitybeh_frame(void);
// for the test: what is fetched ahead of the iterator `it` (MonoBehaviour's vtable vt_mb); an IL body that
// computes nothing (nops, then ret)
void unitybeh_prefetch(uintptr_t it, uintptr_t vt_mb);
void unitybeh_touch(uintptr_t it, uintptr_t vt_mb);      // UA_BEH_PREFETCH=2
int unitybeh_il_empty(const uint8_t *code, uint32_t size);
#endif
