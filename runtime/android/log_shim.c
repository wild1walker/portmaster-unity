// liblog.so: __android_log_* -> stderr as "<P>/<tag>(<tid>): <message>", one write() per line.
#define _GNU_SOURCE
#include "android_shims.h"
#include "shim_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { P_UNKNOWN, P_DEFAULT, P_VERBOSE, P_DEBUG, P_INFO, P_WARN, P_ERROR, P_FATAL, P_SILENT };

static int min_prio = -1;

static int parse_prio(const char *s)
{
    if (!s || !*s) return P_DEBUG;
    if (isdigit((unsigned char)*s)) return atoi(s);
    switch (toupper((unsigned char)*s)) {
    case 'V': return P_VERBOSE; case 'D': return P_DEBUG; case 'I': return P_INFO;
    case 'W': return P_WARN; case 'E': return P_ERROR; case 'F': return P_FATAL; case 'S': return P_SILENT;
    }
    return P_DEBUG;
}

void android_log_set_min_priority(int prio) { min_prio = prio; }

static int get_min_prio(void)
{
    if (min_prio < 0) min_prio = parse_prio(getenv("ANDROID_LOG_LEVEL"));
    return min_prio;
}

int shim_trace_enabled(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("ANDROID_SHIM_TRACE"); v = e && *e && *e != '0'; }
    return v;
}

void shim_log(const char *kind, const char *fmt, ...)
{
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "[android-shim %s] ", kind);
    va_list ap; va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    if (buf[n - 1] != '\n') buf[n++] = '\n';
    (void)!write(2, buf, n);
}

static char prio_char(int prio)
{
    static const char c[] = "??VDIWEFS";
    return prio >= 0 && prio <= P_SILENT ? c[prio] : '?';
}

int __android_log_write(int prio, const char *tag, const char *text)
{
    if (prio == P_DEFAULT || prio == P_UNKNOWN) prio = P_INFO;
    if (prio < get_min_prio() || prio >= P_SILENT) return 0;
    if (!tag) tag = "";
    if (!text) text = "";
    size_t tl = strlen(tag), xl = strlen(text);
    while (xl && (text[xl - 1] == '\n' || text[xl - 1] == '\r')) xl--;   // Unity often ends with \n
    char stackbuf[2048], *buf = stackbuf;
    size_t need = tl + xl + 32;
    if (need > sizeof stackbuf && !(buf = malloc(need))) { buf = stackbuf; xl = sizeof stackbuf - tl - 32; }
    int n = snprintf(buf, 32 + tl, "%c/%s(%ld): ", prio_char(prio), tag, (long)syscall(SYS_gettid));
    memcpy(buf + n, text, xl);
    n += xl;
    buf[n++] = '\n';
    (void)!write(2, buf, n);
    if (buf != stackbuf) free(buf);
    return 1;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    if (prio < get_min_prio() && prio > P_DEFAULT) return 0;
    char stackbuf[2048], *buf = stackbuf;
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(stackbuf, sizeof stackbuf, fmt ? fmt : "", ap);
    if (n >= (int)sizeof stackbuf && (buf = malloc(n + 1))) vsnprintf(buf, n + 1, fmt, ap2);
    else if (!buf) buf = stackbuf;
    va_end(ap2);
    int r = __android_log_write(prio, tag, buf);
    if (buf != stackbuf) free(buf);
    return r;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = __android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

int __android_log_buf_write(int bufID, int prio, const char *tag, const char *text)
{
    (void)bufID;
    return __android_log_write(prio, tag, text);
}

int __android_log_buf_print(int bufID, int prio, const char *tag, const char *fmt, ...)
{
    (void)bufID;
    va_list ap; va_start(ap, fmt);
    int r = __android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...)
{
    char buf[1024];
    if (fmt) {
        va_list ap; va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
    } else {
        snprintf(buf, sizeof buf, "Assertion failed: %s", cond ? cond : "");
    }
    __android_log_write(P_FATAL, tag, buf);
    abort();
}

int __android_log_is_loggable(int prio, const char *tag, int default_prio)
{
    (void)tag; (void)default_prio;
    return prio >= get_min_prio();
}

int __android_log_set_minimum_priority(int prio) { int old = get_min_prio(); min_prio = prio; return old; }
int __android_log_get_minimum_priority(void) { return get_min_prio(); }
