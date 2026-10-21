// Hitch sampler (hitches.c): UA_HITCH_TRACE=<ms>, UA_HITCH_THREADS, UA_HITCH_EVERY.
#ifndef HITCHES_H
#define HITCHES_H
void hitches_init(void);    // once, before Unity starts
void hitches_frame(void);   // from the swap hook, every frame
// the Mono runtime Unity loaded (libmono.so, or libmonobdwgc-2.0.so of Mono 4.x players), NULL before
void *mono_runtime_lib(void);
#endif
