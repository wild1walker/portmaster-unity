// UA_GL_DRIVER: which GLES driver Unity draws with (gldriver.c). Off by default: the Mali blob.
#pragma once

// First thing in main: UA_GL_DRIVER=mesa makes the program start again (execv) with Mesa's
// libraries first in LD_LIBRARY_PATH, EGL_PLATFORM=surfaceless and UA_EGL_PBUFFER=1; it returns
// when nothing is to change (the blob, or Mesa already in place) or Mesa is missing (the blob then).
void gldriver_early(char **argv);
// 1 when this run draws with Mesa (after gldriver_early).
int gldriver_mesa(void);
// The fbdev path with Mesa: Unity's window is a pbuffer, and each swap of it copies the frame into
// the framebuffer's next buffer and pans to it (through fbdev.c's flips). Called by display.c after
// fbdev_setup; panel_w x panel_h is the pbuffer's size when the GPU scaler draws the frame over the
// panel (0: the window's size).
void gldriver_fbdev_present(int panel_w, int panel_h);

// The copy itself (tests): h rows of w pixels from a bottom-up RGBA8 or BGRA8 read (src, w * 4 a
// row) into a top-down 32-bit framebuffer (dst, dst_stride bytes a row), red and blue swapped when
// swap_rb is set.
void gldriver_copy_rows(unsigned char *dst, unsigned long dst_stride, const unsigned char *src, int w, int h, int swap_rb);
