/* com.unity3d.player.UnityWebRequest (Unity 2017.4 - 2018.4): Unity's HTTP(S) transport in Java
 * (HttpURLConnection; file: and jar: URLs libunity reads natively; 2019.4+ have their own transport,
 * whose TLS trust store is up_tls.c). docs/plugins.md, "HTTPS".
 *
 * By default this runtime answers every request as a phone without a network does: run() ends it
 * the way HttpURLConnection does there, so WWW/UnityWebRequest complete with an error at once
 * instead of never finishing (http(s): UnknownHostException, errorCallback 7; no scheme:
 * MalformedURLException, 5; another scheme: a protocol error, 4).
 *
 * UA_HTTP=1 (opt-in) makes real requests with the system's libcurl (dlopen libcurl.so.4, or
 * libcurl-gnutls.so.4), driving libunity's natives in the order Unity's Java class does:
 *   uploadCallback(ptr, ByteBuffer) until it gives no more bytes (the body, read up front)
 *   responseCodeCallback(ptr, code)
 *   headerCallback(ptr, "Status", <status line>), headerCallback(ptr, name, value) for each header
 *     (content-length / content-type added when absent, as URLConnection's getters give them)
 *   contentLengthCallback(ptr, length or -1), responseCodeCallback(ptr, code)
 *   downloadCallback(ptr, ByteBuffer, n) per block (false: stop)
 *   errorCallback(ptr, code, message) on failure: unknown host 7, malformed URL 5, protocol 4,
 *     timeout 14 ("WebRequest timed out."), TLS 16, other I/O 12
 * Redirects are not followed (Unity follows them itself, as with setInstanceFollowRedirects(false));
 * the timeout is the request's (ms, 0 = none); certificates are checked by libcurl against the CA
 * bundle up_tls.c found (the system's), host names included. A game's own CertificateHandler
 * (2018.4: the constructor's flag, validateCertificateCallback) is not consulted: the system's checks
 * still apply, so nothing is accepted that they refuse. No cookie store is kept between requests. */
#define _GNU_SOURCE
#include "up_internal.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define UWR "com/unity3d/player/UnityWebRequest"
#define M(c, n, s, f)  fj_method_define(c, n, s, 0, f, NULL)
#define SM(c, n, s, f) fj_method_define(c, n, s, 1, f, NULL)

static const char *jstr(jvalue v) { const char *s = fj_cstr(v.l); return s ? s : ""; }

/* ---- java.util.HashMap: the request headers libunity puts there (read back by run()) */
typedef struct kv { char *k, *v; struct kv *next; } kv;
static void kv_free(void *p) { for (kv *e = p, *n; e; e = n) { n = e->next; free(e->k); free(e->v); free(e); } }
H(map_put)
{
    const char *k = fj_cstr(a[0].l), *v = fj_cstr(a[1].l);
    if (!k) return fj_l(NULL);
    kv *head = fj_data(self);
    for (kv *e = head; e; e = e->next)
        if (!strcmp(e->k, k)) { free(e->v); e->v = strdup(v ? v : ""); return fj_l(NULL); }
    kv *e = calloc(1, sizeof *e);
    e->k = strdup(k); e->v = strdup(v ? v : "");
    e->next = head;
    fj_set_data(self, NULL, NULL);
    fj_set_data(self, e, kv_free);
    return fj_l(NULL);
}
H(map_size) { int n = 0; for (kv *e = fj_data(self); e; e = e->next) n++; return fj_i(n); }
H(map_get)
{
    const char *k = fj_cstr(a[0].l);
    for (kv *e = fj_data(self); e && k; e = e->next) if (!strcmp(e->k, k)) return fj_l(fj_string(e->v));
    return fj_l(NULL);
}
H(map_contains) { const char *k = fj_cstr(a[0].l); for (kv *e = fj_data(self); e && k; e = e->next) if (!strcmp(e->k, k)) return fj_z(1); return fj_z(0); }

/* ---- the request */
typedef struct {
    jlong ptr;
    char *url, *method;
    kv *headers;                    /* a copy of the Map's pairs */
    int timeout_ms, cert_handler;
    long long upload_len;           /* setupTransferSettings (2018.4) */
    int expect_continue, chunked;
    volatile int aborted;
} uwr;
static void uwr_free(void *p) { uwr *w = p; if (w) { free(w->url); free(w->method); kv_free(w->headers); free(w); } }

