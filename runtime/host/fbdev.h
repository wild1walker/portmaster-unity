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
// Right before the driver creates a window surface: with host flips, the flips still queued are
// waited for and the frame count starts again (the driver's new window starts at the buffer on
// the screen).
void fbdev_new_window(void);
int fbdev_take_stats(double *avg_ms, double *max_ms, double *waited_ms, double *queue_sum);
