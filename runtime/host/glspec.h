// Programs specialized on a uniform's value (glspec.c): UA_GL_SPEC=1|check.
#ifndef GLSPEC_H
#define GLSPEC_H
#include "../loader/so_loader.h"
// The GL wrappers (an empty table when off): register with so_add_shims() after glpass_table()'s and
// before glopt_table()'s; next(name) is what each wrapper calls (NULL: the real GL function).
const so_shim *glspec_table(void *(*next)(const char *name));
// Before the player starts: UA_GL_SPEC turned on or off since the last run (<cache_dir>/uaspec.on):
// <cache_dir>/UnityShaderCache emptied, so its program binaries are all of one kind
void glspec_cache(const char *cache_dir);
void glspec_frame(void);                // from the swap hook, every frame
void *glspec_hard(const char *name);    // its float-taking wrappers with the host's calling convention
#endif
