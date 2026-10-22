// A heap budget for games that switch Unity's garbage collector off (UA_GC_BUDGET), see gcbudget.c
#pragma once
void gcbudget_init(void);                                   // once, before Unity starts
void *gcbudget_dlsym_hook(const char *symbol, void *found); // from the host's dlsym hook
void gcbudget_frame(void *user);                            // UnityMain, after each nativeRender
