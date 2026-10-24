// Draws whose target already holds what they would write, left out (glmemo.c): UA_GL_MEMO.
#ifndef GLMEMO_H
#define GLMEMO_H
#include "../loader/so_loader.h"
// The GL wrappers (an empty table with UA_GL_MEMO=0): register with so_add_shims() before every other GL
// wrapper; next(name) is what each wrapper calls (another host wrapper of that name, NULL: the real one).
const so_shim *glmemo_table(void *(*next)(const char *name));
void glmemo_frame(void);            // from the swap hook, every frame
#endif
