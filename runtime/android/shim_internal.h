// Internal helpers shared by the shim sources.
#ifndef SHIM_INTERNAL_H
#define SHIM_INTERNAL_H
#include <stdio.h>
#include <stdlib.h>

// ANDROID_SHIM_TRACE=1 logs every shim call worth knowing about.
int shim_trace_enabled(void);
#define SHIM_TRACE(...) do { if (shim_trace_enabled()) shim_log("trace", __VA_ARGS__); } while (0)
#define SHIM_WARN(...) shim_log("warn", __VA_ARGS__)
void shim_log(const char *kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Unity's armeabi-v7a libraries use the base (softfp) AAPCS: float/double arguments and return
// values travel in core registers. Every shim that takes or returns floating point must use it
// too, since this code is built for armhf (VFP registers). On AArch64 the arm64-v8a libraries and
// the host share AAPCS64, so it is empty there.
#if defined(__arm__)
#define SOFTFP __attribute__((pcs("aapcs")))
#else
#define SOFTFP
#endif

#endif
