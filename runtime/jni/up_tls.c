/* The system trust store, as Android's Java side gives it to libunity (docs/plugins.md, "HTTPS").
 *
 * Unity 2018.4+ verifies TLS certificates itself (unitytls, on mbedTLS: UnityWebRequest's native
 * transport from 2019.4, the .NET 4.x HttpWebRequest/SslStream of Mono): on Android it takes the
 * trust anchors from the platform,
 *     TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm())
 *         .init((KeyStore) null)   -> the system's CA store
 *         .getTrustManagers()      -> [X509TrustManager]
 *     X509TrustManager.getAcceptedIssuers()  -> X509Certificate[]
 *     Certificate.getEncoded()               -> DER
 * and checks the server's chain, validity and host name against them. Here the CA store is the
 * handheld's own CA bundle: $UA_CA_BUNDLE, $SSL_CERT_FILE, the usual bundle files of Linux
 * distributions, else the certificates of $SSL_CERT_DIR / /etc/ssl/certs / Android's
 * /system/etc/security/cacerts; PEM ("CERTIFICATE" and OpenSSL's "TRUSTED CERTIFICATE") is turned
 * into DER. As on Android, only these anchors are trusted (no user-added store); without a bundle
 * the list is empty and every certificate check fails (logged once): nothing is ever accepted
 * without verification. X509TrustManager.checkServerTrusted/checkClientTrusted (not used by
 * libunity) refuse with a CertificateException rather than accept.
 * KeyStore.getInstance("AndroidCAStore") / load / aliases / getCertificate give the same
 * certificates, as Android's system KeyStore does. */
#define _GNU_SOURCE
#include "up_internal.h"

#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define M(c, n, s, f)       fj_method_define(c, n, s, 0, f, NULL)
#define SM(c, n, s, f)      fj_method_define(c, n, s, 1, f, NULL)

typedef struct { unsigned char *der; size_t len; } cert;
static cert *g_certs;
static int g_ncerts;
static char g_source[600];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
#define MAX_CERTS 2000

/* bundle files of Linux distributions (the list Go's crypto/x509 and others use), most common first */
static const char *const bundles[] = {
    "/etc/ssl/certs/ca-certificates.crt",                 /* Debian, Ubuntu, Arch, Gentoo, Buildroot */
    "/etc/pki/tls/certs/ca-bundle.crt",                   /* Fedora, RHEL 6 */
    "/etc/ssl/ca-bundle.pem",                             /* openSUSE */
    "/etc/pki/tls/cacert.pem",                            /* OpenELEC */
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  /* CentOS, RHEL 7 */
    "/etc/ssl/cert.pem",                                  /* Alpine, macOS-like */
    "/etc/ssl/cacert.pem",                                /* LibreELEC, ROCKNIX */
    "/etc/ssl/certs/ca-bundle.crt",
    "/usr/share/ssl/certs/ca-bundle.crt",
    "/usr/local/share/certs/ca-root-nss.crt",
    NULL,
};
static const char *const dirs[] = { "/etc/ssl/certs", "/system/etc/security/cacerts", "/etc/pki/tls/certs", NULL };