H(uwr_init)          /* (ptr, method, headers, url, [certificate handler,] timeout) */
{
    uwr *w = calloc(1, sizeof *w);
    if (!w) return fj_v();
    w->ptr = a[0].j;
    w->method = strdup(jstr(a[1]));
    w->url = strdup(jstr(a[3]));
    for (kv *e = a[2].l ? fj_data(a[2].l) : NULL; e; e = e->next) {
        kv *c = calloc(1, sizeof *c);
        c->k = strdup(e->k); c->v = strdup(e->v);
        c->next = w->headers; w->headers = c;
    }
    if (ud) { w->cert_handler = a[4].z; w->timeout_ms = a[5].i; }
    else w->timeout_ms = a[4].i;
    fj_set_data(self, w, uwr_free);
    return fj_v();
}
H(uwr_setup)         /* setupTransferSettings(upload length, expect 100-continue, chunked) */
{
    uwr *w = fj_data(self);
    if (w) { w->upload_len = a[0].j; w->expect_continue = a[1].z; w->chunked = a[2].z; }
    return fj_v();
}
H(uwr_abort) { uwr *w = fj_data(self); if (w) w->aborted = 1; return fj_v(); }
H(uwr_void) { return fj_v(); }

static void error_cb(uwr *w, int code, const char *msg)
{
    jvalue b[3] = { fj_j(w->ptr), fj_i(code), fj_l(fj_string(msg)) };
    up_call_native(UWR, "errorCallback", "(JILjava/lang/String;)V", fj_class_find(UWR), b);
}

/* ---- libcurl, by its stable ABI (option and info numbers as libcurl defines them) */
typedef void CURL;
struct curl_slist;
static struct {
    int tried, ok;
    CURL *(*init)(void);
    int (*setopt)(CURL *, int, ...);
    int (*perform)(CURL *);
    int (*getinfo)(CURL *, int, ...);
    void (*cleanup)(CURL *);
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void (*slist_free)(struct curl_slist *);
    const char *(*strerror)(int);
    int (*global_init)(long);
} C;
enum {
    O_URL = 10002, O_CUSTOMREQUEST = 10036, O_HTTPHEADER = 10023, O_POSTFIELDS = 10015, O_POSTFIELDSIZE_LARGE = 30120,
    O_NOBODY = 44, O_HTTPGET = 80, O_FOLLOWLOCATION = 52, O_WRITEFUNCTION = 20011, O_WRITEDATA = 10001,
    O_HEADERFUNCTION = 20079, O_HEADERDATA = 10029, O_TIMEOUT_MS = 155, O_CONNECTTIMEOUT_MS = 156, O_CAINFO = 10065,
    O_CAPATH = 10097, O_NOSIGNAL = 99, O_XFERINFOFUNCTION = 20219, O_XFERINFODATA = 10057, O_NOPROGRESS = 43,
    O_ERRORBUFFER = 10010, O_SSL_VERIFYPEER = 64, O_SSL_VERIFYHOST = 81, O_USERAGENT = 10018,
    I_RESPONSE_CODE = 0x200002,
    E_OK = 0, E_UNSUPPORTED_PROTOCOL = 1, E_URL_MALFORMAT = 3, E_COULDNT_RESOLVE_PROXY = 5, E_COULDNT_RESOLVE_HOST = 6,
    E_WRITE_ERROR = 23, E_OPERATION_TIMEDOUT = 28, E_SSL_CONNECT_ERROR = 35, E_ABORTED_BY_CALLBACK = 42,
    E_PEER_FAILED_VERIFICATION = 60, E_SSL_CERTPROBLEM = 58, E_SSL_CIPHER = 59, E_SSL_ENGINE_NOTFOUND = 53,
    E_USE_SSL_FAILED = 64, E_SSL_CACERT_BADFILE = 77, E_SSL_SHUTDOWN_FAILED = 80, E_SSL_CRL_BADFILE = 82,
    E_SSL_ISSUER_ERROR = 83, E_SSL_PINNEDPUBKEYNOTMATCH = 90, E_SSL_INVALIDCERTSTATUS = 91, E_SSL_CLIENTCERT = 98,
};

static int curl_load(void)
{
    if (C.tried) return C.ok;
    C.tried = 1;
    static const char *const names[] = { "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so", NULL };
    void *h = NULL;
    for (int i = 0; names[i] && !h; i++) h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!h) { up_log("UA_HTTP: no libcurl (libcurl.so.4) on this system: requests fail as without a network"); return 0; }
