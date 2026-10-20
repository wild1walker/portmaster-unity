/*
 * softfp.c - softfp-ABI wrappers for host (hard-float) functions that take or
 * return float/double: GL/GLES (bound dynamically to the pointer the host gave us),
 * libm and libc (strtod, wcstod, strtold_l, atof, difftime, ...; bound statically).
 *
 * armeabi-v7a code passes float arguments and results in core registers
 * (base AAPCS, "softfp"); armhf code expects them in VFP registers. Each wrapper
 * is declared pcs("aapcs") (softfp side) and calls the real host function.
 *
 * so_softfp_wrap(name, fn) is applied automatically by the loader to every import
 * or dlsym() result that comes from the host; it returns fn itself when the
 * function needs no wrapper. The list is printed by so_softfp_list().
 *
 * AArch64: Android and Linux both use AAPCS64 (floats in SIMD registers), so there is
 * nothing to wrap and so_softfp_wrap() returns fn for every name.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include "so_loader.h"

#if defined(__arm__)
/* ---------------- GL / GLES (dynamic: real pointer captured at wrap time) ------ */
typedef unsigned int GLenum;
typedef int GLint;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef unsigned char GLboolean;

#define W(ret, name, params, args)                                  \
    static ret (*r_##name) params;                                  \
    static SO_ABI ret w_##name params { return r_##name args; }

#define F float
W(void, glBlendColor, (F a, F b, F c, F d), (a, b, c, d))
W(void, glClearColor, (F a, F b, F c, F d), (a, b, c, d))
W(void, glClearDepthf, (F a), (a))
W(void, glClearDepthfOES, (F a), (a))
W(void, glDepthRangef, (F a, F b), (a, b))
W(void, glDepthRangefOES, (F a, F b), (a, b))
W(void, glLineWidth, (F a), (a))
W(void, glPolygonOffset, (F a, F b), (a, b))
W(void, glPolygonOffsetClampEXT, (F a, F b, F c), (a, b, c))
W(void, glSampleCoverage, (F a, GLboolean b), (a, b))
W(void, glTexParameterf, (GLenum t, GLenum p, F v), (t, p, v))
W(void, glSamplerParameterf, (GLuint s, GLenum p, F v), (s, p, v))
W(void, glUniform1f, (GLint l, F a), (l, a))
W(void, glUniform2f, (GLint l, F a, F b), (l, a, b))
W(void, glUniform3f, (GLint l, F a, F b, F c), (l, a, b, c))
W(void, glUniform4f, (GLint l, F a, F b, F c, F d), (l, a, b, c, d))
W(void, glVertexAttrib1f, (GLuint i, F a), (i, a))
W(void, glVertexAttrib2f, (GLuint i, F a, F b), (i, a, b))
W(void, glVertexAttrib3f, (GLuint i, F a, F b, F c), (i, a, b, c))
W(void, glVertexAttrib4f, (GLuint i, F a, F b, F c, F d), (i, a, b, c, d))
W(void, glClearBufferfi, (GLenum b, GLint d, F depth, GLint s), (b, d, depth, s))
W(void, glProgramUniform1f, (GLuint p, GLint l, F a), (p, l, a))
W(void, glProgramUniform2f, (GLuint p, GLint l, F a, F b), (p, l, a, b))
W(void, glProgramUniform3f, (GLuint p, GLint l, F a, F b, F c), (p, l, a, b, c))
W(void, glProgramUniform4f, (GLuint p, GLint l, F a, F b, F c, F d), (p, l, a, b, c, d))
W(void, glProgramUniform1fEXT, (GLuint p, GLint l, F a), (p, l, a))
W(void, glProgramUniform2fEXT, (GLuint p, GLint l, F a, F b), (p, l, a, b))
W(void, glProgramUniform3fEXT, (GLuint p, GLint l, F a, F b, F c), (p, l, a, b, c))
W(void, glProgramUniform4fEXT, (GLuint p, GLint l, F a, F b, F c, F d), (p, l, a, b, c, d))
W(void, glMinSampleShading, (F a), (a))
W(void, glMinSampleShadingOES, (F a), (a))
W(void, glPrimitiveBoundingBox, (F a, F b, F c, F d, F e, F f, F g, F h), (a, b, c, d, e, f, g, h))
W(void, glPrimitiveBoundingBoxEXT, (F a, F b, F c, F d, F e, F f, F g, F h), (a, b, c, d, e, f, g, h))
W(void, glPrimitiveBoundingBoxOES, (F a, F b, F c, F d, F e, F f, F g, F h), (a, b, c, d, e, f, g, h))
W(void, glDepthRangeIndexedfOES, (GLuint i, F a, F b), (i, a, b))
W(void, glDepthRangeIndexedfNV, (GLuint i, F a, F b), (i, a, b))
W(void, glViewportIndexedfOES, (GLuint i, F a, F b, F c, F d), (i, a, b, c, d))
W(void, glViewportIndexedfNV, (GLuint i, F a, F b, F c, F d), (i, a, b, c, d))
/* GLES 1.x (Unity 2017 does not use it; cheap to have) */
W(void, glAlphaFunc, (GLenum f, F r), (f, r))
W(void, glPointSize, (F a), (a))
W(void, glColor4f, (F a, F b, F c, F d), (a, b, c, d))
W(void, glTexEnvf, (GLenum t, GLenum p, F v), (t, p, v))
W(void, glTranslatef, (F a, F b, F c), (a, b, c))
W(void, glScalef, (F a, F b, F c), (a, b, c))
W(void, glRotatef, (F a, F b, F c, F d), (a, b, c, d))
W(void, glOrthof, (F a, F b, F c, F d, F e, F f), (a, b, c, d, e, f))
W(void, glFrustumf, (F a, F b, F c, F d, F e, F f), (a, b, c, d, e, f))
#undef F

/* ---------------- libm / libc (static) ---------------- */
#define D1(n) static SO_ABI double s_##n(double a) { return n(a); }
#define F1(n) static SO_ABI float s_##n(float a) { return n(a); }
#define D2(n) static SO_ABI double s_##n(double a, double b) { return n(a, b); }
#define F2(n) static SO_ABI float s_##n(float a, float b) { return n(a, b); }

D1(acos) D1(asin) D1(atan) D1(cos) D1(sin) D1(tan) D1(acosh) D1(asinh) D1(atanh)
D1(cosh) D1(sinh) D1(tanh) D1(exp) D1(exp2) D1(expm1) D1(log) D1(log10) D1(log2)
D1(log1p) D1(logb) D1(sqrt) D1(cbrt) D1(ceil) D1(floor) D1(trunc) D1(round) D1(rint)
D1(nearbyint) D1(fabs) D1(erf) D1(erfc) D1(tgamma) D1(lgamma) D1(significand)
F1(acosf) F1(asinf) F1(atanf) F1(cosf) F1(sinf) F1(tanf) F1(acoshf) F1(asinhf) F1(atanhf)
F1(coshf) F1(sinhf) F1(tanhf) F1(expf) F1(exp2f) F1(expm1f) F1(logf) F1(log10f) F1(log2f)
F1(log1pf) F1(logbf) F1(sqrtf) F1(cbrtf) F1(ceilf) F1(floorf) F1(truncf) F1(roundf) F1(rintf)
F1(nearbyintf) F1(fabsf) F1(erff) F1(erfcf) F1(tgammaf) F1(lgammaf)
D2(atan2) D2(pow) D2(fmod) D2(remainder) D2(drem) D2(hypot) D2(fmin) D2(fmax) D2(fdim)
D2(copysign) D2(nextafter)
F2(atan2f) F2(powf) F2(fmodf) F2(remainderf) F2(hypotf) F2(fminf) F2(fmaxf) F2(fdimf)
F2(copysignf) F2(nextafterf)

static SO_ABI double s_ldexp(double a, int e) { return ldexp(a, e); }
static SO_ABI float s_ldexpf(float a, int e) { return ldexpf(a, e); }
static SO_ABI double s_scalbn(double a, int e) { return scalbn(a, e); }
static SO_ABI float s_scalbnf(float a, int e) { return scalbnf(a, e); }
static SO_ABI double s_scalbln(double a, long e) { return scalbln(a, e); }
static SO_ABI double s_frexp(double a, int *e) { return frexp(a, e); }
static SO_ABI float s_frexpf(float a, int *e) { return frexpf(a, e); }
static SO_ABI double s_modf(double a, double *i) { return modf(a, i); }
static SO_ABI float s_modff(float a, float *i) { return modff(a, i); }
static SO_ABI long s_lround(double a) { return lround(a); }
static SO_ABI long s_lroundf(float a) { return lroundf(a); }
static SO_ABI long s_lrint(double a) { return lrint(a); }
static SO_ABI long s_lrintf(float a) { return lrintf(a); }
static SO_ABI long long s_llround(double a) { return llround(a); }
static SO_ABI long long s_llroundf(float a) { return llroundf(a); }
static SO_ABI long long s_llrint(double a) { return llrint(a); }
static SO_ABI long long s_llrintf(float a) { return llrintf(a); }
static SO_ABI int s_ilogb(double a) { return ilogb(a); }
static SO_ABI int s_ilogbf(float a) { return ilogbf(a); }
static SO_ABI void s_sincos(double a, double *s, double *c) { sincos(a, s, c); }
static SO_ABI void s_sincosf(float a, float *s, float *c) { sincosf(a, s, c); }
static SO_ABI double s_fma(double a, double b, double c) { return fma(a, b, c); }
static SO_ABI float s_fmaf(float a, float b, float c) { return fmaf(a, b, c); }
static SO_ABI double s_remquo(double a, double b, int *q) { return remquo(a, b, q); }
static SO_ABI float s_remquof(float a, float b, int *q) { return remquof(a, b, q); }
static SO_ABI double s_nan(const char *t) { return nan(t); }
static SO_ABI float s_nanf(const char *t) { return nanf(t); }
static SO_ABI double s_lgamma_r(double a, int *s) { return lgamma_r(a, s); }
static SO_ABI int s_isnan(double a) { return isnan(a); }
static SO_ABI int s_isnanf(float a) { return isnan(a); }
static SO_ABI int s_isinf(double a) { return isinf(a); }
static SO_ABI int s_isinff(float a) { return isinf(a); }
static SO_ABI int s_finite(double a) { return isfinite(a); }
static SO_ABI int s_finitef(float a) { return isfinite(a); }
static SO_ABI int s_signbit(double a) { return signbit(a) != 0; }
/* bionic's FP_* values differ from glibc's: FP_INFINITE 1, NAN 2, NORMAL 4, SUBNORMAL 8, ZERO 16 */
static SO_ABI int s___fpclassifyd(double a)
{
    switch (fpclassify(a)) {
    case FP_INFINITE: return 0x01;
    case FP_NAN: return 0x02;
    case FP_NORMAL: return 0x04;
    case FP_SUBNORMAL: return 0x08;
    default: return 0x10;
    }
}
static SO_ABI int s___fpclassifyf(float a) { return s___fpclassifyd(a); }
static SO_ABI double s_strtod(const char *s, char **e) { return strtod(s, e); }
static SO_ABI float s_strtof(const char *s, char **e) { return strtof(s, e); }
static SO_ABI double s_strtold(const char *s, char **e) { return strtod(s, e); }  /* long double == double */
static SO_ABI double s_atof(const char *s) { return atof(s); }
/* the wide and locale forms (libc++ inside libunity 2019.4+, Mono); the locale objects are glibc's
 * (newlocale in hybris_compat.c) */
static SO_ABI double s_wcstod(const wchar_t *s, wchar_t **e) { return wcstod(s, e); }
static SO_ABI float s_wcstof(const wchar_t *s, wchar_t **e) { return wcstof(s, e); }
static SO_ABI double s_wcstold(const wchar_t *s, wchar_t **e) { return wcstod(s, e); }
static SO_ABI double s_strtod_l(const char *s, char **e, locale_t l) { return strtod_l(s, e, l); }
static SO_ABI float s_strtof_l(const char *s, char **e, locale_t l) { return strtof_l(s, e, l); }
static SO_ABI double s_strtold_l(const char *s, char **e, locale_t l) { return strtod_l(s, e, l); }
static SO_ABI double s_wcstod_l(const wchar_t *s, wchar_t **e, locale_t l) { return wcstod_l(s, e, l); }
static SO_ABI float s_wcstof_l(const wchar_t *s, wchar_t **e, locale_t l) { return wcstof_l(s, e, l); }
static SO_ABI double s_wcstold_l(const wchar_t *s, wchar_t **e, locale_t l) { return wcstod_l(s, e, l); }
static SO_ABI double s_difftime(time_t a, time_t b) { return difftime(a, b); }
static SO_ABI double s_drand48(void) { return drand48(); }
static SO_ABI double s_erand48(unsigned short x[3]) { return erand48(x); }

/* ---------------- table ---------------- */
typedef struct { const char *name; void *wrapper; void **real; } wrapent;
#define G(n) { #n, (void *)w_##n, (void **)&r_##n }
#define S(n) { #n, (void *)s_##n, NULL }
static const wrapent g_wraps[] = {
    G(glBlendColor), G(glClearColor), G(glClearDepthf), G(glClearDepthfOES), G(glDepthRangef),
    G(glDepthRangefOES), G(glLineWidth), G(glPolygonOffset), G(glPolygonOffsetClampEXT),
    G(glSampleCoverage), G(glTexParameterf), G(glSamplerParameterf), G(glUniform1f), G(glUniform2f),
    G(glUniform3f), G(glUniform4f), G(glVertexAttrib1f), G(glVertexAttrib2f), G(glVertexAttrib3f),
    G(glVertexAttrib4f), G(glClearBufferfi), G(glProgramUniform1f), G(glProgramUniform2f),
    G(glProgramUniform3f), G(glProgramUniform4f), G(glProgramUniform1fEXT), G(glProgramUniform2fEXT),
    G(glProgramUniform3fEXT), G(glProgramUniform4fEXT), G(glMinSampleShading), G(glMinSampleShadingOES),
    G(glPrimitiveBoundingBox), G(glPrimitiveBoundingBoxEXT), G(glPrimitiveBoundingBoxOES),
    G(glDepthRangeIndexedfOES), G(glDepthRangeIndexedfNV), G(glViewportIndexedfOES),
    G(glViewportIndexedfNV), G(glAlphaFunc), G(glPointSize), G(glColor4f), G(glTexEnvf),
    G(glTranslatef), G(glScalef), G(glRotatef), G(glOrthof), G(glFrustumf),

    S(acos), S(asin), S(atan), S(cos), S(sin), S(tan), S(acosh), S(asinh), S(atanh), S(cosh),
    S(sinh), S(tanh), S(exp), S(exp2), S(expm1), S(log), S(log10), S(log2), S(log1p), S(logb),
    S(sqrt), S(cbrt), S(ceil), S(floor), S(trunc), S(round), S(rint), S(nearbyint), S(fabs), S(erf),
    S(erfc), S(tgamma), S(lgamma), S(significand),
    S(acosf), S(asinf), S(atanf), S(cosf), S(sinf), S(tanf), S(acoshf), S(asinhf), S(atanhf),
    S(coshf), S(sinhf), S(tanhf), S(expf), S(exp2f), S(expm1f), S(logf), S(log10f), S(log2f),
    S(log1pf), S(logbf), S(sqrtf), S(cbrtf), S(ceilf), S(floorf), S(truncf), S(roundf), S(rintf),
    S(nearbyintf), S(fabsf), S(erff), S(erfcf), S(tgammaf), S(lgammaf),
    S(atan2), S(pow), S(fmod), S(remainder), S(drem), S(hypot), S(fmin), S(fmax), S(fdim),
    S(copysign), S(nextafter),
    S(atan2f), S(powf), S(fmodf), S(remainderf), S(hypotf), S(fminf), S(fmaxf), S(fdimf),
    S(copysignf), S(nextafterf),
    S(ldexp), S(ldexpf), S(scalbn), S(scalbnf), S(scalbln), S(frexp), S(frexpf), S(modf), S(modff),
    S(lround), S(lroundf), S(lrint), S(lrintf), S(llround), S(llroundf), S(llrint), S(llrintf),
    S(ilogb), S(ilogbf), S(sincos), S(sincosf), S(fma), S(fmaf), S(remquo), S(remquof), S(nan),
    S(nanf), S(lgamma_r), S(isnan), S(isnanf), S(isinf), S(isinff), S(finite), S(finitef),
    { "__isnan", (void *)s_isnan, NULL }, { "__isnanf", (void *)s_isnanf, NULL },
    { "__isinf", (void *)s_isinf, NULL }, { "__isinff", (void *)s_isinff, NULL },
    { "__isfinite", (void *)s_finite, NULL }, { "__isfinitef", (void *)s_finitef, NULL },
    { "__signbit", (void *)s_signbit, NULL },
    S(__fpclassifyd), S(__fpclassifyf), { "__fpclassify", (void *)s___fpclassifyd, NULL },
    S(strtod), S(strtof), S(strtold), S(atof), S(difftime), S(drand48), S(erand48),
    S(wcstod), S(wcstof), S(wcstold), S(strtod_l), S(strtof_l), S(strtold_l), S(wcstod_l), S(wcstof_l),
    S(wcstold_l),
    { NULL, NULL, NULL }
};

#define WB 512
static const wrapent *g_idx[WB];
static int g_idx_ok;
static const wrapent *g_chain[sizeof g_wraps / sizeof g_wraps[0]];

static uint32_t h32(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static void build_index(void)
{
    /* idempotent; racing threads build identical data */
    for (int i = 0; g_wraps[i].name; i++) {
        if (!g_wraps[i].wrapper) continue;
        uint32_t b = h32(g_wraps[i].name) & (WB - 1);
        g_chain[i] = g_idx[b];
        g_idx[b] = &g_wraps[i];
    }
    __atomic_store_n(&g_idx_ok, 1, __ATOMIC_RELEASE);
}

static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static const wrapent *find(const char *name)
{
    pthread_once(&g_once, build_index);
    for (const wrapent *w = g_idx[h32(name) & (WB - 1)]; w; w = g_chain[w - g_wraps])
        if (!strcmp(w->name, name)) return w;
    return NULL;
}

void *so_softfp_wrap(const char *name, void *fn)
{
    if (!fn || !name) return fn;
    const wrapent *w = find(name);
    if (!w) return fn;
    if (fn == w->wrapper) return fn;
    if (w->real) {
        void *old = __atomic_load_n(w->real, __ATOMIC_ACQUIRE);
        if (!old) __atomic_store_n(w->real, fn, __ATOMIC_RELEASE);
        else if (old != fn && getenv("SO_TRACE"))
            fprintf(stderr, "[so] softfp wrapper %s already bound to %p, ignoring %p\n", name, old, fn);
    }
    return w->wrapper;
}

/* The wrappers that call the host's code statically (libm, strtod, the classification functions):
 * usable when the host library lacks the name itself, e.g. bionic's __isfinite / __isfinitef,
 * which glibc does not export (Mono 4.x's libmonobdwgc-2.0.so of 2018.4 imports __isfinite). */
void *so_softfp_static(const char *name)
{
    const wrapent *w = name ? find(name) : NULL;
    return w && !w->real ? w->wrapper : NULL;
}

int so_softfp_count(void) { return (int)(sizeof g_wraps / sizeof g_wraps[0]) - 1; }

void so_softfp_list(FILE *out)
{
    int n = 0;
    for (int i = 0; g_wraps[i].name; i++) {
        if (!g_wraps[i].wrapper) continue;
        fprintf(out, "%s%s", n ? " " : "", g_wraps[i].name);
        n++;
    }
    fprintf(out, "\n");
}
#else
void *so_softfp_wrap(const char *name, void *fn) { (void)name; return fn; }
void *so_softfp_static(const char *name) { (void)name; return NULL; }
int so_softfp_count(void) { return 0; }
void so_softfp_list(FILE *out) { fprintf(out, "\n"); }
#endif

/* eglGetProcAddress for Android code: host lookup + softfp wrapping. The EGL
 * glue (android/egl_glue.c) has its own, registered first; this is the fallback. */
static void *(*r_eglGetProcAddress)(const char *);
void *so_egl_get_proc_address(const char *name)
{
    if (!r_eglGetProcAddress) {
        void *h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (h) r_eglGetProcAddress = (void *(*)(const char *))dlsym(h, "eglGetProcAddress");
        if (!r_eglGetProcAddress) {
            fprintf(stderr, "[so] eglGetProcAddress(%s): host libEGL not available\n", name);
            return NULL;
        }
    }
    void *p = so_shim_lookup(name);   /* shims may override GL entry points */
    if (p) return p;
    return so_softfp_wrap(name, r_eglGetProcAddress(name));
}
