// GL/EGL hooks of the host (glhooks.c): shader dump/override, swap interval.
#ifndef GLHOOKS_H
#define GLHOOKS_H
#include "../loader/so_loader.h"
const so_shim *glhooks_table(void);   // register with so_add_shims() before the EGL glue
void glhooks_first_frame(void);       // from the swap hook, once a context is current
#endif
