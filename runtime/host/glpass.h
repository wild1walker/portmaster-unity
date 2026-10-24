// GL pass changes that keep every pixel (glpass.c).
#ifndef GLPASS_H
#define GLPASS_H
#include "../loader/so_loader.h"
// The GL wrappers (an empty table when nothing is on): register with so_add_shims() before
// glopt_table()'s; next(name) is what each wrapper calls (another host wrapper of that name,
// NULL: the real GL function).
const so_shim *glpass_table(void *(*next)(const char *name));
void glpass_frame(void);            // from the swap hook, every frame
#endif