static int b64v(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* the length of the DER element at d (tag, length, contents), 0 if malformed */
static size_t der_len(const unsigned char *d, size_t n)
{
    if (n < 2 || d[0] != 0x30) return 0;
    size_t l = d[1], h = 2;
    if (l & 0x80) {
        int k = l & 0x7f;
        if (k < 1 || k > 4 || n < 2 + (size_t)k) return 0;
        l = 0;
        for (int i = 0; i < k; i++) l = (l << 8) | d[2 + i];
        h += k;
    }
    return h + l <= n ? h + l : 0;
}

static void add_der(const unsigned char *d, size_t n)
{
    size_t l = der_len(d, n);           /* a TRUSTED CERTIFICATE has trust settings after the cert */
    if (!l || g_ncerts >= MAX_CERTS) return;
    for (int i = 0; i < g_ncerts; i++)
        if (g_certs[i].len == l && !memcmp(g_certs[i].der, d, l)) return;      /* (a directory's links) */
    cert *c = realloc(g_certs, (g_ncerts + 1) * sizeof *c);
    if (!c) return;
    g_certs = c;
    g_certs[g_ncerts].der = malloc(l);
    if (!g_certs[g_ncerts].der) return;
    memcpy(g_certs[g_ncerts].der, d, l);
    g_certs[g_ncerts].len = l;
    g_ncerts++;
}

/* PEM text -> DER certificates; returns how many were added */
static int add_pem(const char *txt, size_t n)
{
    int before = g_ncerts;
    const char *p = txt, *end = txt + n;
    for (;;) {
        const char *b = NULL, *e;
        const char *b1 = memmem(p, end - p, "-----BEGIN CERTIFICATE-----", 27);
        const char *b2 = memmem(p, end - p, "-----BEGIN TRUSTED CERTIFICATE-----", 35);
        if (b1 && (!b2 || b1 < b2)) { b = b1 + 27; e = memmem(b, end - b, "-----END CERTIFICATE-----", 25); }
        else if (b2) { b = b2 + 35; e = memmem(b, end - b, "-----END TRUSTED CERTIFICATE-----", 33); }
        else break;
        if (!e) break;
        unsigned char *der = malloc((e - b) * 3 / 4 + 4);
        if (!der) break;
        size_t k = 0;
        unsigned acc = 0;
        int bits = 0, bad = 0;
        for (const char *q = b; q < e; q++) {
            if (*q == '=') break;
            int v = b64v((unsigned char)*q);
            if (v < 0) { if (*q != '\n' && *q != '\r' && *q != ' ' && *q != '\t') bad = 1; continue; }
            acc = (acc << 6) | v; bits += 6;
            if (bits >= 8) { bits -= 8; der[k++] = (unsigned char)(acc >> bits); }
        }
        if (!bad) add_der(der, k);
        free(der);
        p = e + 1;
    }
    return g_ncerts - before;
}

static int add_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (64 << 20)) { fclose(f); return 0; }
    char *buf = malloc(st.st_size + 1);
    size_t n = buf ? fread(buf, 1, st.st_size, f) : 0;
    fclose(f);
    if (!buf) return 0;
    buf[n] = 0;
    int k = add_pem(buf, n);
    if (!k && n > 2 && (unsigned char)buf[0] == 0x30) { int b = g_ncerts; add_der((unsigned char *)buf, n); k = g_ncerts - b; }
    free(buf);
    return k;
}

static int add_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int k = 0;
    struct dirent *e;
    while ((e = readdir(d)) && g_ncerts < MAX_CERTS) {
        if (e->d_name[0] == '.') continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        k += add_file(p);
    }
    closedir(d);
    return k;
}

static void load(void)
{
    const char *env = getenv("UA_CA_BUNDLE");
    if (!env || !*env) env = getenv("SSL_CERT_FILE");
    if (env && *env) {
        if (add_file(env)) snprintf(g_source, sizeof g_source, "%s", env);
        else up_log("trust store: %s has no certificates", env);
    }
    for (int i = 0; !g_ncerts && bundles[i]; i++)
        if (add_file(bundles[i])) snprintf(g_source, sizeof g_source, "%s", bundles[i]);
    const char *edir = getenv("SSL_CERT_DIR");
    if (!g_ncerts && edir && *edir) {
        char buf[600];
        snprintf(buf, sizeof buf, "%s", edir);
        for (char *t = strtok(buf, ":"); t; t = strtok(NULL, ":"))
            if (add_dir(t)) snprintf(g_source, sizeof g_source, "%s", t);
    }
    for (int i = 0; !g_ncerts && dirs[i]; i++)
        if (add_dir(dirs[i])) snprintf(g_source, sizeof g_source, "%s", dirs[i]);
    if (g_ncerts) up_log("trust store: %d CA certificates from %s", g_ncerts, g_source);
    else up_log("trust store: no CA certificates found (UA_CA_BUNDLE=<PEM file>, SSL_CERT_FILE, /etc/ssl/certs/ca-certificates.crt): "
                "every HTTPS certificate check fails");
}

int up_tls_ca_count(void) { pthread_once(&g_once, load); return g_ncerts; }

/* ---- classes */
#define TMF "javax/net/ssl/TrustManagerFactory"
#define TM_IMPL "com/android/org/conscrypt/TrustManagerImpl"
#define X509 "java/security/cert/X509Certificate"
#define CERTC "java/security/cert/Certificate"
#define KS "java/security/KeyStore"
#define ENUM_IMPL "java/util/Collections$IteratorEnumeration"

