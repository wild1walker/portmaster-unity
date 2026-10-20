/*
 * up_android.c - the Android framework objects that libunity (and FMOD inside it) reach through
 * JNI, implemented as fakejni classes. Member list and signatures follow
 * spec/jni_bindings_libunity.tsv (every class/member libunity binds) plus the Java side of
 * classes.jar (UnityPlayer, NativeLoader, ReflectionHelper, FMODAudioDevice).
 * Input classes, Choreographer and FMOD audio are in up_input.c.
 */
#define _GNU_SOURCE
#include "up_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <zlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* ---- generic handlers ------------------------------------------------------------------- */
H(h_void)  { return fj_v(); }
H(h_null)  { return fj_l(NULL); }
H(h_true)  { return fj_z(JNI_TRUE); }
H(h_false) { return fj_z(JNI_FALSE); }
H(h_int)   { return fj_i((jint)(intptr_t)ud); }
H(h_self)  { return fj_l(self); }
H(h_obj)   { return fj_l(*(jobject *)ud); }               /* ud = &g_xxx */
H(h_str)   { return fj_l(fj_string((const char *)ud)); }
H(h_strp)  { return fj_l(fj_string(*(const char **)ud)); } /* ud = &UPC.xxx */

#define M(c, n, s, f)       fj_method_define(c, n, s, 0, f, NULL)
#define MU(c, n, s, f, u)   fj_method_define(c, n, s, 0, f, (void *)(u))
#define SM(c, n, s, f)      fj_method_define(c, n, s, 1, f, NULL)
#define SMU(c, n, s, f, u)  fj_method_define(c, n, s, 1, f, (void *)(u))
#define CLS(c, s)           fj_class_define(c, s)

static jobject g_window, g_decor, g_layout_params, g_wm, g_display, g_resources, g_configuration,
               g_locale, g_pm, g_appinfo, g_pkginfo, g_assets, g_intent, g_vibrator, g_audio,
               g_conn, g_content_resolver, g_class_loader, g_display_mgr, g_meta_bundle,
               g_runtime, g_activity_mgr, g_mime, g_empty_bundle, g_input_mgr_obj,
               g_media_router, g_media_route, g_display_mode, g_power;
jobject up_g_input_mgr;   /* shared with up_input.c */

static int g_sysui_vis;
static int g_requested_orientation = 0;   /* SCREEN_ORIENTATION_LANDSCAPE */

static const char *jstr(jvalue v) { const char *s = fj_cstr(v.l); return s ? s : ""; }

/* ---- files ---------------------------------------------------------------------------- */
void up_mkdirs(const char *path)
{
    if (!path || !*path) return;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(buf, 0755); *p = '/'; }
    mkdir(buf, 0755);
}

static const char *fpath(jobject f) { const char *p = fj_data(f); return p ? p : ""; }

jobject up_make_file(const char *path)
{
    return path ? fj_new_by_name("java/io/File", strdup(path), free) : NULL;
}

H(f_init_s)  { fj_set_data(self, strdup(jstr(a[0])), free); return fj_v(); }
H(f_init_ss)
{
    char *p;
    if (asprintf(&p, "%s/%s", ud ? fpath(a[0].l) : jstr(a[0]), jstr(a[1])) < 0) p = strdup("");
    fj_set_data(self, p, free);
    return fj_v();
}
H(f_path)    { return fj_l(fj_string(fpath(self))); }
H(f_name)    { const char *p = fpath(self), *s = strrchr(p, '/'); return fj_l(fj_string(s ? s + 1 : p)); }
H(f_parent)
{
    char *d = strdup(fpath(self));
    char *s = strrchr(d, '/');
    jvalue r = fj_l(NULL);
    if (s) { if (s == d) s[1] = 0; else *s = 0; r = ud ? fj_l(up_make_file(d)) : fj_l(fj_string(d)); }
    free(d);
    return r;
}
H(f_exists)  { struct stat st; return fj_z(stat(fpath(self), &st) == 0); }
H(f_isdir)   { struct stat st; return fj_z(stat(fpath(self), &st) == 0 && S_ISDIR(st.st_mode)); }
H(f_isfile)  { struct stat st; return fj_z(stat(fpath(self), &st) == 0 && S_ISREG(st.st_mode)); }
H(f_length)  { struct stat st; return fj_j(stat(fpath(self), &st) == 0 ? (jlong)st.st_size : 0); }
H(f_mkdir)   { return fj_z(mkdir(fpath(self), 0755) == 0); }
H(f_mkdirs)  { up_mkdirs(fpath(self)); struct stat st; return fj_z(stat(fpath(self), &st) == 0); }
H(f_delete)  { return fj_z(remove(fpath(self)) == 0); }
H(f_space)
{
    struct statvfs v;
    if (statvfs(fpath(self), &v) != 0) return fj_j(0);
    return fj_j((jlong)(ud ? v.f_blocks : v.f_bavail) * v.f_frsize);
}

/* ---- Bundle (string-valued key/value store) --------------------------------------------- */
typedef struct kv { struct kv *next; char *k, *v; } kv;
typedef struct { kv *head; pthread_mutex_t mu; } bundle;

static void bundle_free(void *p)
{
    bundle *b = p;
    for (kv *e = b->head, *n; e; e = n) { n = e->next; free(e->k); free(e->v); free(e); }
    free(b);
}
static jobject bundle_new(void)
{
    bundle *b = calloc(1, sizeof *b);
    pthread_mutex_init(&b->mu, NULL);
    return fj_new_by_name("android/os/Bundle", b, bundle_free);
}
static void bundle_put(jobject o, const char *k, const char *v)
{
    bundle *b = fj_data(o);
    if (!b || !k) return;
    pthread_mutex_lock(&b->mu);
    kv *e;
    for (e = b->head; e; e = e->next) if (!strcmp(e->k, k)) break;
    if (!e) { e = calloc(1, sizeof *e); e->k = strdup(k); e->next = b->head; b->head = e; }
    free(e->v);
    e->v = v ? strdup(v) : NULL;
    pthread_mutex_unlock(&b->mu);
}
/* returns 1 if present; *out points into the bundle (valid while the entry is unchanged) */
static int bundle_get(jobject o, const char *k, const char **out)
{
    bundle *b = fj_data(o);
    if (!b || !k) return 0;
    int found = 0;
    pthread_mutex_lock(&b->mu);
    for (kv *e = b->head; e; e = e->next) if (!strcmp(e->k, k)) { *out = e->v; found = 1; break; }
    pthread_mutex_unlock(&b->mu);
    return found;
}
static int truthy(const char *v) { return v && (!strcasecmp(v, "true") || atoi(v) != 0); }

H(b_init)        { bundle *b = calloc(1, sizeof *b); pthread_mutex_init(&b->mu, NULL); fj_set_data(self, b, bundle_free); return fj_v(); }
H(b_contains)    { const char *v; return fj_z(bundle_get(self, jstr(a[0]), &v)); }
H(b_getString)   { const char *v; if (bundle_get(self, jstr(a[0]), &v) && v) return fj_l(fj_string(v)); return fj_l(ud ? a[1].l : NULL); }
H(b_getBoolean)  { const char *v; if (bundle_get(self, jstr(a[0]), &v)) return fj_z(truthy(v)); return fj_z(ud ? a[1].z : 0); }
H(b_getInt)      { const char *v; if (bundle_get(self, jstr(a[0]), &v) && v) return fj_i(atoi(v)); return fj_i(ud ? a[1].i : 0); }
H(b_getFloat)    { const char *v; if (bundle_get(self, jstr(a[0]), &v) && v) return fj_f(strtof(v, NULL)); return fj_f(ud ? a[1].f : 0); }
H(b_putString)   { bundle_put(self, jstr(a[0]), fj_cstr(a[1].l)); return fj_v(); }
H(b_putInt)      { char t[32]; snprintf(t, sizeof t, "%d", a[1].i); bundle_put(self, jstr(a[0]), t); return fj_v(); }
H(b_putBoolean)  { bundle_put(self, jstr(a[0]), a[1].z ? "true" : "false"); return fj_v(); }
H(b_isEmpty)     { bundle *b = fj_data(self); return fj_z(!b || !b->head); }

/* ---- Intent ---------------------------------------------------------------------------- */
static jobject intent_new(const char *action)
{
    jobject i = fj_new_by_name("android/content/Intent", NULL, NULL);
    if (action) fj_set_obj(i, "mAction", "Ljava/lang/String;", fj_string(action));
    fj_set_obj(i, "mExtras", "Landroid/os/Bundle;", bundle_new());
    return i;
}
static jobject intent_extras(jobject i) { return fj_get_field(i, "mExtras", "Landroid/os/Bundle;").l; }

H(i_init)       { if (ud) fj_set_obj(self, "mAction", "Ljava/lang/String;", a[0].l);
                  fj_set_obj(self, "mExtras", "Landroid/os/Bundle;", bundle_new()); return fj_v(); }
H(i_getAction)  { return fj_get_field(self, "mAction", "Ljava/lang/String;"); }
H(i_getExtras)  { return fj_l(intent_extras(self)); }
H(i_getStringExtra) { const char *v; if (bundle_get(intent_extras(self), jstr(a[0]), &v) && v) return fj_l(fj_string(v)); return fj_l(NULL); }
H(i_getIntExtra)    { const char *v; if (bundle_get(intent_extras(self), jstr(a[0]), &v) && v) return fj_i(atoi(v)); return fj_i(a[1].i); }
H(i_getBoolExtra)   { const char *v; if (bundle_get(intent_extras(self), jstr(a[0]), &v)) return fj_z(truthy(v)); return fj_z(a[1].z); }
H(i_hasExtra)       { const char *v; return fj_z(bundle_get(intent_extras(self), jstr(a[0]), &v)); }
H(i_setData)        { fj_set_obj(self, "mData", "Landroid/net/Uri;", a[0].l); return fj_l(self); }
H(i_getData)        { return fj_get_field(self, "mData", "Landroid/net/Uri;"); }

/* ---- SharedPreferences (persisted in <data_dir>/shared_prefs/<name>.prefs) ------------- */
typedef struct pref { struct pref *next; char *key; char type; char *s; long long i; float f; } pref;
typedef struct prefs { struct prefs *next; char *name, *path; pref *head; pthread_mutex_t mu; } prefs;
static prefs *g_prefs;
static pthread_mutex_t g_prefs_mu = PTHREAD_MUTEX_INITIALIZER;

static void esc_write(FILE *f, const char *s)
{
    for (; *s; s++) {
        unsigned char c = *s;
        if (c == '%' || c == '\t' || c == '\n' || c == '\r') fprintf(f, "%%%02X", c);
        else fputc(c, f);
    }
}
static char *unesc(const char *s)
{
    char *r = malloc(strlen(s) + 1), *o = r;
    while (*s) {
        if (s[0] == '%' && s[1] && s[2]) { char h[3] = { s[1], s[2], 0 }; *o++ = (char)strtol(h, NULL, 16); s += 3; }
        else *o++ = *s++;
    }
    *o = 0;
    return r;
}
static pref *pref_find(prefs *p, const char *k)
{
    for (pref *e = p->head; e; e = e->next) if (!strcmp(e->key, k)) return e;
    return NULL;
}
static pref *pref_set(prefs *p, const char *k, char type)
{
    pref *e = pref_find(p, k);
    if (!e) { e = calloc(1, sizeof *e); e->key = strdup(k); e->next = p->head; p->head = e; }
    free(e->s); e->s = NULL;
    e->type = type;
    return e;
}
static void prefs_save(prefs *p)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", p->path);
    FILE *f = fopen(tmp, "w");
    if (!f) { up_log("cannot write %s: %s", tmp, strerror(errno)); return; }
    for (pref *e = p->head; e; e = e->next) {
        fprintf(f, "%c\t", e->type);
        esc_write(f, e->key);
        fputc('\t', f);
        if (e->type == 's') esc_write(f, e->s ? e->s : "");
        else if (e->type == 'f') fprintf(f, "%.9g", e->f);
        else fprintf(f, "%lld", e->i);
        fputc('\n', f);
    }
    fclose(f);
    rename(tmp, p->path);
}
static void prefs_load(prefs *p)
{
    FILE *f = fopen(p->path, "r");
    if (!f) return;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) > 0) {
        line[strcspn(line, "\n")] = 0;
        char *t1 = strchr(line, '\t');
        if (!t1 || t1 != line + 1) continue;
        char *t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        *t2 = 0;
        char *k = unesc(t1 + 1);
        pref *e = pref_set(p, k, line[0]);
        if (e->type == 's') e->s = unesc(t2 + 1);
        else if (e->type == 'f') e->f = strtof(t2 + 1, NULL);
        else e->i = strtoll(t2 + 1, NULL, 10);
        free(k);
    }
    free(line);
    fclose(f);
}
static prefs *prefs_get(const char *name)
{
    pthread_mutex_lock(&g_prefs_mu);
    prefs *p;
    for (p = g_prefs; p; p = p->next) if (!strcmp(p->name, name)) break;
    if (!p) {
        p = calloc(1, sizeof *p);
        pthread_mutex_init(&p->mu, NULL);
        p->name = strdup(name);
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/shared_prefs", UPC.data_dir);
        up_mkdirs(dir);
        if (asprintf(&p->path, "%s/%s.prefs", dir, name) < 0) abort();
        prefs_load(p);
        p->next = g_prefs;
        g_prefs = p;
        up_log("SharedPreferences %s -> %s", name, p->path);
    }
    pthread_mutex_unlock(&g_prefs_mu);
    return p;
}

#define PREFS(o) ((prefs *)fj_data(o))
H(ctx_getSharedPrefs) { return fj_l(fj_new_by_name("android/content/SharedPreferences", prefs_get(jstr(a[0])), NULL)); }
H(act_getPreferences) { char n[300]; snprintf(n, sizeof n, "%s.UnityPlayerActivity", UPC.package_name);
                        return fj_l(fj_new_by_name("android/content/SharedPreferences", prefs_get(n), NULL)); }
H(sp_contains) { prefs *p = PREFS(self); pthread_mutex_lock(&p->mu); int r = pref_find(p, jstr(a[0])) != NULL; pthread_mutex_unlock(&p->mu); return fj_z(r); }
H(sp_edit)     { return fj_l(fj_new_by_name("android/content/SharedPreferences$Editor", fj_data(self), NULL)); }
H(sp_getString)
{
    prefs *p = PREFS(self);
    jvalue r = fj_l(a[1].l);
    pthread_mutex_lock(&p->mu);
    pref *e = pref_find(p, jstr(a[0]));
    if (e && e->type == 's') r = fj_l(fj_string(e->s ? e->s : ""));
    else if (e) fj_throw(env, "java/lang/ClassCastException", "not a String");
    pthread_mutex_unlock(&p->mu);
    return r;
}
H(sp_getNum)   /* ud: 'i' int, 'b' boolean, 'f' float, 'l' long */
{
    prefs *p = PREFS(self);
    char t = (char)(intptr_t)ud;
    jvalue r = a[1];
    pthread_mutex_lock(&p->mu);
    pref *e = pref_find(p, jstr(a[0]));
    if (e && e->type == t) {
        if (t == 'f') r = fj_f(e->f);
        else if (t == 'b') r = fj_z(e->i != 0);
        else if (t == 'l') r = fj_j(e->i);
        else r = fj_i((jint)e->i);
    } else if (e) fj_throw(env, "java/lang/ClassCastException", "wrong preference type");
    pthread_mutex_unlock(&p->mu);
    return r;
}
H(sp_getAll)   { return fj_l(fj_new_by_name("java/util/HashMap", NULL, NULL)); }   /* empty map */
H(ed_put)      /* ud: type */
{
    prefs *p = PREFS(self);
    char t = (char)(intptr_t)ud;
    pthread_mutex_lock(&p->mu);
    pref *e = pref_set(p, jstr(a[0]), t);
    if (t == 's') e->s = strdup(jstr(a[1]));
    else if (t == 'f') e->f = a[1].f;
    else if (t == 'b') e->i = a[1].z;
    else if (t == 'l') e->i = a[1].j;
    else e->i = a[1].i;
    pthread_mutex_unlock(&p->mu);
    return fj_l(self);
}
H(ed_remove)
{
    prefs *p = PREFS(self);
    pthread_mutex_lock(&p->mu);
    for (pref **pp = &p->head; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->key, jstr(a[0]))) { pref *e = *pp; *pp = e->next; free(e->key); free(e->s); free(e); break; }
    pthread_mutex_unlock(&p->mu);
    return fj_l(self);
}
H(ed_clear)
{
    prefs *p = PREFS(self);
    pthread_mutex_lock(&p->mu);
    for (pref *e = p->head, *n; e; e = n) { n = e->next; free(e->key); free(e->s); free(e); }
    p->head = NULL;
    pthread_mutex_unlock(&p->mu);
    return fj_l(self);
}
H(ed_commit)   { prefs *p = PREFS(self); pthread_mutex_lock(&p->mu); prefs_save(p); pthread_mutex_unlock(&p->mu); return fj_z(JNI_TRUE); }

