// Frame statistics (UA_FPS) and screenshots (<root>/shot.req), see frames.c
#pragma once
void frames_init(const char *root, int width, int height);
// frames presented so far (eglSwapBuffers calls), any thread
unsigned long frames_presented(void);
// the first frame's time (CLOCK_MONOTONIC s; 0 before it): what [hitch] lines count from
double frames_t_start(void);
