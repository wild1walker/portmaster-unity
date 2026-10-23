// Tile-memory traffic a frame could do without (glopt.c): UA_GL_OPT.
#ifndef GLOPT_H
#define GLOPT_H
#include "../loader/so_loader.h"
// The GL wrappers when UA_GL_OPT names something (else an empty table): register with so_add_shims()
// before the other GL wrappers; next(name) is what each wrapper calls (another host wrapper of that
// name, NULL: the real GL function).
const so_shim *glopt_table(void *(*next)(const char *name));
void glopt_frame(void);             // from the swap hook, every frame
#endif