static jobject new_cert(int i)
{
    return fj_new_by_name(X509, (void *)(intptr_t)(i + 1), NULL);    /* data: index + 1 */
}

H(tmf_defaultAlg) { return fj_l(fj_string("PKIX")); }
H(tmf_getInstance)
{
    const char *alg = fj_cstr(a[0].l);
    if (!alg || (strcasecmp(alg, "PKIX") && strcasecmp(alg, "X509") && strcasecmp(alg, "X.509") && strcasecmp(alg, "SunX509"))) {
        fj_throw(env, "java/security/NoSuchAlgorithmException", alg ? alg : "null");
        return fj_l(NULL);
    }
    return fj_l(fj_new_by_name(TMF, NULL, NULL));
}
H(tmf_init)
{
    if (a[0].l) up_log("TrustManagerFactory.init(KeyStore): the system store is used");
    up_tls_ca_count();
    fj_set_field(self, "inited", "Z", fj_z(1));
    return fj_v();
}
H(tmf_getTrustManagers)
{
    if (!fj_get_field(self, "inited", "Z").z) {
        fj_throw(env, "java/lang/IllegalStateException", "TrustManagerFactoryImpl is not initialized");
        return fj_l(NULL);
    }
    jobjectArray arr = fj_array_new("Ljavax/net/ssl/TrustManager;", 1);
    (*env)->SetObjectArrayElement(env, arr, 0, fj_new_by_name(TM_IMPL, NULL, NULL));
    return fj_l(arr);
}
H(tm_acceptedIssuers)
{
    int n = up_tls_ca_count();
    jobjectArray arr = fj_array_new("Ljava/security/cert/X509Certificate;", n);
    for (int i = 0; i < n; i++) {
        jobject c = new_cert(i);
        (*env)->SetObjectArrayElement(env, arr, i, c);
        (*env)->DeleteLocalRef(env, c);
    }
    return fj_l(arr);
}
H(tm_check)
{
    fj_throw(env, "java/security/cert/CertificateException", "chain checks are made by the caller with getAcceptedIssuers()");
    return fj_v();
}
H(cert_getEncoded)
{
    int i = (int)(intptr_t)fj_data(self) - 1;
    if (i < 0 || i >= g_ncerts) { fj_throw(env, "java/security/cert/CertificateEncodingException", "no certificate"); return fj_l(NULL); }
    jbyteArray b = fj_array_new("B", (jsize)g_certs[i].len);
    memcpy(fj_array_data(b), g_certs[i].der, g_certs[i].len);
    return fj_l(b);
}
H(cert_getType) { return fj_l(fj_string("X.509")); }

/* KeyStore.getInstance("AndroidCAStore") (or the default type): the same certificates */
H(ks_defaultType) { return fj_l(fj_string("AndroidCAStore")); }
H(ks_getInstance)
{
    const char *t = fj_cstr(a[0].l);
    if (!t || (strcmp(t, "AndroidCAStore") && strcasecmp(t, "BKS") && strcasecmp(t, "PKCS12") && strcasecmp(t, "JKS"))) {
        fj_throw(env, "java/security/KeyStoreException", t ? t : "null");
        return fj_l(NULL);
    }
    return fj_l(fj_new_by_name(KS, (void *)(intptr_t)(strcmp(t, "AndroidCAStore") == 0), NULL));
}
H(ks_load) { if (fj_data(self)) up_tls_ca_count(); return fj_v(); }
static int ks_n(jobject self) { return fj_data(self) ? up_tls_ca_count() : 0; }
H(ks_size) { return fj_i(ks_n(self)); }
static int alias_index(jobject self, jstring s)
{
    const char *a = fj_cstr(s);
    int i;
    if (!a || sscanf(a, "system:%d", &i) != 1 || i < 0 || i >= ks_n(self)) return -1;
    return i;
}
H(ks_contains) { return fj_z(alias_index(self, a[0].l) >= 0); }
H(ks_getCertificate) { int i = alias_index(self, a[0].l); return fj_l(i >= 0 ? new_cert(i) : NULL); }
H(ks_aliases) { return fj_l(fj_new_by_name(ENUM_IMPL, NULL, NULL)); }  /* field "next" counts up */
H(en_has)
{
    int i = fj_get_field(self, "next", "I").i;
    return fj_z(i < up_tls_ca_count());
}
H(en_next)
{
    int i = fj_get_field(self, "next", "I").i;
    if (i >= up_tls_ca_count()) { fj_throw(env, "java/util/NoSuchElementException", NULL); return fj_l(NULL); }
    fj_set_field(self, "next", "I", fj_i(i + 1));
    char b[32];
    snprintf(b, sizeof b, "system:%d", i);
    return fj_l(fj_string(b));
}

