/*
 * "Android side" of the fakejni test. On 32-bit ARM it is compiled with -mfloat-abi=softfp
 * -ffreestanding so it calls the JNI table exactly like code built by the NDK for armeabi-v7a
 * (floats/doubles in core registers), and its natives are softfp functions. No libc headers
 * (they would need the soft-float stubs); printf/strcmp are declared by hand (variadic or
 * integer-only: same ABI). On AArch64 it is ordinary code: arm64-v8a and the host share AAPCS64.
 */
#include "../include/jni.h"

int printf(const char *, ...);
int strcmp(const char *, const char *);
int snprintf(char *, unsigned long, const char *, ...);

/* nat_wide's result for the arguments both tests pass (u is a 3-character string) */
#define WIDE_SUM (1 + 2.5 * 2 + 3 * 3 + 4.25 * 4 - 5 * 5 + 6.5 * 6 + 7000000000.0 * 7 + 8.125 * 8 + 9 * 9 \
                  + 10.5 * 10 - 11 * 11 + 12.75 * 12 + 13 * 13 + 14.5 * 14 + 1 * 15 - 16 * 16 + 17 * 17   \
                  - 18 * 18 + 19.25 * 19 + 20.5 * 20 + 3 * 21 + 22 * 22)

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  [android] FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int feq(double a, double b) { double d = a - b; return d < 1e-5 && d > -1e-5; }

/* ---------- natives (softfp) ---------- */
static jdouble nat_mixed(JNIEnv *env, jclass cls, jint i, jlong j, jfloat f, jdouble d, jobject o, jint k)
{
    (void)cls;
    const char *s = o ? (*env)->GetStringUTFChars(env, (jstring)o, 0) : "null";
    double r = i + (double)j + f + d + k + (s[0] == 'x' ? 1000.0 : 0.0);
    if (o) (*env)->ReleaseStringUTFChars(env, (jstring)o, s);
    return r;
}

static jfloat nat_halve(JNIEnv *env, jobject self, jfloat f) { (void)env; (void)self; return f / 2.0f; }

static jlong nat_long(JNIEnv *env, jclass cls, jint a, jlong b, jint c, jlong d)
{ (void)env; (void)cls; return (jlong)a * 1000000000000LL + b + c + d; }

static jboolean nat_bool(JNIEnv *env, jobject self, jboolean z) { (void)env; (void)self; return !z; }

static jstring nat_str(JNIEnv *env, jclass cls, jstring a, jint n)
{
    (void)cls;
    const char *s = (*env)->GetStringUTFChars(env, a, 0);
    char buf[128];
    snprintf(buf, sizeof buf, "%s:%d", s, n);
    (*env)->ReleaseStringUTFChars(env, a, s);
    return (*env)->NewStringUTF(env, buf);
}

/* More arguments than registers. AArch64: env, cls, a, c, e, g, i, k fill x0-x7 and b..s fill
 * v0-v7, so m, o, p, q, r, t (a float), u, v go to the stack, in that order. ARM softfp: r0-r3
 * then 8-byte aligned stack words. Each argument has its own weight, so any misplaced one shows. */
static jdouble nat_wide(JNIEnv *env, jclass cls, jint a, jfloat b, jlong c, jdouble d, jint e, jfloat f,
                        jlong g, jdouble h, jint i, jfloat j, jlong k, jdouble l, jint m, jfloat n,
                        jboolean o, jbyte p, jchar q, jshort r, jdouble s, jfloat t, jobject u, jint v)
{
    (void)cls;
    double ulen = u ? (double)(*env)->GetStringLength(env, (jstring)u) : -100.0;
    return a * 1.0 + b * 2.0 + c * 3.0 + d * 4.0 + e * 5.0 + f * 6.0 + g * 7.0 + h * 8.0 + i * 9.0
         + j * 10.0 + k * 11.0 + l * 12.0 + m * 13.0 + n * 14.0 + o * 15.0 + p * 16.0 + q * 17.0
         + r * 18.0 + s * 19.0 + t * 20.0 + ulen * 21.0 + v * 22.0;
}

/* Ten floats: two of them on the AArch64 stack (4-byte values in 8-byte slots), float result. */
static jfloat nat_fsum(JNIEnv *env, jclass cls, jfloat a, jfloat b, jfloat c, jfloat d, jfloat e,
                       jfloat f, jfloat g, jfloat h, jfloat i, jfloat j)
{
    (void)env; (void)cls;
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10;
}

