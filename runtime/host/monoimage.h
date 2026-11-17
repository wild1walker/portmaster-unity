#pragma once
// UA_MONO_IMAGE_MAP: the game's assemblies read in place from the APK (monoimage.c)
void monoimage_set_apk(const char *apk);
void *monoimage_dlsym_hook(const char *sym, void *found);
