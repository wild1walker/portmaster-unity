// Unity's dynamic geometry in one persistently mapped buffer (UA_GL_ARENA), see glarena.c
#ifndef GLARENA_H
#define GLARENA_H
#include "../loader/so_loader.h"
// The GL wrappers when UA_GL_ARENA=1 (else an empty table); next(name) is what each wrapper calls
// (another host wrapper of that name, NULL: the real GL function).
const so_shim *glarena_table(void *(*next)(const char *name));
void glarena_frame(void);           // from the swap hook, every frame: a fence, the ranges the GPU is done with
#endif
