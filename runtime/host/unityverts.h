// unityverts.c: Unity's dynamic batching vertex transform without its per-vertex overhead (UA_FAST_VERTS=1 / check)
#ifndef UNITYVERTS_H
#define UNITYVERTS_H
#include <stddef.h>
#include <stdint.h>
// called by unityhooks_on_load with the relocated libunity.so: hooks the transform when UA_FAST_VERTS is
// set and the library is one whose transform was read
void unityverts_on_load(uintptr_t base, size_t size);
#endif