static const JNINativeMethod natives[] = {
    { "mixed", "(IJFDLjava/lang/String;I)D", (void *)nat_mixed },
    { "halve", "(F)F", (void *)nat_halve },
    { "lng", "(IJIJ)J", (void *)nat_long },
    { "neg", "(Z)Z", (void *)nat_bool },
    { "str", "(Ljava/lang/String;I)Ljava/lang/String;", (void *)nat_str },
    { "wide", "(IFJDIFJDIFJDIFZBCSDFLjava/lang/String;I)D", (void *)nat_wide },
    { "fsum", "(FFFFFFFFFF)F", (void *)nat_fsum },
};

/* V-variant caller, like Unity's jni::Op<>::CallMethod wrappers */
static jobject call_static_obj_v(JNIEnv *env, jclass c, jmethodID m, ...)
{
    va_list ap;
    va_start(ap, m);
    jobject r = (*env)->CallStaticObjectMethodV(env, c, m, ap);
    va_end(ap);
    return r;
}

static jdouble call_static_double_v(JNIEnv *env, jclass c, jmethodID m, ...)
{
    va_list ap;
    va_start(ap, m);
    jdouble r = (*env)->CallStaticDoubleMethodV(env, c, m, ap);
    va_end(ap);
    return r;
}

static jfloat call_float_v(JNIEnv *env, jobject o, jmethodID m, ...)
{
    va_list ap;
    va_start(ap, m);
    jfloat r = (*env)->CallFloatMethodV(env, o, m, ap);
    va_end(ap);
    return r;
}

static int str_is(JNIEnv *env, jobject s, const char *want)
{
    if (!s) { printf("  [android] got null string, want %s\n", want); return 0; }
    const char *c = (*env)->GetStringUTFChars(env, (jstring)s, 0);
    int ok = !strcmp(c, want);
    if (!ok) printf("  [android] got \"%s\", want \"%s\"\n", c, want);
    (*env)->ReleaseStringUTFChars(env, (jstring)s, c);
    return ok;
}

/* Entry from the (hardfp) host: integer-only signature, safe across the ABI boundary.
 * JNI_OnLoad style: receives the JavaVM. Returns the number of failures. */