/* ---- Context / Activity ------------------------------------------------------------------ */
H(ctx_getFilesDir)       { return fj_l(up_make_file(UPC.files_dir)); }
H(ctx_getCacheDir)       { return fj_l(up_make_file(UPC.cache_dir)); }
H(ctx_getExtFilesDir)
{
    const char *sub = fj_cstr(a[0].l);
    if (!sub) return fj_l(up_make_file(UPC.external_files_dir));
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", UPC.external_files_dir, sub);
    up_mkdirs(p);
    return fj_l(up_make_file(p));
}
H(ctx_getExtCacheDir)    { return fj_l(up_make_file(UPC.external_cache_dir)); }
H(ctx_getObbDir)         { return fj_l(up_make_file(UPC.obb_dir)); }
H(ctx_getFileArray)      /* ud: const char **path field */
{
    jarray arr = fj_array_new("Ljava/io/File;", 1);
    JNIEnv *e = fj_env();
    (*e)->SetObjectArrayElement(e, arr, 0, up_make_file(*(const char **)ud));
    return fj_l(arr);
}
H(ctx_getSystemService)
{
    const char *n = jstr(a[0]);
    jobject r = NULL;
    if (!strcmp(n, "window")) r = g_wm;
    else if (!strcmp(n, "audio")) r = g_audio;
    else if (!strcmp(n, "vibrator")) r = g_vibrator;
    else if (!strcmp(n, "connectivity")) r = g_conn;
    else if (!strcmp(n, "display")) r = g_display_mgr;
    else if (!strcmp(n, "input")) r = up_g_input_mgr;
    else if (!strcmp(n, "activity")) r = g_activity_mgr;
    else if (!strcmp(n, "media_router")) r = g_media_router;
    else if (!strcmp(n, "power")) r = g_power;
    else up_log("getSystemService(\"%s\") -> null", n);
    if (fj_tracing()) up_log("getSystemService(\"%s\") -> %p", n, r);
    return fj_l(r);
}
H(ctx_checkPermission)   { if (fj_tracing()) up_log("permission %s granted", jstr(a[0])); return fj_i(0); }
H(ctx_registerReceiver)
{
    /* the only receiver Unity registers is ACTION_BATTERY_CHANGED: return the sticky intent */
    jobject i = intent_new("android.intent.action.BATTERY_CHANGED");
    jobject ex = intent_extras(i);
    bundle_put(ex, "level", "100");
    bundle_put(ex, "scale", "100");
    bundle_put(ex, "status", "5");       /* BATTERY_STATUS_FULL */
    bundle_put(ex, "plugged", "0");
    bundle_put(ex, "health", "2");
    bundle_put(ex, "present", "true");
    bundle_put(ex, "temperature", "250");
    bundle_put(ex, "voltage", "4200");
    return fj_l(i);
}
H(ctx_startActivity)
{
    jobject uri = fj_get_field(a[0].l, "mData", "Landroid/net/Uri;").l;
    const char *s = uri ? fj_cstr(fj_get_field(uri, "s", "Ljava/lang/String;").l) : NULL;
    up_log("startActivity(%s)", s ? s : "?");
    if (s && UPC.on_open_url) UPC.on_open_url(UPC.user, s);
    return fj_v();
}
H(act_runOnUiThread)
{
    if (!a[0].l) return fj_v();
    looper_post(&L_ui, NULL, NULL, a[0].l, 0);
    return fj_v();
}
H(act_getReqOrient)  { return fj_i(g_requested_orientation); }
H(act_setReqOrient)  { g_requested_orientation = a[0].i; up_log("setRequestedOrientation(%d)", a[0].i); return fj_v(); }
H(act_isFinishing)   { return fj_z(up_is_finishing()); }
H(act_finish)        { up_request_finish("Activity.finish()"); return fj_v(); }

/* PackageManager */
H(pm_getAppInfo)     { return fj_l(g_appinfo); }
H(pm_getPkgInfo)     { return fj_l(g_pkginfo); }
H(pm_hasFeature)
{
    const char *f = jstr(a[0]);
    /* audio.low_latency: FMOD then outputs through OpenSL ES (../android: SDL2) at the device
     * rate, as on current phones; without it, Java AudioTrack at 24 kHz */
    int r = !strcmp(f, "android.hardware.gamepad") || !strcmp(f, "android.hardware.opengles.aep") ||
            !strcmp(f, "android.hardware.audio.low_latency");
    if (fj_tracing()) up_log("hasSystemFeature(%s) -> %d", f, r);
    return fj_z(r);
}

/* Settings */
H(set_secure_getString)
{
    const char *k = jstr(a[1]);
    if (!strcmp(k, "android_id")) return fj_l(fj_string(UPC.android_id));
    return fj_l(NULL);
}
H(set_system_getInt) { return fj_i(a[2].i); }

/* Display */
H(disp_metrics)
{
    jobject dm = a[0].l;
    float density = UPC.density_dpi / 160.0f;
    fj_set_int(dm, "widthPixels", UPC.width);
    fj_set_int(dm, "heightPixels", UPC.height);
    fj_set_int(dm, "densityDpi", UPC.density_dpi);
    fj_set_float(dm, "density", density);
    fj_set_float(dm, "scaledDensity", density);
    fj_set_float(dm, "xdpi", (float)UPC.density_dpi);
    fj_set_float(dm, "ydpi", (float)UPC.density_dpi);
    return fj_v();
}
H(disp_width)   { return fj_i(UPC.width); }
H(disp_height)  { return fj_i(UPC.height); }
H(disp_refresh) { return fj_f(UPC.refresh_rate); }
H(dm_getDisplay){ return fj_l(a[0].i == 0 ? g_display : NULL); }
H(res_getDM)
{
    jobject dm = fj_new_by_name("android/util/DisplayMetrics", NULL, NULL);
    jvalue x = fj_l(dm);
    disp_metrics(env, NULL, &x, NULL);
    return fj_l(dm);
}

/* View / Window */
H(view_getSysUi)  { return fj_i(g_sysui_vis); }
H(view_setSysUi)  { g_sysui_vis = a[0].i; return fj_v(); }
H(view_width)     { return fj_i(UPC.width); }
H(view_height)    { return fj_i(UPC.height); }

/* AudioManager / AudioSystem (FMOD) */
H(am_getProperty)
{
    const char *k = jstr(a[0]);
    char b[32];
    if (strstr(k, "FRAMES_PER_BUFFER")) snprintf(b, sizeof b, "%d", UPC.audio_frames_per_buffer);
    else if (strstr(k, "SAMPLE_RATE")) snprintf(b, sizeof b, "%d", UPC.audio_sample_rate);
    else return fj_l(NULL);
    return fj_l(fj_string(b));
}
H(as_rate)    { return fj_i(UPC.audio_sample_rate); }
H(ab_build)   { return fj_l(fj_new_by_name("android/media/AudioAttributes", NULL, NULL)); }
H(afr_build)  { return fj_l(fj_new_by_name("android/media/AudioFocusRequest", NULL, NULL)); }
H(as_frames)  { return fj_i(UPC.audio_frames_per_buffer); }

/* Vibrator */
H(vib_has)    { return fj_z(UPC.on_vibrate != NULL); }
H(vib_vibrate){ if (UPC.on_vibrate) UPC.on_vibrate(UPC.user, a[0].j); return fj_v(); }
H(vib_cancel) { if (UPC.on_vibrate) UPC.on_vibrate(UPC.user, 0); return fj_v(); }

/* Locale */
H(loc_lang)    { return fj_l(fj_string(UPC.language)); }
H(loc_country) { return fj_l(fj_string(UPC.country)); }
H(loc_str)     { char b[64]; snprintf(b, sizeof b, "%s_%s", UPC.language, UPC.country); return fj_l(fj_string(b)); }

/* Environment */
H(env_extDir)  { return fj_l(up_make_file(UPC.external_storage)); }

/* Uri */
H(uri_parse)   { jobject u = fj_new_by_name("android/net/Uri", NULL, NULL); fj_set_obj(u, "s", "Ljava/lang/String;", a[0].l); return fj_l(u); }
H(uri_fromFile){ jobject u = fj_new_by_name("android/net/Uri", NULL, NULL); char *p;
                 if (asprintf(&p, "file://%s", fpath(a[0].l)) < 0) return fj_l(NULL);
                 fj_set_obj(u, "s", "Ljava/lang/String;", fj_string(p)); free(p); return fj_l(u); }
H(uri_toString){ return fj_get_field(self, "s", "Ljava/lang/String;"); }
H(uri_isRelative) { const char *s = fj_cstr(fj_get_field(self, "s", "Ljava/lang/String;").l); return fj_z(!s || !strstr(s, ":")); }
H(uri_identity) { return fj_l(a[0].l); }   /* Uri.encode/decode: pass-through */

/* MimeTypeMap */
H(mime_ext)
{
    const char *u = jstr(a[0]);
    const char *d = strrchr(u, '.'), *sl = strrchr(u, '/');
    if (!d || (sl && sl > d)) return fj_l(fj_string(""));
    char b[32];
    snprintf(b, sizeof b, "%s", d + 1);
    b[strcspn(b, "?#")] = 0;
    return fj_l(fj_string(b));
}
H(mime_type)
{
    static const char *m[][2] = { { "png", "image/png" }, { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" },
                                  { "txt", "text/plain" }, { "json", "application/json" }, { "html", "text/html" },
                                  { "ogg", "audio/ogg" }, { "mp3", "audio/mpeg" }, { "wav", "audio/x-wav" },
                                  { "mp4", "video/mp4" } };
    const char *e = jstr(a[0]);
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) if (!strcasecmp(e, m[i][0])) return fj_l(fj_string(m[i][1]));
    return fj_l(NULL);
}

/* AssetManager.open + InputStream + Scanner (read a whole asset as text). Like Android, assets
 * are read from the APK (a zip): entry "assets/<name>", stored or deflated. UP_ASSETS_DIR=<dir>
 * reads loose files from a directory instead (testing). */
typedef struct { char *buf; size_t len, pos; } stream;
static void stream_free(void *p) { stream *s = p; if (s) { free(s->buf); free(s); } }

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return p[0] | p[1] << 8; }

/* entry <want> of the zip at <path>: its whole data, NUL-terminated, in *out (out NULL: only
 * whether it is there); 0 on success, -1 if missing or unreadable */
static int zip_entry(const char *path, const char *want, char **out, size_t *outlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int rc = -1;
    unsigned char *cd = NULL, *comp = NULL;
    size_t wl = strlen(want);
    /* end of central directory: in the last 64 KiB + 22 bytes */
    if (fseeko(f, 0, SEEK_END) != 0) goto done;
    off_t size = ftello(f);
    off_t tail = size < 65557 ? size : 65557;
    unsigned char *t = malloc(tail);
    if (!t || fseeko(f, size - tail, SEEK_SET) != 0 || fread(t, 1, tail, f) != (size_t)tail) { free(t); goto done; }
    off_t eocd = -1;
    for (off_t i = tail - 22; i >= 0; i--) if (le32(t + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(t); goto done; }
    uint32_t cdsize = le32(t + eocd + 12), cdoff = le32(t + eocd + 16);
    uint16_t entries = le16(t + eocd + 10);
    free(t);
    cd = malloc(cdsize);
    if (!cd || fseeko(f, cdoff, SEEK_SET) != 0 || fread(cd, 1, cdsize, f) != cdsize) goto done;
    size_t p = 0;
    for (unsigned e = 0; e < entries && p + 46 <= cdsize; e++)
    {
        if (le32(cd + p) != 0x02014b50) break;
        uint16_t method = le16(cd + p + 10), nl = le16(cd + p + 28), xl = le16(cd + p + 30), cl = le16(cd + p + 32);
        uint32_t csize = le32(cd + p + 20), usize = le32(cd + p + 24), loff = le32(cd + p + 42);
        if (nl == wl && !memcmp(cd + p + 46, want, wl))
        {
            if (!out) { rc = 0; goto done; }
            unsigned char lh[30];
            if (fseeko(f, loff, SEEK_SET) != 0 || fread(lh, 1, 30, f) != 30 || le32(lh) != 0x04034b50) goto done;
            if (fseeko(f, (off_t)loff + 30 + le16(lh + 26) + le16(lh + 28), SEEK_SET) != 0) goto done;
            char *buf = malloc(usize + 1);
            if (!buf) goto done;
            if (method == 0)
            {
                if (fread(buf, 1, usize, f) != usize) { free(buf); goto done; }
            }
            else if (method == 8)
            {
                comp = malloc(csize ? csize : 1);
                if (!comp || fread(comp, 1, csize, f) != csize) { free(buf); goto done; }
                z_stream z;
                memset(&z, 0, sizeof z);
                if (inflateInit2(&z, -MAX_WBITS) != Z_OK) { free(buf); goto done; }
                z.next_in = comp; z.avail_in = csize; z.next_out = (Bytef *)buf; z.avail_out = usize;
                int zr = inflate(&z, Z_FINISH);
                inflateEnd(&z);
                if (zr != Z_STREAM_END) { free(buf); goto done; }
            }
            else { free(buf); goto done; }
            buf[usize] = 0;
            *out = buf; *outlen = usize;
            rc = 0;
            goto done;
        }
        p += 46 + nl + xl + cl;
    }
done:
    free(cd); free(comp);
    fclose(f);
    return rc;
}

/* the whole entry "assets/<name>" of the APK at <apk> (AssetManager.open) */
static int apk_read_asset(const char *apk, const char *name, char **out, size_t *outlen)
{
    char want[512];
    snprintf(want, sizeof want, "assets/%s", name);
    return zip_entry(apk, want, out, outlen);
}

H(asset_open)
{
    const char *name = jstr(a[0]);
    const char *dir = getenv("UP_ASSETS_DIR");
    stream *s = calloc(1, sizeof *s);
    if (dir)
    {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, name);
        FILE *f = fopen(path, "rb");
        if (f)
        {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            s->buf = malloc(n > 0 ? n + 1 : 1);
            s->len = n > 0 ? fread(s->buf, 1, n, f) : 0;
            s->buf[s->len] = 0;
            fclose(f);
            return fj_l(fj_new_by_name("java/io/InputStream", s, stream_free));
        }
    }
    else if (UPC.apk_path && apk_read_asset(UPC.apk_path, name, &s->buf, &s->len) == 0)
        return fj_l(fj_new_by_name("java/io/InputStream", s, stream_free));
    free(s);
    up_log("AssetManager.open(%s): not found in %s", name, dir ? dir : UPC.apk_path ? UPC.apk_path : "(no APK)");
    fj_throw(env, "java/io/FileNotFoundException", name);
    return fj_l(NULL);
}
H(scanner_init) { fj_set_obj(self, "in", "Ljava/io/InputStream;", a[0].l); return fj_v(); }
H(scanner_next)
{
    jobject in = fj_get_field(self, "in", "Ljava/io/InputStream;").l;
    stream *s = fj_data(in);
    if (!s || s->pos >= s->len) { fj_throw(env, "java/util/NoSuchElementException", NULL); return fj_l(NULL); }
    jstring r = fj_string(s->buf + s->pos);   /* delimiter "\\A": the whole remaining text */
    s->pos = s->len;
    return fj_l(r);
}
H(is_close) { return fj_v(); }

