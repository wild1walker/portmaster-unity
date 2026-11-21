// fmodasync.c: FMOD's once-a-frame update on a worker thread, overlapped with the rest of UnityMain's frame
// (UA_FMOD_ASYNC=1); where it may run, counted (UA_FMOD_COUNT=<s>)
#ifndef FMODASYNC_H
#define FMODASYNC_H
#include <stddef.h>
#include <stdint.h>
// libunity.so once relocated (unityhooks_on_load): the update replaced, FMOD's entries gated
void fmodasync_on_load(uintptr_t base, size_t size);
// the loader's dlsym chain: mono_runtime_invoke / il2cpp_runtime_invoke wrapped (managed code entered on UnityMain
// waits for the update)
void *fmodasync_dlsym_hook(const char *sym, void *p);
// UnityMain, after each frame: the update joined; the lines
void fmodasync_frame(void);
#endif
