/*
 * fakejni.c - JavaVM / JNIEnv in C (see fakejni.h for the model).
 *
 * Layout reference: Android libnativehelper include_jni/jni.h (vendored in include/, with the
 * JNI_ABI attribute, softfp on 32-bit ARM, added to every slot). Ideas from FalsoJNI (Vita) and gmloader;
 * the code is our own.
 */
#define _GNU_SOURCE
#include "fakejni.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

/* ======================================================================================= */
/* core types                                                                              */
/* ======================================================================================= */

#define FJ_MAGIC     0x464a4f42u /* 'FJOB' */
#define FJ_MAGIC_M   0x464a4d54u /* 'FJMT' */
#define FJ_MAGIC_F   0x464a4644u /* 'FJFD' */
#define FJ_MAGIC_DEAD 0xdeadbeefu
#define FJ_MAXARGS   32

enum { K_OBJECT = 1, K_CLASS, K_STRING, K_ARRAY };
enum { F_PERM = 1 };

struct fj_class;
struct fj_method;

typedef struct fj_fval {
    struct fj_fval *next;
    struct fj_field *f;
    jvalue v;
} fj_fval;

typedef struct fj_obj {
    uint32_t magic;
    uint16_t kind;
    uint16_t flags;
    int refs;                 /* locals + globals + weak + containment */
    int globals;              /* global + weak refs (for DeleteGlobalRef sanity) */
    struct fj_class *cls;
    void *data;
    void (*free_data)(void *);
    fj_fval *fields;
    union {
        struct { jchar *chars; jsize len; char *utf8; } s;
        struct { char et; int esize; jsize len; void *data; } a;
    } u;
} fj_obj;

typedef struct fj_sig {
    char ret;                 /* V Z B C S I J F D L */
    uint8_t nargs;
    char a[FJ_MAXARGS];
    uint8_t ok;
} fj_sig;

typedef struct fj_method {
    uint32_t magic;
    struct fj_method *next;   /* in class list */
    struct fj_class *cls;
    char *name, *sig;
    fj_sig ps;
    int is_static;
    int placeholder;
    fj_fn fn;
    void *ud;
    void *native_fn;          /* registered native implementation */
    struct fj_method *alias;  /* placeholder resolved to a real method later */
    struct fj_class *cache_cls; struct fj_method *cache_tgt;
    unsigned long calls;
    int warned;
} fj_method;

typedef struct fj_field {
    uint32_t magic;
    struct fj_field *next;
    struct fj_class *cls;
    char *name, *sig;
    char type;                /* Z B C S I J F D L */
    int is_static;
    int placeholder;
    jvalue value;             /* static value / instance default */
    unsigned long gets, sets;
    int warned;
} fj_field;

typedef struct fj_native_rec {
    struct fj_native_rec *next;
    char *name, *sig;
    void *fn;
} fj_native_rec;

typedef struct fj_iface {
    struct fj_iface *next;
    struct fj_class *cls;
} fj_iface;

typedef struct fj_class {
    fj_obj obj;               /* must be first: jclass == &cls->obj */
    struct fj_class *hnext;
    char *name;
    struct fj_class *super;
    fj_iface *ifaces;
    fj_method *methods;
    fj_field *fields;
    fj_native_rec *natives;
    int placeholder;
    int absent;
    int is_array;
    unsigned long lookups;
} fj_class;

typedef struct fj_thread {
    const struct JNINativeInterface *fns;   /* JNIEnv points here */
    fj_obj **loc; int nloc, caploc;
    int *frm; int nfrm, capfrm;
    fj_obj *exc;
    int attached;
    long tid;
} fj_thread;

/* ======================================================================================= */
/* globals                                                                                 */
/* ======================================================================================= */

static pthread_mutex_t G = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#define LOCK()   pthread_mutex_lock(&G)
#define UNLOCK() pthread_mutex_unlock(&G)

static int g_inited, g_trace, g_nofree, g_strict, g_dump;
static FILE *g_log;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_key_t g_tkey;
static __thread fj_thread *t_cur;
static __thread int t_in_init;

#define HBUCKETS 512
static fj_class *g_classes[HBUCKETS];

static const struct JNINativeInterface g_fns;
static const struct JNIInvokeInterface g_ifns;
static const struct JNIInvokeInterface *g_vm_ptr = &g_ifns;

/* well-known classes */
static fj_class *C_Object, *C_Class, *C_String, *C_Throwable, *C_Method, *C_Field,
                *C_ByteBuffer, *C_Proxy;

static void fj_init_once(void);
static fj_class *class_get(const char *name, int create_placeholder);
static fj_obj *O(jobject o);
static const char *descr(jobject o, char *buf, size_t n);

#define TRACE(...) do { if (g_trace) fj_log(__VA_ARGS__); } while (0)

static long gettid_(void) { return (long)syscall(SYS_gettid); }

void fj_log(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    FILE *f = g_log ? g_log : stderr;
    fprintf(f, "[fj %ld] %s\n", gettid_(), buf);
    fflush(f);
}

void fj_set_trace(int on) { fj_init(); g_trace = on; }
int fj_tracing(void) { return g_trace; }

static char *xstrdup(const char *s) { char *r = strdup(s ? s : ""); if (!r) abort(); return r; }
static void *xcalloc(size_t n, size_t s) { void *p = calloc(n ? n : 1, s ? s : 1); if (!p) abort(); return p; }

/* ======================================================================================= */
/* signatures                                                                              */
/* ======================================================================================= */

static const char *skip_type(const char *p)
{
    while (*p == '[') p++;
    if (*p == 'L') { while (*p && *p != ';') p++; if (*p) p++; return p; }
    return *p ? p + 1 : p;
}

static fj_sig parse_sig(const char *sig)
{
    fj_sig s; memset(&s, 0, sizeof s);
    s.ret = 'V';
    if (!sig || *sig != '(') return s;
    const char *p = sig + 1;
    while (*p && *p != ')') {
        char c = *p;
        if (c == '[' || c == 'L') c = 'L';
        else if (!strchr("ZBCSIJFD", c)) return s;
        if (s.nargs >= FJ_MAXARGS) return s;
        s.a[s.nargs++] = c;
        p = skip_type(p);
    }
    if (*p != ')') return s;
    p++;
    char r = *p;
    if (r == '[' || r == 'L') r = 'L';
    else if (!strchr("VZBCSIJFD", r)) return s;
    s.ret = r;
    s.ok = 1;
    return s;
}

static char field_type(const char *sig)
{
    if (!sig || !*sig) return 'I';
    if (*sig == '[' || *sig == 'L') return 'L';
    return *sig;
}

static void decode_va(const fj_sig *s, va_list ap, jvalue *out)
{
    for (int i = 0; i < s->nargs; i++) {
        out[i].j = 0;
        switch (s->a[i]) {
        case 'Z': out[i].z = (jboolean)va_arg(ap, int); break;
        case 'B': out[i].b = (jbyte)va_arg(ap, int); break;
        case 'C': out[i].c = (jchar)va_arg(ap, int); break;
        case 'S': out[i].s = (jshort)va_arg(ap, int); break;
        case 'I': out[i].i = va_arg(ap, jint); break;
        case 'J': out[i].j = va_arg(ap, jlong); break;
        case 'F': out[i].f = (jfloat)va_arg(ap, double); break;
        case 'D': out[i].d = va_arg(ap, double); break;
        default:  out[i].l = va_arg(ap, jobject); break;
        }
    }
}

static void fmt_value(char t, jvalue v, char *buf, size_t n)
{
    switch (t) {
    case 'Z': snprintf(buf, n, "%s", v.z ? "true" : "false"); break;
    case 'B': snprintf(buf, n, "%d", v.b); break;
    case 'C': snprintf(buf, n, "'\\u%04x'", v.c); break;
    case 'S': snprintf(buf, n, "%d", v.s); break;
    case 'I': snprintf(buf, n, "%d", v.i); break;
    case 'J': snprintf(buf, n, "%lldL", (long long)v.j); break;
    case 'F': snprintf(buf, n, "%gf", v.f); break;
    case 'D': snprintf(buf, n, "%g", v.d); break;
    case 'V': snprintf(buf, n, "void"); break;
    default:  descr(v.l, buf, n); break;
    }
}

static void fmt_args(const fj_sig *s, const jvalue *a, char *buf, size_t n)
{
    size_t off = 0;
    buf[0] = 0;
    for (int i = 0; i < s->nargs && off + 4 < n; i++) {
        char one[200];
        fmt_value(s->a[i], a[i], one, sizeof one);
        off += snprintf(buf + off, n - off, "%s%s", i ? ", " : "", one);
    }
}

/* ======================================================================================= */
/* threads and local references                                                            */
/* ======================================================================================= */

static void obj_unref(fj_obj *o);

static void thread_free_locals(fj_thread *t)
{
    while (t->nloc > 0) {
        fj_obj *o = t->loc[--t->nloc];
        if (o) obj_unref(o);
    }
    t->nfrm = 0;
}

static void thread_dtor(void *p)
{
    fj_thread *t = p;
    if (!t) return;
    thread_free_locals(t);
    if (t->exc) obj_unref(t->exc);
    free(t->loc); free(t->frm); free(t);
}

static fj_thread *TH(void)
{
    fj_thread *t = t_cur;
    if (t) return t;
    fj_init();
    t = xcalloc(1, sizeof *t);
    t->fns = &g_fns;
    t->tid = gettid_();
    t->attached = 1;
    t_cur = t;
    pthread_setspecific(g_tkey, t);
    TRACE("thread %ld attached (implicitly)", t->tid);
    return t;
}

static void obj_ref(fj_obj *o) { if (o) __atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL); }

static jobject add_local(fj_obj *o)
{
    if (!o) return NULL;
    fj_thread *t = TH();
    if (t->nloc == t->caploc) {
        t->caploc = t->caploc ? t->caploc * 2 : 256;
        t->loc = realloc(t->loc, t->caploc * sizeof *t->loc);
        if (!t->loc) abort();
    }
    obj_ref(o);
    t->loc[t->nloc++] = o;
    return (jobject)o;
}

static int del_local(fj_obj *o)
{
    fj_thread *t = TH();
    int lo = t->nfrm ? t->frm[t->nfrm - 1] : 0;
    /* search current frame first, then the whole stack (lenient) */
    for (int pass = 0; pass < 2; pass++) {
        int start = pass ? 0 : lo, end = pass ? lo : t->nloc;
        for (int i = end - 1; i >= start; i--) {
            if (t->loc[i] == o) {
                t->loc[i] = NULL;
                if (i == t->nloc - 1) while (t->nloc > lo && !t->loc[t->nloc - 1]) t->nloc--;
                obj_unref(o);
                return 1;
            }
        }
    }
    return 0;
}

static void push_frame(void)
{
    fj_thread *t = TH();
    if (t->nfrm == t->capfrm) {
        t->capfrm = t->capfrm ? t->capfrm * 2 : 32;
        t->frm = realloc(t->frm, t->capfrm * sizeof *t->frm);
        if (!t->frm) abort();
    }
    t->frm[t->nfrm++] = t->nloc;
}

static void pop_frame(void)
{
    fj_thread *t = TH();
    if (!t->nfrm) { TRACE("PopLocalFrame without frame"); return; }
    int mark = t->frm[--t->nfrm];
    while (t->nloc > mark) {
        fj_obj *o = t->loc[--t->nloc];
        if (o) obj_unref(o);
    }
}

void fj_push_frame(void) { push_frame(); }
void fj_pop_frame(void) { pop_frame(); }

/* ======================================================================================= */
/* objects                                                                                 */
/* ======================================================================================= */

static fj_obj *obj_alloc(fj_class *cls, int kind)
{
    fj_obj *o = xcalloc(1, sizeof *o);
    o->magic = FJ_MAGIC;
    o->kind = kind;
    o->cls = cls ? cls : C_Object;
    return o;
}

static void obj_free(fj_obj *o)
{
    if (g_nofree) return;
    if (o->flags & F_PERM) return;
    TRACE("free %p (%s)", (void *)o, o->cls ? o->cls->name : "?");
    switch (o->kind) {
    case K_STRING: free(o->u.s.chars); free(o->u.s.utf8); break;
    case K_ARRAY:
        if (o->u.a.et == 'L') {
            fj_obj **e = o->u.a.data;
            for (jsize i = 0; i < o->u.a.len; i++) if (e[i]) obj_unref(e[i]);
        }
        free(o->u.a.data);
        break;
    default: break;
    }
    fj_fval *fv = o->fields;
    while (fv) {
        fj_fval *n = fv->next;
        if (fv->f->type == 'L' && fv->v.l) obj_unref(O(fv->v.l));
        free(fv);
        fv = n;
    }
    if (o->free_data) o->free_data(o->data);
    o->magic = FJ_MAGIC_DEAD;
    free(o);
}

static void obj_unref(fj_obj *o)
{
    if (!o) return;
    int r = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
    if (r == 0) obj_free(o);
    else if (r < 0) { TRACE("refcount underflow on %p", (void *)o); o->refs = 0; }
}

/* validate a handle coming from native code */
static fj_obj *O(jobject h)
{
    if (!h) return NULL;
    fj_obj *o = (fj_obj *)h;
    if (o->magic != FJ_MAGIC) {
        fj_log("WARNING: invalid object handle %p (magic %08x)", h, o->magic);
        return NULL;
    }
    return o;
}

static fj_class *CL(jclass h)
{
    fj_obj *o = O(h);
    if (!o) return NULL;
    if (o->kind != K_CLASS) {
        /* lenient: an object passed where a class is expected -> use its class */
        return o->cls;
    }
    return (fj_class *)o;
}

static const char *descr(jobject h, char *buf, size_t n)
{
    if (!h) { snprintf(buf, n, "null"); return buf; }
    fj_obj *o = (fj_obj *)h;
    if (o->magic != FJ_MAGIC) { snprintf(buf, n, "<bad %p>", h); return buf; }
    switch (o->kind) {
    case K_CLASS:  snprintf(buf, n, "class %s", ((fj_class *)o)->name); break;
    case K_STRING: snprintf(buf, n, "\"%.120s\"", o->u.s.utf8 ? o->u.s.utf8 : ""); break;
    case K_ARRAY:  snprintf(buf, n, "%s[%d]@%p", o->cls->name, (int)o->u.a.len, h); break;
    default:       snprintf(buf, n, "<%s@%p>", o->cls ? o->cls->name : "?", h); break;
    }
    return buf;
}

jobject fj_new(jclass cls, void *data, void (*free_data)(void *))
{
    fj_init();
    fj_class *c = CL(cls);
    fj_obj *o = obj_alloc(c, K_OBJECT);
    o->data = data; o->free_data = free_data;
    return add_local(o);
}

jobject fj_new_by_name(const char *cls, void *data, void (*free_data)(void *))
{
    fj_init();
    return fj_new((jclass)class_get(cls, 1), data, free_data);
}

