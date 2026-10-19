// EGL glue: wrapped EGL entry points for Unity (see egl_glue.c).
#ifndef EGL_GLUE_H
#define EGL_GLUE_H
#include "android_shims.h"

#ifdef __cplusplus
extern "C" {
#endif

// eglCreateWindowSurface, eglChooseConfig, eglGetConfigAttrib, eglCreateContext, eglSwapBuffers,
// eglGetProcAddress. Register before the host libEGL so these win; other egl* names resolve to the
// real libEGL. The real libEGL is dlopen'ed from env ANDROID_SHIM_LIBEGL, else libEGL.so.1 / libEGL.so.
extern so_shim egl_shims[];

// A symbol of the real libEGL (dlsym, then its eglGetProcAddress), for the loader's passthrough.
void *egl_glue_real(const char *name);

// Called on Unity's render thread right before each real eglSwapBuffers (NULL to remove).
void egl_glue_set_swap_hook(void (*hook)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
#endif
