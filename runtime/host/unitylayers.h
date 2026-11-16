// unitylayers.c: UaHost::LayersInScene(mask), an internal call a game patch may ask (is a renderer on these layers?)
#ifndef UNITYLAYERS_H
#define UNITYLAYERS_H
#include <stddef.h>
#include <stdint.h>
void unitylayers_on_load(uintptr_t base, size_t size);       // from unityhooks_on_load (libunity relocated)
void *unitylayers_dlsym_hook(const char *symbol, void *found); // wraps mono_add_internal_call
void unitylayers_frame(void);                                // UnityMain after each frame: a count now and then
#endif
