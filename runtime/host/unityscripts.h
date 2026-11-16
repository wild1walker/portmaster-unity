// unityscripts.c: FindRuntimeScript's answers kept on UnityMain (UA_SCRIPT_FIND=1|check)
#ifndef UNITYSCRIPTS_H
#define UNITYSCRIPTS_H
#include <stddef.h>
#include <stdint.h>
// libunity.so once relocated (unityhooks_on_load)
void unityscripts_on_load(uintptr_t base, size_t size);
// UnityMain, after each frame (unityhooks_frame): which thread is UnityMain; the check's lines
void unityscripts_frame(void);
#endif
