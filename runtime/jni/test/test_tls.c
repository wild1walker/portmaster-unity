/* up_tls.c's test (test/tls_test.sh): the trust anchors through JNI exactly as libunity's unitytls
 * asks for them (TrustManagerFactory -> X509TrustManager.getAcceptedIssuers -> getEncoded), then a
 * TLS client on mbedTLS (the library unitytls is built on) that trusts only those anchors and talks
 * to local HTTPS servers:
 *   test_tls <der dir> <port ok> <port untrusted>
 *   - every certificate of $UA_CA_BUNDLE comes back, DER equal to <der dir>/<i>.der
 *   - the server whose chain leads to the test CA: handshake verified, the page is read
 *   - the same server under another host name: refused (CN mismatch)
 *   - a server with a certificate of an unknown CA: refused (not trusted)
 *   - with an empty trust store (TLS_WANT=0, no bundle): all of them refused (nothing is accepted
 *     without verification) */
#include "../fakejni.h"
#include "../include/jni.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void up_tls_register(void);
int up_tls_ca_count(void);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* libunity's sequence; fills chain with the anchors, returns their count */
static int anchors(JNIEnv *env, mbedtls_x509_crt *chain, const char *derdir)
{
    jclass tmf = (*env)->FindClass(env, "javax/net/ssl/TrustManagerFactory");
    jmethodID alg = (*env)->GetStaticMethodID(env, tmf, "getDefaultAlgorithm", "()Ljava/lang/String;");
    jmethodID inst = (*env)->GetStaticMethodID(env, tmf, "getInstance", "(Ljava/lang/String;)Ljavax/net/ssl/TrustManagerFactory;");
    jmethodID init = (*env)->GetMethodID(env, tmf, "init", "(Ljava/security/KeyStore;)V");
    jmethodID tms = (*env)->GetMethodID(env, tmf, "getTrustManagers", "()[Ljavax/net/ssl/TrustManager;");
    jclass x509tm = (*env)->FindClass(env, "javax/net/ssl/X509TrustManager");
    jmethodID issuers = (*env)->GetMethodID(env, x509tm, "getAcceptedIssuers", "()[Ljava/security/cert/X509Certificate;");
    jclass certc = (*env)->FindClass(env, "java/security/cert/Certificate");
    jmethodID enc = (*env)->GetMethodID(env, certc, "getEncoded", "()[B");
    jstring a = (*env)->CallStaticObjectMethod(env, tmf, alg);
    const char *as = (*env)->GetStringUTFChars(env, a, NULL);
    CHECK(as && !strcmp(as, "PKIX"), "default algorithm %s", as ? as : "null");
    jobject f = (*env)->CallStaticObjectMethod(env, tmf, inst, a);
    (*env)->ReleaseStringUTFChars(env, a, as);
    CHECK(f != NULL, "TrustManagerFactory.getInstance");
    (*env)->CallVoidMethod(env, f, init, NULL);
    jobjectArray arr = (*env)->CallObjectMethod(env, f, tms);
    CHECK(arr && (*env)->GetArrayLength(env, arr) == 1, "one trust manager");
    jobject tm = (*env)->GetObjectArrayElement(env, arr, 0);
    CHECK((*env)->IsInstanceOf(env, tm, x509tm), "an X509TrustManager");
    jobjectArray certs = (*env)->CallObjectMethod(env, tm, issuers);
    int n = (*env)->GetArrayLength(env, certs);
    for (int i = 0; i < n; i++) {
        jobject c = (*env)->GetObjectArrayElement(env, certs, i);
        jbyteArray der = (*env)->CallObjectMethod(env, c, enc);
        jsize len = (*env)->GetArrayLength(env, der);
        jbyte *p = (*env)->GetByteArrayElements(env, der, NULL);
        int r = mbedtls_x509_crt_parse_der(chain, (const unsigned char *)p, len);
        CHECK(r == 0, "certificate %d: mbedTLS parses it (%d)", i, r);
        if (strcmp(derdir, "none")) {
            char path[600];
            snprintf(path, sizeof path, "%s/%d.der", derdir, i);
            FILE *fp = fopen(path, "rb");
            unsigned char buf[8192];
            size_t m = fp ? fread(buf, 1, sizeof buf, fp) : 0;
            if (fp) fclose(fp);
            CHECK(m == (size_t)len && !memcmp(buf, p, len), "certificate %d: DER as openssl writes it (%zu / %d bytes)", i, m, (int)len);
        }
        (*env)->ReleaseByteArrayElements(env, der, p, JNI_ABORT);
    }
    return n;
}

/* a GET over TLS; returns 0 when verified and the body contains `want`, else the mbedTLS error or
   the verification flags (negative) */
