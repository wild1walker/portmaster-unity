// Pad navigation for Unity UI menus made for touch (UA_UI_NAV), see uinav.c
#pragma once
void uinav_init(void);          // once, before Unity starts
void uinav_frame(void *user);   // UnityMain, after each nativeRender
