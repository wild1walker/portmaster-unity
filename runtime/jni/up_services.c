/* up_services.c: the phone services an Android game's code reaches through JNI that a handheld does not
 * have, answered as a phone without them answers (an APK ported as it is, docs/apk.md), plus Java's own
 * cryptography, which a handheld can do:
 *   - Google Play Billing (com.android.billingclient.api): the client connects and reports
 *     BILLING_UNAVAILABLE (3) to its listener, on the UI thread as Play does; every query answers the same
 *     with an empty list, no purchase flow starts. Unity IAP 4.x (C# over AndroidJavaObject/Proxy) then
 *     reports the store unavailable instead of waiting for a connection that never comes.
 *   - Play In-App Review (com.google.android.play.core.review): requestReviewFlow and launchReviewFlow give
 *     a Task that has already failed; its listeners are called at once (a review never shows).
 *   - Unity's Mobile Notifications package (com.unity.androidnotifications.UnityNotificationManager, the Java
 *     half its C# calls): a manager whose app has notifications blocked (areNotificationsEnabled 5), no
 *     channels, nothing scheduled or shown; its builder keeps the extras the C# writes. Without it
 *     AndroidNotificationCenter.Initialize threw a NullReferenceException (the manager was null).
 *   - Google Play Games (v2, com.google.android.gms.games: what the Play Games plugin for Unity calls):
 *     PlayGamesSdk.initialize does nothing; the sign-in client's isAuthenticated/signIn/
 *     requestServerSideAccess give a failed task, as on a phone without Play Games: the plugin reports a
 *     sign-in error and the game goes on without it.
 *   - java.security.MessageDigest (MD5, SHA-1, SHA-256), java.security.SecureRandom (getrandom),
 *     javax.crypto.Mac (HmacMD5, HmacSHA1, HmacSHA256), javax.crypto.Cipher (AES in ECB, CBC or CTR, with
 *     PKCS5Padding or NoPadding), SecretKeySpec and IvParameterSpec: real results, written here (no
 *     library). An unknown algorithm throws NoSuchAlgorithmException, as Java does.
 * Without these, the names were placeholders whose calls answered null, which a game's code then used.
 * Names and signatures as the libraries' public API (Play Billing 5-7, Play Core 1.x / Review 2.x, Java SE). */
#include "up_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>

#define M(c, n, s, f)       fj_method_define(c, n, s, 0, f, NULL)
#define MU(c, n, s, f, u)   fj_method_define(c, n, s, 0, f, (void *)(u))
#define SM(c, n, s, f)      fj_method_define(c, n, s, 1, f, NULL)
#define CLS(c, s)           fj_class_define(c, s)

static const char *jstr(jvalue v) { const char *s = fj_cstr(v.l); return s ? s : ""; }
H(h_self)  { return fj_l(self); }
H(h_void)  { return fj_v(); }
H(h_false) { return fj_z(0); }
H(h_true)  { return fj_z(1); }
H(h_null)  { return fj_l(NULL); }

static jobject empty_list(JNIEnv *env)
{
    jclass c = (*env)->FindClass(env, "java/util/ArrayList");
    return (*env)->NewObject(env, c, (*env)->GetMethodID(env, c, "<init>", "()V"));
}

/* ---- Play Billing ---------------------------------------------------------------------------- */
#define BC "com/android/billingclient/api/BillingClient"
#define BR "com/android/billingclient/api/BillingResult"
enum { BILLING_UNAVAILABLE = 3 };

static jobject billing_result(void)
{
    jobject r = fj_new_by_name(BR, NULL, NULL);
    fj_set_field(r, "code", "I", fj_i(BILLING_UNAVAILABLE));
    fj_set_obj(r, "msg", "Ljava/lang/String;", fj_string("Google Play is not on this device"));
    return r;
}
H(br_code) { return fj_get_field(self, "code", "I"); }
H(br_msg)  { return fj_get_field(self, "msg", "Ljava/lang/String;"); }
H(br_new_builder) { return fj_l(fj_new_by_name(BR "$Builder", NULL, NULL)); }
H(brb_code) { fj_set_field(self, "code", "I", a[0]); return fj_l(self); }
H(brb_msg)  { fj_set_obj(self, "msg", "Ljava/lang/String;", a[0].l); return fj_l(self); }
H(brb_build)
{
    jobject r = fj_new_by_name(BR, NULL, NULL);
    fj_set_field(r, "code", "I", fj_get_field(self, "code", "I"));
    fj_set_obj(r, "msg", "Ljava/lang/String;", fj_get_field(self, "msg", "Ljava/lang/String;").l);
    return fj_l(r);
}

/* a listener called on the UI thread, as Play's client does: <method>(BillingResult[, List]) */
struct billing_call { jobject listener; const char *method, *sig; int with_list; };
static void billing_cb(void *p)
{
    struct billing_call *c = p;
    JNIEnv *env = fj_env();
    fj_push_frame();
    jobject r = billing_result();
    if (c->with_list) fj_call(c->listener, c->method, c->sig, r, empty_list(env));
    else fj_call(c->listener, c->method, c->sig, r);
    fj_pop_frame();
    fj_release(c->listener);
    free(c);
}
static void billing_post(jobject listener, const char *method, const char *sig, int with_list)
{
    if (!listener) return;
    struct billing_call *c = malloc(sizeof *c);
    if (!c) return;
    *c = (struct billing_call){ fj_global(listener), method, sig, with_list };
    looper_post(&L_ui, billing_cb, c, NULL, 0);
}
H(bc_new_builder) { return fj_l(fj_new_by_name(BC "$Builder", NULL, NULL)); }
H(bcb_build)      { up_log("Play Billing: a client made: it will report BILLING_UNAVAILABLE"); return fj_l(fj_new_by_name(BC, NULL, NULL)); }
H(bc_start)
{
    billing_post(a[0].l, "onBillingSetupFinished", "(L" BR ";)V", 0);
    return fj_v();
}
H(bc_state)       { return fj_i(0); }           /* ConnectionState.DISCONNECTED */
H(bc_result)      { return fj_l(billing_result()); }
/* query*Async(params, listener): the listener's method from the user data, (BillingResult, List) */
struct bq { const char *method, *sig; };
H(bc_query)       { const struct bq *q = ud; billing_post(a[1].l, q->method, q->sig, 1); return fj_v(); }
H(bc_query_type)  { const struct bq *q = ud; billing_post(a[1].l, q->method, q->sig, 1); return fj_v(); }

