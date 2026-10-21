/* up_http.c's test (test/tls_test.sh): Unity's Java UnityWebRequest (2017.4 - 2018.4) driven as
 * libunity drives it, with natives standing in for libunity's TransportAndroid that record every
 * callback; local HTTP and HTTPS servers.
 *   test_http on    UA_HTTP=1: GET, HEAD, POST with an uploaded body, a 300000-byte body in
 *                   blocks, a download stopped by Unity, HTTPS with the test CA, HTTPS of an
 *                   unknown CA (TLS error 16), an unknown host (7), a malformed URL (5)
 *   test_http off   without UA_HTTP: every http(s) request fails at once (7), as before */
#include "../fakejni.h"
#include "../include/jni.h"
#include "../unityplayer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct { int code, codes, clen, err, errs, hdrs, status_first, upload_calls, stop_after; long body; char first[64], echo[16], errmsg[300]; } R;
static const char *upload_data;

static void JNICALL n_header(JNIEnv *e, jclass c, jlong p, jstring k, jstring v)
{
    const char *ks = (*e)->GetStringUTFChars(e, k, NULL), *vs = (*e)->GetStringUTFChars(e, v, NULL);
    if (!R.hdrs) R.status_first = !strcmp(ks, "Status") && !strncmp(vs, "HTTP/", 5);
    if (!strcasecmp(ks, "X-Echo")) snprintf(R.echo, sizeof R.echo, "%s", vs);
    R.hdrs++;
    (*e)->ReleaseStringUTFChars(e, k, ks); (*e)->ReleaseStringUTFChars(e, v, vs);
}
static jint JNICALL n_upload(JNIEnv *e, jclass c, jlong p, jobject buf)
{
    R.upload_calls++;
    if (!upload_data || R.upload_calls > 1) return 0;
    char *d = (*e)->GetDirectBufferAddress(e, buf);
    size_t n = strlen(upload_data);
    memcpy(d, upload_data, n);
    return (jint)n;
}
static void JNICALL n_clen(JNIEnv *e, jclass c, jlong p, jint n) { R.clen = n; }
static jboolean JNICALL n_download(JNIEnv *e, jclass c, jlong p, jobject buf, jint n)
{
    const char *d = (*e)->GetDirectBufferAddress(e, buf);
    if (!R.body) snprintf(R.first, sizeof R.first, "%.*s", n < 63 ? (int)n : 63, d);
    R.body += n;
    return !(R.stop_after && R.body >= R.stop_after);
}
static jboolean JNICALL n_cert(JNIEnv *e, jclass c, jlong p, jbyteArray der) { return JNI_TRUE; }
static void JNICALL n_code(JNIEnv *e, jclass c, jlong p, jint code) { R.code = code; R.codes++; }
static void JNICALL n_error(JNIEnv *e, jclass c, jlong p, jint code, jstring msg)
{
    const char *m = (*e)->GetStringUTFChars(e, msg, NULL);
    R.err = code; R.errs++;
    snprintf(R.errmsg, sizeof R.errmsg, "%s", m);
    (*e)->ReleaseStringUTFChars(e, msg, m);
}

