// unityicalls.c: engine calls answered from their earlier answers (UA_PROPERTY_ID=1)
#ifndef UNITYICALLS_H
#define UNITYICALLS_H
// the loader's dlsym hook chain: wraps mono_add_internal_call when a switch asks for it
void *unityicalls_dlsym_hook(const char *symbol, void *found);
// UnityMain, after each frame: which thread is UnityMain
void unityicalls_frame(void);
#endif