/* ---- Play In-App Review ------------------------------------------------------------------------ */
#define RV "com/google/android/play/core/review"
#define TASK "com/google/android/play/core/tasks/Task"
#define GTASK "com/google/android/gms/tasks/Task"
static jobject failed_task(const char *cls)
{
    jobject t = fj_new_by_name(cls, NULL, NULL);
    JNIEnv *env = fj_env();
    jclass ec = (*env)->FindClass(env, "java/lang/Exception"), tc = (*env)->FindClass(env, "java/lang/Throwable");
    jobject ex = (*env)->NewObject(env, ec, (*env)->GetMethodID(env, tc, "<init>", "(Ljava/lang/String;)V"),
                                   fj_string("Google Play is not on this device"));
    fj_set_obj(t, "ex", "Ljava/lang/Exception;", ex);
    return t;
}
H(rv_create)  { up_log("Play In-App Review: a manager made: its flows fail"); return fj_l(fj_new_by_name(RV "/ReviewManager", NULL, NULL)); }
H(rv_flow)    { return fj_l(failed_task((const char *)ud)); }
H(task_ex)    { return fj_get_field(self, "ex", "Ljava/lang/Exception;"); }
/* addOnCompleteListener(listener) / addOnFailureListener(listener): called at once (the task is done);
   addOnSuccessListener: never. Each returns the task. */
H(task_complete) { fj_call(a[0].l, "onComplete", (const char *)ud, self); return fj_l(self); }
H(task_failure)  { fj_call(a[0].l, "onFailure", "(Ljava/lang/Exception;)V", fj_get_field(self, "ex", "Ljava/lang/Exception;").l); return fj_l(self); }
H(task_complete2) { fj_call(a[1].l, "onComplete", (const char *)ud, self); return fj_l(self); }
H(task_failure2)  { fj_call(a[1].l, "onFailure", "(Ljava/lang/Exception;)V", fj_get_field(self, "ex", "Ljava/lang/Exception;").l); return fj_l(self); }

/* ---- Unity's Mobile Notifications --------------------------------------------------------------- */
#define UNM "com/unity/androidnotifications/UnityNotificationManager"
#define NB "android/app/Notification$Builder"
static jobject g_unm;
H(unm_get)
{
    if (!g_unm) {
        g_unm = fj_global(fj_new_by_name(UNM, NULL, NULL));
        up_log("Mobile Notifications: a manager made: notifications are blocked for the app, none is shown");
    }
    return fj_l(g_unm);
}
H(unm_target)   { return fj_i(UPC.sdk_int); }
H(unm_enabled)  { return fj_i(5); }                 /* PERMISSION_STATUS_NOTIFICATIONS_BLOCKED_FOR_APP */
H(unm_channels) { return fj_l(fj_array_new("Lcom/unity/androidnotifications/NotificationChannelWrapper;", 0)); }
H(unm_int0)     { return fj_i(0); }
H(unm_builder)
{
    jobject b = fj_new_by_name(NB, NULL, NULL);
    jclass bc = (*env)->FindClass(env, "android/os/Bundle");
    fj_set_obj(b, "extras", "Landroid/os/Bundle;", (*env)->NewObject(env, bc, (*env)->GetMethodID(env, bc, "<init>", "()V")));
    return fj_l(b);
}
H(nb_extras)    { return fj_get_field(self, "extras", "Landroid/os/Bundle;"); }

/* ---- Google Play Games v2 ---------------------------------------------------------------------------- */
#define PG "com/google/android/gms/games"
H(pg_client) { up_log("Play Games: a sign-in client made: its sign-in fails"); return fj_l(fj_new_by_name(PG "/GamesSignInClient", NULL, NULL)); }

/* ---- hashes: MD5, SHA-1, SHA-256 ---------------------------------------------------------------- */
enum { H_MD5, H_SHA1, H_SHA256 };
typedef struct {
    int alg;
    uint32_t s[8];
    uint64_t len;           /* bytes */
    uint8_t buf[64];
    size_t n;
} hash_ctx;

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void md5_block(uint32_t *s, const uint8_t *p)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const uint8_t R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                   5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                   6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t t = d; d = c; c = b;
        b = b + ROL(a + f + K[i] + w[g], R[i]);
        a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}