static int get(mbedtls_x509_crt *ca, const char *host, const char *port, const char *want, unsigned *flags_out)
{
    mbedtls_net_context net; mbedtls_ssl_context ssl; mbedtls_ssl_config conf;
    mbedtls_entropy_context ent; mbedtls_ctr_drbg_context drbg;
    mbedtls_net_init(&net); mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&conf);
    mbedtls_entropy_init(&ent); mbedtls_ctr_drbg_init(&drbg);
    int r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char *)"tls", 3);
    if (!r) r = mbedtls_net_connect(&net, "127.0.0.1", port, MBEDTLS_NET_PROTO_TCP);
    if (!r) r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (!r) {
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);     /* as unitytls by default */
        mbedtls_ssl_conf_ca_chain(&conf, ca, NULL);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
        r = mbedtls_ssl_setup(&ssl, &conf);
    }
    if (!r) r = mbedtls_ssl_set_hostname(&ssl, host);
    if (!r) { mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL); r = mbedtls_ssl_handshake(&ssl); }
    *flags_out = mbedtls_ssl_get_verify_result(&ssl);
    if (!r) {
        char req[256], buf[4096];
        snprintf(req, sizeof req, "GET / HTTP/1.0\r\nHost: %s\r\n\r\n", host);
        mbedtls_ssl_write(&ssl, (const unsigned char *)req, strlen(req));
        int n = 0, k;
        while ((k = mbedtls_ssl_read(&ssl, (unsigned char *)buf + n, sizeof buf - 1 - n)) > 0) n += k;
        buf[n] = 0;
        r = strstr(buf, want) ? 0 : -1;
    }
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_net_free(&net); mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&ent);
    return r;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 4) { fprintf(stderr, "usage: test_tls <der dir> <port ok> <port untrusted>\n"); return 2; }
    fj_init();
    up_tls_register();
    JNIEnv *env = fj_env();
    mbedtls_x509_crt ca;
    mbedtls_x509_crt_init(&ca);
    int n = anchors(env, &ca, argv[1]);
    int want = atoi(getenv("TLS_WANT") ? getenv("TLS_WANT") : "0");
    CHECK(n == want && n == up_tls_ca_count(), "%d anchors, the bundle has %d", n, want);
    unsigned fl;
    int r = get(&ca, "tls.test", argv[2], "hello from tls.test", &fl);
    if (n) CHECK(r == 0 && fl == 0, "trusted server: result -0x%x, flags 0x%x", -r, fl);
    else CHECK(r != 0 && (fl & MBEDTLS_X509_BADCERT_NOT_TRUSTED), "empty store: the test CA's server refused too (result -0x%x, flags 0x%x)", -r, fl);
    r = get(&ca, "other.test", argv[2], "hello", &fl);
    CHECK(r != 0 && (fl & MBEDTLS_X509_BADCERT_CN_MISMATCH), "wrong host name refused: result -0x%x, flags 0x%x", -r, fl);
    r = get(&ca, "tls.test", argv[3], "hello", &fl);
    CHECK(r != 0 && (fl & MBEDTLS_X509_BADCERT_NOT_TRUSTED), "unknown CA refused: result -0x%x, flags 0x%x", -r, fl);
    mbedtls_x509_crt empty;
    mbedtls_x509_crt_init(&empty);
    r = get(&empty, "tls.test", argv[2], "hello", &fl);
    CHECK(r != 0, "no anchors: refused (result -0x%x, flags 0x%x)", -r, fl);
    /* KeyStore("AndroidCAStore") lists the same certificates */
    jclass ks = (*env)->FindClass(env, "java/security/KeyStore");
    jobject s = (*env)->CallStaticObjectMethod(env, ks, (*env)->GetStaticMethodID(env, ks, "getInstance", "(Ljava/lang/String;)Ljava/security/KeyStore;"),
                                               (*env)->NewStringUTF(env, "AndroidCAStore"));
    (*env)->CallVoidMethod(env, s, (*env)->GetMethodID(env, ks, "load", "(Ljava/io/InputStream;[C)V"), NULL, NULL);
    int size = (*env)->CallIntMethod(env, s, (*env)->GetMethodID(env, ks, "size", "()I"));
    jobject en = (*env)->CallObjectMethod(env, s, (*env)->GetMethodID(env, ks, "aliases", "()Ljava/util/Enumeration;"));
    jclass ec = (*env)->FindClass(env, "java/util/Enumeration");
    int k = 0;
    while ((*env)->CallBooleanMethod(env, en, (*env)->GetMethodID(env, ec, "hasMoreElements", "()Z"))) {
        jobject al = (*env)->CallObjectMethod(env, en, (*env)->GetMethodID(env, ec, "nextElement", "()Ljava/lang/Object;"));
        jobject c = (*env)->CallObjectMethod(env, s, (*env)->GetMethodID(env, ks, "getCertificate", "(Ljava/lang/String;)Ljava/security/cert/Certificate;"), al);
        k += c != NULL;
    }
    CHECK(size == n && k == n, "KeyStore AndroidCAStore: size %d, %d certificates by alias", size, k);
    printf("tls: %s (%d failures, %d anchors)\n", fails ? "FAILED" : "PASSED", fails, n);
    return fails != 0;
}
