/*
 * up_unity6.c - the Java side of Unity 2023.1+ (Unity 6) players, Activity path.
 *
 * From 2023.1 the player package's UnityPlayer is abstract, with two implementations:
 * UnityPlayerForActivityOrService (UnityPlayerActivity, the model runtime/jni plays: a SurfaceView,
 * the UnityMain looper thread, message 2269 = next frame) and UnityPlayerForGameActivity
 * (androidx.games GameActivity + the libgame.so a Gradle build compiles from the package's
 * Source/GameActivity; not used here). libunity registers the per-instance natives (nativeRender,
 * nativeResume, nativePause, nativeFocusChanged, nativeRecreateGfxState, ...) on
 * UnityPlayerForActivityOrService and the rest (initJni, nativeInjectEvent, nativeUnitySendMessage,
 * ...) on UnityPlayer: unityplayer.c looks a UnityPlayer native up on both (up_call_native). The
 * player object stays a UnityPlayer to the older players; this file defines the subclass (so
 * libunity's member lookups on it find UnityPlayer's), the members libunity 2023.1 - 6000.6 call by
 * name that earlier ones did not (UnityPlayerJavaWrapper: runningOnMainThread,
 * executeMainThreadJobs, requestUserAuthorization(String, long), ...), and the helper classes the
 * engine asks for device facts (DeviceInfo, ApplicationExitInfoProvider, UnityPlayerUtilities).
 * No Unity code is included; behaviour follows the package's Java side as javap shows it.
 */
#define _GNU_SOURCE
#include "up_internal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>

#define AOS UP_CLS_AOS
#define DM(c, n, s, f)      fj_method_define(c, n, s, 0, f, NULL)
#define DMU(c, n, s, f, u)  fj_method_define(c, n, s, 0, f, (void *)(u))
#define DSM(c, n, s, f)     fj_method_define(c, n, s, 1, f, NULL)
#define DSMU(c, n, s, f, u) fj_method_define(c, n, s, 1, f, (void *)(u))

H(u6_void)  { return fj_v(); }
H(u6_false) { return fj_z(JNI_FALSE); }
H(u6_str)   { return fj_l(fj_string((const char *)ud)); }

/* runningOnMainThread: Java compares with its UnityMain thread */
H(u6_onMain) { return fj_z(up_on_main_thread() ? JNI_TRUE : JNI_FALSE); }

/* executeMainThreadJobs (2023.1+ name of executeGLThreadJobs): the jobs Java queued for UnityMain */
H(u6_execMain)
{
    if (up_on_main_thread()) up_run_gl_jobs();
    return fj_v();
}

/* invokeOnMainThread(Runnable): queued for UnityMain (run before the next frame) */
H(u6_invokeOnMain)
{
    if (a[0].l) up_gl_post(NULL, NULL, a[0].l);
    return fj_v();
}

/* requestUserAuthorization(permission, callback): Java asks Android and, when a callback is given,
   reports the answer with permissionResponseToNative(callback, granted). Every permission is
   granted here; the answer goes to UnityMain, as Java's callback runs on the main looper. */
struct perm_job { jlong cb; };
static void job_perm(void *arg)
{
    struct perm_job *p = arg;
    jvalue a[2] = { fj_j(p->cb), fj_z(JNI_TRUE) };
    fj_push_frame();
    up_call_native(UP_CLS, "permissionResponseToNative", "(JZ)V", NULL, a);
    fj_pop_frame();
    free(p);
}
H(u6_requestAuth)
{
    up_log("requestUserAuthorization(%s): granted", a[0].l ? fj_cstr(a[0].l) : "null");
    if (a[1].j && fj_native_sig(UP_CLS, "permissionResponseToNative")) {
        struct perm_job *p = calloc(1, sizeof *p);
        p->cb = a[1].j;
        up_gl_post(job_perm, p, NULL);
    }
    return fj_v();
}

/* shutdown(): Java's quit path (Application.Quit on 2023.1+) */
H(u6_shutdown) { up_request_finish("UnityPlayer.shutdown"); return fj_v(); }

H(u6_logged) { up_log("%s (ignored)", (const char *)ud); return fj_v(); }

/* DeviceInfo (Analytics / Insights) */
H(di_model)
{
    static char m[160];
    snprintf(m, sizeof m, "%s %s", UPC.manufacturer, UPC.model);
    return fj_l(fj_string(m));
}
H(di_os) { return fj_l(fj_string(UPC.os_release)); }
H(di_lang) { return fj_l(fj_string(UPC.language)); }
H(di_locales)
{
    char l[64];
    snprintf(l, sizeof l, "%s-%s", UPC.language, UPC.country);
    return fj_l(fj_string(l));
}
H(di_space)
{
    struct statvfs s;
    if (statvfs(UPC.files_dir, &s) != 0) return fj_j(0);
    return fj_j((jlong)s.f_blocks * (jlong)s.f_frsize);
}
H(di_boot)
{
    /* System.currentTimeMillis() - SystemClock.elapsedRealtime() */
    struct timespec rt, bt;
    clock_gettime(CLOCK_REALTIME, &rt);
    clock_gettime(CLOCK_BOOTTIME, &bt);
    return fj_j(((jlong)rt.tv_sec - bt.tv_sec) * 1000 + ((jlong)rt.tv_nsec - bt.tv_nsec) / 1000000);
}

