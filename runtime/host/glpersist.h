// Unity's dynamic buffers in persistent mapped storage (UA_GL_PERSIST), see glpersist.c
#ifndef GLPERSIST_H
#define GLPERSIST_H
#include "../loader/so_loader.h"
// The GL wrappers when UA_GL_PERSIST=1 (else an empty table); next(name) is what each wrapper
// calls (another host wrapper of that name, NULL: the real GL function).
const so_shim *glpersist_table(void *(*next)(const char *name));
#endif
