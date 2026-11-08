// Unity's frames shown by a present thread of the host's (UA_PRESENT_THREAD), see glpresent.c
#ifndef GLPRESENT_H
#define GLPRESENT_H
#include "../loader/so_loader.h"
// The GL wrappers when UA_PRESENT_THREAD=1 (else an empty table); next(name): the next layer's function.
const so_shim *glpresent_table(void *(*next)(const char *name));
int glpresent_wanted(void);                     // UA_PRESENT_THREAD=1 and the wrappers are in
// The window's side, from the display backend (display_sdl.c): a context that shares Unity's, made
// current with the window on the present thread, its swap and swap interval there.
typedef struct glpresent_window {
    void *(*create_context)(void *user, void *share);
    int (*make_current)(void *user, void *ctx);
    void (*swap)(void *user);
    void (*swap_interval)(void *user, int interval);
    void *user;
} glpresent_window;
void glpresent_start(int rw, int rh, int dw, int dh, const glpresent_window *w);   // on from here
int glpresent_active(void);
void glpresent_made_current(void *ctx);         // Unity's context current (surfaceless) on this thread; NULL: released
int glpresent_swap(void);                       // Unity's swap on this thread: 0 handed on
void glpresent_swap_interval(int interval);
long glpresent_frames(void);
// What the host's own drawing and reading at the swap (screenshots, the on-screen keyboard, the cursor)
// binds for "framebuffer 0" on this thread: the frame Unity just drew (0 when off).
unsigned glpresent_fb0(void);
void glpresent_shot(const char *path);          // the next frame the window shows written there (PPM)
#endif
