/*
 * fakejni: a JavaVM / JNIEnv implemented in C, for running Android JNI libraries (Unity's
 * libunity.so / libmain.so / libmono.so / libil2cpp.so) on a plain ARM Linux userland without
 * Java: armeabi-v7a libraries on armhf, arm64-v8a libraries on aarch64.
 *
 * Model
 *  - Every jobject is a pointer to an internal object (class, string, array, plain object).
 *    Handles are stable: a local ref, a global ref and the object pointer are the same value.
 *  - Classes are registered from C (fj_class_define). Methods and fields are registered by
 *    (class, name, JNI signature); a method is implemented by a C handler (fj_fn).
 *  - Unknown classes/methods/fields never crash: FindClass/GetMethodID/GetFieldID return a
 *    placeholder (logged once, always). Calling a placeholder returns 0/NULL.
 *  - RegisterNatives is recorded; C code fetches the native with fj_native() and can call it
 *    with the right ABI (softfp on ARM, AAPCS64 on AArch64) through fj_call_native*().
 *  - Reference counting: objects count local refs (per thread, with local frames), global and
 *    weak refs, and containment in object arrays / object fields; freed at zero (FJ_NOFREE=1
 *    disables freeing). Classes are permanent.
 *
 * ABI: on 32-bit ARM all JNI table functions use the base AAPCS (softfp), see JNI_ABI in
 * include/jni.h; on AArch64 Android and Linux share AAPCS64 and JNI_ABI is empty.
 * Handlers (fj_fn) are only called by fakejni itself and use the normal host ABI.
 *
 * Environment: FJ_TRACE=1 logs every JNI call with arguments; FJ_NOFREE=1 never frees
 * objects; FJ_STRICT=1 makes unknown classes absent (FindClass returns NULL + exception);
 * FJ_DUMP=1 prints a usage report (every class/method/field touched, implemented or not)
 * at exit; FJ_LOG=<file> sends the log to a file instead of stderr.
 */
#ifndef FAKEJNI_H
#define FAKEJNI_H

#include <stdarg.h>
#include <stdio.h>
#include "include/jni.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ABI attribute for functions that Android code calls or that we call in it (softfp on ARM). */
#define FJ_ABI JNI_ABI

/* Method implemented in C. `self` is the receiver object for instance methods and
 * constructors ("<init>": the freshly allocated object), the jclass for static methods.
 * `args` holds the decoded arguments (one jvalue per signature parameter). */
typedef jvalue (*fj_fn)(JNIEnv *env, jobject self, const jvalue *args, void *ud);

/* ---- setup -------------------------------------------------------------------------- */
void     fj_init(void);           /* idempotent; called implicitly by everything below */
JavaVM  *fj_vm(void);
JNIEnv  *fj_env(void);            /* env of the calling thread (attaches it if needed)  */
void     fj_set_trace(int on);
int      fj_tracing(void);
void     fj_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void     fj_dump(FILE *out);      /* usage report: classes/methods/fields, call counts   */

/* ---- classes, methods, fields ------------------------------------------------------- */
/* Define (or complete a placeholder for) class `name` ("java/lang/String" form; '.' ok).
 * super_name NULL means java/lang/Object. Returns the class (permanent). */
jclass   fj_class_define(const char *name, const char *super_name);
jclass   fj_class_find(const char *name);           /* NULL if not defined or placeholder */
void     fj_class_implements(const char *name, const char *iface);
void     fj_class_absent(const char *name);         /* FindClass -> NULL + exception      */
const char *fj_class_name(jclass cls);              /* "java/lang/String"                 */
jclass   fj_object_class(jobject obj);

jmethodID fj_method_define(const char *cls, const char *name, const char *sig,
                           int is_static, fj_fn fn, void *ud);
#define FJ_METHOD(cls, name, sig, fn)  fj_method_define(cls, name, sig, 0, fn, NULL)
#define FJ_STATIC(cls, name, sig, fn)  fj_method_define(cls, name, sig, 1, fn, NULL)

/* Instance fields: per-object storage, `init` is the value of objects that never set it.
 * Static fields: `init` is the value. Object values are referenced (kept alive). */
jfieldID fj_field_define(const char *cls, const char *name, const char *sig,
                         int is_static, jvalue init);
void     fj_static_int(const char *cls, const char *name, jint v);           /* "I" */
void     fj_static_long(const char *cls, const char *name, jlong v);         /* "J" */
void     fj_static_bool(const char *cls, const char *name, jboolean v);      /* "Z" */
void     fj_static_float(const char *cls, const char *name, jfloat v);       /* "F" */
void     fj_static_string(const char *cls, const char *name, const char *v); /* String */
void     fj_static_object(const char *cls, const char *name, const char *sig, jobject v);

/* Field access from C by name (signature looked up among the class' fields; created
 * with the given type if unknown: use the fj_set_* variant matching the Java type). */
void     fj_set_field(jobject obj, const char *name, const char *sig, jvalue v);
jvalue   fj_get_field(jobject obj, const char *name, const char *sig);
#define  fj_set_int(o, n, v)    fj_set_field(o, n, "I", fj_i(v))
#define  fj_set_float(o, n, v)  fj_set_field(o, n, "F", fj_f(v))
#define  fj_set_long(o, n, v)   fj_set_field(o, n, "J", fj_j(v))
#define  fj_set_bool(o, n, v)   fj_set_field(o, n, "Z", fj_z(v))
#define  fj_set_obj(o, n, s, v) fj_set_field(o, n, s, fj_l(v))

