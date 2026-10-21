// Frame statistics (UA_FPS) and screenshots (<root>/shot.req), see frames.c
#pragma once
void frames_init(const char *root, int width, int height);
// frames presented so far (eglSwapBuffers calls), any thread
unsigned long frames_presented(void);