void *fj_data(jobject h) { fj_obj *o = O(h); return o ? o->data : NULL; }
void fj_set_data(jobject h, void *data, void (*free_data)(void *))
{
    fj_obj *o = O(h);
    if (o) { o->data = data; o->free_data = free_data; }
}

jobject fj_global(jobject h)
{
    fj_obj *o = O(h);
    if (!o) return NULL;
    obj_ref(o);
    __atomic_add_fetch(&o->globals, 1, __ATOMIC_ACQ_REL);
    return h;
}

void fj_release(jobject h)
{
    fj_obj *o = O(h);
    if (!o) return;
    if (__atomic_load_n(&o->globals, __ATOMIC_ACQUIRE) <= 0) {
        TRACE("DeleteGlobalRef on non-global %p", h);
        return;
    }
    __atomic_sub_fetch(&o->globals, 1, __ATOMIC_ACQ_REL);
    obj_unref(o);
}

/* ======================================================================================= */
/* classes                                                                                 */
/* ======================================================================================= */

static unsigned hash_str(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h % HBUCKETS;
}

static void norm_name(const char *in, char *out, size_t n)
{
    size_t i = 0;
    /* accept "Lfoo/Bar;" too */
    if (in[0] == 'L' && strchr(in, ';') && in[strlen(in) - 1] == ';' && !strchr(in, '.')) {
        in++;
        for (; *in && *in != ';' && i + 1 < n; in++) out[i++] = *in;
        out[i] = 0;
        return;
    }
    for (; *in && i + 1 < n; in++) out[i++] = (*in == '.' && in[0] != '[') ? '/' : *in;
    out[i] = 0;
}

static fj_class *class_lookup_raw(const char *nm)
{
    for (fj_class *c = g_classes[hash_str(nm)]; c; c = c->hnext)
        if (!strcmp(c->name, nm)) return c;
    return NULL;
}

static fj_class *class_new(const char *nm, fj_class *super, int placeholder)
{
    fj_class *c = xcalloc(1, sizeof *c);
    c->obj.magic = FJ_MAGIC;
    c->obj.kind = K_CLASS;
    c->obj.flags = F_PERM;
    c->obj.refs = 1;
    c->obj.cls = C_Class;      /* may be NULL during bootstrap, fixed later */
    c->name = xstrdup(nm);
    c->super = super;
    c->placeholder = placeholder;
    c->is_array = nm[0] == '[';
    unsigned h = hash_str(nm);
    c->hnext = g_classes[h];
    g_classes[h] = c;
    return c;
}

/* get class by name; create a placeholder (logged) when unknown and create_placeholder */
static fj_class *class_get(const char *name, int create_placeholder)
{
    char nm[512];
    if (!name) return NULL;
    norm_name(name, nm, sizeof nm);
    LOCK();
    fj_class *c = class_lookup_raw(nm);
    if (!c && nm[0] == '[') {
        c = class_new(nm, C_Object, 0);   /* array classes are always "real" */
    } else if (!c && create_placeholder) {
        c = class_new(nm, C_Object, 1);
        fj_log("UNKNOWN class %s (placeholder)", nm);
    }
    UNLOCK();
    return c;
}

jclass fj_class_define(const char *name, const char *super_name)
{
    fj_init();
    char nm[512];
    norm_name(name, nm, sizeof nm);
    LOCK();
    fj_class *sup = NULL;
    if (super_name) {
        sup = class_get(super_name, 0);
        if (!sup) sup = (fj_class *)fj_class_define(super_name, NULL);
    } else if (C_Object && strcmp(nm, "java/lang/Object")) {
        sup = C_Object;
    }
    fj_class *c = class_lookup_raw(nm);
    if (!c) c = class_new(nm, sup, 0);
    else {
        if (c->placeholder) TRACE("class %s defined after use as placeholder", nm);
        c->placeholder = 0;
        c->absent = 0;
        if (super_name || !c->super) c->super = sup;
    }
    UNLOCK();
    return (jclass)c;
}

jclass fj_class_find(const char *name)
{
    fj_init();
    fj_class *c = class_get(name, 0);
    return (c && !c->placeholder && !c->absent) ? (jclass)c : NULL;
}

void fj_class_implements(const char *name, const char *iface)
{
    fj_init();
    LOCK();
    fj_class *c = class_get(name, 0);
    if (!c) c = (fj_class *)fj_class_define(name, NULL);
    fj_class *i = class_get(iface, 0);
    if (!i) i = (fj_class *)fj_class_define(iface, NULL);
    fj_iface *e = xcalloc(1, sizeof *e);
    e->cls = i; e->next = c->ifaces; c->ifaces = e;
    UNLOCK();
}

void fj_class_absent(const char *name)
{
    fj_init();
    char nm[512];
    norm_name(name, nm, sizeof nm);
    LOCK();
    fj_class *c = class_lookup_raw(nm);
    if (!c) c = class_new(nm, C_Object, 1);
    c->absent = 1;
    UNLOCK();
}

const char *fj_class_name(jclass h) { fj_class *c = CL(h); return c ? c->name : NULL; }
jclass fj_object_class(jobject h) { fj_obj *o = O(h); if (!o) return NULL; return (jclass)(o->kind == K_CLASS ? C_Class : o->cls); }

static int class_is_sub(fj_class *c, fj_class *target, int depth)
{
    if (!c || !target || depth > 64) return 0;
    if (c == target) return 1;
    if (target == C_Object) return 1;
    for (fj_iface *i = c->ifaces; i; i = i->next)
        if (class_is_sub(i->cls, target, depth + 1)) return 1;
    return class_is_sub(c->super, target, depth + 1);
}

int fj_is_instance(jobject h, const char *cls)
{
    fj_obj *o = O(h);
    fj_class *c = class_get(cls, 0);
    if (!o || !c) return 0;
    return class_is_sub(o->kind == K_CLASS ? C_Class : o->cls, c, 0);
}

/* ======================================================================================= */
/* methods & fields registry                                                               */
/* ======================================================================================= */

static fj_method *method_in(fj_class *c, const char *name, const char *sig, int is_static)
{
    for (fj_method *m = c->methods; m; m = m->next)
        if (m->is_static == is_static && !strcmp(m->name, name) && (!sig || !strcmp(m->sig, sig)))
            return m;
    return NULL;
}

static fj_native_rec *native_in(fj_class *c, const char *name, const char *sig)
{
    for (fj_native_rec *n = c->natives; n; n = n->next)
        if (!strcmp(n->name, name) && (!sig || !strcmp(n->sig, sig))) return n;
    return NULL;
}

/* search c, supers and interfaces for a non-placeholder method */
static fj_method *method_find(fj_class *c, const char *name, const char *sig, int is_static, int depth)
{
    if (!c || depth > 64) return NULL;
    fj_method *m = method_in(c, name, sig, is_static);
    if (m && !m->placeholder) return m;
    fj_method *r = method_find(c->super, name, sig, is_static, depth + 1);
    if (r) return r;
    for (fj_iface *i = c->ifaces; i; i = i->next)
        if ((r = method_find(i->cls, name, sig, is_static, depth + 1))) return r;
    return NULL;
}

static fj_method *method_new(fj_class *c, const char *name, const char *sig, int is_static)
{
    fj_method *m = xcalloc(1, sizeof *m);
    m->magic = FJ_MAGIC_M;
    m->cls = c;
    m->name = xstrdup(name);
    m->sig = xstrdup(sig);
    m->ps = parse_sig(sig);
    if (!m->ps.ok) fj_log("WARNING: cannot parse signature %s.%s%s", c->name, name, sig);
    m->is_static = is_static;
    m->next = c->methods;
    c->methods = m;
    return m;
}

jmethodID fj_method_define(const char *cls, const char *name, const char *sig,
                           int is_static, fj_fn fn, void *ud)
{
    fj_init();
    LOCK();
    fj_class *c = class_get(cls, 0);
    if (!c) c = (fj_class *)fj_class_define(cls, NULL);
    fj_method *m = method_in(c, name, sig, is_static);
    if (!m) m = method_new(c, name, sig, is_static);
    m->placeholder = 0;
    m->fn = fn;
    m->ud = ud;
    UNLOCK();
    return (jmethodID)m;
}

static fj_method *M(jmethodID id)
{
    fj_method *m = (fj_method *)id;
    if (!m) return NULL;
    if (m->magic != FJ_MAGIC_M) { fj_log("WARNING: invalid jmethodID %p", (void *)id); return NULL; }
    return m;
}

static fj_field *field_in(fj_class *c, const char *name, const char *sig, int is_static)
{
    for (fj_field *f = c->fields; f; f = f->next)
        if (f->is_static == is_static && !strcmp(f->name, name) && (!sig || !strcmp(f->sig, sig)))
            return f;
    return NULL;
}

static fj_field *field_find(fj_class *c, const char *name, const char *sig, int is_static, int depth)
{
    if (!c || depth > 64) return NULL;
    fj_field *f = field_in(c, name, sig, is_static);
    if (f && !f->placeholder) return f;
    fj_field *r = field_find(c->super, name, sig, is_static, depth + 1);
    if (r) return r;
    for (fj_iface *i = c->ifaces; i; i = i->next)
        if ((r = field_find(i->cls, name, sig, is_static, depth + 1))) return r;
    return NULL;
}

static fj_field *field_new(fj_class *c, const char *name, const char *sig, int is_static)
{
    fj_field *f = xcalloc(1, sizeof *f);
    f->magic = FJ_MAGIC_F;
    f->cls = c;
    f->name = xstrdup(name);
    f->sig = xstrdup(sig);
    f->type = field_type(sig);
    f->is_static = is_static;
    f->next = c->fields;
    c->fields = f;
    return f;
}

jfieldID fj_field_define(const char *cls, const char *name, const char *sig,
                         int is_static, jvalue init)
{
    fj_init();
    LOCK();
    fj_class *c = class_get(cls, 0);
    if (!c) c = (fj_class *)fj_class_define(cls, NULL);
    fj_field *f = field_in(c, name, sig, is_static);
    if (!f) f = field_new(c, name, sig, is_static);
    f->placeholder = 0;
    if (f->type == 'L') {
        if (init.l) obj_ref(O(init.l));
        if (f->value.l) obj_unref(O(f->value.l));
    }
    f->value = init;
    UNLOCK();
    return (jfieldID)f;
}

static fj_field *FLD(jfieldID id)
{
    fj_field *f = (fj_field *)id;
    if (!f) return NULL;
    if (f->magic != FJ_MAGIC_F) { fj_log("WARNING: invalid jfieldID %p", (void *)id); return NULL; }
    return f;
}

void fj_static_int(const char *c, const char *n, jint v)        { fj_field_define(c, n, "I", 1, fj_i(v)); }
void fj_static_long(const char *c, const char *n, jlong v)      { fj_field_define(c, n, "J", 1, fj_j(v)); }
void fj_static_bool(const char *c, const char *n, jboolean v)   { fj_field_define(c, n, "Z", 1, fj_z(v)); }
void fj_static_float(const char *c, const char *n, jfloat v)    { fj_field_define(c, n, "F", 1, fj_f(v)); }
void fj_static_object(const char *c, const char *n, const char *sig, jobject v)
{
    fj_field_define(c, n, sig, 1, fj_l(v));
}
void fj_static_string(const char *c, const char *n, const char *v)
{
    fj_push_frame();
    fj_field_define(c, n, "Ljava/lang/String;", 1, fj_l(fj_string(v)));
    fj_pop_frame();
}

/* instance field storage */
static jvalue ifield_get(fj_obj *o, fj_field *f)
{
    jvalue v = f->value;
    LOCK();
    for (fj_fval *fv = o->fields; fv; fv = fv->next)
        if (fv->f == f) { v = fv->v; break; }
    UNLOCK();
    return v;
}

static void ifield_set(fj_obj *o, fj_field *f, jvalue v)
{
    LOCK();
    fj_fval *fv;
    for (fv = o->fields; fv; fv = fv->next)
        if (fv->f == f) break;
    if (!fv) {
        fv = xcalloc(1, sizeof *fv);
        fv->f = f;
        fv->next = o->fields;
        o->fields = fv;
        if (f->type == 'L') fv->v.l = NULL;
    }
    if (f->type == 'L') {
        if (v.l) obj_ref(O(v.l));
        if (fv->v.l) obj_unref(O(fv->v.l));
    }
    fv->v = v;
    UNLOCK();
}

static fj_field *field_for_obj(fj_obj *o, const char *name, const char *sig)
{
    LOCK();
    fj_field *f = field_find(o->cls, name, sig, 0, 0);
    if (!f && sig) {
        f = field_in(o->cls, name, sig, 0);
        if (!f) f = field_new(o->cls, name, sig, 0);
    }
    UNLOCK();
    return f;
}

void fj_set_field(jobject h, const char *name, const char *sig, jvalue v)
{
    fj_obj *o = O(h);
    if (!o) return;
    fj_field *f = field_for_obj(o, name, sig);
    if (f) ifield_set(o, f, v);
}

jvalue fj_get_field(jobject h, const char *name, const char *sig)
{
    fj_obj *o = O(h);
    if (!o) return fj_v();
    fj_field *f = field_for_obj(o, name, sig);
    return f ? ifield_get(o, f) : fj_v();
}

/* ======================================================================================= */
/* strings                                                                                 */
/* ======================================================================================= */

