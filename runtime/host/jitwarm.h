// Mono's compiles listed and done ahead at the next launch (UA_JIT_WARM), see jitwarm.c
#pragma once
void jitwarm_init(void);   // once, before Unity starts
void jitwarm_frame(void);  // UnityMain, after each frame

// A runtime's probe for the warm-up's static constructor rule (jitwarm.c, top): ran(klass) is 1 when the class's
// static constructor has run in the current domain (Mono's own flag, MonoVTable.initialized), 0 when not yet.
// flags: JITWARM_INLINE_CCTORS when the runtime's inliner runs the class constructor of an AggressiveInlining callee
// (Unity 6's Mono). Set before the first frame (jitwarm.c sets Unity 2017.4's armeabi-v7a libmono's itself, checked
// byte for byte); without one, a method touching a class that has a static constructor is never compiled ahead.
#define JITWARM_INLINE_CCTORS 1
void jitwarm_set_probe(int (*ran)(void *klass), unsigned flags);
// Mono's mono_class_vtable and mono_domain_get, for a probe (set once the warm-up is installed)
extern void *(*jitwarm_class_vtable)(void *domain, void *klass);
extern void *(*jitwarm_domain_get)(void);
