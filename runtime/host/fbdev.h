// The fbdev display path (Mali's fbdev EGL on the framebuffer): how many buffers the driver's
// window flips between, and when each frame reached the screen (fbdev.c).
#pragma once

// Before Unity's first EGL call, on the fbdev path only: the buffer count (the framebuffer's
// virtual height, UA_FB_BUFFERS, put back when the host ends) and how frames are flipped
// (UA_FB_FLIP: the host's flip thread or the driver's own pans; see fbdev.c).
void fbdev_setup(void);
// Puts the framebuffer's geometry back now (the host's exit and crash paths; idempotent).
void fbdev_restore(void);
// Buffers Mali's window flips between (yres_virtual / yres when the host looked), 0 if unknown.
int fbdev_buffers(void);
// After fbdev_setup: 1 and the size Unity's window should have when UA_RENDER_SIZE / UA_RENDER_SCALE
// asks for one smaller than the panel (the display engine scales it up; fbdev.c), else 0.
int fbdev_render_size(int *w, int *h);
// The render size for a pw x ph panel and mem_kb of memory from the variables' values (NULL: unset):
// 1 and the size, or 0 for the panel's own (fbdev_setup's choice; tests).
// UA_RENDER_SIZE / UA_RENDER_SCALE without Allwinner's display engine: 1, the render size and the
// panel's (display.c's GPU scaler draws the frame over the panel), else 0
int fbdev_gpu_render_size(int *w, int *h, int *pw, int *ph);
int fbdev_pick_render_size(int pw, int ph, const char *size, const char *scale, const char *ram, long mem_kb, int *w, int *h);

// Flips (fbdev.c interposes the driver's FBIOPAN_DISPLAY calls). fbdev_swap() is called right
// before each eglSwapBuffers, fbdev_swap_done(<its result>) right after it (with host flips it waits
// there until the buffer the next frame will draw into has left the screen; a failed swap flips
// nothing). fbdev_take_stats() returns the
// frames whose flip completed since the last call, their average and longest time from the swap to
// the end of their flip (the vblank they are shown from), how long the render thread waited in
// fbdev_swap_done, in ms, and the sum of the queue depths it allowed (host flips). 0 frames when the
// driver never panned.
void fbdev_swap(void);
void fbdev_swap_done(int ok);
// Right before the driver creates a window surface of w x h: with host flips, the flips still queued
// are waited for and the frame count starts again (the driver's new window starts at the buffer on
// the screen); a window smaller than the screen makes the pans from then on carry its size (the
// display engine's crop: it scales the window to the panel).
void fbdev_new_window(int w, int h);
int fbdev_take_stats(double *avg_ms, double *max_ms, double *waited_ms, double *queue_sum);
