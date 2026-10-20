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

// ---- host display ------------------------------------------------------------------------------
// A window whose EGL display, config and window surface the host created itself (SDL2:
// runtime/host/display_sdl.c), with the window system's own present path. With one set and
// egl_host_shims[] registered before egl_shims[], Unity's EGL calls are mapped onto it:
//   eglGetDisplay -> the host display; eglInitialize reports its version; eglTerminate keeps it
//   eglChooseConfig -> the host config only (Unity matches it like any other; contexts and the
//                      surface must share one config)
//   eglCreateWindowSurface -> a fixed handle standing for the host surface (the window system may
//                      replace the real one: SDL's KMSDRM driver does after fullscreen and VT
//                      switches); eglDestroySurface of it is a no-op
//   eglGetCurrentSurface / eglQuerySurface -> that handle / the real surface the host has current
//   eglCreateContext -> a new context with the host config (Unity's share context passed on)
//   eglMakeCurrent (host surface) -> make_current; other surfaces or a release -> the real call
//   eglSwapBuffers (host surface) -> swap hook, then swap; eglSwapInterval -> swap_interval
// Display, config and contexts are the real EGL objects, so eglGetCurrentContext,
// eglGetConfigAttrib and eglGetError go to the real libEGL unchanged.
typedef struct egl_host_display {
    // EGLDisplay, EGLConfig, and the EGLSurface the host created (until a thread makes it current
    // through make_current, eglQuerySurface of Unity's handle reads this one)
    void *display, *config, *surface;
    // Make `context` current with the host surface on the calling thread; NULL: release whatever
    // the host made current on this thread. 0 on success.
    int (*make_current)(void *user, void *context);
    // Present the host surface; the calling thread has made it current through make_current.
    // 0 on success, nonzero if this thread has no current host surface (the real
    // eglSwapBuffers is called instead).
    int (*swap)(void *user);
    int (*swap_interval)(void *user, int interval);   // 0 on success
    void *user;
} egl_host_display;

// Set before Unity's first EGL call (NULL: none, the default; Mali fbdev / UA_EGL_PBUFFER path).
void egl_glue_set_host_display(const egl_host_display *d);
// The wrappers above (and eglGetProcAddress returning them). Register them with so_add_shims
// before egl_shims[], only when a host display is set.
extern so_shim egl_host_shims[];

#ifdef __cplusplus
}
#endif
#endif