void up_tls_register(void)
{
    fj_class_define("javax/net/ssl/TrustManager", NULL);
    fj_class_define("javax/net/ssl/X509TrustManager", NULL);
    fj_class_implements("javax/net/ssl/X509TrustManager", "javax/net/ssl/TrustManager");
    fj_class_define(TMF, NULL);
    fj_field_define(TMF, "inited", "Z", 0, fj_z(0));
    SM(TMF, "getDefaultAlgorithm", "()Ljava/lang/String;", tmf_defaultAlg);
    SM(TMF, "getInstance", "(Ljava/lang/String;)L" TMF ";", tmf_getInstance);
    M(TMF, "init", "(Ljava/security/KeyStore;)V", tmf_init);
    M(TMF, "getTrustManagers", "()[Ljavax/net/ssl/TrustManager;", tmf_getTrustManagers);
    fj_class_define(TM_IMPL, NULL);
    fj_class_implements(TM_IMPL, "javax/net/ssl/X509TrustManager");
    fj_class_implements(TM_IMPL, "javax/net/ssl/TrustManager");
    M(TM_IMPL, "getAcceptedIssuers", "()[Ljava/security/cert/X509Certificate;", tm_acceptedIssuers);
    M("javax/net/ssl/X509TrustManager", "getAcceptedIssuers", "()[Ljava/security/cert/X509Certificate;", tm_acceptedIssuers);
    M(TM_IMPL, "checkServerTrusted", "([Ljava/security/cert/X509Certificate;Ljava/lang/String;)V", tm_check);
    M(TM_IMPL, "checkClientTrusted", "([Ljava/security/cert/X509Certificate;Ljava/lang/String;)V", tm_check);
    fj_class_define(CERTC, NULL);
    fj_class_define(X509, CERTC);
    M(CERTC, "getEncoded", "()[B", cert_getEncoded);
    M(CERTC, "getType", "()Ljava/lang/String;", cert_getType);
    fj_class_define(KS, NULL);
    SM(KS, "getDefaultType", "()Ljava/lang/String;", ks_defaultType);
    SM(KS, "getInstance", "(Ljava/lang/String;)Ljava/security/KeyStore;", ks_getInstance);
    M(KS, "load", "(Ljava/io/InputStream;[C)V", ks_load);
    M(KS, "load", "(Ljava/security/KeyStore$LoadStoreParameter;)V", ks_load);
    M(KS, "size", "()I", ks_size);
    M(KS, "containsAlias", "(Ljava/lang/String;)Z", ks_contains);
    M(KS, "getCertificate", "(Ljava/lang/String;)Ljava/security/cert/Certificate;", ks_getCertificate);
    M(KS, "aliases", "()Ljava/util/Enumeration;", ks_aliases);
    fj_class_define("java/util/Enumeration", NULL);
    fj_class_define(ENUM_IMPL, NULL);
    fj_class_implements(ENUM_IMPL, "java/util/Enumeration");
    fj_field_define(ENUM_IMPL, "next", "I", 0, fj_i(0));
    M(ENUM_IMPL, "hasMoreElements", "()Z", en_has);
    M(ENUM_IMPL, "nextElement", "()Ljava/lang/Object;", en_next);
    M("java/util/Enumeration", "hasMoreElements", "()Z", en_has);
    M("java/util/Enumeration", "nextElement", "()Ljava/lang/Object;", en_next);
}

/* the bundle file (or certificate directory) the store came from, for other TLS clients of the
   runtime (up_http.c's libcurl): NULL when none was found */
const char *up_tls_bundle_path(int *is_dir)
{
    up_tls_ca_count();
    if (!g_source[0]) return NULL;
    struct stat st;
    if (is_dir) *is_dir = stat(g_source, &st) == 0 && S_ISDIR(st.st_mode);
    return g_source;
}
