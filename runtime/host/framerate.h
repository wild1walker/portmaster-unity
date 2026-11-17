// Desktop frame pacing for converted desktop data (UA_FRAME_RATE), see framerate.c
#pragma once
// once, before Unity starts: default_mode "desktop" or "android" (UA_FRAME_RATE wins), the display's rate
void framerate_init(const char *default_mode, float refresh_hz);
// from the host's dlsym hook: wraps the icall registration functions
void *framerate_dlsym_hook(const char *symbol, void *found);
// UnityMain, after each nativeRender (up_config.on_frame)
void framerate_frame(void *user);
int framerate_target_fps(void);   // the rate Unity is given now, 0: none (fbdev.c's UA_FB_PACE)
int framerate_display_hz(void);   // the display's rate as its mode says (fbdev.c's UA_FB_PACE=auto)
