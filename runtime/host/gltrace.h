// GL frame tracer (gltrace.c): UA_GL_TRACE=<s>.
#ifndef GLTRACE_H
#define GLTRACE_H
#include "../loader/so_loader.h"
const so_shim *gltrace_table(void);   // register with so_add_shims() before the EGL glue
void gltrace_frame(void);             // from the swap hook, every frame
#endif
