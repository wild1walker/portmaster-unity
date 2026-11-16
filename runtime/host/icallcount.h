// icallcount.c: the engine's internal calls counted (UA_ICALL_COUNT=<s>, a measurement)
#ifndef ICALLCOUNT_H
#define ICALLCOUNT_H
// the loader's dlsym hook chain: wraps mono_add_internal_call when UA_ICALL_COUNT is set
void *icallcount_dlsym_hook(const char *symbol, void *found);
// UnityMain, after each frame: the log lines every UA_ICALL_COUNT seconds
void icallcount_frame(void);
#endif