static void request(JNIEnv *env, const char *method, const char *url, const char *body, int stop_after)
{
    memset(&R, 0, sizeof R);
    upload_data = body;
    R.stop_after = stop_after;
    jclass hm = (*env)->FindClass(env, "java/util/HashMap");
    jobject map = (*env)->NewObject(env, hm, (*env)->GetMethodID(env, hm, "<init>", "()V"));
    (*env)->CallObjectMethod(env, map, (*env)->GetMethodID(env, hm, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
                             (*env)->NewStringUTF(env, "X-Test"), (*env)->NewStringUTF(env, "abc"));
    jclass c = (*env)->FindClass(env, "com/unity3d/player/UnityWebRequest");
    jobject w = (*env)->NewObject(env, c, (*env)->GetMethodID(env, c, "<init>", "(JLjava/lang/String;Ljava/util/Map;Ljava/lang/String;ZI)V"),
                                  (jlong)42, (*env)->NewStringUTF(env, method), map, (*env)->NewStringUTF(env, url), JNI_FALSE, 5000);
    (*env)->CallVoidMethod(env, w, (*env)->GetMethodID(env, c, "setupTransferSettings", "(JZZ)V"), (jlong)(body ? strlen(body) : 0), JNI_FALSE, JNI_FALSE);
    (*env)->CallVoidMethod(env, w, (*env)->GetMethodID(env, c, "run", "()V"));
    printf("  %s %s: code %d (%d), %d headers, length %d, body %ld \"%.20s\", error %d %s\n", method, url, R.code, R.codes, R.hdrs, R.clen,
           R.body, R.first, R.err, R.errmsg);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int on = argc > 1 && !strcmp(argv[1], "on");
    struct up_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.files_dir = "/tmp/uwr-files"; cfg.data_dir = "/tmp/uwr"; cfg.cache_dir = "/tmp/uwr-cache";
    cfg.external_files_dir = "/tmp/uwr-ext"; cfg.package_name = "com.test.uwr";
    up_register_classes(&cfg);
    JNIEnv *env = fj_env();
    jclass c = (*env)->FindClass(env, "com/unity3d/player/UnityWebRequest");
    JNINativeMethod nm[] = {
        { "headerCallback", "(JLjava/lang/String;Ljava/lang/String;)V", (void *)n_header },
        { "uploadCallback", "(JLjava/nio/ByteBuffer;)I", (void *)n_upload },
        { "contentLengthCallback", "(JI)V", (void *)n_clen },
        { "downloadCallback", "(JLjava/nio/ByteBuffer;I)Z", (void *)n_download },
        { "validateCertificateCallback", "(J[B)Z", (void *)n_cert },
        { "responseCodeCallback", "(JI)V", (void *)n_code },
        { "errorCallback", "(JILjava/lang/String;)V", (void *)n_error },
    };
    (*env)->RegisterNatives(env, c, nm, sizeof nm / sizeof nm[0]);
    if (!on) {
        request(env, "GET", "http://127.0.0.1:8080/", NULL, 0);
        CHECK(R.err == 7 && R.errs == 1 && !R.codes && !R.body, "without UA_HTTP: unknown host at once");
        request(env, "GET", "nothing-here", NULL, 0);
        CHECK(R.err == 5, "no protocol: 5");
    } else {
        request(env, "GET", "http://127.0.0.1:8080/", NULL, 0);
        CHECK(R.code == 200 && R.codes == 2 && R.status_first && !strcmp(R.echo, "abc") && R.clen == 20 && R.body == 20 &&
              !strncmp(R.first, "hello from tls.test", 19) && !R.errs, "GET");
        request(env, "HEAD", "http://127.0.0.1:8080/", NULL, 0);
        CHECK(R.code == 200 && R.body == 0 && !R.errs, "HEAD");
        request(env, "POST", "http://127.0.0.1:8080/form", "name=value&x=1", 0);
        CHECK(R.code == 200 && R.upload_calls == 2 && !strncmp(R.first, "got 14 bytes name=value", 23) && !R.errs, "POST: %s", R.first);
        request(env, "GET", "http://127.0.0.1:8080/big", NULL, 0);
        CHECK(R.code == 200 && R.body == 300000 && R.clen == 300000 && !R.errs, "a big body in blocks");
        request(env, "GET", "http://127.0.0.1:8080/big", NULL, 1);
        CHECK(R.body > 0 && R.body < 300000 && !R.errs, "stopped by Unity: no error, %ld bytes", R.body);
        request(env, "GET", "https://tls.test:8443/", NULL, 0);
        CHECK(R.code == 200 && !strncmp(R.first, "hello from tls.test", 19) && !R.errs, "HTTPS, the test CA in the bundle");
        request(env, "GET", "https://tls.test:8444/", NULL, 0);
        CHECK(R.err == 16 && !R.codes && !R.body, "HTTPS, unknown CA: TLS error 16");
        request(env, "GET", "https://127.0.0.1:8443/", NULL, 0);
        CHECK(R.err == 16 && !R.codes, "HTTPS, wrong host name: TLS error 16");
        request(env, "GET", "http://no-such-host.invalid/", NULL, 0);
        CHECK(R.err == 7 && !R.codes, "unknown host: 7");
        request(env, "GET", "http://[::1/", NULL, 0);
        CHECK(R.err == 5 || R.err == 7 || R.err == 12, "malformed URL: an error (%d)", R.err);
    }
    printf("http %s: %s (%d failures)\n", on ? "on" : "off", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