static void sha1_block(uint32_t *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e;
}
static void sha256_block(uint32_t *s, const uint8_t *p)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}
static size_t hash_size(int alg) { return alg == H_MD5 ? 16 : alg == H_SHA1 ? 20 : 32; }
static void hash_init(hash_ctx *h, int alg)
{
    static const uint32_t md5[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    static const uint32_t sha1[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    static const uint32_t sha256[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memset(h, 0, sizeof *h);
    h->alg = alg;
    memcpy(h->s, alg == H_MD5 ? md5 : alg == H_SHA1 ? sha1 : sha256, alg == H_MD5 ? 16 : alg == H_SHA1 ? 20 : 32);
}
static void hash_block(hash_ctx *h, const uint8_t *p)
{
    if (h->alg == H_MD5) md5_block(h->s, p);
    else if (h->alg == H_SHA1) sha1_block(h->s, p);
    else sha256_block(h->s, p);
}
static void hash_update(hash_ctx *h, const uint8_t *p, size_t n)
{
    h->len += n;
    while (n) {
        size_t k = 64 - h->n < n ? 64 - h->n : n;
        memcpy(h->buf + h->n, p, k);
        h->n += k; p += k; n -= k;
        if (h->n == 64) { hash_block(h, h->buf); h->n = 0; }
    }
}
static size_t hash_final(hash_ctx *h, uint8_t *out)
{
    uint64_t bits = h->len * 8;
    uint8_t pad = 0x80, zero = 0, l[8];
    hash_update(h, &pad, 1);
    while (h->n != 56) hash_update(h, &zero, 1);
    for (int i = 0; i < 8; i++) l[i] = h->alg == H_MD5 ? (uint8_t)(bits >> (8 * i)) : (uint8_t)(bits >> (56 - 8 * i));
    hash_update(h, l, 8);
    size_t n = hash_size(h->alg);
    for (size_t i = 0; i < n / 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = h->alg == H_MD5 ? (uint8_t)(h->s[i] >> (8 * j)) : (uint8_t)(h->s[i] >> (24 - 8 * j));
    return n;
}
static int hash_alg(const char *name)
{
    if (!strcasecmp(name, "MD5")) return H_MD5;
    if (!strcasecmp(name, "SHA-1") || !strcasecmp(name, "SHA1") || !strcasecmp(name, "SHA")) return H_SHA1;
    if (!strcasecmp(name, "SHA-256") || !strcasecmp(name, "SHA256")) return H_SHA256;
    return -1;
}

/* ---- AES (FIPS-197) ---------------------------------------------------------------------------- */
static uint8_t sbox[256], inv_sbox[256];
static void aes_tables(void)
{
    if (sbox[0]) return;
    uint8_t p = 1, q = 1;
    do {        /* the multiplicative inverse by walking powers of 3 and of its inverse, then the affine map */
        p = p ^ (uint8_t)(p << 1) ^ (p & 0x80 ? 0x1b : 0);
        q ^= q << 1; q ^= q << 2; q ^= q << 4; if (q & 0x80) q ^= 0x09;
        uint8_t x = q ^ (uint8_t)((q << 1) | (q >> 7)) ^ (uint8_t)((q << 2) | (q >> 6)) ^ (uint8_t)((q << 3) | (q >> 5)) ^
                    (uint8_t)((q << 4) | (q >> 4));
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) inv_sbox[sbox[i]] = (uint8_t)i;
}
static uint8_t xt(uint8_t x) { return (uint8_t)(x << 1) ^ (x & 0x80 ? 0x1b : 0); }
static uint8_t gm(uint8_t a, uint8_t b) { uint8_t r = 0; while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; } return r; }
typedef struct { int rounds; uint8_t rk[240]; } aes_key;
static int aes_setkey(aes_key *k, const uint8_t *key, size_t n)
{
    if (n != 16 && n != 24 && n != 32) return -1;
    aes_tables();
    int nk = (int)n / 4;
    k->rounds = nk + 6;
    memcpy(k->rk, key, n);
    uint8_t rcon = 1;
    for (int i = nk; i < 4 * (k->rounds + 1); i++) {
        uint8_t t[4];
        memcpy(t, k->rk + (i - 1) * 4, 4);
        if (i % nk == 0) {
            uint8_t u = t[0];
            t[0] = sbox[t[1]] ^ rcon; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[u];
            rcon = xt(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++) t[j] = sbox[t[j]];
        }
        for (int j = 0; j < 4; j++) k->rk[i * 4 + j] = k->rk[(i - nk) * 4 + j] ^ t[j];
    }
    return 0;
}
static void aes_encrypt(const aes_key *k, uint8_t *b)
{
    for (int i = 0; i < 16; i++) b[i] ^= k->rk[i];
    for (int r = 1; r <= k->rounds; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = sbox[b[(i + 4 * (i % 4)) % 16]];     /* SubBytes + ShiftRows */
        if (r < k->rounds)
            for (int c = 0; c < 4; c++) {
                uint8_t *s = t + 4 * c, a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3];
                s[0] = xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3;
                s[1] = a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3;
                s[2] = a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3;
                s[3] = xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3);
            }
        for (int i = 0; i < 16; i++) b[i] = t[i] ^ k->rk[r * 16 + i];
    }
}
static void aes_decrypt(const aes_key *k, uint8_t *b)
{
    for (int i = 0; i < 16; i++) b[i] ^= k->rk[k->rounds * 16 + i];
    for (int r = k->rounds - 1; r >= 0; r--) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[(i + 4 * (i % 4)) % 16] = inv_sbox[b[i]];  /* InvShiftRows + InvSubBytes */
        for (int i = 0; i < 16; i++) t[i] ^= k->rk[r * 16 + i];
        if (r > 0)
            for (int c = 0; c < 4; c++) {
                uint8_t *s = t + 4 * c, a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3];
                s[0] = gm(a0, 14) ^ gm(a1, 11) ^ gm(a2, 13) ^ gm(a3, 9);
                s[1] = gm(a0, 9) ^ gm(a1, 14) ^ gm(a2, 11) ^ gm(a3, 13);
                s[2] = gm(a0, 13) ^ gm(a1, 9) ^ gm(a2, 14) ^ gm(a3, 11);
                s[3] = gm(a0, 11) ^ gm(a1, 13) ^ gm(a2, 9) ^ gm(a3, 14);
            }
        memcpy(b, t, 16);
    }
}

/* ---- the Java classes ----------------------------------------------------------------------------- */
static void throw_alg(JNIEnv *env, const char *what)
{
    fj_throw(env, "java/security/NoSuchAlgorithmException", what);
}
static jbyteArray bytes(const uint8_t *p, size_t n)
{
    jbyteArray r = fj_array_new("B", (jsize)n);
    if (n) memcpy(fj_array_data(r), p, n);
    return r;
}

