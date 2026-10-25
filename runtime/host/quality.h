// The handheld quality profile's caps on QualitySettings setters a game calls (UA_QUALITY), see quality.c
#pragma once

void quality_init(const char *package_caps);            // once, before Unity starts (android_package.json's "quality")
void *quality_dlsym_hook(const char *symbol, void *found); // from the host's dlsym hook
void quality_frame(void *user);                         // UnityMain, after each nativeRender