/* java.util.zip.ZipFile / ZipEntry: Unity 2021.3 checks an OBB with new ZipFile(obb)
 * .getEntry("unity_obb_guid") and reads it through getInputStream + Scanner (2022.3 reads the zip
 * itself). Entries are read whole, stored or deflated, like the APK's assets. */
H(zf_init)
{
    const char *p = jstr(a[0]);
    FILE *f = p ? fopen(p, "rb") : NULL;
    if (!f) { fj_throw(env, "java/io/FileNotFoundException", p ? p : "(null)"); return fj_v(); }
    fclose(f);
    fj_set_data(self, strdup(p), free);
    return fj_v();
}
H(zf_getEntry)
{
    const char *zip = fj_data(self), *n = jstr(a[0]);
    if (!zip || !n || zip_entry(zip, n, NULL, NULL) != 0) return fj_l(NULL);
    return fj_l(fj_new_by_name("java/util/zip/ZipEntry", strdup(n), free));
}
H(zf_getInputStream)
{
    const char *zip = fj_data(self), *n = a[0].l ? fj_data(a[0].l) : NULL;
    stream *st = calloc(1, sizeof *st);
    if (!zip || !n || !st || zip_entry(zip, n, &st->buf, &st->len) != 0) {
        free(st);
        fj_throw(env, "java/io/IOException", n ? n : "(null)");
        return fj_l(NULL);
    }
    return fj_l(fj_new_by_name("java/io/InputStream", st, stream_free));
}
H(ze_getName) { const char *n = fj_data(self); return fj_l(n ? fj_string(n) : NULL); }

/* ClassLoader / System / Runtime / Thread */
/* BaseDexClassLoader.findLibrary: the path of lib<name>.so in nativeLibraryDir, or null when the
 * app does not ship it (Unity probes optional libraries this way, e.g. the .NET 4.x Mono runtime
 * libmonobdwgc-2.0.so before the legacy libmono.so) */
H(cl_findLibrary)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/lib%s.so", UPC.native_lib_dir, jstr(a[0]));
    if (access(p, R_OK) != 0) return fj_l(NULL);
    return fj_l(fj_string(p));
}
H(cl_loadClass)
{
    JNIEnv *e = fj_env();
    char n[512];
    snprintf(n, sizeof n, "%s", jstr(a[0]));
    for (char *p = n; *p; p++)
        if (*p == '.') *p = '/';
    jclass c = (*e)->FindClass(e, n);
    if (!c) {   /* ClassLoader.loadClass throws ClassNotFoundException, not NoClassDefFoundError */
        (*e)->ExceptionClear(e);
        fj_throw(env, "java/lang/ClassNotFoundException", jstr(a[0]));
    }
    return fj_l(c);
}
/* Android's rules for the two: System.loadLibrary(name) takes a bare name ("Directory separator
 * should not appear in library name"), System.load(path) an absolute path; anything else throws
 * UnsatisfiedLinkError without loading. The host maps a name to lib<name>.so. */
