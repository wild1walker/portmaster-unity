// The frame graph of glrec.c (UA_GL_REC=2): what a held frame's passes read and write, and the
// equivalent stream that is submitted instead (glrec_fg.c).
#ifndef GLREC_FG_H
#define GLREC_FG_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint64_t *cmd;                         // the held commands: [op | nargs << 32] then the arguments
    size_t ncmd;                                 // words
    const void *(*ptr)(uint64_t slot);           // a pointer argument's memory
    void (*call)(unsigned op, const uint64_t *a);    // one command to the next layer
    void *const *next;                           // the next layer's entry points, by op
    int at_swap;                                 // held up to the swap (the window's depth/stencil ends there)
    double analysis_s;                           // (out) time spent on the graph
} fg_frame;

int fg_init(void);                               // nonzero: no frame graph (the driver lacks something)
void fg_submit(fg_frame *f);                     // analyse, then submit the equivalent stream
void fg_flush_call(const char *name);            // a call that was not held (program links, queries, ...)
void fg_swap(void);
void fg_context(void);                           // at the first frame (a context is current): the extensions
// shaders that read depth/stencil through framebuffer fetch (glrec.c's wrappers, any thread)
void fg_note_shader(unsigned shader, int fetch);
void fg_note_attach(unsigned program, unsigned shader);
void fg_note_link(unsigned program, int binary);
void fg_note_delete(unsigned program);
int fg_sources_wanted(void);                     // nonzero: turn Unity's program binary cache off
void fg_log(unsigned long frames, double fg_s);

#endif