#define S(f, n) *(void **)&C.f = dlsym(h, n)
    S(init, "curl_easy_init"); S(setopt, "curl_easy_setopt"); S(perform, "curl_easy_perform");
    S(getinfo, "curl_easy_getinfo"); S(cleanup, "curl_easy_cleanup"); S(slist_append, "curl_slist_append");
    S(slist_free, "curl_slist_free_all"); S(strerror, "curl_easy_strerror"); S(global_init, "curl_global_init");
#undef S
    C.ok = C.init && C.setopt && C.perform && C.getinfo && C.cleanup && C.slist_append && C.slist_free && C.strerror;
    if (C.ok && C.global_init) C.global_init(3);              /* CURL_GLOBAL_ALL */
    up_log("UA_HTTP: %s", C.ok ? "real requests through libcurl" : "libcurl lacks the easy API: requests fail as without a network");
    return C.ok;
}

int up_http_enabled(void)
{
    const char *e = getenv("UA_HTTP");
    return e && *e && strcmp(e, "0") && curl_load();
}

typedef struct {
    uwr *w;
    CURL *c;
    long code;
    int sent;                       /* response code and headers given to Unity */
    kv *hdr, **tail;                /* this response's headers, in order */
    char status[256];
    jobject buf;                    /* the 128 KiB direct ByteBuffer, as Java's */
    unsigned char *mem;
} xfer;
#define BLOCK 131072

static void hdr_clear(xfer *x) { kv_free(x->hdr); x->hdr = NULL; x->tail = &x->hdr; x->status[0] = 0; }
static const char *hdr_find(xfer *x, const char *name)
{
    for (kv *e = x->hdr; e; e = e->next) if (!strcasecmp(e->k, name)) return e->v;
    return NULL;
}

static size_t on_header(char *p, size_t size, size_t n, void *ud)
{
    xfer *x = ud;
    size_t len = size * n, l = len;
    while (l && (p[l - 1] == '\r' || p[l - 1] == '\n')) l--;
    if (l >= 5 && !strncmp(p, "HTTP/", 5)) {                  /* a (new) response: 100 Continue, then the real one */
        hdr_clear(x);
        snprintf(x->status, sizeof x->status, "%.*s", (int)l, p);
        return len;
    }
    const char *c = memchr(p, ':', l);
    if (!c) return len;
    const char *v = c + 1;
    while (v < p + l && (*v == ' ' || *v == '\t')) v++;
    kv *e = calloc(1, sizeof *e);
    e->k = strndup(p, c - p);
    e->v = strndup(v, p + l - v);
    *x->tail = e; x->tail = &e->next;
    return len;
}

static void send_head(xfer *x)
{
    if (x->sent) return;
    x->sent = 1;
    uwr *w = x->w;
    jclass cls = fj_class_find(UWR);
    if (x->c) C.getinfo(x->c, I_RESPONSE_CODE, &x->code);
    jvalue rc[2] = { fj_j(w->ptr), fj_i((jint)x->code) };
    up_call_native(UWR, "responseCodeCallback", "(JI)V", cls, rc);
#define HDR(k, v) do { fj_push_frame(); jvalue b[3] = { fj_j(w->ptr), fj_l(fj_string(k)), fj_l(fj_string(v)) }; \
                       up_call_native(UWR, "headerCallback", "(JLjava/lang/String;Ljava/lang/String;)V", cls, b); fj_pop_frame(); } while (0)
    if (x->status[0]) HDR("Status", x->status);
    for (kv *e = x->hdr; e; e = e->next) HDR(e->k, e->v);
    const char *cl = hdr_find(x, "content-length");       /* (case-insensitive, as Android's header map) */
#undef HDR
    jvalue lb[2] = { fj_j(w->ptr), fj_i(cl ? atoi(cl) : -1) };
    up_call_native(UWR, "contentLengthCallback", "(JI)V", cls, lb);
    up_call_native(UWR, "responseCodeCallback", "(JI)V", cls, rc);
}

static size_t on_body(char *p, size_t size, size_t n, void *ud)
{
    xfer *x = ud;
    size_t len = size * n;
    if (x->w->aborted) return 0;
    if (!x->sent) send_head(x);
    jclass cls = fj_class_find(UWR);
    for (size_t off = 0; off < len;) {
        size_t k = len - off > BLOCK ? BLOCK : len - off;
        memcpy(x->mem, p + off, k);
        jvalue b[3] = { fj_j(x->w->ptr), fj_l(x->buf), fj_i((jint)k) };
        jvalue r = up_call_native(UWR, "downloadCallback", "(JLjava/nio/ByteBuffer;I)Z", cls, b);
        if (!r.z) { x->w->aborted = 1; return 0; }          /* Unity stopped the download */
        off += k;
    }
    return len;
}

