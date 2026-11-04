// Mono's compiles listed and done ahead at the next launch (UA_JIT_WARM), see jitwarm.c
#pragma once
void jitwarm_init(void);   // once, before Unity starts
void jitwarm_frame(void);  // UnityMain, after each frame
