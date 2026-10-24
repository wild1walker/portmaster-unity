// Copies of one texture into another left out by renaming (glrename.c): UA_GL_RENAME.
#ifndef GLRENAME_H
#define GLRENAME_H
#include "../loader/so_loader.h"
// The GL wrappers (an empty table when off): register with so_add_shims() before every other GL
// wrapper (it maps the game's texture names); next(name) is what each wrapper calls (another host
// wrapper of that name, NULL: the real GL function).
const so_shim *glrename_table(void *(*next)(const char *name));
void glrename_frame(void);          // from the swap hook, every frame
#endif