static int on_progress(void *ud, long long dt, long long dn, long long ut, long long un)
{
    (void)dt; (void)dn; (void)ut; (void)un;
    return ((xfer *)ud)->w->aborted;
}

const char *up_tls_bundle_path(int *is_dir);      /* up_tls.c */

static void http_run(JNIEnv *env, uwr *w)
{
    jclass cls = fj_class_find(UWR);
    xfer x;
    memset(&x, 0, sizeof x);
    x.w = w;
    x.tail = &x.hdr;
    x.mem = malloc(BLOCK);
    x.buf = x.mem ? (*env)->NewDirectByteBuffer(env, x.mem, BLOCK) : NULL;
    /* the body: uploadCallback fills the buffer until it returns 0 (Java writes each block out) */
    unsigned char *body = NULL;
    size_t nbody = 0;
    for (;;) {
        jvalue b[2] = { fj_j(w->ptr), fj_l(x.buf) };
        int k = up_call_native(UWR, "uploadCallback", "(JLjava/nio/ByteBuffer;)I", cls, b).i;
        if (k <= 0) break;
        unsigned char *nb = realloc(body, nbody + k);
        if (!nb) break;
        body = nb;
        memcpy(body + nbody, x.mem, k);
        nbody += k;
    }
    CURL *c = C.init();
    x.c = c;
    char err[256] = "";
    struct curl_slist *hl = NULL;
    for (kv *e = w->headers; e; e = e->next) {
        char line[2048];
        snprintf(line, sizeof line, "%s: %s", e->k, e->v);
        hl = C.slist_append(hl, line);
    }
    if (!w->expect_continue) hl = C.slist_append(hl, "Expect:");
    C.setopt(c, O_URL, w->url);
    C.setopt(c, O_NOSIGNAL, 1L);
    C.setopt(c, O_FOLLOWLOCATION, 0L);
    C.setopt(c, O_ERRORBUFFER, err);
    C.setopt(c, O_HTTPHEADER, hl);
    C.setopt(c, O_SSL_VERIFYPEER, 1L);
    C.setopt(c, O_SSL_VERIFYHOST, 2L);
    int dir = 0;
    const char *ca = up_tls_bundle_path(&dir);
    if (ca) C.setopt(c, dir ? O_CAPATH : O_CAINFO, ca);
    if (w->timeout_ms > 0) { C.setopt(c, O_TIMEOUT_MS, (long)w->timeout_ms); C.setopt(c, O_CONNECTTIMEOUT_MS, (long)w->timeout_ms); }
    if (!strcasecmp(w->method, "HEAD")) C.setopt(c, O_NOBODY, 1L);
    else if (!strcasecmp(w->method, "GET") && !nbody) C.setopt(c, O_HTTPGET, 1L);
    else C.setopt(c, O_CUSTOMREQUEST, w->method);
    if (nbody || !strcasecmp(w->method, "POST") || !strcasecmp(w->method, "PUT")) {
        C.setopt(c, O_POSTFIELDS, body ? (char *)body : "");
        C.setopt(c, O_POSTFIELDSIZE_LARGE, (long long)nbody);
        if (strcasecmp(w->method, "POST")) C.setopt(c, O_CUSTOMREQUEST, w->method);
    }
    C.setopt(c, O_HEADERFUNCTION, on_header);
    C.setopt(c, O_HEADERDATA, &x);
    C.setopt(c, O_WRITEFUNCTION, on_body);
    C.setopt(c, O_WRITEDATA, &x);
    C.setopt(c, O_NOPROGRESS, 0L);
    C.setopt(c, O_XFERINFOFUNCTION, on_progress);
    C.setopt(c, O_XFERINFODATA, &x);
    int r = C.perform(c);
    if (r == E_OK) send_head(&x);                              /* a response without a body */
    if (r != E_OK && !w->aborted) {
        char msg[600];
        const char *what = err[0] ? err : C.strerror(r);
        int ucode;
        switch (r) {
        case E_COULDNT_RESOLVE_HOST: case E_COULDNT_RESOLVE_PROXY: ucode = 7; snprintf(msg, sizeof msg, "java.net.UnknownHostException: %s", what); break;
        case E_URL_MALFORMAT: ucode = 5; snprintf(msg, sizeof msg, "java.net.MalformedURLException: %s", what); break;
        case E_UNSUPPORTED_PROTOCOL: ucode = 4; snprintf(msg, sizeof msg, "java.net.ProtocolException: %s", what); break;
        case E_OPERATION_TIMEDOUT: ucode = 14; snprintf(msg, sizeof msg, "WebRequest timed out."); break;
        case E_SSL_CONNECT_ERROR: case E_PEER_FAILED_VERIFICATION: case E_SSL_CERTPROBLEM: case E_SSL_CIPHER:
        case E_SSL_ENGINE_NOTFOUND: case E_USE_SSL_FAILED: case E_SSL_CACERT_BADFILE: case E_SSL_SHUTDOWN_FAILED:
        case E_SSL_CRL_BADFILE: case E_SSL_ISSUER_ERROR: case E_SSL_PINNEDPUBKEYNOTMATCH: case E_SSL_INVALIDCERTSTATUS:
        case E_SSL_CLIENTCERT:
            ucode = 16; snprintf(msg, sizeof msg, "javax.net.ssl.SSLHandshakeException: %s", what); break;
        default: ucode = 12; snprintf(msg, sizeof msg, "java.io.IOException: %s", what);
        }
        up_log("UnityWebRequest %s %s: %s (error %d)", w->method, w->url, what, ucode);
        error_cb(w, ucode, msg);
    } else if (r == E_OK) up_log("UnityWebRequest %s %s: %ld", w->method, w->url, x.code);
    C.slist_free(hl);
    C.cleanup(c);
    hdr_clear(&x);
    free(body);
    free(x.mem);       /* the ByteBuffer object keeps a dangling address only until Unity drops it */
}

