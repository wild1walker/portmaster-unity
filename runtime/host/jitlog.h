// Mono's JIT compilations timed (UA_JIT_LOG), see jitlog.c
#pragma once
void jitlog_init(void);   // once, before Unity starts
void jitlog_frame(void);  // UnityMain, after each frame
void jitlog_set_resolver(void *(*sym)(const char *name));   // tests: Mono's functions by name
long long jitlog_compiles(void);                             // compiles counted so far
