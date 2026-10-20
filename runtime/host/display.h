// Unity's display: the EGL window it draws into, picked before up_start (UA_DISPLAY, display.c).
//   fbdev  Mali's fbdev EGL: egl_glue hands Unity's ANativeWindow to the driver as its fbdev_window
//          (Knulli on the Allwinner H700 and other fbdev-flavor Mali blobs). Nothing is opened here.
//   sdl    an SDL2 window and the EGL display, config and surface SDL created for it
//          (display_sdl.c): Wayland, KMSDRM, X11, ... with Mesa or vendor GLES drivers.
//   auto   (default) fbdev when the framebuffer exists and libEGL is an fbdev-only one (no
//          window-system platform among its client extensions), else sdl; sdl failing -> fbdev.
// UA_EGL_PBUFFER=1 (tests on Mesa's surfaceless platform) always takes the fbdev code path.
#pragma once
#include <stddef.h>
#include "../android/egl_glue.h"

// Opens the display and sets the ANativeWindow size (before up_start reads it). 0 on success,
// -1 if the requested display cannot be opened (the host should stop).
int display_init(const char *title);
const char *display_backend(void);          // "fbdev" or "sdl"
// EGL wrappers to register (so_add_shims) after the host's GL hooks and before egl_shims[];
// NULL for fbdev.
const so_shim *display_egl_shims(void);
int display_refresh_rate(void);             // the display's refresh rate in Hz, 0 if unknown
int display_quit_requested(void);           // the window was closed (SDL_QUIT)
// What UA_DISPLAY=auto picks ("fbdev" or "sdl") and why. Only looks at files and libEGL's
// client extension string.
const char *display_auto_choice(char *why, size_t n);

// SDL2 backend (display_sdl.c): window + EGL objects for egl_glue; 0 on success, -1 with SDL's
// video subsystem shut down again.
int display_sdl_open(const char *title, egl_host_display *out, int *width, int *height, int *refresh);
int display_sdl_quit_requested(void);
void *display_sdl_window(void);             // the SDL_Window, NULL if none