int android_side_run(JavaVM *vm)
{
    JNIEnv *env = 0;
    CHECK((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK && env);
    CHECK((*env)->GetVersion(env) == JNI_VERSION_1_6);

    jclass calc = (*env)->FindClass(env, "test/Calc");
    CHECK(calc != 0);

    /* static, all Call variants, mixed args incl. long/double alignment */
    jmethodID mix = (*env)->GetStaticMethodID(env, calc, "mix", "(IJFDLjava/lang/String;ZBCS)Ljava/lang/String;");
    CHECK(mix != 0);
    jstring hi = (*env)->NewStringUTF(env, "hi");
    const char *want = "7 -9000000000 1.5 -2.25 hi 1 -3 65 -300";
    jobject r1 = (*env)->CallStaticObjectMethod(env, calc, mix, (jint)7, (jlong)-9000000000LL, (jfloat)1.5f,
                                                (jdouble)-2.25, hi, (jboolean)1, (jbyte)-3, (jchar)'A', (jshort)-300);
    CHECK(str_is(env, r1, want));
    jobject r2 = call_static_obj_v(env, calc, mix, (jint)7, (jlong)-9000000000LL, (jfloat)1.5f,
                                   (jdouble)-2.25, hi, (jboolean)1, (jbyte)-3, (jchar)'A', (jshort)-300);
    CHECK(str_is(env, r2, want));
    jvalue av[9];
    av[0].i = 7; av[1].j = -9000000000LL; av[2].f = 1.5f; av[3].d = -2.25; av[4].l = hi;
    av[5].z = 1; av[6].b = -3; av[7].c = 'A'; av[8].s = -300;
    jobject r3 = (*env)->CallStaticObjectMethodA(env, calc, mix, av);
    CHECK(str_is(env, r3, want));

    jmethodID sum = (*env)->GetStaticMethodID(env, calc, "sum", "(IJFD)D");
    CHECK(feq((*env)->CallStaticDoubleMethod(env, calc, sum, (jint)1, (jlong)2, (jfloat)0.5f, (jdouble)0.25), 3.75));
    CHECK(feq(call_static_double_v(env, calc, sum, (jint)-1, (jlong)(1LL << 40), (jfloat)0.5f, (jdouble)0.25),
              (double)(1LL << 40) - 0.25));
    jvalue sv[4]; sv[0].i = 3; sv[1].j = 4; sv[2].f = 0.5f; sv[3].d = 0.5;
    CHECK(feq((*env)->CallStaticDoubleMethodA(env, calc, sum, sv), 8.0));

    /* constructor with float arg, instance methods, float return */
    jmethodID ctor = (*env)->GetMethodID(env, calc, "<init>", "(IF)V");
    jobject obj = (*env)->NewObject(env, calc, ctor, (jint)42, (jfloat)2.5f);
    CHECK(obj != 0);
    jmethodID getVal = (*env)->GetMethodID(env, calc, "getVal", "()I");
    CHECK((*env)->CallIntMethod(env, obj, getVal) == 42);
    jmethodID scale = (*env)->GetMethodID(env, calc, "scale", "(F)F");
    CHECK(feq((*env)->CallFloatMethod(env, obj, scale, (jfloat)2.0f), 5.0));
    CHECK(feq(call_float_v(env, obj, scale, (jfloat)4.0f), 10.0));
    jvalue fv[1]; fv[0].f = 0.5f;
    CHECK(feq((*env)->CallFloatMethodA(env, obj, scale, fv), 1.25));
    CHECK(feq((*env)->CallNonvirtualFloatMethod(env, obj, calc, scale, (jfloat)1.0f), 2.5));
    jmethodID lmul = (*env)->GetMethodID(env, calc, "lmul", "(JI)J");
    CHECK((*env)->CallLongMethod(env, obj, lmul, (jlong)3000000000LL, (jint)3) == 9000000000LL);
    jmethodID isPos = (*env)->GetMethodID(env, calc, "isPos", "()Z");
    CHECK((*env)->CallBooleanMethod(env, obj, isPos) == JNI_TRUE);
    jmethodID gb = (*env)->GetMethodID(env, calc, "b", "()B");
    jmethodID gc = (*env)->GetMethodID(env, calc, "c", "()C");
    jmethodID gs = (*env)->GetMethodID(env, calc, "s", "()S");
    jmethodID gd = (*env)->GetMethodID(env, calc, "d", "(D)D");
    CHECK((*env)->CallByteMethod(env, obj, gb) == -5);
    CHECK((*env)->CallCharMethod(env, obj, gc) == 0x263a);
    CHECK((*env)->CallShortMethod(env, obj, gs) == -1234);
    CHECK(feq((*env)->CallDoubleMethod(env, obj, gd, (jdouble)1e10), 2e10));
    jmethodID vv = (*env)->GetMethodID(env, calc, "touch", "(IDIF)V");
    (*env)->CallVoidMethod(env, obj, vv, (jint)1, (jdouble)2.0, (jint)3, (jfloat)4.0f);
    jfieldID ftouch = (*env)->GetFieldID(env, calc, "touched", "D");
    CHECK(feq((*env)->GetDoubleField(env, obj, ftouch), 10.0));

    /* fields with float/double (softfp setters) */
    jfieldID fval = (*env)->GetFieldID(env, calc, "val", "I");
    CHECK((*env)->GetIntField(env, obj, fval) == 42);
    (*env)->SetIntField(env, obj, fval, 43);
    CHECK((*env)->CallIntMethod(env, obj, getVal) == 43);
    jfieldID ff = (*env)->GetFieldID(env, calc, "ratio", "F");
    CHECK(feq((*env)->GetFloatField(env, obj, ff), 2.5));
    (*env)->SetFloatField(env, obj, ff, 0.75f);
    CHECK(feq((*env)->GetFloatField(env, obj, ff), 0.75));
    jfieldID sf = (*env)->GetStaticFieldID(env, calc, "PI", "D");
    CHECK(feq((*env)->GetStaticDoubleField(env, calc, sf), 3.14159));
    (*env)->SetStaticDoubleField(env, calc, sf, 2.5);
    CHECK(feq((*env)->GetStaticDoubleField(env, calc, sf), 2.5));
    jfieldID sname = (*env)->GetStaticFieldID(env, calc, "NAME", "Ljava/lang/String;");
    jobject nm = (*env)->GetStaticObjectField(env, calc, sname);
    CHECK(str_is(env, nm, "calc"));

    /* arrays passed from "Android" to a handler and back */
    jfloatArray fa = (*env)->NewFloatArray(env, 4);
    jmethodID fill = (*env)->GetMethodID(env, calc, "fill", "([F)V");
    (*env)->CallVoidMethod(env, obj, fill, fa);
    jfloat tmp[4];
    (*env)->GetFloatArrayRegion(env, fa, 0, 4, tmp);
    CHECK(feq(tmp[0], 0.0) && feq(tmp[3], 3.0 * 0.75));
    jbyteArray ba = (*env)->NewByteArray(env, 3);
    jbyte bb[3] = { 1, -2, 3 };
    (*env)->SetByteArrayRegion(env, ba, 0, 3, bb);
    jbyte *bp = (*env)->GetByteArrayElements(env, ba, 0);
    CHECK(bp[1] == -2);
    (*env)->ReleaseByteArrayElements(env, ba, bp, 0);
    CHECK((*env)->GetArrayLength(env, ba) == 3);

    /* virtual dispatch through a base-class method id */
    jclass base = (*env)->FindClass(env, "test/Base");
    jclass der = (*env)->FindClass(env, "test/Derived");
    jmethodID who = (*env)->GetMethodID(env, base, "who", "()Ljava/lang/String;");
    jobject d = (*env)->AllocObject(env, der);
    CHECK(str_is(env, (*env)->CallObjectMethod(env, d, who), "derived"));
    CHECK(str_is(env, (*env)->CallNonvirtualObjectMethod(env, d, base, who), "base"));
    CHECK((*env)->IsInstanceOf(env, d, base));
    CHECK(!(*env)->IsInstanceOf(env, obj, base));
    CHECK((*env)->IsAssignableFrom(env, der, base));
    CHECK((*env)->IsSameObject(env, (*env)->GetSuperclass(env, der), base));

    /* exceptions */
    jmethodID boom = (*env)->GetStaticMethodID(env, calc, "boom", "()V");
    CHECK(!(*env)->ExceptionCheck(env));
    (*env)->CallStaticVoidMethod(env, calc, boom);
    CHECK((*env)->ExceptionCheck(env));
    jthrowable ex = (*env)->ExceptionOccurred(env);
    CHECK(ex != 0);
    (*env)->ExceptionClear(env);
    CHECK(!(*env)->ExceptionCheck(env));
    jclass thr = (*env)->GetObjectClass(env, ex);
    jmethodID ts = (*env)->GetMethodID(env, thr, "toString", "()Ljava/lang/String;");
    CHECK(str_is(env, (*env)->CallObjectMethod(env, ex, ts), "java.lang.IllegalStateException: boom!"));

    /* unknown things: placeholders, never crash */
    jclass unk = (*env)->FindClass(env, "com/example/DoesNotExist");
    CHECK(unk != 0);
    jmethodID um = (*env)->GetMethodID(env, unk, "foo", "(IF)Ljava/lang/String;");
    CHECK(um != 0);
    CHECK((*env)->CallObjectMethod(env, obj, um, (jint)1, (jfloat)2.0f) == 0);
    jmethodID um2 = (*env)->GetStaticMethodID(env, unk, "bar", "()D");
    CHECK(feq((*env)->CallStaticDoubleMethod(env, unk, um2), 0.0));
    jfieldID uf = (*env)->GetStaticFieldID(env, unk, "X", "I");
    CHECK((*env)->GetStaticIntField(env, unk, uf) == 0);
    CHECK(!(*env)->ExceptionCheck(env));
    jclass absent = (*env)->FindClass(env, "com/example/Absent");
    CHECK(absent == 0 && (*env)->ExceptionCheck(env));
    (*env)->ExceptionClear(env);

    /* Class.forName(binary name, initialize, loader), as libunity's class lookups use it */
    jclass cc = (*env)->FindClass(env, "java/lang/Class");
    jmethodID fn3 = (*env)->GetStaticMethodID(env, cc, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    jobject k = (*env)->CallStaticObjectMethod(env, cc, fn3, (*env)->NewStringUTF(env, "test.Calc"), (jboolean)0, (jobject)0);
    CHECK(k != 0 && (*env)->IsSameObject(env, k, calc) && !(*env)->ExceptionCheck(env));
    k = (*env)->CallStaticObjectMethod(env, cc, fn3, (*env)->NewStringUTF(env, "com.example.Absent"), (jboolean)0, (jobject)0);
    CHECK(k == 0 && (*env)->ExceptionCheck(env));
    jthrowable cnf = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionClear(env);
    CHECK((*env)->IsInstanceOf(env, cnf, (*env)->FindClass(env, "java/lang/ClassNotFoundException")));

    /* RegisterNatives + Java-side call of a native through the JNI table */
    jclass nat = (*env)->FindClass(env, "test/Natives");
    CHECK((*env)->RegisterNatives(env, nat, natives, sizeof natives / sizeof natives[0]) == JNI_OK);
    jmethodID mm = (*env)->GetStaticMethodID(env, nat, "mixed", "(IJFDLjava/lang/String;I)D");
    jstring xs = (*env)->NewStringUTF(env, "xyz");
    CHECK(feq((*env)->CallStaticDoubleMethod(env, nat, mm, (jint)1, (jlong)2, (jfloat)0.5f, (jdouble)0.25, xs, (jint)10),
              1013.75));
    jmethodID mw = (*env)->GetStaticMethodID(env, nat, "wide", "(IFJDIFJDIFJDIFZBCSDFLjava/lang/String;I)D");
    CHECK(feq((*env)->CallStaticDoubleMethod(env, nat, mw, (jint)1, (jfloat)2.5f, (jlong)3, (jdouble)4.25,
              (jint)-5, (jfloat)6.5f, (jlong)7000000000LL, (jdouble)8.125, (jint)9, (jfloat)10.5f, (jlong)-11,
              (jdouble)12.75, (jint)13, (jfloat)14.5f, (jboolean)1, (jbyte)-16, (jchar)17, (jshort)-18,
              (jdouble)19.25, (jfloat)20.5f, xs, (jint)22), WIDE_SUM));
    jmethodID mf = (*env)->GetStaticMethodID(env, nat, "fsum", "(FFFFFFFFFF)F");
    jvalue fa10[10];
    for (int q = 0; q < 10; q++) fa10[q].f = 0.5f * (q + 1);
    CHECK(feq((*env)->CallStaticFloatMethodA(env, nat, mf, fa10), 192.5));
    jmethodID mh = (*env)->GetMethodID(env, nat, "halve", "(F)F");
    jobject natobj = (*env)->AllocObject(env, nat);
    CHECK(feq((*env)->CallFloatMethod(env, natobj, mh, (jfloat)3.0f), 1.5));

    /* local frames */
    CHECK((*env)->PushLocalFrame(env, 16) == JNI_OK);
    jstring keep = (*env)->NewStringUTF(env, "kept");
    for (int i = 0; i < 100; i++) (*env)->NewStringUTF(env, "garbage");
    jstring kept = (jstring)(*env)->PopLocalFrame(env, keep);
    CHECK(str_is(env, kept, "kept"));
    jobject g = (*env)->NewGlobalRef(env, kept);
    (*env)->DeleteLocalRef(env, kept);
    CHECK(str_is(env, g, "kept"));
    CHECK((*env)->GetObjectRefType(env, g) == JNIGlobalRefType);
    (*env)->DeleteGlobalRef(env, g);

    /* strings: UTF-16 / modified UTF-8 */
    jstring u = (*env)->NewStringUTF(env, "a\xc3\xa9\xe2\x82\xac");      /* a é € */
    CHECK((*env)->GetStringLength(env, u) == 3);
    CHECK((*env)->GetStringUTFLength(env, u) == 6);
    jchar uc[3];
    (*env)->GetStringRegion(env, u, 0, 3, uc);
    CHECK(uc[0] == 'a' && uc[1] == 0xe9 && uc[2] == 0x20ac);
    jchar raw[2] = { 'O', 'K' };
    CHECK(str_is(env, (*env)->NewString(env, raw, 2), "OK"));

    printf("  [android] side done, %d failure(s)\n", fails);
    return fails;
}
