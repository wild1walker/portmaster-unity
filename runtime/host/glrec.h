// GL frame recorder (glrec.c): the render thread's GL calls of a frame held until the swap.
#ifndef GLREC_H
#define GLREC_H
#include "../loader/so_loader.h"

// the wrappers (UA_GL_REC; an empty table when it is off). next(name): the layer below for a GL
// or EGL entry point (another shim table's wrapper, else NULL for the driver's own)
const so_shim *glrec_table(void *(*next)(const char *name));
// at the swap, before anything else draws or reads the frame: submits it
void glrec_swap(void);

#endif
// glhooks.c's program-log wrappers pass Unity's program binaries on (the frame graph's verdicts, glrec_fg.c)
void glrec_note_program_binary(unsigned program, const void *binary, long length);
void glrec_note_get_program_binary(unsigned program, long length, const void *binary);
