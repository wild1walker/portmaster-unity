/* fakejni unit test (host side, armhf). Run under QEMU: see Makefile `make test`. */
#include "../fakejni.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static int feq(double a, double b) { return fabs(a - b) < 1e-5; }

int android_side_run(JavaVM *vm);   /* softfp object, integer-only signature */

/* ---------- handlers for test/Calc ---------- */
static jvalue h_mix(JNIEnv *env, jobject cls, const jvalue *a, void *ud)
{
    char b[256];
    snprintf(b, sizeof b, "%d %lld %g %g %s %d %d %d %d", a[0].i, (long long)a[1].j, a[2].f, a[3].d,
             fj_cstr(a[4].l), a[5].z, a[6].b, a[7].c, a[8].s);
    return fj_l(fj_string(b));
}
static jvalue h_sum(JNIEnv *env, jobject cls, const jvalue *a, void *ud)
{ return fj_d(a[0].i + (double)a[1].j + a[2].f + a[3].d); }
static jvalue h_ctor(JNIEnv *env, jobject self, const jvalue *a, void *ud)
{ fj_set_int(self, "val", a[0].i); fj_set_float(self, "ratio", a[1].f); return fj_v(); }
static jvalue h_getVal(JNIEnv *env, jobject self, const jvalue *a, void *ud)
{ return fj_i(fj_get_field(self, "val", "I").i); }
static jvalue h_scale(JNIEnv *env, jobject self, const jvalue *a, void *ud)
{ return fj_f(a[0].f * fj_get_field(self, "ratio", "F").f); }
static jvalue h_lmul(JNIEnv *env, jobject self, const jvalue *a, void *ud) { return fj_j(a[0].j * a[1].i); }
static jvalue h_isPos(JNIEnv *env, jobject self, const jvalue *a, void *ud) { return fj_z(fj_get_field(self, "val", "I").i > 0); }
static jvalue h_b(JNIEnv *env, jobject self, const jvalue *a, void *ud) { jvalue v = fj_v(); v.b = -5; return v; }
static jvalue h_c(JNIEnv *env, jobject self, const jvalue *a, void *ud) { jvalue v = fj_v(); v.c = 0x263a; return v; }
static jvalue h_s(JNIEnv *env, jobject self, const jvalue *a, void *ud) { jvalue v = fj_v(); v.s = -1234; return v; }
static jvalue h_d(JNIEnv *env, jobject self, const jvalue *a, void *ud) { return fj_d(a[0].d * 2); }
static jvalue h_touch(JNIEnv *env, jobject self, const jvalue *a, void *ud)
{ fj_set_field(self, "touched", "D", fj_d(a[0].i + a[1].d + a[2].i + a[3].f)); return fj_v(); }
static jvalue h_fill(JNIEnv *env, jobject self, const jvalue *a, void *ud)
{
    jfloat *p = fj_array_data(a[0].l);
    float r = fj_get_field(self, "ratio", "F").f;
    for (int i = 0; i < fj_array_len(a[0].l); i++) p[i] = i * r;
    return fj_v();
}
static jvalue h_boom(JNIEnv *env, jobject cls, const jvalue *a, void *ud)
{ fj_throw(env, "java/lang/IllegalStateException", "boom!"); return fj_v(); }
static jvalue h_base_who(JNIEnv *env, jobject self, const jvalue *a, void *ud) { return fj_l(fj_string("base")); }
static jvalue h_der_who(JNIEnv *env, jobject self, const jvalue *a, void *ud) { return fj_l(fj_string("derived")); }

static int freed;
static void count_free(void *p) { (void)p; freed++; }

static void *thread_main(void *arg)
{
    JavaVM *vm = arg;
    JNIEnv *env = NULL;
    int ok = (*vm)->AttachCurrentThread(vm, &env, NULL) == JNI_OK && env;
    jclass calc = (*env)->FindClass(env, "test/Calc");
    jmethodID sum = (*env)->GetStaticMethodID(env, calc, "sum", "(IJFD)D");
    double r = 0;
    for (int i = 0; i < 1000; i++) r += (*env)->CallStaticDoubleMethod(env, calc, sum, (jint)1, (jlong)1, (jfloat)1.0f, (jdouble)1.0);
    ok = ok && feq(r, 4000.0);
    (*vm)->DetachCurrentThread(vm);
    return (void *)(intptr_t)ok;
}

