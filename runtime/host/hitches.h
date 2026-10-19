// Hitch sampler (hitches.c): UA_HITCH_TRACE=<ms>, UA_HITCH_THREADS, UA_HITCH_EVERY.
#ifndef HITCHES_H
#define HITCHES_H
void hitches_init(void);    // once, before Unity starts
void hitches_frame(void);   // from the swap hook, every frame
#endif