/* ---- objects -------------------------------------------------------------------------- */
/* Creation functions return a new LOCAL reference of the calling thread (like NewObject).
 * Use fj_global() to keep an object beyond the current local frame. */
jobject  fj_new(jclass cls, void *data, void (*free_data)(void *));
jobject  fj_new_by_name(const char *cls, void *data, void (*free_data)(void *));
void    *fj_data(jobject obj);                       /* user pointer, NULL if none     */
void     fj_set_data(jobject obj, void *data, void (*free_data)(void *));
jobject  fj_global(jobject obj);                     /* NewGlobalRef                   */
void     fj_release(jobject obj);                    /* DeleteGlobalRef                */
int      fj_is_instance(jobject obj, const char *cls);
void     fj_push_frame(void);                        /* PushLocalFrame for C callers   */
void     fj_pop_frame(void);                         /* PopLocalFrame(NULL)            */

/* strings */
jstring  fj_string(const char *utf8);                /* local ref; NULL -> NULL        */
const char *fj_cstr(jstring s);                      /* UTF-8, valid while s lives; NULL->NULL */

/* arrays: elem_sig is "I", "F", "B", ... or an object type "Ljava/lang/String;" / "[I" */
jarray   fj_array_new(const char *elem_sig, jsize len);
jsize    fj_array_len(jarray a);
void    *fj_array_data(jarray a);                    /* primitive arrays: element storage */
jobjectArray fj_string_array(const char *const *strs, int n);

/* exceptions: raise from a handler; the JNI caller sees ExceptionCheck() == true */
void     fj_throw(JNIEnv *env, const char *cls, const char *msg);

/* ---- natives registered by the libraries ----------------------------------------------- */
/* Raw fnPtr from RegisterNatives (sig NULL: match by name only). NULL if not registered. */
void    *fj_native(const char *cls, const char *name, const char *sig);
/* Call a registered native with the Android ABI, arguments decoded from the signature:
 * (env, self-or-class, args...). For static natives pass the jclass as self (or NULL: the
 * class itself is passed). Host varargs: pass jint/jboolean/jchar/... as int, jobject as a
 * pointer (never a plain 0: it is int-sized), jlong as jlong, jfloat/jdouble as double (C
 * promotion). Returns the result as jvalue. */
jvalue   fj_call_native(const char *cls, const char *name, const char *sig, jobject self, ...);
jvalue   fj_call_native_a(const char *cls, const char *name, const char *sig, jobject self,
                          const jvalue *args);
/* Call any Android function pointer with JNI-style arguments described by a signature:
 * env, self, then args, placed as the Android ABI wants them (32-bit ARM: base AAPCS words;
 * AArch64: AAPCS64 registers and stack). Exposed for the host/tests. */
jvalue   fj_invoke_native(void *fn, JNIEnv *env, jobject self, const char *sig,
                          const jvalue *args);
#define  fj_invoke_softfp fj_invoke_native   /* its name from the 32-bit-only days */

/* ---- interface proxies (java.lang.reflect.Proxy) -------------------------------------------- */
/* Calls on a proxy object (any method not implemented in C) are forwarded to a native:
 *   FJ_PROXY_JNIBRIDGE: bitter/jnibridge/JNIBridge.invoke(J id, Class iface, Method m, Object[] args)
 *   FJ_PROXY_REFLECTION: com/unity3d/player/ReflectionHelper.nativeProxyInvoke(I id, String name, Object[])
 * Primitive arguments are boxed (Integer/Long/Float/Double/Boolean), results unboxed.
 * JNIBridge.newInterfaceProxy is built in; ReflectionHelper.newProxyInstance uses this. */
enum { FJ_PROXY_JNIBRIDGE = 1, FJ_PROXY_REFLECTION = 2 };
jobject  fj_new_proxy(int kind, jlong id, jobjectArray interfaces);

/* ---- calling fake-Java methods from C ---------------------------------------------------- */
/* Invoke a registered Java method (virtual dispatch on obj's class). */
jvalue   fj_call(jobject obj, const char *name, const char *sig, ...);

/* ---- jvalue helpers ---------------------------------------------------------------------- */
static inline jvalue fj_i(jint v)     { jvalue r; r.j = 0; r.i = v; return r; }
static inline jvalue fj_z(jboolean v) { jvalue r; r.j = 0; r.z = v; return r; }
static inline jvalue fj_j(jlong v)    { jvalue r; r.j = v; return r; }
static inline jvalue fj_f(jfloat v)   { jvalue r; r.j = 0; r.f = v; return r; }
static inline jvalue fj_d(jdouble v)  { jvalue r; r.d = v; return r; }
static inline jvalue fj_l(jobject v)  { jvalue r; r.j = 0; r.l = v; return r; }
static inline jvalue fj_v(void)       { jvalue r; r.j = 0; return r; }

#ifdef __cplusplus
}
#endif
#endif
