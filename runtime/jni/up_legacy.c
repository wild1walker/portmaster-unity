/*
 * up_legacy.c - the parts of the Java side of Unity 5.6 - 2017.2 players that later ones dropped.
 *
 * Startup (UnityPlayer's constructor, 5.6 - 2017.2): after initJni(context),
 *   5.x:      nativeFile(getPackageCodePath()) mounts the APK (2017.1+ mount it themselves), and
 *             for each OBB whose signature the APK's settings.xml lists (an MD5 of the OBB's
 *             last 64 KiB), nativeFile(obb path): here every main/patch OBB of the package is
 *             mounted, as the converter packs them together with their APK;
 *   to 2017.2: nativeInitWWW(WWW.class), then nativeInitWebRequest(UnityWebRequest.class)
 *             (unityplayer.c), registered natives deciding which run.
 * com.unity3d.player.WWW (to 2017.2: the WWW class's HTTP transport, a java.lang.Thread that
 * libunity creates with (id, url, post data, headers) and starts) answers as a phone without a
 * network does: errorCallback(id, "java.net.UnknownHostException: ...") from its own thread;
 * file: URLs are read and delivered (headerCallback, readCallback, doneCallback).
 * No Unity code is included; behaviour follows the package's Java side as javap shows it.
 */
#define _GNU_SOURCE
#include "up_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define WWW "com/unity3d/player/WWW"

static int registered(const char *cls, const char *name) { return fj_native_sig(cls, name) != NULL; }

void up_legacy_start(void)
{
    fj_push_frame();
    if (registered(UP_CLS, "nativeFile")) {        /* 5.x: the APK, then the OBBs */
        jvalue a = fj_l(fj_string(UPC.apk_path));
        up_call_native(UP_CLS, "nativeFile", "(Ljava/lang/String;)V", g_player, &a);
        up_log("nativeFile(%s)", UPC.apk_path);
        const char *kinds[] = { "main", "patch" };
        for (int i = 0; i < 2; i++) {
            char p[1024];
            snprintf(p, sizeof p, "%s/%s.%d.%s.obb", UPC.obb_dir, kinds[i], UPC.version_code, UPC.package_name);
            if (access(p, R_OK) != 0) continue;
            jvalue o = fj_l(fj_string(p));
            up_call_native(UP_CLS, "nativeFile", "(Ljava/lang/String;)V", g_player, &o);
            up_log("nativeFile(%s)", p);
        }
    }
    if (registered(UP_CLS, "nativeInitWWW")) {     /* to 2017.2 */
        jvalue c = fj_l(fj_class_find(WWW));
        up_call_native(UP_CLS, "nativeInitWWW", "(Ljava/lang/Class;)V", g_player, &c);
    }
    fj_pop_frame();
}

/* ---- WWW */

typedef struct { int id; char *url; } www;
static void www_free(void *p) { www *w = p; if (w) { free(w->url); free(w); } }

H(www_init)
{
    www *w = calloc(1, sizeof *w);
    w->id = a[0].i;
    const char *u = fj_cstr(a[1].l);
    w->url = strdup(u ? u : "");
    fj_set_data(self, w, www_free);
    return fj_v();
}

static void cb_error(int id, const char *msg)
{
    jvalue a[2] = { fj_i(id), fj_l(fj_string(msg)) };
    up_call_native(WWW, "errorCallback", "(ILjava/lang/String;)V", NULL, a);
}

static void deliver_file(int id, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        char m[1200];
        snprintf(m, sizeof m, "java.io.FileNotFoundException: %s (No such file or directory)", path);
        cb_error(id, m);
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char len[32];
    snprintf(len, sizeof len, "content-length: %ld", n);
    jvalue h[2] = { fj_i(id), fj_l(fj_string(len)) };
    up_call_native(WWW, "headerCallback", "(ILjava/lang/String;)Z", NULL, h);
    jarray buf = fj_array_new("B", 65536);
    size_t r;
    while ((r = fread(fj_array_data(buf), 1, 65536, f)) > 0) {
        jvalue d[3] = { fj_i(id), fj_l(buf), fj_i((jint)r) };
        jvalue ok = up_call_native(WWW, "readCallback", "(I[BI)Z", NULL, d);
        if (!ok.z) break;
    }
    fclose(f);
    jvalue dn = fj_i(id);
    up_call_native(WWW, "doneCallback", "(I)V", NULL, &dn);
}

static void *www_thread(void *arg)
{
    www *w = arg;
    JavaVM *vm = fj_vm();
    JNIEnv *env;
    (*vm)->AttachCurrentThread(vm, &env, NULL);
    fj_push_frame();
    if (!strncasecmp(w->url, "file://", 7)) deliver_file(w->id, w->url + 7);
    else {
        const char *h = strstr(w->url, "://");
        h = h ? h + 3 : w->url;
        int n = (int)strcspn(h, ":/?#");
        char m[400];
        snprintf(m, sizeof m, "java.net.UnknownHostException: Unable to resolve host \"%.*s\": No address associated with hostname",
                 n > 255 ? 255 : n, h);
        up_log("WWW %s: no network here", w->url);
        cb_error(w->id, m);
    }
    fj_pop_frame();
    (*vm)->DetachCurrentThread(vm);
    www_free(w);
    return NULL;
}

H(www_start)
{
    www *w = fj_data(self);
    if (!w) return fj_v();
    www *c = calloc(1, sizeof *c);
    c->id = w->id;
    c->url = strdup(w->url);
    pthread_t t;
    if (pthread_create(&t, NULL, www_thread, c) == 0) pthread_detach(t);
    else www_free(c);
    return fj_v();
}

void up_legacy_register(void)
{
    fj_class_define(WWW, "java/lang/Thread");
    fj_method_define(WWW, "<init>", "(ILjava/lang/String;[BLjava/util/Map;)V", 0, www_init, NULL);
    fj_method_define(WWW, "start", "()V", 0, www_start, NULL);
}
