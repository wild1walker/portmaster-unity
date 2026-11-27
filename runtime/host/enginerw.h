// enginerw.c: measuring and rewriting libunity's own UnityMain code
#ifndef ENGINERW_H
#define ENGINERW_H
#include <stddef.h>
#include <stdint.h>
void enginerw_on_load_match(uintptr_t base, size_t size);   // before any hook changes libunity
void enginerw_on_load_patch(uintptr_t base, size_t size);   // after the other hooks
void enginerw_frame(void);                                  // UnityMain, after each frame
#endif
