// Sampling profiler (prof.c): UA_PROF=<hz>, UA_PROF_THREADS, UA_PROF_SECS, UA_PROF_START, <root>/prof.req.
#ifndef PROF_H
#define PROF_H
void prof_init(const char *root);   // once, before Unity starts; nothing happens unless UA_PROF is set
void prof_swap(void);               // from the swap hook (the render thread), every frame
void prof_main_frame(void);         // from UnityMain, after each frame
#endif