/* MessageDigest: the context is the object's data */
H(md_get)
{
    int alg = hash_alg(jstr(a[0]));
    if (alg < 0) { throw_alg(env, jstr(a[0])); return fj_l(NULL); }
    hash_ctx *h = malloc(sizeof *h);
    hash_init(h, alg);
    return fj_l(fj_new_by_name("java/security/MessageDigest", h, free));
}
H(md_update)      { hash_ctx *h = fj_data(self); if (h && a[0].l) hash_update(h, fj_array_data(a[0].l), (size_t)fj_array_len(a[0].l)); return fj_v(); }
H(md_update_part) { hash_ctx *h = fj_data(self); if (h && a[0].l) hash_update(h, (uint8_t *)fj_array_data(a[0].l) + a[1].i, (size_t)a[2].i); return fj_v(); }
H(md_update_b)    { hash_ctx *h = fj_data(self); uint8_t b = (uint8_t)a[0].b; if (h) hash_update(h, &b, 1); return fj_v(); }
H(md_digest)
{
    hash_ctx *h = fj_data(self);
    if (!h) return fj_l(NULL);
    if (ud && a[0].l) hash_update(h, fj_array_data(a[0].l), (size_t)fj_array_len(a[0].l));    /* digest(byte[]) */
    uint8_t out[32];
    size_t n = hash_final(h, out);
    hash_init(h, h->alg);
    return fj_l(bytes(out, n));
}
H(md_reset)  { hash_ctx *h = fj_data(self); if (h) hash_init(h, h->alg); return fj_v(); }
H(md_length) { hash_ctx *h = fj_data(self); return fj_i(h ? (jint)hash_size(h->alg) : 0); }

/* SecureRandom */
static void fill_random(uint8_t *p, size_t n)
{
    while (n) {
        ssize_t k = getrandom(p, n, 0);
        if (k < 0) { if (errno == EINTR) continue; for (size_t i = 0; i < n; i++) p[i] = (uint8_t)rand(); return; }
        p += k; n -= (size_t)k;
    }
}
H(sr_next_bytes) { if (a[0].l) fill_random(fj_array_data(a[0].l), (size_t)fj_array_len(a[0].l)); return fj_v(); }
H(sr_next_int)
{
    uint32_t v;
    fill_random((uint8_t *)&v, sizeof v);
    if (ud && a[0].i > 0) return fj_i((jint)(v % (uint32_t)a[0].i));      /* nextInt(bound) */
    return fj_i((jint)v);
}
H(sr_next_long) { uint64_t v; fill_random((uint8_t *)&v, sizeof v); return fj_j((jlong)v); }
H(sr_seed)      { return fj_l(bytes((const uint8_t *)"", 0)); }
H(sr_get)       { return fj_l(fj_new_by_name("java/security/SecureRandom", NULL, NULL)); }

/* SecretKeySpec(byte[] key, String alg), IvParameterSpec(byte[] iv): the bytes kept in a field */
H(key_init)    { fj_set_obj(self, "key", "[B", a[0].l); fj_set_obj(self, "alg", "Ljava/lang/String;", ud ? NULL : a[1].l); return fj_v(); }
H(key_encoded) { jobject k = fj_get_field(self, "key", "[B").l; return fj_l(k ? bytes(fj_array_data(k), (size_t)fj_array_len(k)) : NULL); }
H(key_alg)     { return fj_get_field(self, "alg", "Ljava/lang/String;"); }

/* Mac: HMAC over one of the hashes */
typedef struct { int alg; hash_ctx in; uint8_t opad[64]; uint8_t key[64]; int ready; } mac_ctx;
H(mac_get)
{
    const char *n = jstr(a[0]);
    int alg = !strcasecmp(n, "HmacMD5") ? H_MD5 : !strcasecmp(n, "HmacSHA1") ? H_SHA1 : !strcasecmp(n, "HmacSHA256") ? H_SHA256 : -1;
    if (alg < 0) { throw_alg(env, n); return fj_l(NULL); }
    mac_ctx *m = calloc(1, sizeof *m);
    m->alg = alg;
    return fj_l(fj_new_by_name("javax/crypto/Mac", m, free));
}
static void mac_start(mac_ctx *m)
{
    uint8_t ipad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = m->key[i] ^ 0x36; m->opad[i] = m->key[i] ^ 0x5c; }
    hash_init(&m->in, m->alg);
    hash_update(&m->in, ipad, 64);
}
H(mac_init)
{
    mac_ctx *m = fj_data(self);
    jobject k = a[0].l ? fj_get_field(a[0].l, "key", "[B").l : NULL;
    if (!m || !k) { fj_throw(env, "java/security/InvalidKeyException", "no key"); return fj_v(); }
    size_t n = (size_t)fj_array_len(k);
    memset(m->key, 0, 64);
    if (n > 64) { hash_ctx h; hash_init(&h, m->alg); hash_update(&h, fj_array_data(k), n); hash_final(&h, m->key); }
    else memcpy(m->key, fj_array_data(k), n);
    mac_start(m);
    m->ready = 1;
    return fj_v();
}
H(mac_update) { mac_ctx *m = fj_data(self); if (m && m->ready && a[0].l) hash_update(&m->in, fj_array_data(a[0].l), (size_t)fj_array_len(a[0].l)); return fj_v(); }
H(mac_final)
{
    mac_ctx *m = fj_data(self);
    if (!m || !m->ready) { fj_throw(env, "java/lang/IllegalStateException", "MAC not initialized"); return fj_l(NULL); }
    if (ud && a[0].l) hash_update(&m->in, fj_array_data(a[0].l), (size_t)fj_array_len(a[0].l));
    uint8_t inner[32], out[32];
    size_t n = hash_final(&m->in, inner);
    hash_ctx o;
    hash_init(&o, m->alg);
    hash_update(&o, m->opad, 64);
    hash_update(&o, inner, n);
    hash_final(&o, out);
    mac_start(m);
    return fj_l(bytes(out, n));
}
H(mac_length) { mac_ctx *m = fj_data(self); return fj_i(m ? (jint)hash_size(m->alg) : 0); }