/* decode (modified or standard) UTF-8 to UTF-16 */
static jchar *utf8_to_utf16(const char *s, jsize *out_len)
{
    size_t n = strlen(s);
    jchar *u = xcalloc(n + 1, sizeof(jchar));
    jsize k = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p;
        if (c < 0x80) { u[k++] = c; p++; }
        else if ((c & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
            u[k++] = ((c & 0x1f) << 6) | (p[1] & 0x3f); p += 2;
        } else if ((c & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
            u[k++] = ((c & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f); p += 3;
        } else if ((c & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 &&
                   (p[3] & 0xc0) == 0x80) {
            unsigned cp = ((c & 7) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
            cp -= 0x10000;
            u[k++] = 0xd800 + (cp >> 10);
            u[k++] = 0xdc00 + (cp & 0x3ff);
            p += 4;
        } else { u[k++] = 0xfffd; p++; }
    }
    *out_len = k;
    return u;
}

/* modified UTF-8 (JNI) when modified=1, else standard UTF-8 */
static size_t utf16_to_utf8(const jchar *u, jsize len, char *out, int modified)
{
    size_t k = 0;
    for (jsize i = 0; i < len; i++) {
        unsigned c = u[i];
        if (!modified && c >= 0xd800 && c < 0xdc00 && i + 1 < len && u[i + 1] >= 0xdc00 && u[i + 1] < 0xe000) {
            unsigned cp = 0x10000 + ((c - 0xd800) << 10) + (u[i + 1] - 0xdc00);
            i++;
            if (out) { out[k] = 0xf0 | (cp >> 18); out[k + 1] = 0x80 | ((cp >> 12) & 0x3f);
                       out[k + 2] = 0x80 | ((cp >> 6) & 0x3f); out[k + 3] = 0x80 | (cp & 0x3f); }
            k += 4;
        } else if (c != 0 && c < 0x80) { if (out) out[k] = c; k++; }
        else if (c < 0x800) {
            if (out) { out[k] = 0xc0 | (c >> 6); out[k + 1] = 0x80 | (c & 0x3f); }
            k += 2;
        } else {
            if (out) { out[k] = 0xe0 | (c >> 12); out[k + 1] = 0x80 | ((c >> 6) & 0x3f); out[k + 2] = 0x80 | (c & 0x3f); }
            k += 3;
        }
    }
    if (out) out[k] = 0;
    return k;
}

static fj_obj *string_new16(const jchar *u, jsize len, int take)
{
    fj_obj *o = obj_alloc(C_String, K_STRING);
    if (take) o->u.s.chars = (jchar *)u;
    else {
        o->u.s.chars = xcalloc(len + 1, sizeof(jchar));
        if (len) memcpy(o->u.s.chars, u, len * sizeof(jchar));
    }
    o->u.s.len = len;
    size_t n = utf16_to_utf8(o->u.s.chars, len, NULL, 0);
    o->u.s.utf8 = xcalloc(n + 1, 1);
    utf16_to_utf8(o->u.s.chars, len, o->u.s.utf8, 0);
    return o;
}

static fj_obj *string_new(const char *utf8)
{
    jsize len;
    jchar *u = utf8_to_utf16(utf8, &len);
    return string_new16(u, len, 1);
}

jstring fj_string(const char *utf8)
{
    fj_init();
    if (!utf8) return NULL;
    return add_local(string_new(utf8));
}

const char *fj_cstr(jstring h)
{
    fj_obj *o = O(h);
    if (!o || o->kind != K_STRING) return NULL;
    return o->u.s.utf8;
}

static fj_obj *STR(jstring h, const char *who)
{
    fj_obj *o = O(h);
    if (!o) { if (h || g_trace) fj_log("%s: null/invalid string", who); return NULL; }
    if (o->kind != K_STRING) { fj_log("%s: %p is not a string", who, h); return NULL; }
    return o;
}

/* ======================================================================================= */
/* arrays                                                                                  */
/* ======================================================================================= */

static int esize_of(char t)
{
    switch (t) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    case 'J': case 'D': return 8;
    default: return sizeof(void *);
    }
}

static fj_obj *array_new(const char *elem_sig, jsize len)
{
    char cname[512];
    snprintf(cname, sizeof cname, "[%s", elem_sig);
    fj_class *c = class_get(cname, 1);
    fj_obj *o = obj_alloc(c, K_ARRAY);
    o->u.a.et = field_type(elem_sig);
    o->u.a.esize = esize_of(o->u.a.et);
    o->u.a.len = len < 0 ? 0 : len;
    o->u.a.data = xcalloc(o->u.a.len + 1, o->u.a.esize);
    return o;
}

jarray fj_array_new(const char *elem_sig, jsize len) { fj_init(); return add_local(array_new(elem_sig, len)); }

static fj_obj *ARR(jarray h, const char *who)
{
    fj_obj *o = O(h);
    if (!o) { fj_log("%s: null/invalid array", who); return NULL; }
    if (o->kind != K_ARRAY) { fj_log("%s: %p is not an array", who, h); return NULL; }
    return o;
}

jsize fj_array_len(jarray h) { fj_obj *o = O(h); return (o && o->kind == K_ARRAY) ? o->u.a.len : 0; }
void *fj_array_data(jarray h) { fj_obj *o = O(h); return (o && o->kind == K_ARRAY) ? o->u.a.data : NULL; }

static void objarray_set(fj_obj *a, jsize i, jobject v)
{
    fj_obj **e = a->u.a.data;
    fj_obj *nv = O(v);
    LOCK();
    if (nv) obj_ref(nv);
    if (e[i]) obj_unref(e[i]);
    e[i] = nv;
    UNLOCK();
}

jobjectArray fj_string_array(const char *const *strs, int n)
{
    fj_init();
    fj_obj *a = array_new("Ljava/lang/String;", n);
    for (int i = 0; i < n; i++) {
        fj_obj *s = strs[i] ? string_new(strs[i]) : NULL;
        if (s) { obj_ref(s); ((fj_obj **)a->u.a.data)[i] = s; /* owned by the array */ }
    }
    return add_local(a);
}

/* ======================================================================================= */
/* exceptions                                                                              */
/* ======================================================================================= */

static void set_exception(fj_thread *t, fj_obj *e)
{
    if (e) obj_ref(e);
    if (t->exc) obj_unref(t->exc);
    t->exc = e;
}

static fj_obj *exception_new(fj_class *c, const char *msg)
{
    fj_obj *e = obj_alloc(c ? c : C_Throwable, K_OBJECT);
    if (msg) {
        fj_field *f = field_for_obj(e, "detailMessage", "Ljava/lang/String;");
        fj_obj *s = string_new(msg);
        obj_ref(s);
        ifield_set(e, f, fj_l((jobject)s));
        obj_unref(s);
    }
    return e;
}

void fj_throw(JNIEnv *env, const char *cls, const char *msg)
{
    (void)env;
    fj_class *c = class_get(cls ? cls : "java/lang/RuntimeException", 0);
    if (!c) c = (fj_class *)fj_class_define(cls, "java/lang/RuntimeException");
    fj_obj *e = exception_new(c, msg);
    obj_ref(e);
    set_exception(TH(), e);
    obj_unref(e);
    fj_log("exception raised: %s: %s", c->name, msg ? msg : "");
}

/* ======================================================================================= */
/* invocation                                                                              */
/* ======================================================================================= */

/* A native registered by Android code is called with arguments decoded at run time from its
 * JNI signature: they are laid out where the callee's calling convention expects them, then the
 * function is called through a pointer type whose fixed parameter list is long enough for any
 * signature (FJ_MAXARGS arguments after env and self). */
#if defined(__arm__)
/* armeabi-v7a, base AAPCS ("softfp"): every argument is a run of 32-bit words in r0-r3, then on
 * the stack; 64-bit values start at an even word (register pair / 8-byte aligned slot). The
 * result comes back in r0 or r0:r1. */
typedef uint64_t (__attribute__((pcs("aapcs"))) *fn_words_t)(
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
#define NWORDS 40
#elif defined(__aarch64__)
/* arm64-v8a, AAPCS64 (Android and Linux use the same one): the first 8 integer and pointer
 * arguments go in x0-x7, the first 8 float and double arguments in v0-v7 (a float as single
 * precision in s<n>, not promoted), and every argument after its register class has run out
 * takes the next 8-byte stack slot, in argument order. A pointer type taking 8 integers, 8
 * doubles and NSTACK more integers makes the compiler fill exactly those registers and slots:
 * a float travels as a double whose low 32 bits are the float (s<n> is the low half of d<n>),
 * and a float or narrow integer on the stack sits in the low bytes of its slot, as the callee
 * reads it. The result comes back in x0 or in d0/s0, so there is one pointer type for each.
 * NSTACK: env, self and FJ_MAXARGS integers leave 2 + 32 - 8 for the stack, the most possible. */
#define NSTACK 26
#define A64_PARAMS \
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,             \
    double, double, double, double, double, double, double, double,                             \
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,   \
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,   \
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t
typedef uint64_t (*fn_a64_x_t)(A64_PARAMS);
typedef double (*fn_a64_d_t)(A64_PARAMS);
#define A64_ARGS(x, v, st) \
    x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], \
    st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], st[8], st[9], st[10], st[11], st[12],   \
    st[13], st[14], st[15], st[16], st[17], st[18], st[19], st[20], st[21], st[22], st[23], st[24], \
    st[25]
#endif

/* The raw result (r0:r1, x0, or the bits of d0 whose low half is s0) as the signature's type. */
static jvalue ret_from_bits(char r, uint64_t bits)
{
    jvalue v; v.j = 0;
    uint32_t lo = (uint32_t)bits;
    switch (r) {
    case 'V': break;
    case 'Z': v.z = (jboolean)lo; break;
    case 'B': v.b = (jbyte)lo; break;
    case 'C': v.c = (jchar)lo; break;
    case 'S': v.s = (jshort)lo; break;
    case 'I': v.i = (jint)lo; break;
    case 'F': memcpy(&v.f, &lo, 4); break;
    case 'J': v.j = (jlong)bits; break;
    case 'D': memcpy(&v.d, &bits, 8); break;
    default:  v.l = (jobject)(uintptr_t)bits; break;   /* r0 on ARM (uintptr_t is 32-bit), x0 on AArch64 */
    }
    return v;
}

jvalue fj_invoke_native(void *fn, JNIEnv *env, jobject self, const char *sig, const jvalue *args)
{
    fj_sig s = parse_sig(sig);
    jvalue none; none.j = 0;
    if (!fn) return none;
#if defined(__arm__)
    uint32_t w[NWORDS];
    int k = 0;
    memset(w, 0, sizeof w);
    w[k++] = (uint32_t)(uintptr_t)env;
    w[k++] = (uint32_t)(uintptr_t)self;
    for (int i = 0; i < s.nargs; i++) {
        char t = s.a[i];
        if (t == 'J' || t == 'D') {
            if (k & 1) k++;                       /* doublewords: even reg pair / 8-aligned stack */
            if (k + 2 > NWORDS) goto toolong;
            uint64_t b; memcpy(&b, &args[i].j, 8);
            w[k++] = (uint32_t)b; w[k++] = (uint32_t)(b >> 32);
        } else {
            if (k + 1 > NWORDS) goto toolong;
            uint32_t x;
            switch (t) {
            case 'Z': x = args[i].z; break;
            case 'B': x = (uint32_t)(int32_t)args[i].b; break;
            case 'C': x = args[i].c; break;
            case 'S': x = (uint32_t)(int32_t)args[i].s; break;
            case 'F': memcpy(&x, &args[i].f, 4); break;
            case 'I': x = (uint32_t)args[i].i; break;
            default:  x = (uint32_t)(uintptr_t)args[i].l; break;
            }
            w[k++] = x;
        }
    }
    {
        fn_words_t f = (fn_words_t)fn;
        uint64_t r = f(w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11],
                       w[12], w[13], w[14], w[15], w[16], w[17], w[18], w[19], w[20], w[21], w[22],
                       w[23], w[24], w[25], w[26], w[27], w[28], w[29], w[30], w[31], w[32], w[33],
                       w[34], w[35], w[36], w[37], w[38], w[39]);
        return ret_from_bits(s.ret, r);
    }
toolong:
    fj_log("fj_invoke_native: too many argument words for %s", sig);
    return none;
#elif defined(__aarch64__)
    uint64_t x[8], st[NSTACK];
    double v[8];
    int nx = 0, nv = 0, ns = 0;
    memset(x, 0, sizeof x); memset(v, 0, sizeof v); memset(st, 0, sizeof st);
    x[nx++] = (uintptr_t)env;
    x[nx++] = (uintptr_t)self;
    for (int i = 0; i < s.nargs; i++) {
        char t = s.a[i];
        uint64_t b = 0;
        if (t == 'F' || t == 'D') {
            if (t == 'F') memcpy(&b, &args[i].f, 4);   /* upper half zero: d<n> = s<n> zero-extended */
            else memcpy(&b, &args[i].d, 8);
            if (nv < 8) { memcpy(&v[nv++], &b, 8); continue; }
        } else {
            /* narrow values widened like a compiler does (AAPCS64 leaves the upper bits to the
             * callee, but sign/zero extension is what any callee can rely on) */
            switch (t) {
            case 'Z': b = args[i].z; break;
            case 'B': b = (uint64_t)(int64_t)args[i].b; break;
            case 'C': b = args[i].c; break;
            case 'S': b = (uint64_t)(int64_t)args[i].s; break;
            case 'I': b = (uint64_t)(int64_t)args[i].i; break;
            case 'J': b = (uint64_t)args[i].j; break;
            default:  b = (uintptr_t)args[i].l; break;
            }
            if (nx < 8) { x[nx++] = b; continue; }
        }
        if (ns == NSTACK) { fj_log("fj_invoke_native: too many arguments for %s", sig); return none; }
        st[ns++] = b;
    }
    if (s.ret == 'F' || s.ret == 'D') {
        double d = ((fn_a64_d_t)fn)(A64_ARGS(x, v, st));
        uint64_t bits;
        memcpy(&bits, &d, 8);
        return ret_from_bits(s.ret, bits);
    }
    return ret_from_bits(s.ret, ((fn_a64_x_t)fn)(A64_ARGS(x, v, st)));
#else
    /* other architectures (no Android player to run): natives cannot be called */
    (void)env; (void)self; (void)args; (void)s;
    fj_log("fj_invoke_native: unsupported on this architecture (%s)", sig);
    return none;
#endif
}

enum { CALL_VIRTUAL, CALL_NONVIRTUAL, CALL_STATIC, CALL_CTOR };

static fj_method *resolve_virtual(fj_method *m, fj_obj *o)
{
    if (!o) return m;
    fj_class *rc = o->kind == K_CLASS ? C_Class : o->cls;
    if (!rc || rc == m->cls) return m;
    LOCK();
    fj_method *t;
    if (m->cache_cls == rc) t = m->cache_tgt;
    else {
        t = method_find(rc, m->name, m->sig, 0, 0);
        /* only accept an override that actually has an implementation */
        if (!t || (!t->fn && !t->native_fn)) t = m;
        m->cache_tgt = t;
        m->cache_cls = rc;
    }
    UNLOCK();
    return t;
}

static jvalue proxy_invoke(JNIEnv *env, fj_obj *proxy, fj_method *m, const jvalue *args);

static jvalue invoke(JNIEnv *env, jobject self, fj_method *m, const jvalue *args, int kind,
                     const char *api)
{
    jvalue r; r.j = 0;
    if (!m) { fj_log("%s: null method id", api); return r; }
    fj_obj *o = (kind == CALL_STATIC) ? NULL : O(self);
    fj_method *t = m;
    if (t->placeholder) {
        if (t->alias) t = t->alias;
        else {
            LOCK();
            fj_method *real = method_find(t->cls, t->name, t->sig, t->is_static, 0);
            if (real && real != t) t->alias = real, t = real;
            UNLOCK();
        }
    }
    if (kind == CALL_VIRTUAL) t = resolve_virtual(t, o);
    __atomic_add_fetch(&m->calls, 1, __ATOMIC_RELAXED);
    if (t != m) __atomic_add_fetch(&t->calls, 1, __ATOMIC_RELAXED);

    if (g_trace) {
        char a[1024], sb[200];
        fmt_args(&m->ps, args, a, sizeof a);
        fj_log("%s %s.%s%s self=%s (%s)%s", api, m->cls->name, m->name, m->sig,
               descr(self, sb, sizeof sb), a, t->fn || t->native_fn ? "" : " [unimplemented]");
    }
    if (kind == CALL_VIRTUAL && !self) fj_log("WARNING: %s %s.%s on null object", api, m->cls->name, m->name);

    if (t->fn) {
        r = t->fn(env, kind == CALL_STATIC ? (jobject)(t->cls) : self, args, t->ud);
    } else if (t->native_fn) {
        r = fj_invoke_native(t->native_fn, env, kind == CALL_STATIC ? (jobject)t->cls : self, t->sig, args);
    } else if (o && o->cls == C_Proxy) {
        r = proxy_invoke(env, o, m, args);
    } else {
        if (!m->warned) {
            m->warned = 1;
            char a[1024];
            fmt_args(&m->ps, args, a, sizeof a);
            fj_log("UNIMPLEMENTED call %s.%s%s (%s) -> default", m->cls->name, m->name, m->sig, a);
        }
    }
    if (g_trace && m->ps.ret != 'V') {
        char rb[200];
        fmt_value(m->ps.ret, r, rb, sizeof rb);
        fj_log("  -> %s", rb);
    }
    return r;
}

static jvalue invoke_v(JNIEnv *env, jobject self, jmethodID id, va_list ap, int kind, const char *api)
{
    fj_method *m = M(id);
    jvalue a[FJ_MAXARGS];
    if (!m) { jvalue r; r.j = 0; fj_log("%s: invalid method", api); return r; }
    decode_va(&m->ps, ap, a);
    return invoke(env, self, m, a, kind, api);
}

static jvalue invoke_a(JNIEnv *env, jobject self, jmethodID id, const jvalue *args, int kind, const char *api)
{
    fj_method *m = M(id);
    jvalue a[FJ_MAXARGS];
    if (!m) { jvalue r; r.j = 0; fj_log("%s: invalid method", api); return r; }
    for (int i = 0; i < m->ps.nargs; i++) a[i] = args ? args[i] : fj_v();
    return invoke(env, self, m, a, kind, api);
}

jvalue fj_call(jobject obj, const char *name, const char *sig, ...)
{
    fj_obj *o = O(obj);
    jvalue r; r.j = 0;
    if (!o) return r;
    LOCK();
    fj_class *c = o->kind == K_CLASS ? (fj_class *)o : o->cls;
    int is_static = o->kind == K_CLASS;
    fj_method *m = method_find(c, name, sig, is_static, 0);
    if (!m && c == C_Proxy) {
        /* JNIBridge proxy: dispatch through the implemented interface's method entry */
        fj_obj *ifs = NULL;
        for (fj_fval *fv = o->fields; fv; fv = fv->next)
            if (!strcmp(fv->f->name, "interfaces")) ifs = O(fv->v.l);
        for (jsize i = 0; ifs && ifs->kind == K_ARRAY && i < ifs->u.a.len && !m; i++) {
            fj_class *ic = CL((jobject)((fj_obj **)ifs->u.a.data)[i]);
            if (!ic) continue;
            m = method_in(ic, name, sig, 0);
            if (!m) m = method_find(ic, name, sig, 0, 0);
        }
        if (!m && ifs && ifs->kind == K_ARRAY && ifs->u.a.len) {
            fj_class *ic = CL((jobject)((fj_obj **)ifs->u.a.data)[0]);
            if (ic) { m = method_new(ic, name, sig, 0); m->placeholder = 1; }
        }
    }
    if (!m && is_static) { is_static = 0; c = C_Class; m = method_find(c, name, sig, 0, 0); }
    if (!m) { m = method_in(c, name, sig, is_static); if (!m) { m = method_new(c, name, sig, is_static); m->placeholder = 1; } }
    UNLOCK();
    va_list ap;
    va_start(ap, sig);
    r = invoke_v(fj_env(), obj, (jmethodID)m, ap, is_static ? CALL_STATIC : CALL_VIRTUAL, "fj_call");
    va_end(ap);
    return r;
}

/* natives */
void *fj_native(const char *cls, const char *name, const char *sig)
{
    fj_class *c = class_get(cls, 0);
    if (!c) return NULL;
    LOCK();
    fj_native_rec *n = native_in(c, name, sig);
    UNLOCK();
    return n ? n->fn : NULL;
}

jvalue fj_call_native_a(const char *cls, const char *name, const char *sig, jobject self, const jvalue *args)
{
    jvalue r; r.j = 0;
    fj_class *c = class_get(cls, 0);
    if (!c) { fj_log("fj_call_native: no class %s", cls); return r; }
    LOCK();
    fj_native_rec *n = native_in(c, name, sig);
    UNLOCK();
    if (!n) { fj_log("fj_call_native: %s.%s%s not registered", cls, name, sig ? sig : ""); return r; }
    JNIEnv *env = fj_env();
    if (!self) self = (jobject)c;
    if (g_trace) {
        fj_sig s = parse_sig(n->sig);
        char a[1024], sb[200];
        fmt_args(&s, args, a, sizeof a);
        fj_log("-> native %s.%s%s self=%s (%s)", cls, name, n->sig, descr(self, sb, sizeof sb), a);
    }
    r = fj_invoke_native(n->fn, env, self, n->sig, args);
    if (g_trace) {
        fj_sig s = parse_sig(n->sig);
        char rb[200];
        fmt_value(s.ret, r, rb, sizeof rb);
        fj_log("<- native %s.%s = %s", cls, name, rb);
    }
    return r;
}

jvalue fj_call_native(const char *cls, const char *name, const char *sig, jobject self, ...)
{
    jvalue a[FJ_MAXARGS];
    const char *s = sig;
    if (!s) {
        fj_class *c = class_get(cls, 0);
        fj_native_rec *n = c ? native_in(c, name, NULL) : NULL;
        s = n ? n->sig : "()V";
    }
    fj_sig ps = parse_sig(s);
    va_list ap;
    va_start(ap, self);
    decode_va(&ps, ap, a);
    va_end(ap);
    return fj_call_native_a(cls, name, s, self, a);
}

/* ======================================================================================= */
/* JNI functions                                                                           */
/* ======================================================================================= */

#define ENV_UNUSED (void)env

static jint FJ_ABI j_GetVersion(JNIEnv *env) { ENV_UNUSED; TRACE("GetVersion"); return JNI_VERSION_1_6; }

static jclass FJ_ABI j_DefineClass(JNIEnv *env, const char *name, jobject loader, const jbyte *buf, jsize len)
{
    ENV_UNUSED; (void)loader; (void)buf;
    fj_log("UNSUPPORTED DefineClass(%s, %d bytes)", name ? name : "?", (int)len);
    return NULL;
}

static jclass FJ_ABI j_FindClass(JNIEnv *env, const char *name)
{
    if (!name) return NULL;
    fj_class *c = class_get(name, 0);
    int missing = !c || c->absent || (c->placeholder && g_strict);
    if (missing && !c && !g_strict) {
        c = class_get(name, 1);          /* unknown: placeholder (logged in class_get) */
        missing = 0;
    }
    if (missing) {
        if (!c) { fj_class_absent(name); c = class_get(name, 0); }
        if (!c->lookups++) fj_log("FindClass(%s) -> NULL (absent)", name);
        TRACE("FindClass %s -> NULL", name);
        fj_throw(env, "java/lang/NoClassDefFoundError", name);
        return NULL;
    }
    c->lookups++;
    TRACE("FindClass %s -> %p%s", name, (void *)c, c->placeholder ? " [placeholder]" : "");
    return (jclass)c;
}

static jmethodID FJ_ABI j_FromReflectedMethod(JNIEnv *env, jobject m)
{
    ENV_UNUSED;
    fj_obj *o = O(m);
    TRACE("FromReflectedMethod %p", m);
    return o ? (jmethodID)o->data : NULL;
}

static jfieldID FJ_ABI j_FromReflectedField(JNIEnv *env, jobject f)
{
    ENV_UNUSED;
    fj_obj *o = O(f);
    TRACE("FromReflectedField %p", f);
    return o ? (jfieldID)o->data : NULL;
}

static jobject FJ_ABI j_ToReflectedMethod(JNIEnv *env, jclass cls, jmethodID mid, jboolean isStatic)
{
    ENV_UNUSED; (void)cls; (void)isStatic;
    fj_obj *o = obj_alloc(C_Method, K_OBJECT);
    o->data = (void *)mid;
    TRACE("ToReflectedMethod %p", (void *)mid);
    return add_local(o);
}

static jobject FJ_ABI j_ToReflectedField(JNIEnv *env, jclass cls, jfieldID fid, jboolean isStatic)
{
    ENV_UNUSED; (void)cls; (void)isStatic;
    fj_obj *o = obj_alloc(C_Field, K_OBJECT);
    o->data = (void *)fid;
    TRACE("ToReflectedField %p", (void *)fid);
    return add_local(o);
}

static jclass FJ_ABI j_GetSuperclass(JNIEnv *env, jclass cls)
{
    ENV_UNUSED;
    fj_class *c = CL(cls);
    TRACE("GetSuperclass %s", c ? c->name : "null");
    return c ? (jclass)c->super : NULL;
}

static jboolean FJ_ABI j_IsAssignableFrom(JNIEnv *env, jclass a, jclass b)
{
    ENV_UNUSED;
    fj_class *ca = CL(a), *cb = CL(b);
    jboolean r = class_is_sub(ca, cb, 0);
    TRACE("IsAssignableFrom %s <- %s = %d", cb ? cb->name : "null", ca ? ca->name : "null", r);
    return r;
}

static jint FJ_ABI j_Throw(JNIEnv *env, jthrowable t)
{
    ENV_UNUSED;
    TRACE("Throw %p", t);
    set_exception(TH(), O(t));
    return JNI_OK;
}

static jint FJ_ABI j_ThrowNew(JNIEnv *env, jclass cls, const char *msg)
{
    fj_class *c = CL(cls);
    fj_throw(env, c ? c->name : "java/lang/RuntimeException", msg);
    return JNI_OK;
}

static jthrowable FJ_ABI j_ExceptionOccurred(JNIEnv *env)
{
    ENV_UNUSED;
    fj_thread *t = TH();
    TRACE("ExceptionOccurred -> %p", (void *)t->exc);
    return t->exc ? add_local(t->exc) : NULL;
}

static void FJ_ABI j_ExceptionDescribe(JNIEnv *env)
{
    ENV_UNUSED;
    fj_thread *t = TH();
    if (t->exc) {
        jvalue m = ifield_get(t->exc, field_for_obj(t->exc, "detailMessage", "Ljava/lang/String;"));
        fj_log("ExceptionDescribe: %s: %s", t->exc->cls->name, m.l ? fj_cstr(m.l) : "");
    }
}

static void FJ_ABI j_ExceptionClear(JNIEnv *env)
{
    ENV_UNUSED;
    TRACE("ExceptionClear");
    set_exception(TH(), NULL);
}

static void FJ_ABI j_FatalError(JNIEnv *env, const char *msg)
{
    ENV_UNUSED;
    fj_log("FatalError: %s", msg ? msg : "");
    abort();
}

static jint FJ_ABI j_PushLocalFrame(JNIEnv *env, jint cap)
{
    ENV_UNUSED;
    TRACE("PushLocalFrame %d", cap);
    push_frame();
    return JNI_OK;
}

static jobject FJ_ABI j_PopLocalFrame(JNIEnv *env, jobject result)
{
    ENV_UNUSED;
    TRACE("PopLocalFrame %p", result);
    fj_obj *o = O(result);
    obj_ref(o);
    pop_frame();
    jobject r = add_local(o);
    obj_unref(o);
    return r;
}

static jobject FJ_ABI j_NewGlobalRef(JNIEnv *env, jobject obj)
{
    ENV_UNUSED;
    TRACE("NewGlobalRef %p", obj);
    return fj_global(obj);
}

static void FJ_ABI j_DeleteGlobalRef(JNIEnv *env, jobject obj)
{
    ENV_UNUSED;
    TRACE("DeleteGlobalRef %p", obj);
    fj_release(obj);
}

static void FJ_ABI j_DeleteLocalRef(JNIEnv *env, jobject obj)
{
    ENV_UNUSED;
    fj_obj *o = O(obj);
    if (!o) return;
    if (!del_local(o)) TRACE("DeleteLocalRef %p: not a local ref of this thread (ignored)", obj);
}

static jboolean FJ_ABI j_IsSameObject(JNIEnv *env, jobject a, jobject b)
{
    ENV_UNUSED;
    TRACE("IsSameObject %p %p", a, b);
    return a == b;
}

static jobject FJ_ABI j_NewLocalRef(JNIEnv *env, jobject obj)
{
    ENV_UNUSED;
    TRACE("NewLocalRef %p", obj);
    return add_local(O(obj));
}

static jint FJ_ABI j_EnsureLocalCapacity(JNIEnv *env, jint cap) { ENV_UNUSED; (void)cap; return JNI_OK; }

static jobject FJ_ABI j_AllocObject(JNIEnv *env, jclass cls)
{
    ENV_UNUSED;
    fj_class *c = CL(cls);
    TRACE("AllocObject %s", c ? c->name : "null");
    return add_local(obj_alloc(c, K_OBJECT));
}

static jobject new_object_common(JNIEnv *env, jclass cls, jmethodID mid, const jvalue *args, const char *api)
{
    fj_class *c = CL(cls);
    fj_method *m = M(mid);
    if (!c) { fj_log("%s: null class", api); return NULL; }
    jobject o = add_local(obj_alloc(c, K_OBJECT));
    if (m) {
        fj_method *t = m;
        if (t->placeholder && !t->fn) {
            LOCK();
            fj_method *real = method_in(c, t->name, t->sig, 0);
            if (real && !real->placeholder) t = real;
            UNLOCK();
        }
        if (g_trace) {
            char a[1024];
            fmt_args(&m->ps, args, a, sizeof a);
            fj_log("%s %s%s (%s) -> %p%s", api, c->name, m->sig, a, o, t->fn ? "" : " [no ctor]");
        }
        if (t->fn) {
            jvalue r = t->fn(env, o, args, t->ud);
            /* a constructor handler may substitute another object (e.g. a cached singleton) */
            if (r.l && r.l != o) return r.l;
        } else if (!m->warned && strcmp(c->name, "java/lang/Object")) {
            m->warned = 1;
            fj_log("UNIMPLEMENTED constructor %s.<init>%s (object created anyway)", c->name, m->sig);
        }
    }
    return o;
}

static jobject FJ_ABI j_NewObjectV(JNIEnv *env, jclass cls, jmethodID mid, va_list ap)
{
    fj_method *m = M(mid);
    jvalue a[FJ_MAXARGS];
    if (m) decode_va(&m->ps, ap, a);
    return new_object_common(env, cls, mid, a, "NewObjectV");
}

static jobject FJ_ABI j_NewObject(JNIEnv *env, jclass cls, jmethodID mid, ...)
{
    fj_method *m = M(mid);
    jvalue a[FJ_MAXARGS];
    va_list ap;
    va_start(ap, mid);
    if (m) decode_va(&m->ps, ap, a);
    va_end(ap);
    return new_object_common(env, cls, mid, a, "NewObject");
}

static jobject FJ_ABI j_NewObjectA(JNIEnv *env, jclass cls, jmethodID mid, const jvalue *args)
{
    fj_method *m = M(mid);
    jvalue a[FJ_MAXARGS];
    if (m) for (int i = 0; i < m->ps.nargs; i++) a[i] = args[i];
    return new_object_common(env, cls, mid, a, "NewObjectA");
}

static jclass FJ_ABI j_GetObjectClass(JNIEnv *env, jobject obj)
{
    ENV_UNUSED;
    jclass c = fj_object_class(obj);
    TRACE("GetObjectClass %p -> %s", obj, c ? ((fj_class *)c)->name : "null");
    return c;
}

static jboolean FJ_ABI j_IsInstanceOf(JNIEnv *env, jobject obj, jclass cls)
{
    ENV_UNUSED;
    if (!obj) return JNI_TRUE;   /* JNI: null is an instance of every class */
    fj_obj *o = O(obj);
    fj_class *c = CL(cls);
    jboolean r = o && class_is_sub(o->kind == K_CLASS ? C_Class : o->cls, c, 0);
    TRACE("IsInstanceOf %p (%s) %s -> %d", obj, o ? o->cls->name : "?", c ? c->name : "null", r);
    return r;
}

static jmethodID get_method(jclass cls, const char *name, const char *sig, int is_static, const char *api)
{
    fj_class *c = CL(cls);
    if (!c || !name || !sig) { fj_log("%s: bad args (%p, %s, %s)", api, cls, name ? name : "null", sig ? sig : "null"); return NULL; }
    LOCK();
    int ctor = !strcmp(name, "<init>");
    fj_method *m = ctor ? method_in(c, name, sig, is_static) : method_find(c, name, sig, is_static, 0);
    if (!m) {
        m = method_in(c, name, sig, is_static);
        if (!m) {
            m = method_new(c, name, sig, is_static);
            fj_native_rec *n = native_in(c, name, sig);
            if (n) m->native_fn = n->fn;
            else {
                m->placeholder = 1;
                fj_log("UNKNOWN %smethod %s.%s%s (placeholder)", is_static ? "static " : "", c->name, name, sig);
            }
        }
    }
    UNLOCK();
    TRACE("%s %s.%s%s -> %p%s", api, c->name, name, sig, (void *)m, m->placeholder ? " [placeholder]" : "");
    return (jmethodID)m;
}

static jmethodID FJ_ABI j_GetMethodID(JNIEnv *env, jclass cls, const char *name, const char *sig)
{
    ENV_UNUSED;
    return get_method(cls, name, sig, 0, "GetMethodID");
}

static jmethodID FJ_ABI j_GetStaticMethodID(JNIEnv *env, jclass cls, const char *name, const char *sig)
{
    ENV_UNUSED;
    return get_method(cls, name, sig, 1, "GetStaticMethodID");
}

/* --- Call<Type>Method families --- */

#define CALL_FAMILY(T, jt, fld)                                                                 \
static jt FJ_ABI j_Call##T##Method(JNIEnv *env, jobject o, jmethodID m, ...)                    \
{ va_list ap; va_start(ap, m); jvalue r = invoke_v(env, o, m, ap, CALL_VIRTUAL, "Call" #T "Method"); \
  va_end(ap); return r.fld; }                                                                   \
static jt FJ_ABI j_Call##T##MethodV(JNIEnv *env, jobject o, jmethodID m, va_list ap)            \
{ return invoke_v(env, o, m, ap, CALL_VIRTUAL, "Call" #T "MethodV").fld; }                        \
static jt FJ_ABI j_Call##T##MethodA(JNIEnv *env, jobject o, jmethodID m, const jvalue *a)       \
{ return invoke_a(env, o, m, a, CALL_VIRTUAL, "Call" #T "MethodA").fld; }                         \
static jt FJ_ABI j_CallNonvirtual##T##Method(JNIEnv *env, jobject o, jclass c, jmethodID m, ...) \
{ (void)c; va_list ap; va_start(ap, m);                                                         \
  jvalue r = invoke_v(env, o, m, ap, CALL_NONVIRTUAL, "CallNonvirtual" #T "Method");             \
  va_end(ap); return r.fld; }                                                                   \
static jt FJ_ABI j_CallNonvirtual##T##MethodV(JNIEnv *env, jobject o, jclass c, jmethodID m, va_list ap) \
{ (void)c; return invoke_v(env, o, m, ap, CALL_NONVIRTUAL, "CallNonvirtual" #T "MethodV").fld; }  \
static jt FJ_ABI j_CallNonvirtual##T##MethodA(JNIEnv *env, jobject o, jclass c, jmethodID m, const jvalue *a) \
{ (void)c; return invoke_a(env, o, m, a, CALL_NONVIRTUAL, "CallNonvirtual" #T "MethodA").fld; }   \
static jt FJ_ABI j_CallStatic##T##Method(JNIEnv *env, jclass c, jmethodID m, ...)                \
{ va_list ap; va_start(ap, m); jvalue r = invoke_v(env, c, m, ap, CALL_STATIC, "CallStatic" #T "Method"); \
  va_end(ap); return r.fld; }                                                                   \
static jt FJ_ABI j_CallStatic##T##MethodV(JNIEnv *env, jclass c, jmethodID m, va_list ap)       \
{ return invoke_v(env, c, m, ap, CALL_STATIC, "CallStatic" #T "MethodV").fld; }                   \
static jt FJ_ABI j_CallStatic##T##MethodA(JNIEnv *env, jclass c, jmethodID m, const jvalue *a)  \
{ return invoke_a(env, c, m, a, CALL_STATIC, "CallStatic" #T "MethodA").fld; }

CALL_FAMILY(Object, jobject, l)
CALL_FAMILY(Boolean, jboolean, z)
CALL_FAMILY(Byte, jbyte, b)
CALL_FAMILY(Char, jchar, c)
CALL_FAMILY(Short, jshort, s)
CALL_FAMILY(Int, jint, i)
CALL_FAMILY(Long, jlong, j)
CALL_FAMILY(Float, jfloat, f)
CALL_FAMILY(Double, jdouble, d)

static void FJ_ABI j_CallVoidMethod(JNIEnv *env, jobject o, jmethodID m, ...)
{ va_list ap; va_start(ap, m); invoke_v(env, o, m, ap, CALL_VIRTUAL, "CallVoidMethod"); va_end(ap); }
static void FJ_ABI j_CallVoidMethodV(JNIEnv *env, jobject o, jmethodID m, va_list ap)
{ invoke_v(env, o, m, ap, CALL_VIRTUAL, "CallVoidMethodV"); }
static void FJ_ABI j_CallVoidMethodA(JNIEnv *env, jobject o, jmethodID m, const jvalue *a)
{ invoke_a(env, o, m, a, CALL_VIRTUAL, "CallVoidMethodA"); }
static void FJ_ABI j_CallNonvirtualVoidMethod(JNIEnv *env, jobject o, jclass c, jmethodID m, ...)
{ (void)c; va_list ap; va_start(ap, m); invoke_v(env, o, m, ap, CALL_NONVIRTUAL, "CallNonvirtualVoidMethod"); va_end(ap); }
static void FJ_ABI j_CallNonvirtualVoidMethodV(JNIEnv *env, jobject o, jclass c, jmethodID m, va_list ap)
{ (void)c; invoke_v(env, o, m, ap, CALL_NONVIRTUAL, "CallNonvirtualVoidMethodV"); }
static void FJ_ABI j_CallNonvirtualVoidMethodA(JNIEnv *env, jobject o, jclass c, jmethodID m, const jvalue *a)
{ (void)c; invoke_a(env, o, m, a, CALL_NONVIRTUAL, "CallNonvirtualVoidMethodA"); }
static void FJ_ABI j_CallStaticVoidMethod(JNIEnv *env, jclass c, jmethodID m, ...)
{ va_list ap; va_start(ap, m); invoke_v(env, c, m, ap, CALL_STATIC, "CallStaticVoidMethod"); va_end(ap); }
static void FJ_ABI j_CallStaticVoidMethodV(JNIEnv *env, jclass c, jmethodID m, va_list ap)
{ invoke_v(env, c, m, ap, CALL_STATIC, "CallStaticVoidMethodV"); }
static void FJ_ABI j_CallStaticVoidMethodA(JNIEnv *env, jclass c, jmethodID m, const jvalue *a)
{ invoke_a(env, c, m, a, CALL_STATIC, "CallStaticVoidMethodA"); }

/* --- fields --- */

static jfieldID get_field(jclass cls, const char *name, const char *sig, int is_static, const char *api)
{
    fj_class *c = CL(cls);
    if (!c || !name || !sig) { fj_log("%s: bad args", api); return NULL; }
    LOCK();
    fj_field *f = field_find(c, name, sig, is_static, 0);
    if (!f) {
        f = field_in(c, name, sig, is_static);
        if (!f) {
            f = field_new(c, name, sig, is_static);
            f->placeholder = 1;
            fj_log("UNKNOWN %sfield %s.%s %s (placeholder, value 0/null)", is_static ? "static " : "", c->name, name, sig);
        }
    }
    UNLOCK();
    TRACE("%s %s.%s %s -> %p%s", api, c->name, name, sig, (void *)f, f->placeholder ? " [placeholder]" : "");
    return (jfieldID)f;
}

static jfieldID FJ_ABI j_GetFieldID(JNIEnv *env, jclass cls, const char *name, const char *sig)
{ ENV_UNUSED; return get_field(cls, name, sig, 0, "GetFieldID"); }
static jfieldID FJ_ABI j_GetStaticFieldID(JNIEnv *env, jclass cls, const char *name, const char *sig)
{ ENV_UNUSED; return get_field(cls, name, sig, 1, "GetStaticFieldID"); }

static jvalue field_get(jobject obj, jfieldID fid, int is_static, const char *api)
{
    fj_field *f = FLD(fid);
    jvalue v; v.j = 0;
    if (!f) return v;
    __atomic_add_fetch(&f->gets, 1, __ATOMIC_RELAXED);
    if (is_static || f->is_static) {
        LOCK(); v = f->value; UNLOCK();
    } else {
        fj_obj *o = O(obj);
        if (!o) { fj_log("%s %s.%s on null object", api, f->cls->name, f->name); return v; }
        v = ifield_get(o, f);
    }
    if (f->placeholder && !f->warned) { f->warned = 1; fj_log("read of unknown field %s.%s %s", f->cls->name, f->name, f->sig); }
    if (g_trace) {
        char b[200], sb[200];
        fmt_value(f->type, v, b, sizeof b);
        fj_log("%s %s.%s of %s -> %s", api, f->cls->name, f->name, descr(obj, sb, sizeof sb), b);
    }
    if (f->type == 'L' && v.l) v.l = add_local(O(v.l));
    return v;
}

static void field_set(jobject obj, jfieldID fid, jvalue v, int is_static, const char *api)
{
    fj_field *f = FLD(fid);
    if (!f) return;
    __atomic_add_fetch(&f->sets, 1, __ATOMIC_RELAXED);
    if (g_trace) {
        char b[200], sb[200];
        fmt_value(f->type, v, b, sizeof b);
        fj_log("%s %s.%s of %s = %s", api, f->cls->name, f->name, descr(obj, sb, sizeof sb), b);
    }
    if (is_static || f->is_static) {
        LOCK();
        if (f->type == 'L') { if (v.l) obj_ref(O(v.l)); if (f->value.l) obj_unref(O(f->value.l)); }
        f->value = v;
        UNLOCK();
    } else {
        fj_obj *o = O(obj);
        if (!o) { fj_log("%s %s.%s on null object", api, f->cls->name, f->name); return; }
        ifield_set(o, f, v);
    }
}

#define FIELD_FAMILY(T, jt, fld)                                                              \
static jt FJ_ABI j_Get##T##Field(JNIEnv *env, jobject o, jfieldID f)                          \
{ ENV_UNUSED; return field_get(o, f, 0, "Get" #T "Field").fld; }                              \
static void FJ_ABI j_Set##T##Field(JNIEnv *env, jobject o, jfieldID f, jt v)                  \
{ ENV_UNUSED; jvalue x; x.j = 0; x.fld = v; field_set(o, f, x, 0, "Set" #T "Field"); }        \
static jt FJ_ABI j_GetStatic##T##Field(JNIEnv *env, jclass c, jfieldID f)                     \
{ ENV_UNUSED; return field_get(c, f, 1, "GetStatic" #T "Field").fld; }                        \
static void FJ_ABI j_SetStatic##T##Field(JNIEnv *env, jclass c, jfieldID f, jt v)             \
{ ENV_UNUSED; jvalue x; x.j = 0; x.fld = v; field_set(c, f, x, 1, "SetStatic" #T "Field"); }

FIELD_FAMILY(Object, jobject, l)
FIELD_FAMILY(Boolean, jboolean, z)
FIELD_FAMILY(Byte, jbyte, b)
FIELD_FAMILY(Char, jchar, c)
FIELD_FAMILY(Short, jshort, s)
FIELD_FAMILY(Int, jint, i)
FIELD_FAMILY(Long, jlong, j)
FIELD_FAMILY(Float, jfloat, f)
FIELD_FAMILY(Double, jdouble, d)

/* --- strings --- */

static const char empty_utf[] = "";

static jstring FJ_ABI j_NewString(JNIEnv *env, const jchar *u, jsize len)
{
    ENV_UNUSED;
    if (!u && len) return NULL;
    fj_obj *s = string_new16(u, len, 0);
    TRACE("NewString(len %d) -> \"%s\"", (int)len, s->u.s.utf8);
    return add_local(s);
}

static jsize FJ_ABI j_GetStringLength(JNIEnv *env, jstring str)
{
    ENV_UNUSED;
    fj_obj *s = STR(str, "GetStringLength");
    TRACE("GetStringLength %p -> %d", str, s ? (int)s->u.s.len : 0);
    return s ? s->u.s.len : 0;
}

static const jchar *FJ_ABI j_GetStringChars(JNIEnv *env, jstring str, jboolean *isCopy)
{
    ENV_UNUSED;
    fj_obj *s = STR(str, "GetStringChars");
    if (isCopy) *isCopy = JNI_TRUE;
    jchar *c = xcalloc((s ? s->u.s.len : 0) + 1, sizeof(jchar));
    if (s && s->u.s.len) memcpy(c, s->u.s.chars, s->u.s.len * sizeof(jchar));
    TRACE("GetStringChars %p", str);
    return c;
}

static void FJ_ABI j_ReleaseStringChars(JNIEnv *env, jstring str, const jchar *chars)
{
    ENV_UNUSED; (void)str;
    free((void *)chars);
}

static jstring FJ_ABI j_NewStringUTF(JNIEnv *env, const char *utf)
{
    ENV_UNUSED;
    if (!utf) { TRACE("NewStringUTF(NULL) -> null"); return NULL; }
    jobject r = add_local(string_new(utf));
    TRACE("NewStringUTF \"%.200s\" -> %p", utf, r);
    return r;
}

static jsize FJ_ABI j_GetStringUTFLength(JNIEnv *env, jstring str)
{
    ENV_UNUSED;
    fj_obj *s = STR(str, "GetStringUTFLength");
    jsize n = s ? (jsize)utf16_to_utf8(s->u.s.chars, s->u.s.len, NULL, 1) : 0;
    TRACE("GetStringUTFLength %p -> %d", str, (int)n);
    return n;
}

static const char *FJ_ABI j_GetStringUTFChars(JNIEnv *env, jstring str, jboolean *isCopy)
{
    ENV_UNUSED;
    fj_obj *s = STR(str, "GetStringUTFChars");
    if (isCopy) *isCopy = JNI_TRUE;
    if (!s) return empty_utf;
    size_t n = utf16_to_utf8(s->u.s.chars, s->u.s.len, NULL, 1);
    char *r = xcalloc(n + 1, 1);
    utf16_to_utf8(s->u.s.chars, s->u.s.len, r, 1);
    TRACE("GetStringUTFChars %p -> \"%.200s\"", str, r);
    return r;
}

static void FJ_ABI j_ReleaseStringUTFChars(JNIEnv *env, jstring str, const char *chars)
{
    ENV_UNUSED; (void)str;
    if (chars && chars != empty_utf) free((void *)chars);
}

static void FJ_ABI j_GetStringRegion(JNIEnv *env, jstring str, jsize start, jsize len, jchar *buf)
{
    fj_obj *s = STR(str, "GetStringRegion");
    if (!s || !buf) return;
    if (start < 0 || len < 0 || start + len > s->u.s.len) {
        fj_throw(env, "java/lang/StringIndexOutOfBoundsException", "GetStringRegion");
        return;
    }
    memcpy(buf, s->u.s.chars + start, len * sizeof(jchar));
}

static void FJ_ABI j_GetStringUTFRegion(JNIEnv *env, jstring str, jsize start, jsize len, char *buf)
{
    fj_obj *s = STR(str, "GetStringUTFRegion");
    if (!s || !buf) return;
    if (start < 0 || len < 0 || start + len > s->u.s.len) {
        fj_throw(env, "java/lang/StringIndexOutOfBoundsException", "GetStringUTFRegion");
        return;
    }
    utf16_to_utf8(s->u.s.chars + start, len, buf, 1);
}

static const jchar *FJ_ABI j_GetStringCritical(JNIEnv *env, jstring str, jboolean *isCopy)
{
    ENV_UNUSED;
    fj_obj *s = STR(str, "GetStringCritical");
    if (isCopy) *isCopy = JNI_FALSE;
    static const jchar nothing[1];
    return s ? s->u.s.chars : nothing;
}

static void FJ_ABI j_ReleaseStringCritical(JNIEnv *env, jstring str, const jchar *c)
{ ENV_UNUSED; (void)str; (void)c; }

/* --- arrays --- */

static jsize FJ_ABI j_GetArrayLength(JNIEnv *env, jarray arr)
{
    ENV_UNUSED;
    fj_obj *a = ARR(arr, "GetArrayLength");
    TRACE("GetArrayLength %p -> %d", arr, a ? (int)a->u.a.len : 0);
    return a ? a->u.a.len : 0;
}

static jobjectArray FJ_ABI j_NewObjectArray(JNIEnv *env, jsize len, jclass cls, jobject init)
{
    ENV_UNUSED;
    fj_class *c = CL(cls);
    char es[520];
    if (!c) snprintf(es, sizeof es, "Ljava/lang/Object;");
    else if (c->name[0] == '[') snprintf(es, sizeof es, "%s", c->name);
    else snprintf(es, sizeof es, "L%s;", c->name);
    fj_obj *a = array_new(es, len);
    if (init) for (jsize i = 0; i < a->u.a.len; i++) objarray_set(a, i, init);
    TRACE("NewObjectArray %d of %s", (int)len, es);
    return add_local(a);
}

static jobject FJ_ABI j_GetObjectArrayElement(JNIEnv *env, jobjectArray arr, jsize i)
{
    fj_obj *a = ARR(arr, "GetObjectArrayElement");
    if (!a) return NULL;
    if (a->u.a.et != 'L' || i < 0 || i >= a->u.a.len) {
        fj_throw(env, "java/lang/ArrayIndexOutOfBoundsException", "GetObjectArrayElement");
        return NULL;
    }
    LOCK();
    fj_obj *e = ((fj_obj **)a->u.a.data)[i];
    jobject r = add_local(e);
    UNLOCK();
    TRACE("GetObjectArrayElement %p[%d] -> %p", arr, (int)i, (void *)e);
    return r;
}

static void FJ_ABI j_SetObjectArrayElement(JNIEnv *env, jobjectArray arr, jsize i, jobject v)
{
    fj_obj *a = ARR(arr, "SetObjectArrayElement");
    if (!a) return;
    if (a->u.a.et != 'L' || i < 0 || i >= a->u.a.len) {
        fj_throw(env, "java/lang/ArrayIndexOutOfBoundsException", "SetObjectArrayElement");
        return;
    }
    TRACE("SetObjectArrayElement %p[%d] = %p", arr, (int)i, v);
    objarray_set(a, i, v);
}

static void *prim_elems(jarray arr, char et, jboolean *isCopy, const char *api)
{
    fj_obj *a = ARR(arr, api);
    if (isCopy) *isCopy = JNI_FALSE;
    if (!a) return NULL;
    if (a->u.a.et != et) TRACE("%s: element type mismatch (%c vs %c)", api, a->u.a.et, et);
    TRACE("%s %p (%d elems)", api, arr, (int)a->u.a.len);
    return a->u.a.data;
}

static void prim_region(JNIEnv *env, jarray arr, jsize start, jsize len, void *buf, int set, const char *api)
{
    fj_obj *a = ARR(arr, api);
    if (!a) return;
    if (start < 0 || len < 0 || start + len > a->u.a.len) {
        fj_throw(env, "java/lang/ArrayIndexOutOfBoundsException", api);
        return;
    }
    TRACE("%s %p [%d,+%d)", api, arr, (int)start, (int)len);
    char *base = (char *)a->u.a.data + (size_t)start * a->u.a.esize;
    if (set) memcpy(base, buf, (size_t)len * a->u.a.esize);
    else memcpy(buf, base, (size_t)len * a->u.a.esize);
}

#define ARRAY_FAMILY(T, jt, sigc)                                                                    \
static jt##Array FJ_ABI j_New##T##Array(JNIEnv *env, jsize len)                                      \
{ ENV_UNUSED; TRACE("New" #T "Array %d", (int)len); return add_local(array_new(sigc, len)); }        \
static jt *FJ_ABI j_Get##T##ArrayElements(JNIEnv *env, jt##Array a, jboolean *c)                     \
{ ENV_UNUSED; return prim_elems(a, sigc[0], c, "Get" #T "ArrayElements"); }                          \
static void FJ_ABI j_Release##T##ArrayElements(JNIEnv *env, jt##Array a, jt *e, jint mode)           \
{ ENV_UNUSED; (void)a; (void)e; (void)mode; }                                                        \
static void FJ_ABI j_Get##T##ArrayRegion(JNIEnv *env, jt##Array a, jsize s, jsize l, jt *b)          \
{ prim_region(env, a, s, l, b, 0, "Get" #T "ArrayRegion"); }                                         \
static void FJ_ABI j_Set##T##ArrayRegion(JNIEnv *env, jt##Array a, jsize s, jsize l, const jt *b)    \
{ prim_region(env, a, s, l, (void *)b, 1, "Set" #T "ArrayRegion"); }

ARRAY_FAMILY(Boolean, jboolean, "Z")
ARRAY_FAMILY(Byte, jbyte, "B")
ARRAY_FAMILY(Char, jchar, "C")
ARRAY_FAMILY(Short, jshort, "S")
ARRAY_FAMILY(Int, jint, "I")
ARRAY_FAMILY(Long, jlong, "J")
ARRAY_FAMILY(Float, jfloat, "F")
ARRAY_FAMILY(Double, jdouble, "D")

static void *FJ_ABI j_GetPrimitiveArrayCritical(JNIEnv *env, jarray arr, jboolean *isCopy)
{
    ENV_UNUSED;
    fj_obj *a = ARR(arr, "GetPrimitiveArrayCritical");
    if (isCopy) *isCopy = JNI_FALSE;
    TRACE("GetPrimitiveArrayCritical %p", arr);
    return a ? a->u.a.data : NULL;
}

static void FJ_ABI j_ReleasePrimitiveArrayCritical(JNIEnv *env, jarray arr, void *p, jint mode)
{ ENV_UNUSED; (void)arr; (void)p; (void)mode; }

/* --- natives --- */

static jint FJ_ABI j_RegisterNatives(JNIEnv *env, jclass cls, const JNINativeMethod *ms, jint n)
{
    ENV_UNUSED;
    fj_class *c = CL(cls);
    if (!c || !ms || n < 0) return JNI_ERR;
    LOCK();
    for (jint i = 0; i < n; i++) {
        fj_native_rec *r = native_in(c, ms[i].name, ms[i].signature);
        if (!r) {
            r = xcalloc(1, sizeof *r);
            r->name = xstrdup(ms[i].name);
            r->sig = xstrdup(ms[i].signature);
            r->next = c->natives;
            c->natives = r;
        }
        r->fn = ms[i].fnPtr;
        /* hook up existing method entries (non-static and static) */
        for (fj_method *m = c->methods; m; m = m->next)
            if (!strcmp(m->name, r->name) && !strcmp(m->sig, r->sig)) { m->native_fn = r->fn; m->placeholder = 0; }
        if (g_trace || getenv("FJ_NATIVES"))
            fj_log("RegisterNatives %s.%s%s -> %p", c->name, ms[i].name, ms[i].signature, ms[i].fnPtr);
    }
    UNLOCK();
    return JNI_OK;
}

static jint FJ_ABI j_UnregisterNatives(JNIEnv *env, jclass cls)
{
    ENV_UNUSED;
    fj_class *c = CL(cls);
    if (!c) return JNI_ERR;
    LOCK();
    for (fj_native_rec *r = c->natives; r; r = r->next) r->fn = NULL;
    for (fj_method *m = c->methods; m; m = m->next) m->native_fn = NULL;
    UNLOCK();
    TRACE("UnregisterNatives %s", c->name);
    return JNI_OK;
}

static jint FJ_ABI j_MonitorEnter(JNIEnv *env, jobject o) { ENV_UNUSED; TRACE("MonitorEnter %p (no-op)", o); return JNI_OK; }
static jint FJ_ABI j_MonitorExit(JNIEnv *env, jobject o) { ENV_UNUSED; TRACE("MonitorExit %p (no-op)", o); return JNI_OK; }

static jint FJ_ABI j_GetJavaVM(JNIEnv *env, JavaVM **vm)
{
    ENV_UNUSED;
    TRACE("GetJavaVM");
    if (vm) *vm = fj_vm();
    return JNI_OK;
}

static jweak FJ_ABI j_NewWeakGlobalRef(JNIEnv *env, jobject o) { ENV_UNUSED; TRACE("NewWeakGlobalRef %p", o); return fj_global(o); }
static void FJ_ABI j_DeleteWeakGlobalRef(JNIEnv *env, jweak o) { ENV_UNUSED; TRACE("DeleteWeakGlobalRef %p", o); fj_release(o); }

static jboolean FJ_ABI j_ExceptionCheck(JNIEnv *env)
{
    ENV_UNUSED;
    jboolean r = TH()->exc != NULL;
    if (r) TRACE("ExceptionCheck -> true");
    return r;
}

static jobject FJ_ABI j_NewDirectByteBuffer(JNIEnv *env, void *addr, jlong cap)
{
    ENV_UNUSED;
    fj_obj *o = obj_alloc(C_ByteBuffer, K_OBJECT);
    o->data = addr;
    jobject r = add_local(o);
    fj_set_field(r, "capacity", "J", fj_j(cap));
    TRACE("NewDirectByteBuffer %p cap %lld -> %p", addr, (long long)cap, r);
    return r;
}

static void *FJ_ABI j_GetDirectBufferAddress(JNIEnv *env, jobject buf)
{
    ENV_UNUSED;
    TRACE("GetDirectBufferAddress %p", buf);
    return fj_data(buf);
}

static jlong FJ_ABI j_GetDirectBufferCapacity(JNIEnv *env, jobject buf)
{
    ENV_UNUSED;
    TRACE("GetDirectBufferCapacity %p", buf);
    if (!O(buf)) return -1;
    return fj_get_field(buf, "capacity", "J").j;
}

static jobjectRefType FJ_ABI j_GetObjectRefType(JNIEnv *env, jobject o)
{
    ENV_UNUSED;
    fj_obj *p = O(o);
    if (!p) return JNIInvalidRefType;
    return p->globals > 0 ? JNIGlobalRefType : JNILocalRefType;
}

/* the function table, in jni.h order */
static const struct JNINativeInterface g_fns = {
    NULL, NULL, NULL, NULL,
    j_GetVersion, j_DefineClass, j_FindClass,
    j_FromReflectedMethod, j_FromReflectedField, j_ToReflectedMethod,
    j_GetSuperclass, j_IsAssignableFrom, j_ToReflectedField,
    j_Throw, j_ThrowNew, j_ExceptionOccurred, j_ExceptionDescribe, j_ExceptionClear, j_FatalError,
    j_PushLocalFrame, j_PopLocalFrame,
    j_NewGlobalRef, j_DeleteGlobalRef, j_DeleteLocalRef, j_IsSameObject, j_NewLocalRef,
    j_EnsureLocalCapacity, j_AllocObject, j_NewObject, j_NewObjectV, j_NewObjectA,
    j_GetObjectClass, j_IsInstanceOf, j_GetMethodID,
    j_CallObjectMethod, j_CallObjectMethodV, j_CallObjectMethodA,
    j_CallBooleanMethod, j_CallBooleanMethodV, j_CallBooleanMethodA,
    j_CallByteMethod, j_CallByteMethodV, j_CallByteMethodA,
    j_CallCharMethod, j_CallCharMethodV, j_CallCharMethodA,
    j_CallShortMethod, j_CallShortMethodV, j_CallShortMethodA,
    j_CallIntMethod, j_CallIntMethodV, j_CallIntMethodA,
    j_CallLongMethod, j_CallLongMethodV, j_CallLongMethodA,
    j_CallFloatMethod, j_CallFloatMethodV, j_CallFloatMethodA,
    j_CallDoubleMethod, j_CallDoubleMethodV, j_CallDoubleMethodA,
    j_CallVoidMethod, j_CallVoidMethodV, j_CallVoidMethodA,
    j_CallNonvirtualObjectMethod, j_CallNonvirtualObjectMethodV, j_CallNonvirtualObjectMethodA,
    j_CallNonvirtualBooleanMethod, j_CallNonvirtualBooleanMethodV, j_CallNonvirtualBooleanMethodA,
    j_CallNonvirtualByteMethod, j_CallNonvirtualByteMethodV, j_CallNonvirtualByteMethodA,
    j_CallNonvirtualCharMethod, j_CallNonvirtualCharMethodV, j_CallNonvirtualCharMethodA,
    j_CallNonvirtualShortMethod, j_CallNonvirtualShortMethodV, j_CallNonvirtualShortMethodA,
    j_CallNonvirtualIntMethod, j_CallNonvirtualIntMethodV, j_CallNonvirtualIntMethodA,
    j_CallNonvirtualLongMethod, j_CallNonvirtualLongMethodV, j_CallNonvirtualLongMethodA,
    j_CallNonvirtualFloatMethod, j_CallNonvirtualFloatMethodV, j_CallNonvirtualFloatMethodA,
    j_CallNonvirtualDoubleMethod, j_CallNonvirtualDoubleMethodV, j_CallNonvirtualDoubleMethodA,
    j_CallNonvirtualVoidMethod, j_CallNonvirtualVoidMethodV, j_CallNonvirtualVoidMethodA,
    j_GetFieldID,
    j_GetObjectField, j_GetBooleanField, j_GetByteField, j_GetCharField, j_GetShortField,
    j_GetIntField, j_GetLongField, j_GetFloatField, j_GetDoubleField,
    j_SetObjectField, j_SetBooleanField, j_SetByteField, j_SetCharField, j_SetShortField,
    j_SetIntField, j_SetLongField, j_SetFloatField, j_SetDoubleField,
    j_GetStaticMethodID,
    j_CallStaticObjectMethod, j_CallStaticObjectMethodV, j_CallStaticObjectMethodA,
    j_CallStaticBooleanMethod, j_CallStaticBooleanMethodV, j_CallStaticBooleanMethodA,
    j_CallStaticByteMethod, j_CallStaticByteMethodV, j_CallStaticByteMethodA,
    j_CallStaticCharMethod, j_CallStaticCharMethodV, j_CallStaticCharMethodA,
    j_CallStaticShortMethod, j_CallStaticShortMethodV, j_CallStaticShortMethodA,
    j_CallStaticIntMethod, j_CallStaticIntMethodV, j_CallStaticIntMethodA,
    j_CallStaticLongMethod, j_CallStaticLongMethodV, j_CallStaticLongMethodA,
    j_CallStaticFloatMethod, j_CallStaticFloatMethodV, j_CallStaticFloatMethodA,
    j_CallStaticDoubleMethod, j_CallStaticDoubleMethodV, j_CallStaticDoubleMethodA,
    j_CallStaticVoidMethod, j_CallStaticVoidMethodV, j_CallStaticVoidMethodA,
    j_GetStaticFieldID,
    j_GetStaticObjectField, j_GetStaticBooleanField, j_GetStaticByteField, j_GetStaticCharField,
    j_GetStaticShortField, j_GetStaticIntField, j_GetStaticLongField, j_GetStaticFloatField,
    j_GetStaticDoubleField,
    j_SetStaticObjectField, j_SetStaticBooleanField, j_SetStaticByteField, j_SetStaticCharField,
    j_SetStaticShortField, j_SetStaticIntField, j_SetStaticLongField, j_SetStaticFloatField,
    j_SetStaticDoubleField,
    j_NewString, j_GetStringLength, j_GetStringChars, j_ReleaseStringChars,
    j_NewStringUTF, j_GetStringUTFLength, j_GetStringUTFChars, j_ReleaseStringUTFChars,
    j_GetArrayLength, j_NewObjectArray, j_GetObjectArrayElement, j_SetObjectArrayElement,
    j_NewBooleanArray, j_NewByteArray, j_NewCharArray, j_NewShortArray,
    j_NewIntArray, j_NewLongArray, j_NewFloatArray, j_NewDoubleArray,
    j_GetBooleanArrayElements, j_GetByteArrayElements, j_GetCharArrayElements, j_GetShortArrayElements,
    j_GetIntArrayElements, j_GetLongArrayElements, j_GetFloatArrayElements, j_GetDoubleArrayElements,
    j_ReleaseBooleanArrayElements, j_ReleaseByteArrayElements, j_ReleaseCharArrayElements,
    j_ReleaseShortArrayElements, j_ReleaseIntArrayElements, j_ReleaseLongArrayElements,
    j_ReleaseFloatArrayElements, j_ReleaseDoubleArrayElements,
    j_GetBooleanArrayRegion, j_GetByteArrayRegion, j_GetCharArrayRegion, j_GetShortArrayRegion,
    j_GetIntArrayRegion, j_GetLongArrayRegion, j_GetFloatArrayRegion, j_GetDoubleArrayRegion,
    j_SetBooleanArrayRegion, j_SetByteArrayRegion, j_SetCharArrayRegion, j_SetShortArrayRegion,
    j_SetIntArrayRegion, j_SetLongArrayRegion, j_SetFloatArrayRegion, j_SetDoubleArrayRegion,
    j_RegisterNatives, j_UnregisterNatives,
    j_MonitorEnter, j_MonitorExit,
    j_GetJavaVM,
    j_GetStringRegion, j_GetStringUTFRegion,
    j_GetPrimitiveArrayCritical, j_ReleasePrimitiveArrayCritical,
    j_GetStringCritical, j_ReleaseStringCritical,
    j_NewWeakGlobalRef, j_DeleteWeakGlobalRef,
    j_ExceptionCheck,
    j_NewDirectByteBuffer, j_GetDirectBufferAddress, j_GetDirectBufferCapacity,
    j_GetObjectRefType,
};

/* ======================================================================================= */
/* JavaVM                                                                                  */
/* ======================================================================================= */

static jint FJ_ABI vm_DestroyJavaVM(JavaVM *vm) { (void)vm; fj_log("DestroyJavaVM (ignored)"); return JNI_OK; }

static jint FJ_ABI vm_AttachCurrentThread(JavaVM *vm, JNIEnv **penv, void *args)
{
    (void)vm; (void)args;
    fj_thread *t = TH();
    if (!t->attached) TRACE("AttachCurrentThread (re-attach)");
    else TRACE("AttachCurrentThread");
    t->attached = 1;
    if (penv) *penv = (JNIEnv *)&t->fns;
    return JNI_OK;
}

static jint FJ_ABI vm_DetachCurrentThread(JavaVM *vm)
{
    (void)vm;
    fj_thread *t = TH();
    TRACE("DetachCurrentThread (%d locals freed)", t->nloc);
    thread_free_locals(t);
    t->attached = 0;
    return JNI_OK;
}

static jint FJ_ABI vm_GetEnv(JavaVM *vm, void **penv, jint version)
{
    (void)vm;
    fj_thread *t = TH();      /* auto-attach: always succeeds */
    if (g_trace) fj_log("GetEnv(0x%x)", version);
    if (penv) *penv = (JNIEnv *)&t->fns;
    return JNI_OK;
}

static const struct JNIInvokeInterface g_ifns = {
    NULL, NULL, NULL,
    vm_DestroyJavaVM, vm_AttachCurrentThread, vm_DetachCurrentThread, vm_GetEnv,
    vm_AttachCurrentThread,   /* AttachCurrentThreadAsDaemon */
};

JavaVM *fj_vm(void) { fj_init(); return (JavaVM *)&g_vm_ptr; }
JNIEnv *fj_env(void) { return (JNIEnv *)&TH()->fns; }

/* ======================================================================================= */
/* built-in classes                                                                        */
/* ======================================================================================= */

static jvalue m_obj_hashCode(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{ (void)e; (void)a; (void)ud; return fj_i((jint)((uintptr_t)self >> 3)); }
static jvalue m_obj_equals(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{ (void)e; (void)ud; return fj_z(self == a[0].l); }
static jvalue m_obj_getClass(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{ (void)e; (void)a; (void)ud; return fj_l(fj_object_class(self)); }
static jvalue m_obj_toString(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{
    (void)e; (void)a; (void)ud;
    fj_obj *o = O(self);
    if (o && o->kind == K_STRING) return fj_l(self);
    char b[600];
    if (o && o->kind == K_CLASS) snprintf(b, sizeof b, "class %s", ((fj_class *)o)->name);
    else snprintf(b, sizeof b, "%s@%x", o ? o->cls->name : "null", (unsigned)((uintptr_t)self >> 3));
    for (char *p = b; *p; p++) if (*p == '/') *p = '.';
    return fj_l(fj_string(b));
}

static jvalue m_class_getName(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{
    (void)e; (void)a; (void)ud;
    fj_class *c = CL(self);
    char b[512];
    snprintf(b, sizeof b, "%s", c ? c->name : "null");
    for (char *p = b; *p; p++) if (*p == '/') *p = '.';
    return fj_l(fj_string(b));
}
static jvalue m_class_getSimpleName(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{
    (void)e; (void)a; (void)ud;
    fj_class *c = CL(self);
    const char *n = c ? c->name : "null";
    const char *s = strrchr(n, '/');
    s = s ? s + 1 : n;
    const char *d = strrchr(s, '$');
    return fj_l(fj_string(d ? d + 1 : s));
}
static jvalue m_class_forName(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{
    (void)self; (void)ud;
    const char *n = fj_cstr(a[0].l);
    if (!n) return fj_l(NULL);
    return fj_l(j_FindClass(e, n));
}

static jvalue m_str_length(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{ (void)e; (void)a; (void)ud; fj_obj *o = O(self); return fj_i(o && o->kind == K_STRING ? o->u.s.len : 0); }
static jvalue m_str_equals(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{
    (void)e; (void)ud;
    fj_obj *x = O(self), *y = O(a[0].l);
    if (!x || !y || x->kind != K_STRING || y->kind != K_STRING) return fj_z(self == a[0].l);
    return fj_z(x->u.s.len == y->u.s.len && !memcmp(x->u.s.chars, y->u.s.chars, x->u.s.len * sizeof(jchar)));
}
static jvalue m_str_hashCode(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{
    (void)e; (void)a; (void)ud;
    fj_obj *o = O(self);
    jint h = 0;
    if (o && o->kind == K_STRING) for (jsize i = 0; i < o->u.s.len; i++) h = 31 * h + o->u.s.chars[i];
    return fj_i(h);
}
static jvalue m_str_isEmpty(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{ (void)e; (void)a; (void)ud; fj_obj *o = O(self); return fj_z(!o || o->kind != K_STRING || !o->u.s.len); }
static jvalue m_str_getBytes(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{
    (void)e; (void)a; (void)ud;
    const char *s = fj_cstr(self);
    size_t n = s ? strlen(s) : 0;
    jarray arr = fj_array_new("B", (jsize)n);
    if (n) memcpy(fj_array_data(arr), s, n);
    return fj_l(arr);
}
static jvalue m_str_ctor_bytes(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{
    (void)e; (void)self; (void)ud;
    jsize n = fj_array_len(a[0].l);
    char *b = xcalloc(n + 1, 1);
    if (n) memcpy(b, fj_array_data(a[0].l), n);
    jstring s = fj_string(b);
    free(b);
    return fj_l(s);   /* substitute the allocated object */
}

static jvalue m_thr_getMessage(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{ (void)e; (void)a; (void)ud; jvalue v = fj_get_field(self, "detailMessage", "Ljava/lang/String;"); return fj_l(v.l ? add_local(O(v.l)) : NULL); }
static jvalue m_thr_toString(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{
    (void)e; (void)a; (void)ud;
    fj_obj *o = O(self);
    jvalue v = fj_get_field(self, "detailMessage", "Ljava/lang/String;");
    char b[1024];
    snprintf(b, sizeof b, "%s: %s", o ? o->cls->name : "?", v.l ? fj_cstr(v.l) : "");
    for (char *p = b; *p && *p != ':'; p++) if (*p == '/') *p = '.';
    return fj_l(fj_string(b));
}
static jvalue m_thr_ctor(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{ (void)e; (void)ud; fj_set_field(self, "detailMessage", "Ljava/lang/String;", fj_l(a[0].l)); return fj_l(NULL); }
static jvalue m_thr_getStackTrace(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{ (void)e; (void)self; (void)a; (void)ud; return fj_l(fj_array_new("Ljava/lang/StackTraceElement;", 0)); }

static jvalue m_meth_getName(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{ (void)e; (void)a; (void)ud; fj_method *m = M((jmethodID)fj_data(self)); return fj_l(fj_string(m ? m->name : "")); }

/* boxing */
#define BOX(cls, T, sig, getter, fld)                                                          \
static jvalue m_##T##_valueOf(JNIEnv *e, jobject self, const jvalue *a, void *ud)              \
{ (void)e; (void)self; (void)ud; jobject o = fj_new_by_name(cls, NULL, NULL);                  \
  fj_set_field(o, "value", sig, a[0]); return fj_l(o); }                                       \
static jvalue m_##T##_ctor(JNIEnv *e, jobject self, const jvalue *a, void *ud)                 \
{ (void)e; (void)ud; fj_set_field(self, "value", sig, a[0]); return fj_l(NULL); }              \
static jvalue m_##T##_get(JNIEnv *e, jobject self, const jvalue *a, void *ud)                  \
{ (void)e; (void)a; (void)ud; return fj_get_field(self, "value", sig); }
BOX("java/lang/Integer", Integer, "I", intValue, i)
BOX("java/lang/Long", Long, "J", longValue, j)
BOX("java/lang/Float", Float, "F", floatValue, f)
BOX("java/lang/Double", Double, "D", doubleValue, d)
BOX("java/lang/Boolean", Boolean, "Z", booleanValue, z)

static void define_box(const char *cls, const char *prim, fj_fn valueOf, fj_fn ctor, fj_fn get, const char *getter)
{
    char sig[64];
    fj_class_define(cls, "java/lang/Number");
    snprintf(sig, sizeof sig, "(%s)L%s;", prim, cls);
    FJ_STATIC(cls, "valueOf", sig, valueOf);
    snprintf(sig, sizeof sig, "(%s)V", prim);
    FJ_METHOD(cls, "<init>", sig, ctor);
    snprintf(sig, sizeof sig, "()%s", prim);
    FJ_METHOD(cls, getter, sig, get);
    fj_field_define(cls, "value", prim, 0, fj_v());
}

/* JNIBridge interface proxies (Unity's AndroidJavaProxy / native listeners) */
jobject fj_new_proxy(int kind, jlong id, jobjectArray interfaces)
{
    fj_init();
    fj_obj *p = obj_alloc(C_Proxy, K_OBJECT);
    jobject r = add_local(p);
    fj_set_field(r, "ptr", "J", fj_j(id));
    fj_set_field(r, "kind", "I", fj_i(kind));
    fj_set_field(r, "interfaces", "[Ljava/lang/Class;", fj_l(interfaces));
    if (g_trace) {
        fj_obj *ifs = O(interfaces);
        fj_class *c0 = (ifs && ifs->kind == K_ARRAY && ifs->u.a.len) ? CL((jobject)((fj_obj **)ifs->u.a.data)[0]) : NULL;
        fj_log("new proxy kind=%d id=%llx iface=%s -> %p", kind, (long long)id, c0 ? c0->name : "?", r);
    }
    return r;
}

static jvalue m_bridge_newProxy(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{
    (void)e; (void)self; (void)ud;
    return fj_l(fj_new_proxy(FJ_PROXY_JNIBRIDGE, a[0].j, a[1].l));
}
static jvalue m_bridge_disable(JNIEnv *e, jobject self, const jvalue *a, void *ud)
{ (void)e; (void)self; (void)ud; fj_set_field(a[0].l, "ptr", "J", fj_j(0)); return fj_v(); }

static jobject box_value(char t, jvalue v)
{
    switch (t) {
    case 'Z': return m_Boolean_valueOf(NULL, NULL, &v, NULL).l;
    case 'B': case 'C': case 'S': case 'I': { jvalue x = fj_i(t == 'B' ? v.b : t == 'C' ? v.c : t == 'S' ? v.s : v.i); return m_Integer_valueOf(NULL, NULL, &x, NULL).l; }
    case 'J': return m_Long_valueOf(NULL, NULL, &v, NULL).l;
    case 'F': return m_Float_valueOf(NULL, NULL, &v, NULL).l;
    case 'D': return m_Double_valueOf(NULL, NULL, &v, NULL).l;
    default: return v.l;
    }
}

static jvalue unbox_value(char t, jobject o)
{
    if (t == 'L' || t == 'V' || !o) return fj_l(t == 'L' ? o : NULL);
    fj_obj *b = O(o);
    if (!b) return fj_v();
    const char *sig = t == 'Z' ? "Z" : t == 'J' ? "J" : t == 'F' ? "F" : t == 'D' ? "D" : "I";
    /* boxed value of the actual box type */
    const char *bn = b->cls->name;
    const char *bs = !strcmp(bn, "java/lang/Long") ? "J" : !strcmp(bn, "java/lang/Float") ? "F" :
                     !strcmp(bn, "java/lang/Double") ? "D" : !strcmp(bn, "java/lang/Boolean") ? "Z" : "I";
    jvalue v = fj_get_field(o, "value", bs);
    (void)sig;
    return v;
}

static jvalue proxy_invoke(JNIEnv *env, fj_obj *p, fj_method *m, const jvalue *args)
{
    jvalue r; r.j = 0;
    jlong ptr = fj_get_field((jobject)p, "ptr", "J").j;
    int kind = fj_get_field((jobject)p, "kind", "I").i;
    if (!ptr && kind != FJ_PROXY_REFLECTION) { TRACE("proxy %p disabled; %s ignored", (void *)p, m->name); return r; }
    fj_class *bridge = kind == FJ_PROXY_REFLECTION ? class_get("com/unity3d/player/ReflectionHelper", 0)
                                                   : class_get("bitter/jnibridge/JNIBridge", 0);
    const char *nname = kind == FJ_PROXY_REFLECTION ? "nativeProxyInvoke" : "invoke";
    fj_native_rec *n = bridge ? native_in(bridge, nname, NULL) : NULL;
    if (!n || !n->fn) { fj_log("proxy call %s.%s but %s not registered", m->cls->name, m->name, nname); return r; }
    fj_push_frame();
    jobjectArray arr = j_NewObjectArray(env, m->ps.nargs, (jclass)C_Object, NULL);
    for (int i = 0; i < m->ps.nargs; i++) objarray_set(O(arr), i, box_value(m->ps.a[i], args[i]));
    jvalue a[4];
    if (kind == FJ_PROXY_REFLECTION) {
        a[0] = fj_i((jint)ptr);
        a[1] = fj_l(fj_string(m->name));
        a[2] = fj_l(arr);
    } else {
        a[0] = fj_j(ptr);
        a[1] = fj_l((jobject)m->cls);
        a[2] = fj_l(j_ToReflectedMethod(env, (jclass)m->cls, (jmethodID)m, 0));
        a[3] = fj_l(arr);
    }
    TRACE("proxy -> %s(%llx, %s.%s)", nname, (long long)ptr, m->cls->name, m->name);
    jvalue res = fj_invoke_native(n->fn, env, (jobject)bridge, n->sig, a);
    fj_obj *ro = O(res.l);
    if (ro) obj_ref(ro);
    fj_pop_frame();
    r = unbox_value(m->ps.ret, res.l);
    if (m->ps.ret == 'L' && ro) r.l = add_local(ro);
    if (ro) obj_unref(ro);
    return r;
}

static void define_builtins(void)
{
    C_Object = class_new("java/lang/Object", NULL, 0);
    C_Class = class_new("java/lang/Class", C_Object, 0);
    C_Object->obj.cls = C_Class;
    C_Class->obj.cls = C_Class;
    C_String = (fj_class *)fj_class_define("java/lang/String", NULL);
    fj_class_implements("java/lang/String", "java/lang/CharSequence");

    FJ_METHOD("java/lang/Object", "hashCode", "()I", m_obj_hashCode);
    FJ_METHOD("java/lang/Object", "equals", "(Ljava/lang/Object;)Z", m_obj_equals);
    FJ_METHOD("java/lang/Object", "toString", "()Ljava/lang/String;", m_obj_toString);
    FJ_METHOD("java/lang/Object", "getClass", "()Ljava/lang/Class;", m_obj_getClass);
    FJ_METHOD("java/lang/Object", "<init>", "()V", NULL);

    FJ_METHOD("java/lang/Class", "getName", "()Ljava/lang/String;", m_class_getName);
    FJ_METHOD("java/lang/Class", "getSimpleName", "()Ljava/lang/String;", m_class_getSimpleName);
    FJ_STATIC("java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;", m_class_forName);

    FJ_METHOD("java/lang/String", "length", "()I", m_str_length);
    FJ_METHOD("java/lang/String", "equals", "(Ljava/lang/Object;)Z", m_str_equals);
    FJ_METHOD("java/lang/String", "hashCode", "()I", m_str_hashCode);
    FJ_METHOD("java/lang/String", "isEmpty", "()Z", m_str_isEmpty);
    FJ_METHOD("java/lang/String", "getBytes", "()[B", m_str_getBytes);
    FJ_METHOD("java/lang/String", "getBytes", "(Ljava/lang/String;)[B", m_str_getBytes);
    FJ_METHOD("java/lang/String", "<init>", "([B)V", m_str_ctor_bytes);
    FJ_METHOD("java/lang/String", "<init>", "([BLjava/lang/String;)V", m_str_ctor_bytes);

    C_Throwable = (fj_class *)fj_class_define("java/lang/Throwable", NULL);
    fj_field_define("java/lang/Throwable", "detailMessage", "Ljava/lang/String;", 0, fj_v());
    FJ_METHOD("java/lang/Throwable", "getMessage", "()Ljava/lang/String;", m_thr_getMessage);
    FJ_METHOD("java/lang/Throwable", "getLocalizedMessage", "()Ljava/lang/String;", m_thr_getMessage);
    FJ_METHOD("java/lang/Throwable", "toString", "()Ljava/lang/String;", m_thr_toString);
    FJ_METHOD("java/lang/Throwable", "<init>", "(Ljava/lang/String;)V", m_thr_ctor);
    FJ_METHOD("java/lang/Throwable", "getStackTrace", "()[Ljava/lang/StackTraceElement;", m_thr_getStackTrace);
    FJ_METHOD("java/lang/Throwable", "setStackTrace", "([Ljava/lang/StackTraceElement;)V", NULL);
    FJ_METHOD("java/lang/Throwable", "printStackTrace", "()V", NULL);
    fj_class_define("java/lang/Exception", "java/lang/Throwable");
    fj_class_define("java/lang/Error", "java/lang/Throwable");
    fj_class_define("java/lang/RuntimeException", "java/lang/Exception");
    fj_class_define("java/lang/LinkageError", "java/lang/Error");
    fj_class_define("java/lang/NoClassDefFoundError", "java/lang/LinkageError");
    fj_class_define("java/lang/ClassNotFoundException", "java/lang/Exception");
    fj_class_define("java/lang/NoSuchMethodError", "java/lang/LinkageError");
    fj_class_define("java/lang/NoSuchFieldError", "java/lang/LinkageError");
    fj_class_define("java/lang/IllegalArgumentException", "java/lang/RuntimeException");
    fj_class_define("java/lang/IllegalStateException", "java/lang/RuntimeException");
    fj_class_define("java/lang/NullPointerException", "java/lang/RuntimeException");
    fj_class_define("java/lang/SecurityException", "java/lang/RuntimeException");
    fj_class_define("java/lang/ClassCastException", "java/lang/RuntimeException");
    fj_class_define("java/lang/UnsupportedOperationException", "java/lang/RuntimeException");
    fj_class_define("java/lang/IndexOutOfBoundsException", "java/lang/RuntimeException");
    fj_class_define("java/lang/ArrayIndexOutOfBoundsException", "java/lang/IndexOutOfBoundsException");
    fj_class_define("java/lang/StringIndexOutOfBoundsException", "java/lang/IndexOutOfBoundsException");
    fj_class_define("java/util/NoSuchElementException", "java/lang/RuntimeException");
    fj_class_define("java/io/IOException", "java/lang/Exception");
    fj_class_define("java/lang/StackTraceElement", NULL);
    fj_class_define("java/lang/CharSequence", NULL);
    fj_class_define("java/lang/Runnable", NULL);
    fj_class_define("java/lang/Number", NULL);

    C_Method = (fj_class *)fj_class_define("java/lang/reflect/Method", NULL);
    FJ_METHOD("java/lang/reflect/Method", "getName", "()Ljava/lang/String;", m_meth_getName);
    C_Field = (fj_class *)fj_class_define("java/lang/reflect/Field", NULL);
    fj_class_define("java/nio/Buffer", NULL);
    C_ByteBuffer = (fj_class *)fj_class_define("java/nio/ByteBuffer", "java/nio/Buffer");
    fj_class_define("java/nio/DirectByteBuffer", "java/nio/ByteBuffer");

    define_box("java/lang/Integer", "I", m_Integer_valueOf, m_Integer_ctor, m_Integer_get, "intValue");
    define_box("java/lang/Long", "J", m_Long_valueOf, m_Long_ctor, m_Long_get, "longValue");
    define_box("java/lang/Float", "F", m_Float_valueOf, m_Float_ctor, m_Float_get, "floatValue");
    define_box("java/lang/Double", "D", m_Double_valueOf, m_Double_ctor, m_Double_get, "doubleValue");
    define_box("java/lang/Boolean", "Z", m_Boolean_valueOf, m_Boolean_ctor, m_Boolean_get, "booleanValue");

    C_Proxy = (fj_class *)fj_class_define("java/lang/reflect/Proxy", NULL);
    fj_class_define("bitter/jnibridge/JNIBridge", NULL);
    FJ_STATIC("bitter/jnibridge/JNIBridge", "newInterfaceProxy", "(J[Ljava/lang/Class;)Ljava/lang/Object;", m_bridge_newProxy);
    FJ_STATIC("bitter/jnibridge/JNIBridge", "disableInterfaceProxy", "(Ljava/lang/Object;)V", m_bridge_disable);
}

/* ======================================================================================= */
/* init / dump                                                                             */
/* ======================================================================================= */

static void dump_at_exit(void) { fj_dump(g_log ? g_log : stderr); }

static void fj_init_once(void)
{
    const char *lf = getenv("FJ_LOG");
    if (lf && *lf) g_log = fopen(lf, "w");
    g_trace = getenv("FJ_TRACE") && atoi(getenv("FJ_TRACE"));
    g_nofree = getenv("FJ_NOFREE") && atoi(getenv("FJ_NOFREE"));
    g_strict = getenv("FJ_STRICT") && atoi(getenv("FJ_STRICT"));
    g_dump = getenv("FJ_DUMP") && atoi(getenv("FJ_DUMP"));
    pthread_key_create(&g_tkey, thread_dtor);
    t_in_init = 1;
    define_builtins();
    t_in_init = 0;
    __atomic_store_n(&g_inited, 1, __ATOMIC_RELEASE);
    if (g_dump) atexit(dump_at_exit);
}

void fj_init(void)
{
    if (__builtin_expect(g_inited, 1) || t_in_init) return;
    pthread_once(&g_once, fj_init_once);
}

void fj_dump(FILE *out)
{
    if (!out) out = stderr;
    LOCK();
    fprintf(out, "==== fakejni usage report ====\n");
    for (int b = 0; b < HBUCKETS; b++) {
        for (fj_class *c = g_classes[b]; c; c = c->hnext) {
            int used = c->lookups > 0 || c->natives;
            for (fj_method *m = c->methods; m && !used; m = m->next) used = m->calls > 0 || m->placeholder;
            for (fj_field *f = c->fields; f && !used; f = f->next) used = f->gets || f->sets || f->placeholder;
            if (!used) continue;
            fprintf(out, "class %s%s%s (FindClass x%lu)\n", c->name, c->placeholder ? " [PLACEHOLDER]" : "",
                    c->absent ? " [ABSENT]" : "", c->lookups);
            for (fj_method *m = c->methods; m; m = m->next) {
                if (!m->calls && !m->placeholder) continue;
                fprintf(out, "  %s%s%s  calls=%lu%s\n", m->is_static ? "static " : "", m->name, m->sig, m->calls,
                        m->placeholder ? " [UNKNOWN]" : (m->fn ? "" : m->native_fn ? " [native]" : " [no handler]"));
            }
            for (fj_field *f = c->fields; f; f = f->next) {
                if (!f->gets && !f->sets && !f->placeholder) continue;
                fprintf(out, "  field %s%s %s  get=%lu set=%lu%s\n", f->is_static ? "static " : "", f->name, f->sig,
                        f->gets, f->sets, f->placeholder ? " [UNKNOWN]" : "");
            }
            for (fj_native_rec *n = c->natives; n; n = n->next)
                fprintf(out, "  native %s%s -> %p\n", n->name, n->sig, n->fn);
        }
    }
    fprintf(out, "==== end ====\n");
    fflush(out);
    UNLOCK();
}
