// nativebeh.c: the scripts' update loops (Update, LateUpdate, FixedUpdate) as our own code on the 64-bit player
// (UA_NATIVE_BEH=1 | verify; UA_NATIVE_BEH_STATS=<s>)
#ifndef NATIVEBEH_H
#define NATIVEBEH_H
#include <stddef.h>
#include <stdint.h>
// libunity.so once relocated (unityhooks_on_load), before unityprefetch_on_load: the loops replaced
void nativebeh_on_load(uintptr_t base, size_t size);
// UnityMain, after each frame (unityhooks_frame): UA_NATIVE_BEH_STATS's lines, the trace's frame boundary
void nativebeh_frame(void);
#endif
