#pragma once
// Mono's ahead-of-time compiler and images with the player's own libmono (monoaot.c)
#include <stddef.h>

// UA_MONO_AOT=<dir>: so_set_dlopen_rewrite's function: an <assembly>.dll.so Mono looks for (beside its
// assembly, inside the OBB) served from <dir> when the file is there, else NULL
const char *monoaot_rewrite(const char *path, char *buf, size_t size);

// UA_MONO_MAIN: the Mono library (handle, lookup) run as the mono program with these arguments
// (space-separated; UA_MONO_DIRS for mono_set_dirs); mono_main's result, 1 without mono_main
int monoaot_main(void *mono, void *(*sym)(void *handle, const char *name), const char *args);
