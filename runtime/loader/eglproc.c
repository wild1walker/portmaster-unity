/*
 * eglproc.c - eglGetProcAddress for Android code: the shims first, then the host's EGL.
 * (It was softfp.c: the 32-bit player's softfp wrappers for float-taking host functions, removed
 * with armhf; the last tree with them is the tag last-armhf.)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include "so_loader.h"

/* eglGetProcAddress for Android code: host lookup. The EGL
 * glue (android/egl_glue.c) has its own, registered first; this is the fallback. */
static void *(*r_eglGetProcAddress)(const char *);
void *so_egl_get_proc_address(const char *name)
{
    if (!r_eglGetProcAddress) {
        void *h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (h) r_eglGetProcAddress = (void *(*)(const char *))dlsym(h, "eglGetProcAddress");
        if (!r_eglGetProcAddress) {
            fprintf(stderr, "[so] eglGetProcAddress(%s): host libEGL not available\n", name);
            return NULL;
        }
    }
    void *p = so_shim_lookup(name);   /* shims may override GL entry points */
    if (p) return p;
    return (void *)r_eglGetProcAddress(name);
}
