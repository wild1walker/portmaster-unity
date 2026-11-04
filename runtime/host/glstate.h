// GL state calls that change nothing, counted or left out (UA_GL_STATE), see glstate.c
#ifndef GLSTATE_H
#define GLSTATE_H
#include "../loader/so_loader.h"
// The wrappers when UA_GL_STATE is count or 1 (else an empty table); next(name) is what each
// wrapper calls (another host wrapper of that name, eglMakeCurrent's included; NULL: the driver's).
const so_shim *glstate_table(void *(*next)(const char *name));
void glstate_frame(void);           // from the swap hook, every frame
#endif