int main(void)
{
    fj_init();
    JavaVM *vm = fj_vm();
    JNIEnv *env = fj_env();

    fj_class_define("test/Calc", NULL);
    FJ_STATIC("test/Calc", "mix", "(IJFDLjava/lang/String;ZBCS)Ljava/lang/String;", h_mix);
    FJ_STATIC("test/Calc", "sum", "(IJFD)D", h_sum);
    FJ_STATIC("test/Calc", "boom", "()V", h_boom);
    FJ_METHOD("test/Calc", "<init>", "(IF)V", h_ctor);
    FJ_METHOD("test/Calc", "getVal", "()I", h_getVal);
    FJ_METHOD("test/Calc", "scale", "(F)F", h_scale);
    FJ_METHOD("test/Calc", "lmul", "(JI)J", h_lmul);
    FJ_METHOD("test/Calc", "isPos", "()Z", h_isPos);
    FJ_METHOD("test/Calc", "b", "()B", h_b);
    FJ_METHOD("test/Calc", "c", "()C", h_c);
    FJ_METHOD("test/Calc", "s", "()S", h_s);
    FJ_METHOD("test/Calc", "d", "(D)D", h_d);
    FJ_METHOD("test/Calc", "touch", "(IDIF)V", h_touch);
    FJ_METHOD("test/Calc", "fill", "([F)V", h_fill);
    fj_field_define("test/Calc", "val", "I", 0, fj_i(0));
    fj_field_define("test/Calc", "ratio", "F", 0, fj_f(1.0f));
    fj_field_define("test/Calc", "touched", "D", 0, fj_d(0));
    fj_field_define("test/Calc", "PI", "D", 1, fj_d(3.14159));
    fj_static_string("test/Calc", "NAME", "calc");
    fj_class_define("test/Base", NULL);
    fj_class_define("test/Derived", "test/Base");
    FJ_METHOD("test/Base", "who", "()Ljava/lang/String;", h_base_who);
    FJ_METHOD("test/Derived", "who", "()Ljava/lang/String;", h_der_who);
    fj_class_define("test/Natives", NULL);
    fj_class_absent("com/example/Absent");

    printf("== android side (softfp caller)\n");
    fails += android_side_run(vm);

    printf("== host side\n");
    /* host calls through the jni.h table (JNI_ABI pointers) */
    jclass calc = (*env)->FindClass(env, "test/Calc");
    jmethodID sum = (*env)->GetStaticMethodID(env, calc, "sum", "(IJFD)D");
    CHECK(feq((*env)->CallStaticDoubleMethod(env, calc, sum, (jint)2, (jlong)3, (jfloat)0.25f, (jdouble)0.5), 5.75));

    /* natives registered by the android side, called from C with the softfp ABI */
    CHECK(fj_native("test/Natives", "mixed", NULL) != NULL);
    CHECK(fj_native("test/Natives", "nope", NULL) == NULL);
    jvalue r = fj_call_native("test/Natives", "mixed", "(IJFDLjava/lang/String;I)D", NULL,
                              (jint)5, (jlong)6, 0.5, 0.25, fj_string("abc"), (jint)1);
    CHECK(feq(r.d, 12.75));
    jvalue a[4] = { fj_i(2), fj_j(5), fj_i(7), fj_j(-1) };
    r = fj_call_native_a("test/Natives", "lng", "(IJIJ)J", NULL, a);
    CHECK(r.j == 2000000000000LL + 5 + 7 - 1);
    r = fj_call_native("test/Natives", "halve", "(F)F", NULL, 9.0);
    CHECK(feq(r.f, 4.5));
    r = fj_call_native("test/Natives", "neg", "(Z)Z", NULL, (jint)JNI_TRUE);
    CHECK(r.z == JNI_FALSE);
    r = fj_call_native("test/Natives", "str", NULL, NULL, fj_string("n"), (jint)7);
    CHECK(r.l && !strcmp(fj_cstr(r.l), "n:7"));

    /* fj_call into a handler from C */
    jobject o = fj_new_by_name("test/Calc", NULL, NULL);
    fj_set_int(o, "val", 9);
    CHECK(fj_call(o, "getVal", "()I").i == 9);
    CHECK(feq(fj_call(o, "scale", "(F)F", 3.0).f, 3.0));
    CHECK(fj_is_instance(o, "java/lang/Object") && !fj_is_instance(o, "test/Base"));

    /* strings: 4-byte UTF-8 in, surrogate pair inside, modified UTF-8 out */
    jstring emo = fj_string("x\xf0\x9f\x98\x80y");
    CHECK((*env)->GetStringLength(env, emo) == 4);
    CHECK((*env)->GetStringUTFLength(env, emo) == 8);
    const char *m = (*env)->GetStringUTFChars(env, emo, NULL);
    CHECK(!strcmp(m, "x\xed\xa0\xbd\xed\xb8\x80y"));
    (*env)->ReleaseStringUTFChars(env, emo, m);
    CHECK(!strcmp(fj_cstr(emo), "x\xf0\x9f\x98\x80y"));
    jstring mod = (*env)->NewStringUTF(env, "x\xed\xa0\xbd\xed\xb8\x80y");   /* modified form in */
    CHECK(!strcmp(fj_cstr(mod), "x\xf0\x9f\x98\x80y"));
    const jchar *jc = (*env)->GetStringChars(env, emo, NULL);
    CHECK(jc[1] == 0xd83d && jc[2] == 0xde00);
    (*env)->ReleaseStringChars(env, emo, jc);

    /* arrays */
    jintArray ia = (*env)->NewIntArray(env, 5);
    jint in[5] = { 1, 2, 3, 4, 5 };
    (*env)->SetIntArrayRegion(env, ia, 0, 5, in);
    jint *ip = (*env)->GetIntArrayElements(env, ia, NULL);
    CHECK(ip[4] == 5);
    ip[0] = 100;
    (*env)->ReleaseIntArrayElements(env, ia, ip, 0);
    jint out[2];
    (*env)->GetIntArrayRegion(env, ia, 0, 2, out);
    CHECK(out[0] == 100 && out[1] == 2);
    (*env)->GetIntArrayRegion(env, ia, 4, 2, out);
    CHECK((*env)->ExceptionCheck(env));
    (*env)->ExceptionClear(env);
    jlongArray la = (*env)->NewLongArray(env, 2);
    jlong lv[2] = { -1, 1LL << 50 };
    (*env)->SetLongArrayRegion(env, la, 0, 2, lv);
    CHECK(((jlong *)(*env)->GetPrimitiveArrayCritical(env, la, NULL))[1] == 1LL << 50);
    jdoubleArray da = (*env)->NewDoubleArray(env, 1);
    (*env)->SetDoubleArrayRegion(env, da, 0, 1, (jdouble[]){ 0.125 });
    CHECK(feq(((jdouble *)fj_array_data(da))[0], 0.125));
    const char *ss[] = { "one", "two", NULL };
    jobjectArray sa = fj_string_array(ss, 3);
    CHECK((*env)->GetArrayLength(env, sa) == 3);
    CHECK(!strcmp(fj_cstr((*env)->GetObjectArrayElement(env, sa, 1)), "two"));
    CHECK((*env)->GetObjectArrayElement(env, sa, 2) == NULL);
    jclass scls = (*env)->FindClass(env, "java/lang/String");
    jobjectArray oa = (*env)->NewObjectArray(env, 2, scls, fj_string("init"));
    CHECK(!strcmp(fj_cstr((*env)->GetObjectArrayElement(env, oa, 0)), "init"));
    (*env)->SetObjectArrayElement(env, oa, 1, fj_string("set"));
    CHECK(!strcmp(fj_cstr((*env)->GetObjectArrayElement(env, oa, 1)), "set"));
    CHECK(!strcmp(fj_class_name((*env)->GetObjectClass(env, oa)), "[Ljava/lang/String;"));
    jclass iac = (*env)->FindClass(env, "[I");
    CHECK(iac && (*env)->IsInstanceOf(env, ia, iac));

    /* direct byte buffers */
    char mem[16];
    jobject bb = (*env)->NewDirectByteBuffer(env, mem, sizeof mem);
    CHECK((*env)->GetDirectBufferAddress(env, bb) == mem);
    CHECK((*env)->GetDirectBufferCapacity(env, bb) == 16);

    /* reference counting: objects freed when their local frame is popped */
    freed = 0;
    jclass ccls = fj_class_define("test/Counted", NULL);
    fj_push_frame();
    jobject keep = NULL;
    for (int i = 0; i < 10; i++) {
        jobject x = fj_new(ccls, &freed, count_free);
        if (i == 3) keep = fj_global(x);
    }
    jobject inarr = fj_new(ccls, &freed, count_free);
    jobjectArray holder = (*env)->NewObjectArray(env, 1, ccls, NULL);
    (*env)->SetObjectArrayElement(env, holder, 0, inarr);
    jobject gholder = fj_global(holder);
    fj_pop_frame();
    CHECK(freed == 9);              /* 10 + 1 created, 1 global, 1 held by a global array */
    fj_release(gholder);
    CHECK(freed == 10);
    fj_release(keep);
    CHECK(freed == 11);
    jobject t1 = fj_new(ccls, &freed, count_free);
    (*env)->DeleteLocalRef(env, t1);
    CHECK(freed == 12);

    /* exceptions raised by handlers, ExceptionOccurred/Describe */
    jmethodID boom = (*env)->GetStaticMethodID(env, calc, "boom", "()V");
    (*env)->CallStaticVoidMethodA(env, calc, boom, NULL);
    jthrowable ex = (*env)->ExceptionOccurred(env);
    CHECK(ex && fj_is_instance(ex, "java/lang/RuntimeException"));
    jmethodID gm = (*env)->GetMethodID(env, (*env)->FindClass(env, "java/lang/Throwable"), "getMessage", "()Ljava/lang/String;");
    CHECK(!strcmp(fj_cstr((*env)->CallObjectMethod(env, ex, gm)), "boom!"));
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    CHECK(!(*env)->ExceptionCheck(env));
    (*env)->ThrowNew(env, (*env)->FindClass(env, "java/io/IOException"), "io");
    CHECK((*env)->ExceptionCheck(env));
    (*env)->ExceptionClear(env);

    /* Class / Object built-ins */
    jmethodID gn = (*env)->GetMethodID(env, (*env)->FindClass(env, "java/lang/Class"), "getName", "()Ljava/lang/String;");
    CHECK(!strcmp(fj_cstr((*env)->CallObjectMethod(env, calc, gn)), "test.Calc"));
    jmethodID eq = (*env)->GetMethodID(env, scls, "equals", "(Ljava/lang/Object;)Z");
    CHECK((*env)->CallBooleanMethod(env, fj_string("q"), eq, fj_string("q")));
    jclass integer = (*env)->FindClass(env, "java/lang/Integer");
    jmethodID vo = (*env)->GetStaticMethodID(env, integer, "valueOf", "(I)Ljava/lang/Integer;");
    jmethodID iv = (*env)->GetMethodID(env, integer, "intValue", "()I");
    CHECK((*env)->CallIntMethod(env, (*env)->CallStaticObjectMethod(env, integer, vo, (jint)77), iv) == 77);

    /* threads */
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, thread_main, vm);
    for (int i = 0; i < 4; i++) { void *ok; pthread_join(th[i], &ok); CHECK(ok); }

    /* GetEnv / GetJavaVM */
    JNIEnv *e2 = NULL;
    CHECK((*vm)->GetEnv(vm, (void **)&e2, JNI_VERSION_1_6) == JNI_OK && e2 == env);
    JavaVM *vm2 = NULL;
    CHECK((*env)->GetJavaVM(env, &vm2) == JNI_OK && vm2 == vm);

    if (getenv("FJ_DUMP_TEST")) fj_dump(stdout);
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
