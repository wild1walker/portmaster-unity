// unitymath.c: Unity's 4x4 matrix inverse with its last results kept (UA_INV_MEMO=1)
#ifndef UNITYMATH_H
#define UNITYMATH_H
#include <stddef.h>
#include <stdint.h>
// called by unityhooks_on_load with the relocated libunity.so
void unitymath_on_load(uintptr_t base, size_t size);
#endif
