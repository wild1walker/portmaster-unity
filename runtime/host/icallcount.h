// icallcount.c: the engine's internal calls (UA_ICALL_COUNT=<s>) and the runtime API's calls (UA_API_COUNT=<s>) counted, measurements
#ifndef ICALLCOUNT_H
#define ICALLCOUNT_H
// the loader's dlsym hook chain: wraps mono_add_internal_call when UA_ICALL_COUNT is set
void *icallcount_dlsym_hook(const char *symbol, void *found);
// the last of the hook chain: UA_API_COUNT's counting thunks for the runtime's API (mono_*, il2cpp_*)
void *icallcount_api_hook(const char *symbol, void *found);
// UnityMain, after each frame: the log lines every UA_ICALL_COUNT seconds
void icallcount_frame(void);
#endif
