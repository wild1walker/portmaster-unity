// unitypool.c: Unity's pool of GLES buffers (BufferManagerGLES) in lists that cost O(1) (UA_GLES_POOL=1)
#ifndef UNITYPOOL_H
#define UNITYPOOL_H
#include <stddef.h>
#include <stdint.h>
// called by unityhooks_on_load with the relocated libunity.so: hooks the pool when UA_GLES_POOL=1 and
// the library is one whose pool was read
void unitypool_on_load(uintptr_t base, size_t size);
#endif