H(uwr_run)
{
    uwr *w = fj_data(self);
    if (!w) return fj_v();
    if (w->cert_handler && up_http_enabled()) {
        static int once;
        if (!once++) up_log("UA_HTTP: the game's CertificateHandler is not consulted; the system's certificate checks apply");
    }
    if (up_http_enabled() && (!strncasecmp(w->url, "http://", 7) || !strncasecmp(w->url, "https://", 8))) {
        fj_push_frame();
        http_run(env, w);
        fj_pop_frame();
        return fj_v();
    }
    const char *u = w->url, *sep = strstr(u, "://");
    char msg[600];
    int code;
    if (!sep) { code = 5; snprintf(msg, sizeof msg, "no protocol: %s", u); }
    else if (!strncasecmp(u, "http://", 7) || !strncasecmp(u, "https://", 8)) {
        const char *h = sep + 3;
        int n = (int)strcspn(h, ":/?#");
        code = 7;
        snprintf(msg, sizeof msg, "Unable to resolve host \"%.*s\": No address associated with hostname", n > 255 ? 255 : n, h);
    } else { code = 4; snprintf(msg, sizeof msg, "unknown protocol: %.*s", (int)(sep - u) > 32 ? 32 : (int)(sep - u), u); }
    up_log("UnityWebRequest %s: no network here, error %d", u, code);
    error_cb(w, code, msg);
    return fj_v();
}

void up_http_register(void)
{
    fj_class_define(UWR, NULL);
    fj_class_implements(UWR, "java/lang/Runnable");
    fj_method_define(UWR, "<init>", "(JLjava/lang/String;Ljava/util/Map;Ljava/lang/String;I)V", 0, uwr_init, NULL);           /* 2017.4 */
    fj_method_define(UWR, "<init>", "(JLjava/lang/String;Ljava/util/Map;Ljava/lang/String;ZI)V", 0, uwr_init, (void *)1);    /* 2018.4 */
    M(UWR, "setupTransferSettings", "(JZZ)V", uwr_setup);
    M(UWR, "run", "()V", uwr_run);
    M(UWR, "abort", "()V", uwr_abort);
    SM(UWR, "clearCookieCache", "(Ljava/lang/String;Ljava/lang/String;)V", uwr_void);   /* no cookie store */
    const char *HM = "java/util/HashMap";
    M(HM, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", map_put);
    M(HM, "get", "(Ljava/lang/Object;)Ljava/lang/Object;", map_get);
    M(HM, "size", "()I", map_size);
    M(HM, "containsKey", "(Ljava/lang/Object;)Z", map_contains);
}