static int load_named(const char *n, int by_name)
{
    if (!n || !*n || (by_name ? strchr(n, '/') != NULL : n[0] != '/')) return 0;
    return UPC.on_load_library ? UPC.on_load_library(UPC.user, n) == 1 : 0;
}
H(sys_load)
{
    const char *n = jstr(a[0]);
    int ok = load_named(n, ud != NULL);
    up_log("System.load%s(%s) -> %d", ud ? "Library" : "", n, ok);
    if (!ok) fj_throw(env, "java/lang/UnsatisfiedLinkError", n ? n : "(null)");
    return fj_v();
}
H(sys_nanoTime)  { return fj_j(up_now_ns()); }
H(sys_millis)    { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return fj_j((jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000); }
H(sys_idHash)    { return fj_i((jint)((uintptr_t)a[0].l >> 3)); }
/* What an Android device running the player's ABI reports: the libraries are built for the
 * same architecture as this program. */
#if defined(__aarch64__)
#define UP_OS_ARCH "aarch64"
#define UP_CPU_ABI "arm64-v8a"
#define UP_CPU_ABI2 ""
#else
#define UP_OS_ARCH "armv7l"
#define UP_CPU_ABI "armeabi-v7a"
#define UP_CPU_ABI2 "armeabi"
#endif

H(sys_getProperty)
{
    const char *k = jstr(a[0]);
    const char *v = !strcmp(k, "os.arch") ? UP_OS_ARCH : !strcmp(k, "java.vm.version") ? "2.1.0" :
                    !strcmp(k, "line.separator") ? "\n" : !strcmp(k, "file.separator") ? "/" :
                    !strcmp(k, "os.name") ? "Linux" : !strcmp(k, "java.io.tmpdir") ? UPC.cache_dir : NULL;
    return fj_l(v ? fj_string(v) : NULL);
}
H(rt_procs)      { return fj_i(UPC.cpu_count); }
H(rt_maxMem)     { return fj_j(512LL * 1024 * 1024); }
H(clock_uptime)  { return fj_j(up_now_ms()); }
H(thr_current)   { return fj_l(fj_new_by_name("java/lang/Thread", NULL, NULL)); }
H(thr_setName)   { if (fj_tracing()) up_log("Thread.setName(%s)", jstr(a[0])); return fj_v(); }
H(int_parse)     { return fj_i(atoi(jstr(a[0]))); }
H(num_floatValue){ jvalue v = fj_get_field(self, "value", "F"); const char *c = fj_class_name(fj_object_class(self));
                   if (c && !strcmp(c, "java/lang/Integer")) return fj_f((float)fj_get_field(self, "value", "I").i);
                   if (c && !strcmp(c, "java/lang/Long")) return fj_f((float)fj_get_field(self, "value", "J").j);
                   if (c && !strcmp(c, "java/lang/Double")) return fj_f((float)fj_get_field(self, "value", "D").d);
                   return v; }
H(num_intValue)  { const char *c = fj_class_name(fj_object_class(self));
                   if (c && !strcmp(c, "java/lang/Float")) return fj_i((jint)fj_get_field(self, "value", "F").f);
                   if (c && !strcmp(c, "java/lang/Long")) return fj_i((jint)fj_get_field(self, "value", "J").j);
                   if (c && !strcmp(c, "java/lang/Double")) return fj_i((jint)fj_get_field(self, "value", "D").d);
                   return fj_get_field(self, "value", "I"); }

/* StringBuilder */
H(sb_init)       { fj_set_data(self, strdup(""), free); return fj_v(); }
H(sb_append)
{
    const char *cur = fj_data(self), *add = fj_cstr(a[0].l);
    char *n;
    if (asprintf(&n, "%s%s", cur ? cur : "", add ? add : "null") < 0) return fj_l(self);
    fj_set_data(self, n, free);
    return fj_l(self);
}
H(sb_toString)   { const char *s = fj_data(self); return fj_l(fj_string(s ? s : "")); }

/* simple collections: ArrayList / HashSet backed by an Object[] field "elems" */
static jobjectArray coll_elems(jobject self) { return fj_get_field(self, "elems", "[Ljava/lang/Object;").l; }
H(coll_init)     { fj_set_obj(self, "elems", "[Ljava/lang/Object;", fj_array_new("Ljava/lang/Object;", 0)); return fj_v(); }
H(coll_size)     { return fj_i(fj_array_len(coll_elems(self))); }
H(coll_isEmpty)  { return fj_z(fj_array_len(coll_elems(self)) == 0); }
H(coll_get)
{
    jobjectArray e = coll_elems(self);
    if (a[0].i < 0 || a[0].i >= fj_array_len(e)) { fj_throw(env, "java/lang/IndexOutOfBoundsException", NULL); return fj_l(NULL); }
    return fj_l(((jobject *)fj_array_data(e))[a[0].i]);
}
H(coll_contains)
{
    jobjectArray e = coll_elems(self);
    jobject *d = fj_array_data(e);
    for (int i = 0; i < fj_array_len(e); i++) {
        if (d[i] == a[0].l) return fj_z(1);
        if (fj_cstr(d[i]) && fj_cstr(a[0].l) && !strcmp(fj_cstr(d[i]), fj_cstr(a[0].l))) return fj_z(1);
    }
    return fj_z(0);
}
H(coll_add)
{
    if (ud && coll_contains(env, self, a, NULL).z) return fj_z(0);   /* set semantics */
    JNIEnv *e = fj_env();
    jobjectArray old = coll_elems(self);
    int n = fj_array_len(old);
    jobjectArray nw = fj_array_new("Ljava/lang/Object;", n + 1);
    for (int i = 0; i < n; i++) (*e)->SetObjectArrayElement(e, nw, i, ((jobject *)fj_array_data(old))[i]);
    (*e)->SetObjectArrayElement(e, nw, n, a[0].l);
    fj_set_obj(self, "elems", "[Ljava/lang/Object;", nw);
    return fj_z(1);
}
H(coll_addAll)
{
    jobjectArray other = coll_elems(a[0].l);
    for (int i = 0; i < fj_array_len(other); i++) {
        jvalue x = fj_l(((jobject *)fj_array_data(other))[i]);
        coll_add(env, self, &x, ud);
    }
    return fj_z(fj_array_len(other) > 0);
}
H(coll_iterator)
{
    jobject it = fj_new_by_name("java/util/Iterator$Impl", NULL, NULL);
    fj_set_obj(it, "elems", "[Ljava/lang/Object;", coll_elems(self));
    fj_set_int(it, "pos", 0);
    return fj_l(it);
}
H(it_hasNext)    { return fj_z(fj_get_field(self, "pos", "I").i < fj_array_len(coll_elems(self))); }
H(it_next)
{
    int p = fj_get_field(self, "pos", "I").i;
    jobjectArray e = coll_elems(self);
    if (p >= fj_array_len(e)) { fj_throw(env, "java/util/NoSuchElementException", NULL); return fj_l(NULL); }
    fj_set_int(self, "pos", p + 1);
    return fj_l(((jobject *)fj_array_data(e))[p]);
}
H(map_entrySet)  { jobject s = fj_new_by_name("java/util/HashSet", NULL, NULL); coll_init(env, s, NULL, NULL); return fj_l(s); }

/* Looper / Handler / Message / HandlerThread */
static up_looper *looper_of(jobject o) { return o ? fj_data(o) : NULL; }
static jobject looper_obj(up_looper *l)
{
    if (!l) return NULL;
    if (!l->obj) l->obj = fj_global(fj_new_by_name("android/os/Looper", l, NULL));
    return l->obj;
}
H(lp_main)      { return fj_l(looper_obj(&L_ui)); }
H(lp_my)        { return fj_l(looper_obj(t_looper)); }
H(lp_prepare)
{
    if (!t_looper) { up_looper *l = calloc(1, sizeof *l); looper_init(l, "java"); t_looper = l; }
    return fj_v();
}
H(lp_loop)      { if (t_looper) looper_loop(t_looper); return fj_v(); }
H(lp_quit)      { up_looper *l = looper_of(self); if (l) { pthread_mutex_lock(&l->mu); l->quit = 1; pthread_cond_broadcast(&l->cv); pthread_mutex_unlock(&l->mu); } return fj_v(); }
H(h_init)       /* ud: 0 = (), 1 = (Looper), 2 = (Looper, Callback) */
{
    jobject lp = ud ? a[0].l : looper_obj(t_looper ? t_looper : &L_ui);
    fj_set_obj(self, "mLooper", "Landroid/os/Looper;", lp);
    if ((intptr_t)ud == 2) fj_set_obj(self, "mCallback", "Landroid/os/Handler$Callback;", a[1].l);
    return fj_v();
}
static up_looper *handler_looper(jobject h)
{
    up_looper *l = looper_of(fj_get_field(h, "mLooper", "Landroid/os/Looper;").l);
    return l ? l : &L_ui;
}
H(h_getLooper)  { return fj_get_field(self, "mLooper", "Landroid/os/Looper;"); }
H(h_post)       { looper_post(handler_looper(self), NULL, NULL, a[0].l, ud ? a[1].j : 0); return fj_z(1); }
H(h_obtain)     /* ud: 0 = (I), 1 = (III), 2 = (ILObject) */
{
    jobject m = fj_new_by_name("android/os/Message", NULL, NULL);
    fj_set_obj(m, "target", "Landroid/os/Handler;", self);
    fj_set_int(m, "what", a[0].i);
    if ((intptr_t)ud == 1) { fj_set_int(m, "arg1", a[1].i); fj_set_int(m, "arg2", a[2].i); }
    if ((intptr_t)ud == 2) fj_set_obj(m, "obj", "Ljava/lang/Object;", a[1].l);
    return fj_l(m);
}
static void job_dispatch_message(void *arg)
{
    jobject m = arg;
    jobject h = fj_get_field(m, "target", "Landroid/os/Handler;").l;
    jobject cb = h ? fj_get_field(h, "mCallback", "Landroid/os/Handler$Callback;").l : NULL;
    if (cb) fj_call(cb, "handleMessage", "(Landroid/os/Message;)Z", m);
    else if (h) fj_call(h, "handleMessage", "(Landroid/os/Message;)V", m);
    fj_release(m);
}
H(msg_send)
{
    jobject h = fj_get_field(self, "target", "Landroid/os/Handler;").l;
    if (!h) return fj_v();
    looper_post(handler_looper(h), job_dispatch_message, fj_global(self), NULL, 0);
    return fj_v();
}
H(msg_getTarget) { return fj_get_field(self, "target", "Landroid/os/Handler;"); }
H(h_sendMessage) { fj_set_obj(a[0].l, "target", "Landroid/os/Handler;", self); msg_send(env, a[0].l, NULL, NULL); return fj_z(1); }

typedef struct { up_looper l; pthread_t th; int started; pthread_mutex_t mu; pthread_cond_t cv; char name[64]; } hthread;
static void *hthread_main(void *p)
{
    hthread *t = p;
    pthread_setname_np(pthread_self(), t->name);
    JavaVM *vm = fj_vm();
    JNIEnv *e;
    (*vm)->AttachCurrentThread(vm, &e, NULL);
    pthread_mutex_lock(&t->mu);
    t->started = 1;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
    looper_loop(&t->l);
    (*vm)->DetachCurrentThread(vm);
    return NULL;
}
H(ht_init)
{
    hthread *t = calloc(1, sizeof *t);
    snprintf(t->name, sizeof t->name, "%.15s", jstr(a[0]));
    looper_init(&t->l, t->name);
    pthread_mutex_init(&t->mu, NULL);
    pthread_cond_init(&t->cv, NULL);
    fj_set_data(self, t, NULL);   /* never freed: the thread may outlive the object */
    return fj_v();
}
H(ht_start)
{
    hthread *t = fj_data(self);
    if (!t || t->started) return fj_v();
    up_log("HandlerThread %s start", t->name);
    pthread_create(&t->th, NULL, hthread_main, t);
    return fj_v();
}
H(ht_getLooper)
{
    hthread *t = fj_data(self);
    if (!t) return fj_l(NULL);
    pthread_mutex_lock(&t->mu);
    while (!t->started) pthread_cond_wait(&t->cv, &t->mu);
    pthread_mutex_unlock(&t->mu);
    return fj_l(looper_obj(&t->l));
}
H(ht_quit)      { hthread *t = fj_data(self); if (t) { jvalue x; lp_quit(env, looper_obj(&t->l), &x, NULL); } return fj_z(1); }
H(ht_isAlive)   { hthread *t = fj_data(self); return fj_z(t && t->started && !t->l.quit); }
H(ht_join)      { hthread *t = fj_data(self); if (t && t->started) pthread_join(t->th, NULL); return fj_v(); }

/* ---- framework members the 2018.4-2022.3 players bind ---------------------------------- */
H(h_arg0)          { return fj_l(a[0].l); }
H(h_long)          { return fj_j((jlong)(intptr_t)ud); }
/* one display mode: the panel at its refresh rate */
H(disp_modes)
{
    jobjectArray arr = fj_array_new("Landroid/view/Display$Mode;", 1);
    JNIEnv *e = fj_env();
    (*e)->SetObjectArrayElement(e, arr, 0, g_display_mode);
    return fj_l(arr);
}
/* Android's own values for a panel without vsync offsets: the app is woken at vsync and has one
   refresh period to present */
H(disp_deadline)   { return fj_j((jlong)(1e9 / UPC.refresh_rate)); }
H(view_getDisplay) { return fj_l(g_display); }
H(pi_longVersion)  { return fj_j(UPC.version_code); }
H(pm_queryIntents) { jobject l = fj_new_by_name("java/util/ArrayList", NULL, NULL); coll_init(env, l, NULL, NULL); return fj_l(l); }
H(am_getDevices)   /* the built-in speaker (AudioDeviceInfo.TYPE_BUILTIN_SPEAKER) */
{
    jobjectArray arr = fj_array_new("Landroid/media/AudioDeviceInfo;", 1);
    JNIEnv *e = fj_env();
    (*e)->SetObjectArrayElement(e, arr, 0, fj_new_by_name("android/media/AudioDeviceInfo", NULL, NULL));
    return fj_l(arr);
}
H(set_global_getString)
{
    const char *k = jstr(a[1]);
    if (!strcmp(k, "device_name")) return fj_l(fj_string(UPC.model));   /* SystemInfo.deviceName */
    return fj_l(NULL);
}
static jobject g_pad;
H(pad_init)
{
    if (!g_pad) g_pad = fj_global(fj_new_by_name("com/unity3d/player/PlayAssetDeliveryUnityWrapper", NULL, NULL));
    return fj_l(g_pad);
}
H(pad_logged)      { up_log("Play Asset Delivery: getAssetPackState(%s): no asset packs here", jstr(a[0])); return fj_v(); }
H(act_moveTaskToBack) { up_log("Activity.moveTaskToBack(%d): ignored (no other task to show)", a[0].z); return fj_z(0); }
H(proc_setPriority)   { if (fj_tracing()) up_log("Process.setThreadPriority(%d, %d) ignored", a[0].i, a[1].i); return fj_v(); }

/* ---- UnityPlayer (Java methods native code calls) ------------------------------------- */
H(up_execGL)        { up_run_gl_jobs(); return fj_v(); }
H(up_kill)          { up_log("UnityPlayer.kill()"); up_request_finish("UnityPlayer.kill()"); return fj_v(); }
H(up_isFinishing)   { return fj_z(up_is_finishing()); }
H(up_showSoftInput) { up_log("showSoftInput(\"%s\") ignored (no on-screen keyboard)", jstr(a[0])); return fj_v(); }
H(up_loadLibrary)    /* System.loadLibrary(name), false instead of UnsatisfiedLinkError */
{
    const char *n = jstr(a[0]);
    int ok = load_named(n, 1);
    up_log("UnityPlayer.loadLibrary(%s) -> %d", n, ok);
    return fj_z(ok);
}
H(up_showVideo)     { up_log("showVideoPlayer(%s) unsupported", jstr(a[0])); return fj_z(0); }
H(up_reportError)   { up_log("Unity error: %s: %s", jstr(a[0]), jstr(a[1])); return fj_v(); }
H(up_sendMessage)   { up_send_message_j(a[0].l, a[1].l, a[2].l); return fj_v(); }
H(up_reportSoftInputStr)
{
    if (a[1].i == 1) { /* hideSoftInput */ }
    if (a[2].z) fj_call_native(UP_CLS, "nativeSoftInputCanceled", "()V", self);
    else if (a[0].l) fj_call_native(UP_CLS, "nativeSetInputString", "(Ljava/lang/String;)V", self, a[0].l);
    if (a[1].i == 1) fj_call_native(UP_CLS, "nativeSoftInputClosed", "()V", self);
    return fj_v();
}

/* Methods libunity added to UnityPlayerJavaWrapper from 2018.4 on, with what a handheld answers */
static float g_brightness = 1.0f;          /* Screen.brightness: the window's own value */
static char *g_clipboard;
static pthread_mutex_t g_clip_mu = PTHREAD_MUTEX_INITIALIZER;
H(up_getClipboard)
{
    pthread_mutex_lock(&g_clip_mu);
    jstring r = fj_string(g_clipboard ? g_clipboard : "");
    pthread_mutex_unlock(&g_clip_mu);
    return fj_l(r);
}
H(up_setClipboard)
{
    pthread_mutex_lock(&g_clip_mu);
    free(g_clipboard);
    g_clipboard = strdup(jstr(a[0]));
    pthread_mutex_unlock(&g_clip_mu);
    return fj_v();
}
H(up_getBrightness)  { return fj_f(g_brightness); }
H(up_setBrightness)
{
    g_brightness = a[0].f < 0 ? 1.0f : a[0].f > 1 ? 1.0f : a[0].f;
    up_log("Screen.brightness = %.2f (not applied to the panel)", (double)g_brightness);
    return fj_v();
}
H(up_logged)         { up_log("UnityPlayer.%s ignored (one full-screen view, no system back)", (const char *)ud); return fj_v(); }
static void job_hide_preserved(void *arg)
{
    (void)arg;
    if (fj_native_sig(UP_CLS, "nativeHidePreservedContent")) up_native("nativeHidePreservedContent", "()V", NULL);
}
static void ui_hide_preserved(void *arg) { (void)arg; up_gl_post(job_hide_preserved, NULL, NULL); }
/* 2021.3+: the engine asks Java to drop the last frame it kept on screen across a surface
   change; Java does that on the UI thread and acknowledges with nativeHidePreservedContent on
   the GL thread (the pairing is inferred from the names). There is no such view here. */
H(up_hidePreserved)  { looper_post(&L_ui, ui_hide_preserved, NULL, NULL, 0); return fj_v(); }
/* Application.Unload (2019.4: notifyOnUnityPlayerUnloaded, 2020.3+: pauseJavaAndCallUnloadCallback):
   UnityPlayerActivity then moves its task to the back; a handheld port has no back, so quit */
H(up_unloaded)       { up_request_finish("Application.Unload"); return fj_v(); }

/* ReflectionHelper (managed AndroidJavaObject/Class support) */
H(rh_getMethodID)
{
    JNIEnv *e = fj_env();
    const char *n = jstr(a[1]), *s = jstr(a[2]);
    jmethodID m = a[3].z ? (*e)->GetStaticMethodID(e, a[0].l, n, s) : (*e)->GetMethodID(e, a[0].l, n, s);
    return fj_l((*e)->ToReflectedMethod(e, a[0].l, m, a[3].z));
}
H(rh_getFieldID)
{
    JNIEnv *e = fj_env();
    const char *n = jstr(a[1]), *s = jstr(a[2]);
    jfieldID f = a[3].z ? (*e)->GetStaticFieldID(e, a[0].l, n, s) : (*e)->GetFieldID(e, a[0].l, n, s);
    return fj_l((*e)->ToReflectedField(e, a[0].l, f, a[3].z));
}
H(rh_getCtorID)
{
    JNIEnv *e = fj_env();
    char sig[512];
    snprintf(sig, sizeof sig, "%sV", jstr(a[1]));   /* "(args)" -> "(args)V" */
    if (strchr(jstr(a[1]), ')') && sig[strlen(sig) - 2] != ')') snprintf(sig, sizeof sig, "%s", jstr(a[1]));
    jmethodID m = (*e)->GetMethodID(e, a[0].l, "<init>", sig);
    return fj_l((*e)->ToReflectedMethod(e, a[0].l, m, 0));
}
H(rh_fieldSig)      { const char *sg = fj_reflected_sig(a[0].l); return fj_l(sg ? fj_string(sg) : NULL); }

/* AndroidJavaProxy: Proxy.newProxyInstance with Unity's invocation handler, which forwards each
 * call to the managed object through ReflectionHelper.nativeProxyInvoke(id, method name, args).
 *   2017.4-2018.4: newProxyInstance(int id, Class | Class[]), nativeProxyInvoke(int, ...)
 *   2019.4:        long ids; a managed exception is flagged on the proxy with
 *                  setNativeExceptionOnProxy(proxy, handle, methodNotFound)
 *   2020.3+:       newProxyInstance(UnityPlayer, long id, Class | Class[]); calls are bracketed by
 *                  beginProxyCall(id)/endProxyCall() (refused once the player is gone)
 *   2021.3+:       managed exceptions come back as the result, made by createInvocationError(
 *                  handle, methodNotFound); nativeProxyJNIFreeGCHandle(handle) releases them
 * Proxies of an earlier launch (before ReflectionHelper.endUnityLaunch) are not called. */
#define RH "com/unity3d/player/ReflectionHelper"
#define RH_ERROR "com/unity3d/player/ReflectionHelper$InvocationError"
static int g_launch = 1;

static jobject rh_proxy(jlong id, jobject ifaces_or_class, int single)
{
    jobjectArray arr = ifaces_or_class;
    if (single) {
        arr = fj_array_new("Ljava/lang/Class;", 1);
        JNIEnv *e = fj_env();
        (*e)->SetObjectArrayElement(e, arr, 0, ifaces_or_class);
    }
    jobject p = fj_new_proxy(FJ_PROXY_REFLECTION, id, arr);
    fj_set_int(p, "rhLaunch", g_launch);
    return p;
}
/* ud bit 0: a single Class (not Class[]), bit 1: a UnityPlayer comes first (2020.3+), bit 2: long id */
H(rh_newProxy)
{
    intptr_t f = (intptr_t)ud;
    const jvalue *x = (f & 2) ? a + 1 : a;
    return fj_l(rh_proxy((f & 4) ? x[0].j : (jlong)x[0].i, x[1].l, f & 1));
}
H(rh_endLaunch)     { g_launch++; up_log("ReflectionHelper.endUnityLaunch: earlier proxies are detached"); return fj_v(); }
H(rh_beginCall)     { return fj_z(!up_unity_quit_done()); }
H(rh_setException)
{
    if (!a[0].l) return fj_v();
    fj_set_long(a[0].l, "rhException", a[1].j);
    fj_set_bool(a[0].l, "rhNotFound", a[2].z);
    return fj_v();
}
H(rh_invocationError)
{
    jobject e = fj_new_by_name(RH_ERROR, NULL, NULL);
    fj_set_long(e, "handle", a[0].j);
    fj_set_bool(e, "notFound", a[1].z);
    return fj_l(e);
}

/* a managed exception in a proxy call: Unity logs it (nativeProxyLogJNIInvokeException) and Java
 * throws it to the caller; here the caller gets null/0 and no Java exception */
static void rh_failed(jlong handle, int not_found, const char *method)
{
    up_log("AndroidJavaProxy.%s: %s in managed code", method, not_found ? "no such method" : "exception");
    jvalue h = fj_j(handle);
    const char *sig = fj_native_sig(RH, "nativeProxyLogJNIInvokeException");
    if (handle && sig && !strcmp(sig, "(J)V")) up_call_native(RH, "nativeProxyLogJNIInvokeException", "(J)V", NULL, &h);
    if (handle && fj_native_sig(RH, "nativeProxyJNIFreeGCHandle"))
        up_call_native(RH, "nativeProxyJNIFreeGCHandle", "(J)V", NULL, &h);
}

static jobject rh_invoke(JNIEnv *env, jobject proxy, jlong id, const char *method, jobjectArray args)
{
    int launch = fj_get_field(proxy, "rhLaunch", "I").i;
    if (launch && launch != g_launch) {
        up_log("AndroidJavaProxy.%s: proxy of an ended Unity launch, not called", method);
        return NULL;
    }
    if (!rh_beginCall(env, NULL, NULL, NULL).z) return NULL;   /* beginProxyCall(id) */
    const char *sig = fj_native_sig(RH, "nativeProxyInvoke");
    if (!sig) { up_log("AndroidJavaProxy.%s: ReflectionHelper.nativeProxyInvoke not registered", method); return NULL; }
    jvalue a[3] = { sig[1] == 'I' ? fj_i((jint)id) : fj_j(id), fj_l(fj_string(method)), fj_l(args) };
    fj_set_long(proxy, "rhException", 0);
    jvalue r = up_call_native(RH, "nativeProxyInvoke", sig, NULL, a);
    jlong exc = fj_get_field(proxy, "rhException", "J").j;
    if (exc) {                                                  /* 2019.4, 2020.3 */
        fj_set_long(proxy, "rhException", 0);
        rh_failed(exc, fj_get_field(proxy, "rhNotFound", "Z").z, method);
        return NULL;
    }
    if (r.l && fj_is_instance(r.l, RH_ERROR)) {                 /* 2021.3+ */
        rh_failed(fj_get_field(r.l, "handle", "J").j, fj_get_field(r.l, "notFound", "Z").z, method);
        return NULL;
    }
    return r.l;                                                 /* endProxyCall() */
}

/* ======================================================================================= */
/* registration                                                                            */
/* ======================================================================================= */

static void constants(void)
{
    const char *C = "android/content/Context";
    fj_static_string(C, "AUDIO_SERVICE", "audio");
    fj_static_string(C, "CONNECTIVITY_SERVICE", "connectivity");
    fj_static_string(C, "DISPLAY_SERVICE", "display");
    fj_static_string(C, "LOCATION_SERVICE", "location");
    fj_static_string(C, "MEDIA_ROUTER_SERVICE", "media_router");
    fj_static_string(C, "VIBRATOR_SERVICE", "vibrator");
    fj_static_string(C, "WINDOW_SERVICE", "window");
    fj_static_string(C, "INPUT_SERVICE", "input");
    fj_static_string(C, "ACTIVITY_SERVICE", "activity");
    fj_static_string(C, "SENSOR_SERVICE", "sensor");
    fj_static_string(C, "TELEPHONY_SERVICE", "phone");
    fj_static_string(C, "POWER_SERVICE", "power");
    fj_static_string(C, "WIFI_SERVICE", "wifi");
    fj_static_int(C, "MODE_PRIVATE", 0);
    fj_static_int(C, "BIND_AUTO_CREATE", 1);
    fj_static_string("android/Manifest$permission", "VIBRATE", "android.permission.VIBRATE");
    fj_static_int("android/R$attr", "progressBarStyleLarge", 0x01010288);
    fj_static_int("android/R$attr", "progressBarStyleLargeInverse", 0x0101028a);
    fj_static_int("android/R$attr", "progressBarStyleSmall", 0x01010287);
    fj_static_int("android/R$attr", "progressBarStyleSmallInverse", 0x01010289);
    fj_static_int("android/content/DialogInterface", "BUTTON_NEGATIVE", -2);
    fj_static_int("android/content/DialogInterface", "BUTTON_POSITIVE", -1);
    fj_static_string("android/content/Intent", "ACTION_BATTERY_CHANGED", "android.intent.action.BATTERY_CHANGED");
    fj_static_string("android/content/Intent", "ACTION_VIEW", "android.intent.action.VIEW");
    fj_static_string("android/content/Intent", "ACTION_MAIN", "android.intent.action.MAIN");
    const char *AI = "android/content/pm/ActivityInfo";
    fj_static_int(AI, "SCREEN_ORIENTATION_UNSPECIFIED", -1);
    fj_static_int(AI, "SCREEN_ORIENTATION_LANDSCAPE", 0);
    fj_static_int(AI, "SCREEN_ORIENTATION_PORTRAIT", 1);
    fj_static_int(AI, "SCREEN_ORIENTATION_SENSOR", 4);
    fj_static_int(AI, "SCREEN_ORIENTATION_SENSOR_LANDSCAPE", 6);
    fj_static_int(AI, "SCREEN_ORIENTATION_SENSOR_PORTRAIT", 7);
    fj_static_int(AI, "SCREEN_ORIENTATION_REVERSE_LANDSCAPE", 8);
    fj_static_int(AI, "SCREEN_ORIENTATION_REVERSE_PORTRAIT", 9);
    fj_static_int(AI, "SCREEN_ORIENTATION_FULL_SENSOR", 10);
    fj_static_int(AI, "SCREEN_ORIENTATION_USER_LANDSCAPE", 11);    /* 2018.4+ players read these too */
    fj_static_int(AI, "SCREEN_ORIENTATION_USER_PORTRAIT", 12);
    fj_static_int(AI, "SCREEN_ORIENTATION_FULL_USER", 13);
    const char *PM = "android/content/pm/PackageManager";
    fj_static_string(PM, "FEATURE_AUDIO_LOW_LATENCY", "android.hardware.audio.low_latency");
    fj_static_int(PM, "GET_ACTIVITIES", 1);
    fj_static_int(PM, "GET_META_DATA", 128);
    fj_static_int(PM, "PERMISSION_GRANTED", 0);
    fj_static_int("android/graphics/Color", "TRANSPARENT", 0);
    fj_static_int("android/graphics/Color", "BLACK", (jint)0xff000000);
    fj_static_int("android/graphics/ImageFormat", "NV21", 17);
    fj_static_int("android/media/AudioAttributes", "CONTENT_TYPE_MUSIC", 2);
    fj_static_int("android/media/AudioAttributes", "USAGE_GAME", 14);
    const char *AM = "android/media/AudioManager";
    fj_static_int(AM, "AUDIOFOCUS_GAIN", 1);
    fj_static_int(AM, "AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK", -3);
    fj_static_int(AM, "AUDIOFOCUS_REQUEST_GRANTED", 1);
    fj_static_int(AM, "STREAM_MUSIC", 3);
    fj_static_string(AM, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER", "android.media.property.OUTPUT_FRAMES_PER_BUFFER");
    fj_static_string(AM, "PROPERTY_OUTPUT_SAMPLE_RATE", "android.media.property.OUTPUT_SAMPLE_RATE");
    fj_static_int("android/media/MediaRouter", "ROUTE_TYPE_LIVE_VIDEO", 2);
    fj_static_int("android/net/ConnectivityManager", "TYPE_MOBILE", 0);
    fj_static_int("android/net/ConnectivityManager", "TYPE_WIFI", 1);
    const char *B = "android/os/Build";
    fj_static_string(B, "DEVICE", UPC.device);
    fj_static_string(B, "ID", "LMY47D");
    fj_static_string(B, "DISPLAY", "LMY47D");   /* the build id shown to users (Swappy's device list reads it) */
    fj_static_string(B, "MANUFACTURER", UPC.manufacturer);
    fj_static_string(B, "MODEL", UPC.model);
    fj_static_string(B, "BRAND", UPC.brand);
    fj_static_string(B, "PRODUCT", UPC.product);
    fj_static_string(B, "HARDWARE", UPC.hardware);
    fj_static_string(B, "FINGERPRINT", UPC.fingerprint);
    fj_static_string(B, "BOARD", UPC.device);
    fj_static_string(B, "TAGS", "release-keys");
    fj_static_string(B, "CPU_ABI", UP_CPU_ABI);
    fj_static_string(B, "CPU_ABI2", UP_CPU_ABI2);
    fj_static_string("android/os/Build$VERSION", "INCREMENTAL", "1");
    fj_static_string("android/os/Build$VERSION", "RELEASE", UPC.os_release);
    fj_static_string("android/os/Build$VERSION", "CODENAME", "REL");
    fj_static_int("android/os/Build$VERSION", "SDK_INT", UPC.sdk_int);
    fj_static_string("android/os/Environment", "MEDIA_MOUNTED", "mounted");
    fj_static_string("android/provider/Settings$Secure", "ANDROID_ID", "android_id");
    fj_static_string("android/provider/Settings$System", "SCREEN_OFF_TIMEOUT", "screen_off_timeout");
    fj_static_string("android/provider/Settings$System", "ACCELEROMETER_ROTATION", "accelerometer_rotation");
    fj_static_string("android/provider/Settings$System", "SCREEN_BRIGHTNESS", "screen_brightness");
    fj_static_string("android/provider/Settings$Global", "DEVICE_NAME", "device_name");
    fj_static_int("android/view/Gravity", "LEFT", 3);
    fj_static_int("android/view/Gravity", "TOP", 48);
    fj_static_int("android/view/Gravity", "CENTER", 17);
    const char *V = "android/view/View";
    fj_static_int(V, "VISIBLE", 0);
    fj_static_int(V, "SYSTEM_UI_FLAG_LOW_PROFILE", 1);
    fj_static_int(V, "SYSTEM_UI_FLAG_HIDE_NAVIGATION", 2);
    fj_static_int(V, "SYSTEM_UI_FLAG_FULLSCREEN", 4);
    fj_static_int(V, "SYSTEM_UI_FLAG_LAYOUT_STABLE", 0x100);
    fj_static_int(V, "SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION", 0x200);
    fj_static_int(V, "SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN", 0x400);
    fj_static_int(V, "SYSTEM_UI_FLAG_IMMERSIVE", 0x800);
    fj_static_int(V, "SYSTEM_UI_FLAG_IMMERSIVE_STICKY", 0x1000);
    fj_static_int("android/view/Window", "FEATURE_NO_TITLE", 1);
    const char *LP = "android/view/WindowManager$LayoutParams";
    fj_static_int(LP, "FLAG_DIM_BEHIND", 2);
    fj_static_int(LP, "FLAG_NOT_FOCUSABLE", 8);
    fj_static_int(LP, "FLAG_KEEP_SCREEN_ON", 0x80);
    fj_static_int(LP, "FLAG_FULLSCREEN", 0x400);
}

void up_android_register(void)
{
    /* class hierarchy */
    CLS("android/content/Context", NULL);
    CLS("android/content/ContextWrapper", "android/content/Context");
    CLS("android/view/ContextThemeWrapper", "android/content/ContextWrapper");
    CLS("android/app/Activity", "android/view/ContextThemeWrapper");
    /* Swappy (Unity 2019.3+) reads the display through NativeActivity's getWindowManager() on
     * Unity's activity: inherited from Activity, as on Android */
    CLS("android/app/NativeActivity", "android/app/Activity");
    CLS("com/unity3d/player/UnityPlayerActivity", "android/app/Activity");
    CLS("com/unity3d/player/UnityPlayerNativeActivity", "com/unity3d/player/UnityPlayerActivity");
    CLS("android/view/View", NULL);
    CLS("android/view/ViewGroup", "android/view/View");
    CLS("android/widget/FrameLayout", "android/view/ViewGroup");
    CLS(UP_CLS, "android/widget/FrameLayout");
    CLS("android/view/SurfaceView", "android/view/View");
    CLS("android/view/SurfaceHolder", NULL);
    CLS("android/view/Surface", NULL);
    CLS("android/view/Window", NULL);
    CLS("android/view/WindowManager", NULL);
    CLS("android/view/WindowManager$LayoutParams", NULL);
    CLS("android/view/Display", NULL);
    CLS("android/util/DisplayMetrics", NULL);
    CLS("android/content/res/Resources", NULL);
    CLS("android/content/res/Configuration", NULL);
    CLS("android/content/res/AssetManager", NULL);
    CLS("android/content/pm/PackageItemInfo", NULL);
    CLS("android/content/pm/ApplicationInfo", "android/content/pm/PackageItemInfo");
    /* declared on PackageItemInfo in Android (Unity looks metaData up there) */
    fj_field_define("android/content/pm/PackageItemInfo", "metaData", "Landroid/os/Bundle;", 0, fj_l(NULL));
    fj_field_define("android/content/pm/PackageItemInfo", "packageName", "Ljava/lang/String;", 0, fj_l(NULL));
    fj_field_define("android/content/pm/PackageItemInfo", "name", "Ljava/lang/String;", 0, fj_l(NULL));
    CLS("android/content/pm/PackageInfo", NULL);
    CLS("android/content/pm/PackageManager", NULL);
    CLS("android/content/ContentResolver", NULL);
    CLS("android/content/Intent", NULL);
    CLS("android/content/IntentFilter", NULL);
    CLS("android/content/SharedPreferences", NULL);
    CLS("android/content/SharedPreferences$Editor", NULL);
    CLS("android/os/Bundle", NULL);
    CLS("android/os/Looper", NULL);
    CLS("android/os/Handler", NULL);
    CLS("android/os/Message", NULL);
    CLS("java/lang/Thread", NULL);
    fj_class_implements("java/lang/Thread", "java/lang/Runnable");
    CLS("android/os/HandlerThread", "java/lang/Thread");
    CLS("android/os/Vibrator", NULL);
    CLS("android/media/AudioManager", NULL);
    CLS("android/media/AudioSystem", NULL);
    CLS("android/net/ConnectivityManager", NULL);
    CLS("android/net/Uri", NULL);
    CLS("android/hardware/display/DisplayManager", NULL);
    CLS("android/media/MediaRouter", NULL);
    CLS("android/media/MediaRouter$RouteInfo", NULL);
    CLS("android/app/ActivityManager", NULL);
    CLS("android/webkit/MimeTypeMap", NULL);
    CLS("java/io/File", NULL);
    CLS("java/io/InputStream", NULL);
    CLS("java/io/FileNotFoundException", "java/io/IOException");
    CLS("java/lang/UnsatisfiedLinkError", "java/lang/LinkageError");
    CLS("java/util/Scanner", NULL);
    CLS("java/util/zip/ZipFile", NULL);
    CLS("java/util/zip/ZipEntry", NULL);
    CLS("java/util/Locale", NULL);
    CLS("java/lang/ClassLoader", NULL);
    CLS("java/lang/System", NULL);
    CLS("java/lang/Runtime", NULL);
    CLS("java/lang/StringBuilder", NULL);
    CLS("java/lang/reflect/Constructor", NULL);
    CLS("java/util/Collection", NULL);
    CLS("java/util/List", NULL);
    CLS("java/util/Set", NULL);
    CLS("java/util/Map", NULL);
    CLS("java/util/Iterator", NULL);
    CLS("java/util/AbstractCollection", NULL);
    fj_class_implements("java/util/AbstractCollection", "java/util/Collection");
    CLS("java/util/ArrayList", "java/util/AbstractCollection");
    fj_class_implements("java/util/ArrayList", "java/util/List");
    CLS("java/util/HashSet", "java/util/AbstractCollection");
    fj_class_implements("java/util/HashSet", "java/util/Set");
    CLS("java/util/HashMap", NULL);
    fj_class_implements("java/util/HashMap", "java/util/Map");
    CLS("java/util/Iterator$Impl", NULL);
    fj_class_implements("java/util/Iterator$Impl", "java/util/Iterator");
    CLS("com/unity3d/player/NativeLoader", NULL);
    CLS("com/unity3d/player/ReflectionHelper", NULL);
    CLS("com/unity3d/player/UnityWebRequest", NULL);
    /* optional Unity features we do not provide: real Java would have these classes, but the
     * VR/AR paths only run when the build enables them; keep the placeholders. */

    constants();

    /* interfaces / abstract methods (implemented by subclasses or by JNIBridge proxies);
     * declared so lookups through the interface type are not reported as unknown */
    static const char *const abstract[][3] = {
        { "java/lang/Runnable", "run", "()V" },
        { "android/view/Choreographer$FrameCallback", "doFrame", "(J)V" },
        { "android/os/Handler$Callback", "handleMessage", "(Landroid/os/Message;)Z" },
        { "android/hardware/input/InputManager$InputDeviceListener", "onInputDeviceAdded", "(I)V" },
        { "android/hardware/input/InputManager$InputDeviceListener", "onInputDeviceRemoved", "(I)V" },
        { "android/hardware/input/InputManager$InputDeviceListener", "onInputDeviceChanged", "(I)V" },
        { "android/hardware/display/DisplayManager$DisplayListener", "onDisplayAdded", "(I)V" },
        { "android/hardware/display/DisplayManager$DisplayListener", "onDisplayRemoved", "(I)V" },
        { "android/hardware/display/DisplayManager$DisplayListener", "onDisplayChanged", "(I)V" },
        { "android/view/SurfaceHolder$Callback", "surfaceCreated", "(Landroid/view/SurfaceHolder;)V" },
        { "android/view/SurfaceHolder$Callback", "surfaceChanged", "(Landroid/view/SurfaceHolder;III)V" },
        { "android/view/SurfaceHolder$Callback", "surfaceDestroyed", "(Landroid/view/SurfaceHolder;)V" },
        { "android/view/View$OnSystemUiVisibilityChangeListener", "onSystemUiVisibilityChange", "(I)V" },
        { "android/media/AudioManager$OnAudioFocusChangeListener", "onAudioFocusChange", "(I)V" },
        { "android/content/DialogInterface$OnClickListener", "onClick", "(Landroid/content/DialogInterface;I)V" },
        { "android/content/DialogInterface$OnCancelListener", "onCancel", "(Landroid/content/DialogInterface;)V" },
        { "android/content/ServiceConnection", "onServiceConnected", "(Landroid/content/ComponentName;Landroid/os/IBinder;)V" },
        { "android/content/ServiceConnection", "onServiceDisconnected", "(Landroid/content/ComponentName;)V" },
        { "android/content/ServiceConnection", "onBindingDied", "(Landroid/content/ComponentName;)V" },
        { "android/view/InputEvent", "getSource", "()I" },
        { "android/view/InputEvent", "getDeviceId", "()I" },
        { "android/view/InputEvent", "getEventTime", "()J" },
        { "java/util/Collection", "size", "()I" },
        { "java/util/Collection", "isEmpty", "()Z" },
        { "java/util/Collection", "iterator", "()Ljava/util/Iterator;" },
        { "java/util/Collection", "contains", "(Ljava/lang/Object;)Z" },
        { "java/util/List", "size", "()I" },
        { "java/util/List", "isEmpty", "()Z" },
        { "java/util/List", "get", "(I)Ljava/lang/Object;" },
        { "java/util/List", "iterator", "()Ljava/util/Iterator;" },
        { "java/util/List", "contains", "(Ljava/lang/Object;)Z" },
        { "java/util/Set", "iterator", "()Ljava/util/Iterator;" },
        { "java/util/Iterator", "hasNext", "()Z" },
        { "java/util/Iterator", "next", "()Ljava/lang/Object;" },
        { "java/util/Map", "entrySet", "()Ljava/util/Set;" },
        { "java/util/Map$Entry", "getKey", "()Ljava/lang/Object;" },
        { "java/util/Map$Entry", "getValue", "()Ljava/lang/Object;" },
    };
    for (size_t i = 0; i < sizeof abstract / sizeof abstract[0]; i++) {
        if (!fj_class_find(abstract[i][0])) CLS(abstract[i][0], NULL);
        M(abstract[i][0], abstract[i][1], abstract[i][2], NULL);
    }
    CLS("android/content/BroadcastReceiver", NULL);

    /* Context (inherited by Activity) */
    const char *C = "android/content/Context";
    MU(C, "getPackageName", "()Ljava/lang/String;", h_strp, &UPC.package_name);
    MU(C, "getPackageCodePath", "()Ljava/lang/String;", h_strp, &UPC.apk_path);
    MU(C, "getPackageResourcePath", "()Ljava/lang/String;", h_strp, &UPC.apk_path);
    M(C, "getFilesDir", "()Ljava/io/File;", ctx_getFilesDir);
    M(C, "getCacheDir", "()Ljava/io/File;", ctx_getCacheDir);
    M(C, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", ctx_getExtFilesDir);
    M(C, "getExternalCacheDir", "()Ljava/io/File;", ctx_getExtCacheDir);
    M(C, "getObbDir", "()Ljava/io/File;", ctx_getObbDir);
    MU(C, "getObbDirs", "()[Ljava/io/File;", ctx_getFileArray, &UPC.obb_dir);
    MU(C, "getExternalFilesDirs", "(Ljava/lang/String;)[Ljava/io/File;", ctx_getFileArray, &UPC.external_files_dir);
    MU(C, "getExternalCacheDirs", "()[Ljava/io/File;", ctx_getFileArray, &UPC.external_cache_dir);
    MU(C, "getAssets", "()Landroid/content/res/AssetManager;", h_obj, &g_assets);
    MU(C, "getContentResolver", "()Landroid/content/ContentResolver;", h_obj, &g_content_resolver);
    MU(C, "getPackageManager", "()Landroid/content/pm/PackageManager;", h_obj, &g_pm);
    MU(C, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;", h_obj, &g_appinfo);
    MU(C, "getApplicationContext", "()Landroid/content/Context;", h_obj, &g_activity);
    MU(C, "getResources", "()Landroid/content/res/Resources;", h_obj, &g_resources);
    MU(C, "getClassLoader", "()Ljava/lang/ClassLoader;", h_obj, &g_class_loader);
    M(C, "getSharedPreferences", "(Ljava/lang/String;I)Landroid/content/SharedPreferences;", ctx_getSharedPrefs);
    M(C, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", ctx_getSystemService);
    M(C, "checkCallingOrSelfPermission", "(Ljava/lang/String;)I", ctx_checkPermission);
    M(C, "checkSelfPermission", "(Ljava/lang/String;)I", ctx_checkPermission);
    M(C, "registerReceiver", "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;", ctx_registerReceiver);
    M(C, "unregisterReceiver", "(Landroid/content/BroadcastReceiver;)V", h_void);
    M(C, "startActivity", "(Landroid/content/Intent;)V", ctx_startActivity);
    M(C, "bindService", "(Landroid/content/Intent;Landroid/content/ServiceConnection;I)Z", h_false);
    M(C, "unbindService", "(Landroid/content/ServiceConnection;)V", h_void);

    const char *A = "android/app/Activity";
    MU(A, "getIntent", "()Landroid/content/Intent;", h_obj, &g_intent);
    M(A, "getPreferences", "(I)Landroid/content/SharedPreferences;", act_getPreferences);
    M(A, "getRequestedOrientation", "()I", act_getReqOrient);
    M(A, "setRequestedOrientation", "(I)V", act_setReqOrient);
    MU(A, "getWindow", "()Landroid/view/Window;", h_obj, &g_window);
    MU(A, "getWindowManager", "()Landroid/view/WindowManager;", h_obj, &g_wm);
    M(A, "runOnUiThread", "(Ljava/lang/Runnable;)V", act_runOnUiThread);
    M(A, "isFinishing", "()Z", act_isFinishing);
    M(A, "finish", "()V", act_finish);
    M(A, "requestWindowFeature", "(I)Z", h_true);
    MU(A, "getCurrentFocus", "()Landroid/view/View;", h_obj, &g_player);

    /* PackageManager & info objects */
    const char *PM = "android/content/pm/PackageManager";
    M(PM, "getApplicationInfo", "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;", pm_getAppInfo);
    M(PM, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", pm_getPkgInfo);
    M(PM, "getInstallerPackageName", "(Ljava/lang/String;)Ljava/lang/String;", h_null);
    M(PM, "hasSystemFeature", "(Ljava/lang/String;)Z", pm_hasFeature);
    M(PM, "checkPermission", "(Ljava/lang/String;Ljava/lang/String;)I", h_int);

    /* Settings */
    SM("android/provider/Settings$Secure", "getString", "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;", set_secure_getString);
    SM("android/provider/Settings$System", "getInt", "(Landroid/content/ContentResolver;Ljava/lang/String;I)I", set_system_getInt);

    /* Display / metrics / resources */
    const char *D = "android/view/Display";
    M(D, "getDisplayId", "()I", h_int);
    M(D, "getWidth", "()I", disp_width);
    M(D, "getHeight", "()I", disp_height);
    M(D, "getRawWidth", "()I", disp_width);
    M(D, "getRawHeight", "()I", disp_height);
    M(D, "getRotation", "()I", h_int);
    M(D, "getOrientation", "()I", h_int);
    M(D, "getRefreshRate", "()F", disp_refresh);
    M(D, "getMetrics", "(Landroid/util/DisplayMetrics;)V", disp_metrics);
    M(D, "getRealMetrics", "(Landroid/util/DisplayMetrics;)V", disp_metrics);
    M("android/util/DisplayMetrics", "<init>", "()V", h_void);
    fj_field_define("android/util/DisplayMetrics", "widthPixels", "I", 0, fj_i(0));
    fj_field_define("android/util/DisplayMetrics", "heightPixels", "I", 0, fj_i(0));
    fj_field_define("android/util/DisplayMetrics", "densityDpi", "I", 0, fj_i(160));
    fj_field_define("android/util/DisplayMetrics", "density", "F", 0, fj_f(1.0f));
    M("android/content/res/Resources", "getDisplayMetrics", "()Landroid/util/DisplayMetrics;", res_getDM);
    MU("android/content/res/Resources", "getConfiguration", "()Landroid/content/res/Configuration;", h_obj, &g_configuration);
    M("android/content/res/Resources", "getIdentifier", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I", h_int);
    MU("android/view/WindowManager", "getDefaultDisplay", "()Landroid/view/Display;", h_obj, &g_display);
    M("android/hardware/display/DisplayManager", "getDisplay", "(I)Landroid/view/Display;", dm_getDisplay);
    M("android/hardware/display/DisplayManager", "registerDisplayListener", "(Landroid/hardware/display/DisplayManager$DisplayListener;Landroid/os/Handler;)V", h_void);
    M("android/hardware/display/DisplayManager", "unregisterDisplayListener", "(Landroid/hardware/display/DisplayManager$DisplayListener;)V", h_void);
    /* the device route (the built-in screen): no secondary presentation display */
    MU("android/media/MediaRouter", "getSelectedRoute", "(I)Landroid/media/MediaRouter$RouteInfo;", h_obj, &g_media_route);
    M("android/media/MediaRouter$RouteInfo", "getPresentationDisplay", "()Landroid/view/Display;", h_null);

    /* Window / View */
    const char *W = "android/view/Window";
    MU(W, "getDecorView", "()Landroid/view/View;", h_obj, &g_decor);
    MU(W, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;", h_obj, &g_layout_params);
    M(W, "setFlags", "(II)V", h_void);
    M(W, "addFlags", "(I)V", h_void);
    M(W, "clearFlags", "(I)V", h_void);
    M(W, "requestFeature", "(I)Z", h_true);
    M(W, "setBackgroundDrawable", "(Landroid/graphics/drawable/Drawable;)V", h_void);
    M(W, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V", h_void);
    const char *V = "android/view/View";
    M(V, "getSystemUiVisibility", "()I", view_getSysUi);
    M(V, "setSystemUiVisibility", "(I)V", view_setSysUi);
    M(V, "setOnSystemUiVisibilityChangeListener", "(Landroid/view/View$OnSystemUiVisibilityChangeListener;)V", h_void);
    M(V, "setVisibility", "(I)V", h_void);
    M(V, "getWidth", "()I", view_width);
    M(V, "getHeight", "()I", view_height);
    M(V, "requestFocus", "()Z", h_true);
    MU("android/view/SurfaceView", "getHolder", "()Landroid/view/SurfaceHolder;", h_obj, &g_holder);
    MU("android/view/SurfaceHolder", "getSurface", "()Landroid/view/Surface;", h_obj, &g_surface);
    M("android/view/SurfaceHolder", "addCallback", "(Landroid/view/SurfaceHolder$Callback;)V", h_void);
    M("android/view/SurfaceHolder", "setFormat", "(I)V", h_void);
    M("android/view/SurfaceHolder", "setFixedSize", "(II)V", h_void);
    M("android/view/SurfaceHolder", "setSizeFromLayout", "()V", h_void);
    M("android/view/Surface", "isValid", "()Z", h_true);
    M("android/view/Surface", "release", "()V", h_void);
    fj_field_define("android/view/WindowManager$LayoutParams", "gravity", "I", 0, fj_i(0));
    fj_field_define("android/view/WindowManager$LayoutParams", "flags", "I", 0, fj_i(0x480));

    /* Intent / Bundle */
    const char *I = "android/content/Intent";
    MU(I, "<init>", "(Ljava/lang/String;)V", i_init, 1);
    M(I, "<init>", "()V", i_init);
    M(I, "getAction", "()Ljava/lang/String;", i_getAction);
    M(I, "getExtras", "()Landroid/os/Bundle;", i_getExtras);
    M(I, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;", i_getStringExtra);
    M(I, "getIntExtra", "(Ljava/lang/String;I)I", i_getIntExtra);
    M(I, "getBooleanExtra", "(Ljava/lang/String;Z)Z", i_getBoolExtra);
    M(I, "hasExtra", "(Ljava/lang/String;)Z", i_hasExtra);
    M(I, "setData", "(Landroid/net/Uri;)Landroid/content/Intent;", i_setData);
    M(I, "setDataAndType", "(Landroid/net/Uri;Ljava/lang/String;)Landroid/content/Intent;", i_setData);
    M(I, "getData", "()Landroid/net/Uri;", i_getData);
    M(I, "setPackage", "(Ljava/lang/String;)Landroid/content/Intent;", h_self);
    M(I, "addFlags", "(I)Landroid/content/Intent;", h_self);
    M("android/content/IntentFilter", "<init>", "()V", h_void);
    M("android/content/IntentFilter", "<init>", "(Ljava/lang/String;)V", h_void);
    M("android/content/IntentFilter", "addAction", "(Ljava/lang/String;)V", h_void);
    const char *BU = "android/os/Bundle";
    M(BU, "<init>", "()V", b_init);
    M(BU, "containsKey", "(Ljava/lang/String;)Z", b_contains);
    M(BU, "getString", "(Ljava/lang/String;)Ljava/lang/String;", b_getString);
    MU(BU, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", b_getString, 1);
    M(BU, "getBoolean", "(Ljava/lang/String;)Z", b_getBoolean);
    MU(BU, "getBoolean", "(Ljava/lang/String;Z)Z", b_getBoolean, 1);
    M(BU, "getInt", "(Ljava/lang/String;)I", b_getInt);
    MU(BU, "getInt", "(Ljava/lang/String;I)I", b_getInt, 1);
    M(BU, "getFloat", "(Ljava/lang/String;)F", b_getFloat);
    MU(BU, "getFloat", "(Ljava/lang/String;F)F", b_getFloat, 1);
    M(BU, "putString", "(Ljava/lang/String;Ljava/lang/String;)V", b_putString);
    M(BU, "putInt", "(Ljava/lang/String;I)V", b_putInt);
    M(BU, "putBoolean", "(Ljava/lang/String;Z)V", b_putBoolean);
    M(BU, "isEmpty", "()Z", b_isEmpty);

    /* SharedPreferences */
    const char *SP = "android/content/SharedPreferences", *ED = "android/content/SharedPreferences$Editor";
    M(SP, "contains", "(Ljava/lang/String;)Z", sp_contains);
    M(SP, "edit", "()Landroid/content/SharedPreferences$Editor;", sp_edit);
    M(SP, "getAll", "()Ljava/util/Map;", sp_getAll);
    M(SP, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", sp_getString);
    MU(SP, "getInt", "(Ljava/lang/String;I)I", sp_getNum, 'i');
    MU(SP, "getBoolean", "(Ljava/lang/String;Z)Z", sp_getNum, 'b');
    MU(SP, "getFloat", "(Ljava/lang/String;F)F", sp_getNum, 'f');
    MU(SP, "getLong", "(Ljava/lang/String;J)J", sp_getNum, 'l');
    MU(ED, "putString", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;", ed_put, 's');
    MU(ED, "putInt", "(Ljava/lang/String;I)Landroid/content/SharedPreferences$Editor;", ed_put, 'i');
    MU(ED, "putBoolean", "(Ljava/lang/String;Z)Landroid/content/SharedPreferences$Editor;", ed_put, 'b');
    MU(ED, "putFloat", "(Ljava/lang/String;F)Landroid/content/SharedPreferences$Editor;", ed_put, 'f');
    MU(ED, "putLong", "(Ljava/lang/String;J)Landroid/content/SharedPreferences$Editor;", ed_put, 'l');
    M(ED, "remove", "(Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;", ed_remove);
    M(ED, "clear", "()Landroid/content/SharedPreferences$Editor;", ed_clear);
    M(ED, "commit", "()Z", ed_commit);
    M(ED, "apply", "()V", ed_commit);

    /* services */
    M("android/os/Vibrator", "hasVibrator", "()Z", vib_has);
    M("android/os/Vibrator", "vibrate", "(J)V", vib_vibrate);
    M("android/os/Vibrator", "cancel", "()V", vib_cancel);
    const char *AU = "android/media/AudioManager";
    M(AU, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", am_getProperty);
    MU(AU, "requestAudioFocus", "(Landroid/media/AudioManager$OnAudioFocusChangeListener;II)I", h_int, 1);
    MU(AU, "requestAudioFocus", "(Landroid/media/AudioFocusRequest;)I", h_int, 1);
    MU(AU, "abandonAudioFocus", "(Landroid/media/AudioManager$OnAudioFocusChangeListener;)I", h_int, 1);
    MU(AU, "abandonAudioFocusRequest", "(Landroid/media/AudioFocusRequest;)I", h_int, 1);
    M(AU, "isBluetoothA2dpOn", "()Z", h_false);
    MU(AU, "getStreamVolume", "(I)I", h_int, 10);
    MU(AU, "getStreamMaxVolume", "(I)I", h_int, 15);
    MU(AU, "getRingerMode", "()I", h_int, 2);
    SM("android/media/AudioSystem", "getPrimaryOutputSamplingRate", "()I", as_rate);
    SM("android/media/AudioSystem", "getPrimaryOutputFrameCount", "()I", as_frames);
    M("android/media/AudioAttributes$Builder", "<init>", "()V", h_void);
    M("android/media/AudioAttributes$Builder", "setContentType", "(I)Landroid/media/AudioAttributes$Builder;", h_self);
    M("android/media/AudioAttributes$Builder", "setUsage", "(I)Landroid/media/AudioAttributes$Builder;", h_self);
    M("android/media/AudioFocusRequest$Builder", "<init>", "(I)V", h_void);
    M("android/media/AudioFocusRequest$Builder", "setAcceptsDelayedFocusGain", "(Z)Landroid/media/AudioFocusRequest$Builder;", h_self);
    M("android/media/AudioFocusRequest$Builder", "setAudioAttributes", "(Landroid/media/AudioAttributes;)Landroid/media/AudioFocusRequest$Builder;", h_self);
    M("android/media/AudioFocusRequest$Builder", "setOnAudioFocusChangeListener", "(Landroid/media/AudioManager$OnAudioFocusChangeListener;)Landroid/media/AudioFocusRequest$Builder;", h_self);
    M("android/net/ConnectivityManager", "getActiveNetworkInfo", "()Landroid/net/NetworkInfo;", h_null);   /* offline */

    /* misc framework */
    M("java/util/Locale", "getLanguage", "()Ljava/lang/String;", loc_lang);
    M("java/util/Locale", "getCountry", "()Ljava/lang/String;", loc_country);
    M("java/util/Locale", "toString", "()Ljava/lang/String;", loc_str);
    SMU("java/util/Locale", "getDefault", "()Ljava/util/Locale;", h_obj, &g_locale);
    SMU("android/os/Environment", "getExternalStorageState", "()Ljava/lang/String;", h_str, "mounted");
    SM("android/os/Environment", "getExternalStorageDirectory", "()Ljava/io/File;", env_extDir);
    SM("android/os/SystemClock", "uptimeMillis", "()J", clock_uptime);
    SM("android/os/SystemClock", "elapsedRealtime", "()J", clock_uptime);
    SM("android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;", uri_parse);
    SM("android/net/Uri", "fromFile", "(Ljava/io/File;)Landroid/net/Uri;", uri_fromFile);
    SM("android/net/Uri", "encode", "(Ljava/lang/String;)Ljava/lang/String;", uri_identity);
    SM("android/net/Uri", "decode", "(Ljava/lang/String;)Ljava/lang/String;", uri_identity);
    M("android/net/Uri", "toString", "()Ljava/lang/String;", uri_toString);
    M("android/net/Uri", "isRelative", "()Z", uri_isRelative);
    SMU("android/webkit/MimeTypeMap", "getSingleton", "()Landroid/webkit/MimeTypeMap;", h_obj, &g_mime);
    SM("android/webkit/MimeTypeMap", "getFileExtensionFromUrl", "(Ljava/lang/String;)Ljava/lang/String;", mime_ext);
    M("android/webkit/MimeTypeMap", "getMimeTypeFromExtension", "(Ljava/lang/String;)Ljava/lang/String;", mime_type);
    M("android/content/res/AssetManager", "open", "(Ljava/lang/String;)Ljava/io/InputStream;", asset_open);
    M("java/io/InputStream", "close", "()V", is_close);
    M("java/util/Scanner", "<init>", "(Ljava/io/InputStream;Ljava/lang/String;)V", scanner_init);
    M("java/util/Scanner", "<init>", "(Ljava/io/InputStream;)V", scanner_init);
    M("java/util/Scanner", "useDelimiter", "(Ljava/lang/String;)Ljava/util/Scanner;", h_self);
    M("java/util/Scanner", "next", "()Ljava/lang/String;", scanner_next);
    M("java/util/Scanner", "close", "()V", h_void);
    const char *ZF = "java/util/zip/ZipFile";
    M(ZF, "<init>", "(Ljava/lang/String;)V", zf_init);
    M(ZF, "getEntry", "(Ljava/lang/String;)Ljava/util/zip/ZipEntry;", zf_getEntry);
    M(ZF, "getInputStream", "(Ljava/util/zip/ZipEntry;)Ljava/io/InputStream;", zf_getInputStream);
    M(ZF, "close", "()V", h_void);
    M("java/util/zip/ZipEntry", "getName", "()Ljava/lang/String;", ze_getName);
    M("android/graphics/drawable/ColorDrawable", "<init>", "(I)V", h_void);
    SMU("android/graphics/ImageFormat", "getBitsPerPixel", "(I)I", h_int, 12);
    M("android/app/ActivityManager", "getMemoryClass", "()I", h_int);
    fj_method_define("android/app/ActivityManager", "getMemoryClass", "()I", 0, h_int, (void *)256);
    fj_method_define("android/app/ActivityManager", "getLargeMemoryClass", "()I", 0, h_int, (void *)512);

    /* files */
    const char *F = "java/io/File";
    M(F, "<init>", "(Ljava/lang/String;)V", f_init_s);
    M(F, "<init>", "(Ljava/lang/String;Ljava/lang/String;)V", f_init_ss);
    MU(F, "<init>", "(Ljava/io/File;Ljava/lang/String;)V", f_init_ss, 1);
    M(F, "getAbsolutePath", "()Ljava/lang/String;", f_path);
    M(F, "getCanonicalPath", "()Ljava/lang/String;", f_path);
    M(F, "getPath", "()Ljava/lang/String;", f_path);
    M(F, "toString", "()Ljava/lang/String;", f_path);
    M(F, "getName", "()Ljava/lang/String;", f_name);
    M(F, "getParent", "()Ljava/lang/String;", f_parent);
    MU(F, "getParentFile", "()Ljava/io/File;", f_parent, 1);
    M(F, "exists", "()Z", f_exists);
    M(F, "isDirectory", "()Z", f_isdir);
    M(F, "isFile", "()Z", f_isfile);
    M(F, "canRead", "()Z", f_exists);
    M(F, "canWrite", "()Z", f_exists);
    M(F, "length", "()J", f_length);
    M(F, "mkdir", "()Z", f_mkdir);
    M(F, "mkdirs", "()Z", f_mkdirs);
    M(F, "delete", "()Z", f_delete);
    M(F, "getUsableSpace", "()J", f_space);
    M(F, "getFreeSpace", "()J", f_space);
    MU(F, "getTotalSpace", "()J", f_space, 1);

    /* java.lang */
    M("java/lang/ClassLoader", "findLibrary", "(Ljava/lang/String;)Ljava/lang/String;", cl_findLibrary);
    M("java/lang/ClassLoader", "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", cl_loadClass);
    MU("java/lang/Class", "getClassLoader", "()Ljava/lang/ClassLoader;", h_obj, &g_class_loader);
    SM("java/lang/System", "load", "(Ljava/lang/String;)V", sys_load);
    SMU("java/lang/System", "loadLibrary", "(Ljava/lang/String;)V", sys_load, 1);
    SM("java/lang/System", "nanoTime", "()J", sys_nanoTime);
    SM("java/lang/System", "currentTimeMillis", "()J", sys_millis);
    SM("java/lang/System", "identityHashCode", "(Ljava/lang/Object;)I", sys_idHash);
    SM("java/lang/System", "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", sys_getProperty);
    SMU("java/lang/Runtime", "getRuntime", "()Ljava/lang/Runtime;", h_obj, &g_runtime);
    M("java/lang/Runtime", "availableProcessors", "()I", rt_procs);
    M("java/lang/Runtime", "maxMemory", "()J", rt_maxMem);
    SM("java/lang/Thread", "currentThread", "()Ljava/lang/Thread;", thr_current);
    M("java/lang/Thread", "setName", "(Ljava/lang/String;)V", thr_setName);
    M("java/lang/Thread", "setPriority", "(I)V", h_void);
    M("java/lang/Thread", "isAlive", "()Z", h_true);
    M("java/lang/Thread", "join", "()V", h_void);
    M("java/lang/Thread", "start", "()V", h_void);
    SM("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", int_parse);
    M("java/lang/Number", "floatValue", "()F", num_floatValue);
    M("java/lang/Number", "intValue", "()I", num_intValue);
    M("java/lang/String", "<init>", "()V", h_void);
    M("java/lang/StringBuilder", "<init>", "()V", sb_init);
    M("java/lang/StringBuilder", "append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;", sb_append);
    M("java/lang/StringBuilder", "toString", "()Ljava/lang/String;", sb_toString);
    M("java/nio/Buffer", "clear", "()Ljava/nio/Buffer;", h_self);

    /* collections */
    const char *AL = "java/util/ArrayList", *HS = "java/util/HashSet", *AC = "java/util/AbstractCollection";
    M(AL, "<init>", "()V", coll_init);
    M(HS, "<init>", "()V", coll_init);
    M(AC, "size", "()I", coll_size);
    M(AC, "isEmpty", "()Z", coll_isEmpty);
    M(AC, "contains", "(Ljava/lang/Object;)Z", coll_contains);
    M(AC, "iterator", "()Ljava/util/Iterator;", coll_iterator);
    M(AC, "add", "(Ljava/lang/Object;)Z", coll_add);
    MU(HS, "add", "(Ljava/lang/Object;)Z", coll_add, 1);
    M(AC, "addAll", "(Ljava/util/Collection;)Z", coll_addAll);
    MU(HS, "addAll", "(Ljava/util/Collection;)Z", coll_addAll, 1);
    M(AL, "get", "(I)Ljava/lang/Object;", coll_get);
    M("java/util/Iterator$Impl", "hasNext", "()Z", it_hasNext);
    M("java/util/Iterator$Impl", "next", "()Ljava/lang/Object;", it_next);
    M("java/util/HashMap", "entrySet", "()Ljava/util/Set;", map_entrySet);

    /* Looper / Handler / Message / HandlerThread */
    SM("android/os/Looper", "getMainLooper", "()Landroid/os/Looper;", lp_main);
    SM("android/os/Looper", "myLooper", "()Landroid/os/Looper;", lp_my);
    SM("android/os/Looper", "prepare", "()V", lp_prepare);
    SM("android/os/Looper", "loop", "()V", lp_loop);
    M("android/os/Looper", "quit", "()V", lp_quit);
    M("android/os/Looper", "quitSafely", "()V", lp_quit);
    const char *HN = "android/os/Handler";
    M(HN, "<init>", "()V", h_init);
    MU(HN, "<init>", "(Landroid/os/Looper;)V", h_init, 1);
    MU(HN, "<init>", "(Landroid/os/Looper;Landroid/os/Handler$Callback;)V", h_init, 2);
    M(HN, "getLooper", "()Landroid/os/Looper;", h_getLooper);
    M(HN, "post", "(Ljava/lang/Runnable;)Z", h_post);
    MU(HN, "postDelayed", "(Ljava/lang/Runnable;J)Z", h_post, 1);
    M(HN, "obtainMessage", "(I)Landroid/os/Message;", h_obtain);
    MU(HN, "obtainMessage", "(III)Landroid/os/Message;", h_obtain, 1);
    MU(HN, "obtainMessage", "(ILjava/lang/Object;)Landroid/os/Message;", h_obtain, 2);
    M(HN, "sendMessage", "(Landroid/os/Message;)Z", h_sendMessage);
    M("android/os/Message", "sendToTarget", "()V", msg_send);
    M("android/os/Message", "getTarget", "()Landroid/os/Handler;", msg_getTarget);
    fj_field_define("android/os/Message", "what", "I", 0, fj_i(0));
    fj_field_define("android/os/Message", "arg1", "I", 0, fj_i(0));
    fj_field_define("android/os/Message", "arg2", "I", 0, fj_i(0));
    const char *HT = "android/os/HandlerThread";
    M(HT, "<init>", "(Ljava/lang/String;)V", ht_init);
    M(HT, "<init>", "(Ljava/lang/String;I)V", ht_init);
    M(HT, "start", "()V", ht_start);
    M(HT, "getLooper", "()Landroid/os/Looper;", ht_getLooper);
    M(HT, "quit", "()Z", ht_quit);
    M(HT, "quitSafely", "()Z", ht_quit);
    M(HT, "isAlive", "()Z", ht_isAlive);
    M(HT, "join", "()V", ht_join);

    /* UnityPlayer: Java methods called from native code */
    M(UP_CLS, "executeGLThreadJobs", "()V", up_execGL);
    M(UP_CLS, "disableLogger", "()V", h_void);
    M(UP_CLS, "kill", "()V", up_kill);
    M(UP_CLS, "isFinishing", "()Z", up_isFinishing);
    MU(UP_CLS, "getSplashMode", "()I", h_int, 0);
    M(UP_CLS, "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;)V", up_showSoftInput);
    M(UP_CLS, "hideSoftInput", "()V", h_void);
    M(UP_CLS, "setSoftInputStr", "(Ljava/lang/String;)V", h_void);
    M(UP_CLS, "reportSoftInputStr", "(Ljava/lang/String;IZ)V", up_reportSoftInputStr);
    M(UP_CLS, "addPhoneCallListener", "()V", h_void);
    M(UP_CLS, "toggleGyroscopeSensor", "(Z)V", h_void);
    M(UP_CLS, "initializeGoogleVr", "()Z", h_false);
    M(UP_CLS, "initializeGoogleAr", "()Z", h_false);
    M(UP_CLS, "showVideoPlayer", "(Ljava/lang/String;IIIZII)Z", up_showVideo);
    M(UP_CLS, "loadLibrary", "(Ljava/lang/String;)Z", up_loadLibrary);
    M(UP_CLS, "getView", "()Landroid/view/View;", h_self);
    MU(UP_CLS, "getSettings", "()Landroid/os/Bundle;", h_obj, &g_empty_bundle);
    M(UP_CLS, "reportError", "(Ljava/lang/String;Ljava/lang/String;)V", up_reportError);
    M(UP_CLS, "addViewToPlayer", "(Landroid/view/View;Z)Z", h_true);
    M(UP_CLS, "removeViewFromPlayer", "(Landroid/view/View;)V", h_void);
    SM(UP_CLS, "UnitySendMessage", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", up_sendMessage);
    fj_field_define(UP_CLS, "currentActivity", "Landroid/app/Activity;", 1, fj_l(NULL));
    /* added after 2017.4 (UnityPlayerJavaWrapper of 2018.4-2022.3) */
    M(UP_CLS, "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;IZ)V", up_showSoftInput);    /* 2018.4 */
    M(UP_CLS, "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;IZZ)V", up_showSoftInput);   /* 2019.4+ */
    M(UP_CLS, "setCharacterLimit", "(I)V", h_void);
    M(UP_CLS, "setHideInputField", "(Z)V", h_void);
    M(UP_CLS, "setSelection", "(II)V", h_void);
    M(UP_CLS, "getKeyboardLayout", "()Ljava/lang/String;", h_null);
    M(UP_CLS, "getClipboardText", "()Ljava/lang/String;", up_getClipboard);
    M(UP_CLS, "setClipboardText", "(Ljava/lang/String;)V", up_setClipboard);
    M(UP_CLS, "requestUserAuthorization", "(Ljava/lang/String;)V", h_void);   /* every permission is granted */
    M(UP_CLS, "skipPermissionsDialog", "()Z", h_false);
    M(UP_CLS, "getLaunchURL", "()Ljava/lang/String;", h_null);                /* no deep link */
    M(UP_CLS, "getNetworkConnectivity", "()I", h_int);                        /* NetworkReachability.NotReachable */
    M(UP_CLS, "getNetworkProxySettings", "(Ljava/lang/String;)Ljava/lang/String;", h_null);
    M(UP_CLS, "isUaaLUseCase", "()Z", h_false);                               /* a game, not Unity as a Library */
    M(UP_CLS, "getUaaLLaunchProcessType", "()I", h_int);
    M(UP_CLS, "notifyOnUnityPlayerUnloaded", "()V", up_unloaded);
    M(UP_CLS, "pauseJavaAndCallUnloadCallback", "()V", up_unloaded);
    M(UP_CLS, "startOrientationListener", "(I)Z", h_false);                   /* no orientation sensor */
    M(UP_CLS, "stopOrientationListener", "()Z", h_false);
    M(UP_CLS, "getScreenBrightness", "()F", up_getBrightness);
    M(UP_CLS, "setScreenBrightness", "(F)V", up_setBrightness);
    M(UP_CLS, "hidePreservedContent", "()V", up_hidePreserved);
    MU(UP_CLS, "setMainSurfaceViewAspectRatio", "(F)V", up_logged, "setMainSurfaceViewAspectRatio");
    M(UP_CLS, "startActivityIndicator", "(I)V", h_void);
    M(UP_CLS, "stopActivityIndicator", "()V", h_void);
    MU(UP_CLS, "setBackButtonLeavesApp", "(Z)V", up_logged, "setBackButtonLeavesApp");
    M(UP_CLS, "runOnUiThread", "(Ljava/lang/Runnable;)V", act_runOnUiThread);
    MU(UP_CLS, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;", h_obj, &g_appinfo);

    /* Android framework members the 2018.4-2022.3 players bind */
    MU(D, "getAppVsyncOffsetNanos", "()J", h_long, 0);
    M(D, "getPresentationDeadlineNanos", "()J", disp_deadline);
    M(D, "isWideColorGamut", "()Z", h_false);
    M(D, "isHdr", "()Z", h_false);
    M(D, "getHdrCapabilities", "()Landroid/view/Display$HdrCapabilities;", h_null);
    M(D, "getSupportedModes", "()[Landroid/view/Display$Mode;", disp_modes);
    MU(D, "getMode", "()Landroid/view/Display$Mode;", h_obj, &g_display_mode);
    const char *DMODE = "android/view/Display$Mode";
    CLS(DMODE, NULL);
    MU(DMODE, "getModeId", "()I", h_int, 1);
    M(DMODE, "getPhysicalWidth", "()I", disp_width);
    M(DMODE, "getPhysicalHeight", "()I", disp_height);
    M(DMODE, "getRefreshRate", "()F", disp_refresh);
    M("android/content/res/Configuration", "isScreenWideColorGamut", "()Z", h_false);
    M(V, "getDisplay", "()Landroid/view/Display;", view_getDisplay);
    M(V, "getLeft", "()I", h_int);
    M(V, "getTop", "()I", h_int);
    MU(V, "getRootView", "()Landroid/view/View;", h_obj, &g_decor);
    M(V, "getRootWindowInsets", "()Landroid/view/WindowInsets;", h_null);   /* no cutout, no insets */
    M(V, "onApplyWindowInsets", "(Landroid/view/WindowInsets;)Landroid/view/WindowInsets;", h_arg0);
    M(V, "requestApplyInsets", "()V", h_void);
    M(V, "setOnApplyWindowInsetsListener", "(Landroid/view/View$OnApplyWindowInsetsListener;)V", h_void);
    M(V, "addOnLayoutChangeListener", "(Landroid/view/View$OnLayoutChangeListener;)V", h_void);
    M(V, "removeOnLayoutChangeListener", "(Landroid/view/View$OnLayoutChangeListener;)V", h_void);
    M(V, "setOnTouchListener", "(Landroid/view/View$OnTouchListener;)V", h_void);
    M(V, "setPointerIcon", "(Landroid/view/PointerIcon;)V", h_void);
    M(W, "setSustainedPerformanceMode", "(Z)V", h_void);
    M("android/os/PowerManager", "isSustainedPerformanceModeSupported", "()Z", h_false);
    SM("android/os/Process", "setThreadPriority", "(II)V", proc_setPriority);
    M(A, "findViewById", "(I)Landroid/view/View;", h_null);
    M(A, "isInMultiWindowMode", "()Z", h_false);
    M(A, "moveTaskToBack", "(Z)Z", act_moveTaskToBack);
    fj_field_define("android/content/pm/ApplicationInfo", "splitPublicSourceDirs", "[Ljava/lang/String;", 0, fj_l(NULL));
    fj_field_define("android/content/pm/PackageInfo", "reqFeatures", "[Landroid/content/pm/FeatureInfo;", 0, fj_l(NULL));
    M("android/content/pm/PackageInfo", "getLongVersionCode", "()J", pi_longVersion);
    M(PM, "queryIntentActivities", "(Landroid/content/Intent;I)Ljava/util/List;", pm_queryIntents);
    M(I, "addCategory", "(Ljava/lang/String;)Landroid/content/Intent;", h_self);
    M("android/content/DialogInterface", "dismiss", "()V", h_void);
    M("android/media/AudioAttributes$Builder", "build", "()Landroid/media/AudioAttributes;", ab_build);
    M("android/media/AudioFocusRequest$Builder", "build", "()Landroid/media/AudioFocusRequest;", afr_build);
    M(AU, "getDevices", "(I)[Landroid/media/AudioDeviceInfo;", am_getDevices);
    MU("android/media/AudioDeviceInfo", "getType", "()I", h_int, 2);
    M(AU, "startBluetoothSco", "()V", h_void);
    M(AU, "stopBluetoothSco", "()V", h_void);
    SM("android/provider/Settings$System", "getString", "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;", h_null);
    SM("android/provider/Settings$Global", "getString", "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;", set_global_getString);
    M("android/hardware/SensorManager", "getDefaultSensor", "(I)Landroid/hardware/Sensor;", h_null);   /* sensors: NDK ASensor */
    SM("java/net/NetworkInterface", "getByName", "(Ljava/lang/String;)Ljava/net/NetworkInterface;", h_null);

    /* other classes of the player's Java side that libunity uses (2019.4+) */
    CLS("com/unity3d/player/OrientationLockListener", NULL);   /* 2020.3+: its native, see up_start */
    SM("com/unity3d/player/MultiWindowSupport", "isInMultiWindowMode", "(Landroid/app/Activity;)Z", h_false);
    SM("com/unity3d/player/UnityPermissions", "skipPermissionsDialog", "(Landroid/app/Activity;)Z", h_false);
    /* Play Asset Delivery: what the wrapper answers in an app built without the Play Core
       library (Unity data and StreamingAssets come from the APK/OBB, not from asset packs) */
    const char *PAD = "com/unity3d/player/PlayAssetDeliveryUnityWrapper";
    SM(PAD, "init", "(Landroid/content/Context;)Lcom/unity3d/player/PlayAssetDeliveryUnityWrapper;", pad_init);
    M(PAD, "playCoreApiMissing", "()Z", h_true);
    M(PAD, "getAssetPackPath", "(Ljava/lang/String;)Ljava/lang/String;", h_null);
    M(PAD, "getAssetPackState", "(Ljava/lang/String;Lcom/unity3d/player/IAssetPackManagerStatusQueryCallback;)V", pad_logged);
    M(PAD, "registerDownloadStatusListener", "(Lcom/unity3d/player/IAssetPackManagerDownloadStatusCallback;)Ljava/lang/Object;", h_null);
    M(PAD, "unregisterDownloadStatusListener", "(Ljava/lang/Object;)V", h_void);
    M("com/unity3d/player/UnityCoreAssetPacksStatusCallbacks", "<init>", "()V", h_void);

    /* Android Frame Pacing (Swappy, 2019.4+) inside libunity finds its Java helpers
     * (ChoreographerCallback, SwappyDisplayManager: up_input.c) through the activity's class
     * loader, and else would load them from its embedded dex (InMemoryDexClassLoader, or
     * PathClassLoader on a temporary file before API 26), which cannot run here: those loaders are
     * absent. Swappy switches itself off where EGL lacks eglPresentationTimeANDROID (egl_glue.c);
     * Unity then paces frames as 2017.4 does (vsync + Choreographer through JNIBridge). */
    fj_class_absent("dalvik/system/InMemoryDexClassLoader");
    fj_class_absent("dalvik/system/PathClassLoader");

    SM(RH, "getMethodID", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Method;", rh_getMethodID);
    SM(RH, "getFieldID", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Field;", rh_getFieldID);
    SM(RH, "getConstructorID", "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/reflect/Constructor;", rh_getCtorID);
    SM(RH, "getFieldSignature", "(Ljava/lang/reflect/Field;)Ljava/lang/String;", rh_fieldSig);
    SMU(RH, "newProxyInstance", "(ILjava/lang/Class;)Ljava/lang/Object;", rh_newProxy, 1);
    SMU(RH, "newProxyInstance", "(I[Ljava/lang/Class;)Ljava/lang/Object;", rh_newProxy, 0);
    SMU(RH, "newProxyInstance", "(JLjava/lang/Class;)Ljava/lang/Object;", rh_newProxy, 4 | 1);
    SMU(RH, "newProxyInstance", "(J[Ljava/lang/Class;)Ljava/lang/Object;", rh_newProxy, 4);
    SMU(RH, "newProxyInstance", "(Lcom/unity3d/player/UnityPlayer;JLjava/lang/Class;)Ljava/lang/Object;", rh_newProxy, 4 | 2 | 1);
    SMU(RH, "newProxyInstance", "(Lcom/unity3d/player/UnityPlayer;J[Ljava/lang/Class;)Ljava/lang/Object;", rh_newProxy, 4 | 2);
    SM(RH, "endUnityLaunch", "()V", rh_endLaunch);
    SM(RH, "beginProxyCall", "(J)Z", rh_beginCall);
    SM(RH, "endProxyCall", "()V", h_void);
    SM(RH, "setNativeExceptionOnProxy", "(Ljava/lang/Object;JZ)V", rh_setException);
    SM(RH, "createInvocationError", "(JZ)Ljava/lang/Object;", rh_invocationError);
    CLS(RH_ERROR, "java/lang/Error");
    fj_set_proxy_handler(rh_invoke);

    extern void up_input_register(void);
    up_input_register();
}

void up_android_create_objects(void)
{
#define NEWG(var, cls) var = fj_global(fj_new_by_name(cls, NULL, NULL))
    fj_push_frame();
    NEWG(g_activity, "com/unity3d/player/UnityPlayerActivity");
    NEWG(g_player, UP_CLS);
    fj_static_object(UP_CLS, "currentActivity", "Landroid/app/Activity;", g_activity);
    NEWG(g_surfaceview, "android/view/SurfaceView");
    NEWG(g_holder, "android/view/SurfaceHolder");
    g_surface = fj_global(fj_new_by_name("android/view/Surface", UPC.native_window, NULL));
    NEWG(g_window, "android/view/Window");
    NEWG(g_decor, "android/view/View");
    NEWG(g_layout_params, "android/view/WindowManager$LayoutParams");
    NEWG(g_wm, "android/view/WindowManager");
    NEWG(g_display, "android/view/Display");
    NEWG(g_resources, "android/content/res/Resources");
    NEWG(g_configuration, "android/content/res/Configuration");
    fj_set_int(g_configuration, "orientation", 2);          /* ORIENTATION_LANDSCAPE */
    fj_set_int(g_configuration, "touchscreen", 1);          /* TOUCHSCREEN_NOTOUCH */
    fj_set_int(g_configuration, "keyboard", 1);             /* KEYBOARD_NOKEYS */
    fj_set_int(g_configuration, "navigation", 2);           /* NAVIGATION_DPAD */
    fj_set_int(g_configuration, "uiMode", 0x11);            /* NORMAL | NIGHT_NO */
    fj_set_int(g_configuration, "screenLayout", 0x13);
    fj_set_int(g_configuration, "densityDpi", UPC.density_dpi);
    NEWG(g_locale, "java/util/Locale");
    fj_set_obj(g_configuration, "locale", "Ljava/util/Locale;", g_locale);
    NEWG(g_pm, "android/content/pm/PackageManager");
    g_assets = fj_global(fj_new_by_name("android/content/res/AssetManager", UPC.asset_manager, NULL));
    NEWG(g_vibrator, "android/os/Vibrator");
    NEWG(g_audio, "android/media/AudioManager");
    NEWG(g_conn, "android/net/ConnectivityManager");
    NEWG(g_content_resolver, "android/content/ContentResolver");
    NEWG(g_class_loader, "java/lang/ClassLoader");
    NEWG(g_display_mgr, "android/hardware/display/DisplayManager");
    NEWG(g_media_router, "android/media/MediaRouter");
    NEWG(g_media_route, "android/media/MediaRouter$RouteInfo");
    NEWG(g_runtime, "java/lang/Runtime");
    NEWG(g_activity_mgr, "android/app/ActivityManager");
    NEWG(g_mime, "android/webkit/MimeTypeMap");
    NEWG(g_display_mode, "android/view/Display$Mode");
    NEWG(g_power, "android/os/PowerManager");
    up_g_input_mgr = fj_global(fj_new_by_name("android/hardware/input/InputManager", NULL, NULL));
    g_input_mgr_obj = up_g_input_mgr;
    g_empty_bundle = fj_global(bundle_new());

    /* metaData bundle + ApplicationInfo / PackageInfo */
    g_meta_bundle = fj_global(bundle_new());
    for (const char *const *m = UPC.meta_data; m && *m; m++) {
        const char *eq = strchr(*m, '=');
        if (!eq) continue;
        char k[256];
        snprintf(k, sizeof k, "%.*s", (int)(eq - *m), *m);
        bundle_put(g_meta_bundle, k, eq + 1);
    }
    NEWG(g_appinfo, "android/content/pm/ApplicationInfo");
    fj_set_obj(g_appinfo, "sourceDir", "Ljava/lang/String;", fj_string(UPC.apk_path));
    fj_set_obj(g_appinfo, "publicSourceDir", "Ljava/lang/String;", fj_string(UPC.apk_path));
    fj_set_obj(g_appinfo, "nativeLibraryDir", "Ljava/lang/String;", fj_string(UPC.native_lib_dir));
    fj_set_obj(g_appinfo, "dataDir", "Ljava/lang/String;", fj_string(UPC.data_dir));
    fj_set_obj(g_appinfo, "packageName", "Ljava/lang/String;", fj_string(UPC.package_name));
    fj_set_obj(g_appinfo, "processName", "Ljava/lang/String;", fj_string(UPC.package_name));
    fj_set_obj(g_appinfo, "metaData", "Landroid/os/Bundle;", g_meta_bundle);
    fj_set_int(g_appinfo, "targetSdkVersion", UPC.sdk_int);
    fj_set_int(g_appinfo, "flags", 0);
    NEWG(g_pkginfo, "android/content/pm/PackageInfo");
    fj_set_int(g_pkginfo, "versionCode", UPC.version_code);
    fj_set_obj(g_pkginfo, "versionName", "Ljava/lang/String;", fj_string(UPC.version_name));
    fj_set_obj(g_pkginfo, "packageName", "Ljava/lang/String;", fj_string(UPC.package_name));
    fj_set_long(g_pkginfo, "firstInstallTime", 1500000000000LL);
    fj_set_long(g_pkginfo, "lastUpdateTime", 1500000000000LL);

    /* launch intent (Unity reads the "unity" extra as its command line) */
    g_intent = fj_global(intent_new("android.intent.action.MAIN"));
    if (UPC.command_line) bundle_put(intent_extras(g_intent), "unity", UPC.command_line);

    looper_obj(&L_ui);
    looper_obj(&L_main);
    fj_pop_frame();
#undef NEWG
}