/* UnityPlayerForActivityOrService$MemoryUsage: Low 1, Medium 2, High 3, Critical 4 (`value`) */
static const char *const MEM_NAMES[] = { "Low", "Medium", "High", "Critical" };

void up_unity6_register(void)
{
    const char *U = UP_CLS;
    fj_class_define(AOS, UP_CLS);
    /* UnityPlayerJavaWrapper's members new in 2023.1 - 6000.6 */
    DM(U, "runningOnMainThread", "()Z", u6_onMain);
    DM(AOS, "runningOnMainThread", "()Z", u6_onMain);
    DM(U, "executeMainThreadJobs", "()V", u6_execMain);
    DM(U, "invokeOnMainThread", "(Ljava/lang/Runnable;)V", u6_invokeOnMain);
    DM(U, "requestUserAuthorization", "(Ljava/lang/String;J)V", u6_requestAuth);
    DM(U, "shutdown", "()V", u6_shutdown);
    DM(AOS, "shutdown", "()V", u6_shutdown);
    DM(U, "shouldSetGameState", "()Z", u6_false);          /* no GameManager */
    DM(U, "shouldReportFullyDrawn", "()Z", u6_false);
    DM(U, "reportFullyDrawn", "()V", u6_void);
    DMU(U, "setRunWithoutFocus", "(Z)V", u6_logged, "setRunWithoutFocus");
    DM(U, "requestPointerCapture", "()V", u6_void);          /* the host owns the pointer */
    DM(U, "releasePointerCapture", "()V", u6_void);
    fj_class_define(AOS "$MemoryUsage", NULL);
    for (int i = 0; i < 4; i++)
        fj_field_define(AOS "$MemoryUsage", MEM_NAMES[i], "L" AOS "$MemoryUsage;", 1, fj_l(NULL));
    fj_field_define(AOS "$MemoryUsage", "value", "I", 0, fj_i(0));
    /* device facts */
    const char *DI = "com/unity3d/player/DeviceInfo";
    fj_class_define(DI, NULL);
    DSM(DI, "getDeviceModel", "()Ljava/lang/String;", di_model);
    DSM(DI, "getOsVersion", "()Ljava/lang/String;", di_os);
    DSM(DI, "getSystemLanguage", "()Ljava/lang/String;", di_lang);
    DSM(DI, "getLocaleList", "(Landroid/content/Context;)Ljava/lang/String;", di_locales);
    DSM(DI, "getTotalSpace", "(Landroid/content/Context;)J", di_space);
    DSM(DI, "getSystemBootTime", "()J", di_boot);
    const char *AE = "com/unity3d/player/ApplicationExitInfoProvider";
    fj_class_define(AE, NULL);
    DSMU(AE, "getANRRecordsJson", "(I)Ljava/lang/String;", u6_str, "[]");   /* no ANRs recorded */
    DSM(AE, "setProcessStateSummary", "(Landroid/app/Activity;[B)V", u6_void);
    /* identifiers a phone's services would give: none (their natives are never called back) */
    DSM("com/unity3d/player/AndroidAdvertisingIdHelper", "requestGoogleAdId", "(Landroid/content/Context;)V", u6_void);
    DSM("com/unity3d/player/AndroidAppSetIdHelper", "requestAppSetId", "(Landroid/content/Context;)V", u6_void);
    DSM("com/unity3d/player/FirebaseIdentifiersHelper", "requestFirebaseIdentifiers", "(Landroid/content/Context;)V", u6_void);
    DSM("com/unity3d/player/UnityGameState", "setGameState", "(Landroid/content/Context;ZI)V", u6_void);
    DSM("com/unity3d/player/UnityGameState", "setGameState", "(Landroid/content/Context;ZIII)V", u6_void);
    /* 6000.6: a debugging helper libunity creates at startup */
    const char *PU = "com/unity3d/player/UnityPlayerUtilities";
    fj_class_define(PU, NULL);
    DM(PU, "<init>", "()V", u6_void);
    DM(PU, "dumpReferenceTables", "()Z", u6_false);
}

void up_unity6_objects(void)
{
    /* the activity's field libunity reads (UnityPlayerActivity.mUnityPlayer, "referenced from
       native code" in the package's UnityPlayerActivity.java) */
    fj_set_obj(g_activity, "mUnityPlayer", "L" AOS ";", g_player);
    for (int i = 0; i < 4; i++) {
        jobject e = fj_global(fj_new_by_name(AOS "$MemoryUsage", NULL, NULL));
        fj_set_int(e, "value", i + 1);
        fj_static_object(AOS "$MemoryUsage", MEM_NAMES[i], "L" AOS "$MemoryUsage;", e);
    }
}
