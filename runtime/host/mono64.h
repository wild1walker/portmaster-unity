#ifndef UA_MONO64_H
#define UA_MONO64_H
// the Mono route on the IL2CPP player (mono64.c). scripting: android_package.json's "scripting"
// ("mono" or ""); gc_time_slice_ms: its "gc_time_slice_ms" (boot.config's gc-max-time-slice, 0:
// none). Returns 1 when the bridge is in place, 0 when not asked for, -1 on an error.
int mono64_init(const char *libdir, const char *scripting, const char *unity_version, int gc_time_slice_ms);
#endif
