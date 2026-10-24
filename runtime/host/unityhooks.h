// unityhooks.c: opt-in changes to functions inside libunity.so, found by Unity's symbol names and
// checked against the bytes they were read from (UA_GFX_JOBS=legacy)
#ifndef UNITYHOOKS_H
#define UNITYHOOKS_H
#include "../loader/so_loader.h"
// the loader's load hook (so_set_load_hook): patches libunity.so once it is relocated
void unityhooks_on_load(so_module *m);
// 1 when UA_GFX_JOBS=legacy (and UA_UNITY_HOOKS is not 0)
int unityhooks_gfx_jobs(void);
#endif