/* Cipher: AES/ECB|CBC|CTR/PKCS5Padding|NoPadding ("AES" alone: ECB with PKCS5, as Java's default) */
enum { C_ECB, C_CBC, C_CTR };
typedef struct {
    int mode, pad, encrypt, ready;
    aes_key key;
    uint8_t iv[16], buf[16];
    size_t n;                   /* bytes waiting in buf (ECB/CBC), or used of the counter's block (CTR) */
    uint8_t *out; size_t out_n, out_cap;
} cipher_ctx;
static void cipher_free(void *p) { cipher_ctx *c = p; free(c->out); free(c); }
H(ci_get)
{
    char t[64];
    snprintf(t, sizeof t, "%s", jstr(a[0]));
    char *mode = strchr(t, '/'), *pad = mode ? strchr(mode + 1, '/') : NULL;
    if (mode) *mode++ = 0;
    if (pad) *pad++ = 0;
    cipher_ctx *c = calloc(1, sizeof *c);
    c->mode = !mode || !strcasecmp(mode, "ECB") ? C_ECB : !strcasecmp(mode, "CBC") ? C_CBC : !strcasecmp(mode, "CTR") ? C_CTR : -1;
    c->pad = !pad || !strcasecmp(pad, "PKCS5Padding") || !strcasecmp(pad, "PKCS7Padding") ? 1 : !strcasecmp(pad, "NoPadding") ? 0 : -1;
    if (strcasecmp(t, "AES") || c->mode < 0 || c->pad < 0) {
        free(c);
        fj_throw(env, "java/security/NoSuchAlgorithmException", jstr(a[0]));
        return fj_l(NULL);
    }
    if (c->mode == C_CTR) c->pad = 0;
    return fj_l(fj_new_by_name("javax/crypto/Cipher", c, cipher_free));
}
H(ci_init)
{
    cipher_ctx *c = fj_data(self);
    jobject k = a[1].l ? fj_get_field(a[1].l, "key", "[B").l : NULL;
    if (!c || !k || aes_setkey(&c->key, fj_array_data(k), (size_t)fj_array_len(k))) {
        fj_throw(env, "java/security/InvalidKeyException", "an AES key is 16, 24 or 32 bytes");
        return fj_v();
    }
    c->encrypt = a[0].i == 1;                            /* Cipher.ENCRYPT_MODE 1, DECRYPT_MODE 2 */
    memset(c->iv, 0, 16);
    jobject iv = ud && a[2].l ? fj_get_field(a[2].l, "key", "[B").l : NULL;
    if (iv && fj_array_len(iv) == 16) memcpy(c->iv, fj_array_data(iv), 16);
    else if (c->mode != C_ECB && c->encrypt && !iv) fill_random(c->iv, 16);   /* Java picks one; getIV() gives it */
    c->n = 0; c->out_n = 0; c->ready = 1;
    return fj_v();
}
static void out_put(cipher_ctx *c, const uint8_t *p, size_t n)
{
    if (c->out_n + n > c->out_cap) {
        c->out_cap = (c->out_n + n) * 2 + 64;
        c->out = realloc(c->out, c->out_cap);
    }
    memcpy(c->out + c->out_n, p, n);
    c->out_n += n;
}
static void ci_block(cipher_ctx *c, uint8_t *b)
{
    if (c->mode == C_CBC) {
        if (c->encrypt) { for (int i = 0; i < 16; i++) b[i] ^= c->iv[i]; aes_encrypt(&c->key, b); memcpy(c->iv, b, 16); }
        else { uint8_t prev[16]; memcpy(prev, b, 16); aes_decrypt(&c->key, b); for (int i = 0; i < 16; i++) b[i] ^= c->iv[i]; memcpy(c->iv, prev, 16); }
    } else if (c->encrypt) aes_encrypt(&c->key, b);
    else aes_decrypt(&c->key, b);
}
/* the input through the cipher; a block that may be the last (decrypting with padding) is kept back */
static void ci_feed(cipher_ctx *c, const uint8_t *p, size_t n)
{
    if (c->mode == C_CTR) {
        for (size_t i = 0; i < n; i++) {
            if (c->n == 0) { memcpy(c->buf, c->iv, 16); aes_encrypt(&c->key, c->buf); for (int j = 15; j >= 0 && !++c->iv[j]; j--) {} }
            uint8_t b = p[i] ^ c->buf[c->n];
            out_put(c, &b, 1);
            c->n = (c->n + 1) & 15;
        }
        return;
    }
    while (n) {
        if (c->n == 16 && (c->encrypt || !c->pad || n > 0)) { uint8_t b[16]; memcpy(b, c->buf, 16); ci_block(c, b); out_put(c, b, 16); c->n = 0; }
        size_t k = 16 - c->n < n ? 16 - c->n : n;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; n -= k;
        if (c->n == 16 && (c->encrypt || !c->pad)) { uint8_t b[16]; memcpy(b, c->buf, 16); ci_block(c, b); out_put(c, b, 16); c->n = 0; }
    }
}
static jbyteArray ci_take(cipher_ctx *c)
{
    jbyteArray r = bytes(c->out, c->out_n);
    c->out_n = 0;
    return r;
}
H(ci_update)
{
    cipher_ctx *c = fj_data(self);
    if (!c || !c->ready) { fj_throw(env, "java/lang/IllegalStateException", "Cipher not initialized"); return fj_l(NULL); }
    if (a[0].l) ci_feed(c, fj_array_data(a[0].l), (size_t)fj_array_len(a[0].l));
    return fj_l(ci_take(c));
}
H(ci_final)
{
    cipher_ctx *c = fj_data(self);
    if (!c || !c->ready) { fj_throw(env, "java/lang/IllegalStateException", "Cipher not initialized"); return fj_l(NULL); }
    if (ud && a[0].l) ci_feed(c, fj_array_data(a[0].l), (size_t)fj_array_len(a[0].l));
    if (c->mode != C_CTR) {
        if (c->encrypt && c->pad) {
            uint8_t padv = (uint8_t)(16 - c->n), b[16];
            memset(c->buf + c->n, padv, padv);
            memcpy(b, c->buf, 16); ci_block(c, b); out_put(c, b, 16);
        } else if (c->n == 16 && !c->encrypt && c->pad) {
            uint8_t b[16];
            memcpy(b, c->buf, 16); ci_block(c, b);
            uint8_t padv = b[15];
            int bad = padv < 1 || padv > 16;
            for (int i = 16 - padv; !bad && i < 16; i++) bad = b[i] != padv;
            if (bad) { c->n = 0; c->out_n = 0; fj_throw(env, "javax/crypto/BadPaddingException", "pad block corrupted"); return fj_l(NULL); }
            out_put(c, b, 16 - padv);
        } else if (c->n) {
            c->n = 0; c->out_n = 0;
            fj_throw(env, "javax/crypto/IllegalBlockSizeException", "the input is not a multiple of 16 bytes");
            return fj_l(NULL);
        }
    }
    c->n = 0;
    return fj_l(ci_take(c));
}
H(ci_iv)      { cipher_ctx *c = fj_data(self); return fj_l(c && c->mode != C_ECB ? bytes(c->iv, 16) : NULL); }
H(ci_block_size) { return fj_i(16); }

