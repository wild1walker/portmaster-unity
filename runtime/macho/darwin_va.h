// darwin_va.h - variable arguments as macOS passes them on arm64: every one on the stack, in
// 8-byte slots, whatever its type; the registers hold the named arguments only. A va_list
// there is one pointer into those slots.
#ifndef UA_DARWIN_VA_H
#define UA_DARWIN_VA_H

#include <stdarg.h>
#include <stddef.h>

// In a variadic shim: ap reads the caller's stack slots (AAPCS64's va_start would begin with
// the registers the caller never filled).
#define DARWIN_VA_START(ap, last) do { va_start(ap, last); (ap).__gr_offs = 0; (ap).__vr_offs = 0; } while (0)
// In a shim that is handed a Darwin va_list: ap reads from it.
#define DARWIN_VA_FROM(ap, darwin_list) do { \
        (ap).__stack = (darwin_list); (ap).__gr_top = NULL; (ap).__vr_top = NULL; \
        (ap).__gr_offs = 0; (ap).__vr_offs = 0; } while (0)

// printf and scanf formats as glibc reads them: long double is double on macOS ("%Lf" loses
// its L), "%p" prints 0x0 for NULL there, %D %O %U are %ld %lo %lu. Returns fmt itself when
// nothing is to change, buf when the result fits it, else malloc'd memory (free it when it is
// neither); NULL: no memory.
const char    *darwin_fmt(const char *fmt, int scan, char *buf, size_t buflen);
const wchar_t *darwin_wfmt(const wchar_t *fmt, int scan, wchar_t *buf, size_t buflen);

#endif
