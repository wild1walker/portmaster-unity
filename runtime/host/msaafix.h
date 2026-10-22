// MSAA present of Unity 2017.1 - 2017.3 players (msaafix.c): UA_MSAA_FIX.
#ifndef MSAAFIX_H
#define MSAAFIX_H
#include "../loader/so_loader.h"
// The GL wrappers when on (else an empty table): register with so_add_shims() before
// gltrace_table() and the EGL glue; next(name) is what each wrapper calls (another host
// wrapper of that name, NULL: the real GL function).
const so_shim *msaafix_table(int on, void *(*next)(const char *name));
void msaafix_set_size(int width, int height);   // the window, before Unity starts
void msaafix_frame(void);                        // from the swap hook, every frame
#endif
