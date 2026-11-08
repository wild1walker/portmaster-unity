// Draws merged in held frames (UA_GL_MERGE; glmerge.c), with UA_GL_REC recording.
#ifndef GLMERGE_H
#define GLMERGE_H
#include "glrec_fg.h"
int glmerge_init(void);                  // UA_GL_MERGE read; nonzero: on
int glmerge_on(void);
void glmerge_set_next(void *const *next);  // the next layer's entry points, by glrec op
// the render thread's shader and program calls (with the context)
char *glmerge_source(unsigned shader, int count, const char *const *str, const int *len);   // rewritten (malloc'd) or NULL
void glmerge_attach(unsigned program, unsigned shader);
void glmerge_linked(unsigned program);   // after glLinkProgram
void glmerge_binary(unsigned program);   // after glProgramBinary
void glmerge_deleted(unsigned program);
void glmerge_submit(fg_frame *f);        // a held frame, replayed with runs of draws made as one
void glmerge_log(void);
#endif