void up_services_register(void)
{
    /* Play Billing */
    CLS(BR, NULL);
    M(BR, "getResponseCode", "()I", br_code);
    M(BR, "getDebugMessage", "()Ljava/lang/String;", br_msg);
    SM(BR, "newBuilder", "()L" BR "$Builder;", br_new_builder);
    CLS(BR "$Builder", NULL);
    M(BR "$Builder", "setResponseCode", "(I)L" BR "$Builder;", brb_code);
    M(BR "$Builder", "setDebugMessage", "(Ljava/lang/String;)L" BR "$Builder;", brb_msg);
    M(BR "$Builder", "build", "()L" BR ";", brb_build);
    CLS(BC, NULL);
    CLS(BC "$Builder", NULL);
    SM(BC, "newBuilder", "(Landroid/content/Context;)L" BC "$Builder;", bc_new_builder);
    M(BC "$Builder", "setListener", "(Lcom/android/billingclient/api/PurchasesUpdatedListener;)L" BC "$Builder;", h_self);
    M(BC "$Builder", "enablePendingPurchases", "()L" BC "$Builder;", h_self);
    M(BC "$Builder", "enablePendingPurchases", "(Lcom/android/billingclient/api/PendingPurchasesParams;)L" BC "$Builder;", h_self);
    M(BC "$Builder", "enableAlternativeBillingOnly", "()L" BC "$Builder;", h_self);
    M(BC "$Builder", "enableUserChoiceBilling", "(Lcom/android/billingclient/api/UserChoiceBillingListener;)L" BC "$Builder;", h_self);
    M(BC "$Builder", "build", "()L" BC ";", bcb_build);
    CLS("com/android/billingclient/api/PendingPurchasesParams", NULL);
    CLS("com/android/billingclient/api/PendingPurchasesParams$Builder", NULL);
    SM("com/android/billingclient/api/PendingPurchasesParams", "newBuilder", "()Lcom/android/billingclient/api/PendingPurchasesParams$Builder;",
       h_null);   /* (a builder chain on null: the C# side does not look; see enablePendingPurchases above) */
    M(BC, "startConnection", "(Lcom/android/billingclient/api/BillingClientStateListener;)V", bc_start);
    M(BC, "endConnection", "()V", h_void);
    M(BC, "isReady", "()Z", h_false);
    M(BC, "getConnectionState", "()I", bc_state);
    M(BC, "isFeatureSupported", "(Ljava/lang/String;)L" BR ";", bc_result);
    M(BC, "launchBillingFlow", "(Landroid/app/Activity;Lcom/android/billingclient/api/BillingFlowParams;)L" BR ";", bc_result);
    static const struct bq pd = { "onProductDetailsResponse", "(L" BR ";Ljava/util/List;)V" },
                           sd = { "onSkuDetailsResponse", "(L" BR ";Ljava/util/List;)V" },
                           pr = { "onQueryPurchasesResponse", "(L" BR ";Ljava/util/List;)V" },
                           ph = { "onPurchaseHistoryResponse", "(L" BR ";Ljava/util/List;)V" };
    MU(BC, "queryProductDetailsAsync", "(Lcom/android/billingclient/api/QueryProductDetailsParams;Lcom/android/billingclient/api/ProductDetailsResponseListener;)V", bc_query, &pd);
    MU(BC, "querySkuDetailsAsync", "(Lcom/android/billingclient/api/SkuDetailsParams;Lcom/android/billingclient/api/SkuDetailsResponseListener;)V", bc_query, &sd);
    MU(BC, "queryPurchasesAsync", "(Lcom/android/billingclient/api/QueryPurchasesParams;Lcom/android/billingclient/api/PurchasesResponseListener;)V", bc_query, &pr);
    MU(BC, "queryPurchasesAsync", "(Ljava/lang/String;Lcom/android/billingclient/api/PurchasesResponseListener;)V", bc_query_type, &pr);
    MU(BC, "queryPurchaseHistoryAsync", "(Lcom/android/billingclient/api/QueryPurchaseHistoryParams;Lcom/android/billingclient/api/PurchaseHistoryResponseListener;)V", bc_query, &ph);
    MU(BC, "queryPurchaseHistoryAsync", "(Ljava/lang/String;Lcom/android/billingclient/api/PurchaseHistoryResponseListener;)V", bc_query_type, &ph);

    /* Play In-App Review (Play Core's Task, and the review library's own: com.google.android.gms.tasks) */
    CLS(RV "/ReviewManagerFactory", NULL);
    CLS(RV "/ReviewManager", NULL);
    SM(RV "/ReviewManagerFactory", "create", "(Landroid/content/Context;)L" RV "/ReviewManager;", rv_create);
    MU(RV "/ReviewManager", "requestReviewFlow", "()L" TASK ";", rv_flow, TASK);
    MU(RV "/ReviewManager", "launchReviewFlow", "(Landroid/app/Activity;L" RV "/ReviewInfo;)L" TASK ";", rv_flow, TASK);
    MU(RV "/ReviewManager", "requestReviewFlow", "()L" GTASK ";", rv_flow, GTASK);
    MU(RV "/ReviewManager", "launchReviewFlow", "(Landroid/app/Activity;L" RV "/ReviewInfo;)L" GTASK ";", rv_flow, GTASK);
    const char *tasks[2][2] = { { TASK, "com/google/android/play/core/tasks" }, { GTASK, "com/google/android/gms/tasks" } };
    static char ocs[2][200];            /* onComplete's signature for each Task class (the handlers' user data) */
    for (int i = 0; i < 2; i++) {
        const char *T = tasks[i][0], *P = tasks[i][1];
        char sig[256];
        CLS(T, NULL);
        M(T, "isComplete", "()Z", h_true);
        M(T, "isSuccessful", "()Z", h_false);
        M(T, "getResult", "()Ljava/lang/Object;", h_null);
        M(T, "getException", "()Ljava/lang/Exception;", task_ex);
        snprintf(ocs[i], sizeof ocs[i], "(L%s;)V", T);
        snprintf(sig, sizeof sig, "(L%s/OnCompleteListener;)L%s;", P, T);
        MU(T, "addOnCompleteListener", sig, task_complete, ocs[i]);
        snprintf(sig, sizeof sig, "(Landroid/app/Activity;L%s/OnCompleteListener;)L%s;", P, T);
        MU(T, "addOnCompleteListener", sig, task_complete2, ocs[i]);
        snprintf(sig, sizeof sig, "(L%s/OnFailureListener;)L%s;", P, T);
        M(T, "addOnFailureListener", sig, task_failure);
        snprintf(sig, sizeof sig, "(Landroid/app/Activity;L%s/OnFailureListener;)L%s;", P, T);
        M(T, "addOnFailureListener", sig, task_failure2);
        snprintf(sig, sizeof sig, "(L%s/OnSuccessListener;)L%s;", P, T);
        M(T, "addOnSuccessListener", sig, h_self);
        snprintf(sig, sizeof sig, "(Landroid/app/Activity;L%s/OnSuccessListener;)L%s;", P, T);
        M(T, "addOnSuccessListener", sig, h_self);
    }

    /* Play Games v2 */
    CLS(PG "/PlayGamesSdk", NULL);
    SM(PG "/PlayGamesSdk", "initialize", "(Landroid/content/Context;)V", h_void);
    CLS(PG "/PlayGames", NULL);
    CLS(PG "/GamesSignInClient", NULL);
    SM(PG "/PlayGames", "getGamesSignInClient", "(Landroid/app/Activity;)L" PG "/GamesSignInClient;", pg_client);
    MU(PG "/GamesSignInClient", "isAuthenticated", "()L" GTASK ";", rv_flow, GTASK);
    MU(PG "/GamesSignInClient", "signIn", "()L" GTASK ";", rv_flow, GTASK);
    MU(PG "/GamesSignInClient", "requestServerSideAccess", "(Ljava/lang/String;Z)L" GTASK ";", rv_flow, GTASK);

    /* Mobile Notifications (the package's Java side, 2.x) */
    CLS(UNM, NULL);
    CLS("com/unity/androidnotifications/NotificationCallback", NULL);
    CLS("com/unity/androidnotifications/NotificationChannelWrapper", NULL);
    SM(UNM, "getNotificationManagerImpl", "(Landroid/app/Activity;Lcom/unity/androidnotifications/NotificationCallback;)L" UNM ";", unm_get);
    SM(UNM, "getNotificationManagerImpl", "(Landroid/content/Context;)L" UNM ";", unm_get);
    M(UNM, "getTargetSdk", "()I", unm_target);
    M(UNM, "areNotificationsEnabled", "()I", unm_enabled);
    M(UNM, "getNotificationChannels", "()[Lcom/unity/androidnotifications/NotificationChannelWrapper;", unm_channels);
    M(UNM, "getNotificationChannel", "(Ljava/lang/String;)Lcom/unity/androidnotifications/NotificationChannelWrapper;", h_null);
    M(UNM, "canScheduleExactAlarms", "()Z", h_false);
    M(UNM, "checkIfPendingNotificationIsRegistered", "(I)Z", h_false);
    M(UNM, "checkNotificationStatus", "(I)I", unm_int0);      /* 0: neither scheduled nor shown */
    M(UNM, "scheduleNotification", "(L" NB ";Z)I", unm_int0);
    M(UNM, "createNotificationBuilder", "(Ljava/lang/String;)L" NB ";", unm_builder);
    M(UNM, "registerNotificationChannel", "(Ljava/lang/String;Ljava/lang/String;ILjava/lang/String;ZZZZ[JILjava/lang/String;)V", h_void);
    M(UNM, "registerNotificationChannelGroup", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", h_void);
    M(UNM, "deleteNotificationChannel", "(Ljava/lang/String;)V", h_void);
    M(UNM, "deleteNotificationChannelGroup", "(Ljava/lang/String;)V", h_void);
    M(UNM, "cancelPendingNotification", "(I)V", h_void);
    M(UNM, "cancelDisplayedNotification", "(I)V", h_void);
    M(UNM, "cancelAllPendingNotificationIntents", "()V", h_void);
    M(UNM, "cancelAllNotifications", "()V", h_void);
    M(UNM, "showNotificationSettings", "(Ljava/lang/String;)V", h_void);
    M(UNM, "getNotificationFromIntent", "(Landroid/content/Intent;)Landroid/app/Notification;", h_null);
    CLS(NB, NULL);
    M(NB, "getExtras", "()Landroid/os/Bundle;", nb_extras);
    static const char *const setters[][2] = {
        { "setContentTitle", "(Ljava/lang/CharSequence;)" }, { "setContentText", "(Ljava/lang/CharSequence;)" },
        { "setAutoCancel", "(Z)" }, { "setNumber", "(I)" }, { "setStyle", "(Landroid/app/Notification$Style;)" },
        { "setWhen", "(J)" }, { "setGroup", "(Ljava/lang/String;)" }, { "setGroupSummary", "(Z)" },
        { "setSortKey", "(Ljava/lang/String;)" }, { "setShowWhen", "(Z)" } };
    static char nb_sigs[sizeof setters / sizeof setters[0]][128];
    for (size_t i = 0; i < sizeof setters / sizeof setters[0]; i++) {
        snprintf(nb_sigs[i], sizeof nb_sigs[i], "%sL" NB ";", setters[i][1]);
        M(NB, setters[i][0], nb_sigs[i], h_self);
    }

    /* java.security and javax.crypto */
    CLS("java/security/MessageDigest", NULL);
    SM("java/security/MessageDigest", "getInstance", "(Ljava/lang/String;)Ljava/security/MessageDigest;", md_get);
    M("java/security/MessageDigest", "update", "([B)V", md_update);
    M("java/security/MessageDigest", "update", "([BII)V", md_update_part);
    M("java/security/MessageDigest", "update", "(B)V", md_update_b);
    M("java/security/MessageDigest", "digest", "()[B", md_digest);
    MU("java/security/MessageDigest", "digest", "([B)[B", md_digest, 1);
    M("java/security/MessageDigest", "reset", "()V", md_reset);
    M("java/security/MessageDigest", "getDigestLength", "()I", md_length);
    CLS("java/security/SecureRandom", NULL);
    M("java/security/SecureRandom", "<init>", "()V", h_void);
    M("java/security/SecureRandom", "<init>", "([B)V", h_void);
    SM("java/security/SecureRandom", "getInstance", "(Ljava/lang/String;)Ljava/security/SecureRandom;", sr_get);
    SM("java/security/SecureRandom", "getInstanceStrong", "()Ljava/security/SecureRandom;", sr_get);
    M("java/security/SecureRandom", "nextBytes", "([B)V", sr_next_bytes);
    M("java/security/SecureRandom", "nextInt", "()I", sr_next_int);
    MU("java/security/SecureRandom", "nextInt", "(I)I", sr_next_int, 1);
    M("java/security/SecureRandom", "nextLong", "()J", sr_next_long);
    M("java/security/SecureRandom", "setSeed", "([B)V", h_void);
    M("java/security/SecureRandom", "generateSeed", "(I)[B", sr_seed);
    CLS("java/security/Key", NULL);
    CLS("java/security/spec/AlgorithmParameterSpec", NULL);
    CLS("javax/crypto/spec/SecretKeySpec", NULL);
    fj_class_implements("javax/crypto/spec/SecretKeySpec", "java/security/Key");
    M("javax/crypto/spec/SecretKeySpec", "<init>", "([BLjava/lang/String;)V", key_init);
    M("javax/crypto/spec/SecretKeySpec", "getEncoded", "()[B", key_encoded);
    M("javax/crypto/spec/SecretKeySpec", "getAlgorithm", "()Ljava/lang/String;", key_alg);
    CLS("javax/crypto/spec/IvParameterSpec", NULL);
    fj_class_implements("javax/crypto/spec/IvParameterSpec", "java/security/spec/AlgorithmParameterSpec");
    MU("javax/crypto/spec/IvParameterSpec", "<init>", "([B)V", key_init, 1);
    M("javax/crypto/spec/IvParameterSpec", "getIV", "()[B", key_encoded);
    CLS("javax/crypto/Mac", NULL);
    SM("javax/crypto/Mac", "getInstance", "(Ljava/lang/String;)Ljavax/crypto/Mac;", mac_get);
    M("javax/crypto/Mac", "init", "(Ljava/security/Key;)V", mac_init);
    M("javax/crypto/Mac", "update", "([B)V", mac_update);
    M("javax/crypto/Mac", "doFinal", "()[B", mac_final);
    MU("javax/crypto/Mac", "doFinal", "([B)[B", mac_final, 1);
    M("javax/crypto/Mac", "getMacLength", "()I", mac_length);
    CLS("javax/crypto/Cipher", NULL);
    fj_static_int("javax/crypto/Cipher", "ENCRYPT_MODE", 1);
    fj_static_int("javax/crypto/Cipher", "DECRYPT_MODE", 2);
    SM("javax/crypto/Cipher", "getInstance", "(Ljava/lang/String;)Ljavax/crypto/Cipher;", ci_get);
    M("javax/crypto/Cipher", "init", "(ILjava/security/Key;)V", ci_init);
    MU("javax/crypto/Cipher", "init", "(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V", ci_init, 1);
    M("javax/crypto/Cipher", "update", "([B)[B", ci_update);
    M("javax/crypto/Cipher", "doFinal", "()[B", ci_final);
    MU("javax/crypto/Cipher", "doFinal", "([B)[B", ci_final, 1);
    M("javax/crypto/Cipher", "getIV", "()[B", ci_iv);
    M("javax/crypto/Cipher", "getBlockSize", "()I", ci_block_size);
    CLS("java/security/NoSuchAlgorithmException", "java/lang/Exception");
    CLS("java/security/InvalidKeyException", "java/lang/Exception");
    CLS("javax/crypto/BadPaddingException", "java/lang/Exception");
    CLS("javax/crypto/IllegalBlockSizeException", "java/lang/Exception");
}
