// unityphys2d.c: Unity 2017.4's 2D physics without its per-collider scans (UA_PHYS2D)
#ifndef UNITYPHYS2D_H
#define UNITYPHYS2D_H
#include <stddef.h>
#include <stdint.h>
void unityphys2d_on_load(uintptr_t base, size_t size);        // rows checked, before any hook changes libunity
void unityphys2d_on_load_patch(uintptr_t base, size_t size);  // the hooks, after the other hooks
void unityphys2d_frame(void);                                 // UnityMain, after each frame
#endif
