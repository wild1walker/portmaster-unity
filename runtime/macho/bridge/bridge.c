// bridge.c - the library Unity's Android player loads as its libil2cpp.so when the game's code
// is a macOS build's GameAssembly.dylib: the image is loaded here (../dyld.c), the il2cpp_*
// functions the player calls are passed on to it (bridge.S), and the engine functions the
// player registers for the game's code (internal calls) are wrapped where the two sides'
// calling conventions differ. docs/macho-route.md.
#define _GNU_SOURCE
#include "ua_macho.h"
#include "../darwin.h"
#include "../cxx/ua_unwind.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern char **environ;

#define LOG(...) fprintf(stderr, "[macho] " __VA_ARGS__)
#define HIDDEN __attribute__((visibility("hidden")))
#define DIE(...) do { LOG(__VA_ARGS__); fflush(NULL); abort(); } while (0)

static struct ua_macho_host host;
static struct darwin_image *ga;

// ---- threads ---------------------------------------------------------------------------
// set once a thread has what macOS code needs of it (bridge.S reads it on every call)
HIDDEN __thread char ua_thread_in __attribute__((tls_model("initial-exec")));

HIDDEN void ua_macho_thread_first(void)
{
    darwin_thread_enter();
    ua_thread_in = 1;
}

// ---- the API ---------------------------------------------------------------------------
struct api_row { const char *name; void **slot, **alt; };      // alt: bridge.S, APIH
extern const struct api_row __start_ua_api_tab[], __stop_ua_api_tab[];

HIDDEN void ua_api_missing(const char *name)
{
    DIE("the player called %s, which the game's GameAssembly does not export\n", name);
}

static void *ga_fn(const char *name)
{
    char sym[128];
    snprintf(sym, sizeof sym, "_%s", name);
    void *p = macho_export(ga->m, sym);
    if (!p) DIE("GameAssembly exports no %s\n", name);
    return p;
}

// What the bridge itself asks the image (all arguments pointers or 32 bits wide: nothing to
// extend; the image extends its results).
typedef struct Il2CppClass Il2CppClass;
typedef struct Il2CppType Il2CppType;
typedef struct MethodInfo MethodInfo;
typedef struct FieldInfo FieldInfo;
static struct {
    void (*add_internal_call)(const char *, void *);
    void *(*domain_get)(void);
    void **(*domain_get_assemblies)(void *, size_t *);
    void *(*assembly_get_image)(void *);
    Il2CppClass *(*class_from_name)(void *, const char *, const char *);
    Il2CppClass *(*class_get_nested_types)(Il2CppClass *, void **);
    const char *(*class_get_name)(Il2CppClass *);
    const MethodInfo *(*class_get_methods)(Il2CppClass *, void **);
    const char *(*method_get_name)(const MethodInfo *);
    uint32_t (*method_get_flags)(const MethodInfo *, uint32_t *);
    uint32_t (*method_get_param_count)(const MethodInfo *);
    const Il2CppType *(*method_get_param)(const MethodInfo *, uint32_t);
    const Il2CppType *(*method_get_return_type)(const MethodInfo *);
    int (*method_is_instance)(const MethodInfo *);
    int (*type_get_type)(const Il2CppType *);
    int (*type_is_byref)(const Il2CppType *);
    Il2CppClass *(*class_from_type)(const Il2CppType *);
    int (*class_is_valuetype)(Il2CppClass *);
    int (*class_is_enum)(Il2CppClass *);
    const Il2CppType *(*class_enum_basetype)(Il2CppClass *);
    int32_t (*class_value_size)(Il2CppClass *, uint32_t *);
    FieldInfo *(*class_get_fields)(Il2CppClass *, void **);
    int (*field_get_flags)(FieldInfo *);
    const Il2CppType *(*field_get_type)(FieldInfo *);
    void *(*get_corlib)(void);
    const char *(*class_get_namespace)(Il2CppClass *);
    Il2CppClass *(*class_get_declaring_type)(Il2CppClass *);
    const Il2CppType *(*class_get_type)(Il2CppClass *);
    void *(*value_box)(Il2CppClass *, void *);
    const MethodInfo *(*class_get_method_from_name)(Il2CppClass *, const char *, int);
    void *(*class_get_image)(Il2CppClass *);
    const char *(*class_get_assemblyname)(Il2CppClass *);
    const char *(*image_get_name)(void *);
} il;

// ---- internal calls --------------------------------------------------------------------
// The player registers its functions by name (il2cpp_add_internal_call) before the runtime
// starts; the image's code asks for one by name at its first use and keeps the pointer. So
// each is registered as a stub of ours, and what the stub does is decided at its first call,
// when the method's signature can be read (bridge.S: ua_icall_lazy, ua_icall_thunk).
struct icall_rec { void *handler, *target; const struct icall_layout *layout; };
struct icall_move { uint16_t from, to, bytes; };
struct icall_layout { uint8_t result, pad; uint16_t stack, n, pad2; struct icall_move moves[]; };
_Static_assert(sizeof(struct icall_move) == 6 && sizeof(struct icall_layout) == 8, "bridge.S reads these");

#define BLOCK_STUBS 4096
#define BLOCK_CODE  (BLOCK_STUBS * 16)                  // a multiple of every page size
struct icall_block {
    struct icall_block *next;
    uint32_t *code;
    struct icall_rec *recs;
    int used;
    const char *names[BLOCK_STUBS];
};
static struct icall_block *blocks;
static pthread_mutex_t icall_lock = PTHREAD_MUTEX_INITIALIZER;
static long n_icalls, n_plain, n_thunked;

HIDDEN void ua_icall_lazy(void);
HIDDEN void ua_icall_thunk(void);

static struct icall_block *block_new(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t data = (BLOCK_STUBS * sizeof(struct icall_rec) + page - 1) & ~(page - 1);
    struct icall_block *b = calloc(1, sizeof *b);
    uint8_t *m = mmap(NULL, BLOCK_CODE + data, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!b || m == MAP_FAILED) DIE("no memory for the internal calls' stubs\n");
    b->code = (uint32_t *)m;
    b->recs = (struct icall_rec *)(m + BLOCK_CODE);
    for (int i = 0; i < BLOCK_STUBS; i++) {
        uint32_t d = (uint32_t)((uint8_t *)&b->recs[i] - (uint8_t *)&b->code[i * 4]);
        b->code[i * 4 + 0] = 0x10000011u | (d & 3) << 29 | (d >> 2 & 0x7ffff) << 5;    // adr x17, its record
        b->code[i * 4 + 1] = 0xf9400230u;                                               // ldr x16, [x17]
        b->code[i * 4 + 2] = 0xd61f0200u;                                               // br x16
        b->code[i * 4 + 3] = 0xd4200000u;                                               // brk
    }
    if (mprotect(m, BLOCK_CODE, PROT_READ | PROT_EXEC) != 0) DIE("the internal calls' stubs: mprotect failed\n");
    __builtin___clear_cache((char *)m, (char *)m + BLOCK_CODE);
    b->next = blocks;
    blocks = b;
    return b;
}

// il2cpp_add_internal_call, as the player calls it
static void add_internal_call(const char *name, void *fn)
{
    pthread_mutex_lock(&icall_lock);
    struct icall_block *b = blocks;
    if (!b || b->used == BLOCK_STUBS) b = block_new();
    int i = b->used++;
    b->names[i] = name;                     // the player's own constant (the image keeps it too)
    b->recs[i].target = fn;
    b->recs[i].handler = (void *)ua_icall_lazy;
    n_icalls++;
    pthread_mutex_unlock(&icall_lock);
    il.add_internal_call(name, &b->code[i * 4]);
}

// IL2CPP's type codes (ECMA-335's element types)
enum { T_BOOLEAN = 2, T_CHAR, T_I1, T_U1, T_I2, T_U2, T_I4, T_U4, T_I8, T_U8, T_R4, T_R8, T_VALUETYPE = 0x11, T_GENERICINST = 0x15,
       T_TYPEDBYREF = 0x16 };

// How a value travels: in an integer register (or 8 bytes of stack on the player's side, its
// own size on the image's), a float register, or as a struct.
struct passed { enum { P_INT, P_FLOAT, P_HFA, P_STRUCT } how; int size, count; int narrow; };

// A struct's fields, flattened: all float or all double, at most four, nothing else -> the
// count (a homogeneous aggregate, passed in float registers); 0 else.
static int hfa_fields(Il2CppClass *k, int *elem, int depth)
{
    if (depth > 8) return -1;
    void *iter = NULL;
    FieldInfo *f;
    int n = 0;
    while ((f = il.class_get_fields(k, &iter))) {
        if (il.field_get_flags(f) & 0x10) continue;         // static
        const Il2CppType *t = il.field_get_type(f);
        int code = il.type_get_type(t);
        if (il.type_is_byref(t)) return -1;
        if (code == T_R4 || code == T_R8) {
            if (*elem && *elem != code) return -1;
            *elem = code;
            n++;
        } else if (code == T_VALUETYPE || code == T_GENERICINST) {
            Il2CppClass *fk = il.class_from_type(t);
            if (!fk || !il.class_is_valuetype(fk) || il.class_is_enum(fk)) return -1;
            int m = hfa_fields(fk, elem, depth + 1);
            if (m <= 0) return -1;
            n += m;
        } else return -1;
        if (n > 4) return -1;
    }
    return n;
}

static struct passed how_passed(const Il2CppType *t)
{
    struct passed p = { P_INT, 8, 1, 0 };
    if (il.type_is_byref(t)) return p;
    for (;;) {
        int code = il.type_get_type(t);
        switch (code) {
        case T_BOOLEAN: case T_U1: p.size = 1; p.narrow = 1; return p;
        case T_I1: p.size = 1; p.narrow = 2; return p;
        case T_CHAR: case T_U2: p.size = 2; p.narrow = 3; return p;
        case T_I2: p.size = 2; p.narrow = 4; return p;
        case T_I4: case T_U4: p.size = 4; return p;
        case T_R4: p.how = P_FLOAT; p.size = 4; return p;
        case T_R8: p.how = P_FLOAT; p.size = 8; return p;
        case T_TYPEDBYREF: p.how = P_STRUCT; p.size = 24; return p;
        case T_VALUETYPE: case T_GENERICINST: {
            Il2CppClass *k = il.class_from_type(t);
            if (!k || !il.class_is_valuetype(k)) return p;          // a generic class: a reference
            if (il.class_is_enum(k)) { t = il.class_enum_basetype(k); continue; }
            uint32_t align = 0;
            int elem = 0;
            p.size = il.class_value_size(k, &align);
            int n = hfa_fields(k, &elem, 0);
            if (n > 0 && p.size == n * (elem == T_R4 ? 4 : 8)) { p.how = P_HFA; p.count = n; p.size /= n; }
            else p.how = P_STRUCT;
            return p;
        }
        default: return p;                  // references, pointers, native ints, 64-bit ints
        }
    }
}

// One method's layout: 0 with *out NULL when both sides agree on the whole call; -1 with why
// when the thunk cannot carry it.
static int method_layout(const MethodInfo *m, struct icall_layout **out, char *why, size_t whylen)
{
    struct icall_move moves[64];
    int n = 0, ngrn = il.method_is_instance(m) ? 1 : 0, nsrn = 0;
    unsigned from = 0, to = 0;
    uint32_t count = il.method_get_param_count(m);
    for (uint32_t i = 0; i < count; i++) {
        struct passed p = how_passed(il.method_get_param(m, i));
        int stack = 0;
        switch (p.how) {
        case P_INT:   if (ngrn < 8) ngrn++; else stack = 1; break;
        case P_FLOAT: if (nsrn < 8) nsrn++; else stack = 1; break;
        case P_HFA:   if (nsrn + p.count <= 8) nsrn += p.count; else stack = 2; break;
        case P_STRUCT:
            if (p.size > 16) { if (ngrn < 8) ngrn++; else { stack = 1; p.size = 8; } }      // a pointer to a copy
            else if (ngrn + (p.size + 7) / 8 <= 8) ngrn += (p.size + 7) / 8;
            else stack = 2;
            break;
        }
        if (stack == 2) { snprintf(why, whylen, "argument %u is a struct passed on the stack", i); return -1; }
        if (!stack) continue;
        if (n == 64) { snprintf(why, whylen, "more than 64 arguments on the stack"); return -1; }
        from = (from + p.size - 1) & ~(unsigned)(p.size - 1);
        moves[n].from = (uint16_t)from;
        moves[n].to = (uint16_t)to;
        moves[n].bytes = (uint16_t)p.size;
        from += p.size;
        to += 8;
        n++;
    }
    struct passed r = how_passed(il.method_get_return_type(m));
    int result = r.how == P_INT ? r.narrow : 0;
    *out = NULL;
    if (!n && !result) return 0;
    struct icall_layout *l = calloc(1, sizeof *l + n * sizeof l->moves[0]);
    if (!l) DIE("no memory for an internal call's layout\n");
    l->result = (uint8_t)result;
    l->stack = (uint16_t)((to + 15) & ~15u);
    l->n = (uint16_t)n;
    memcpy(l->moves, moves, n * sizeof moves[0]);
    *out = l;
    return 0;
}

static int same_layout(const struct icall_layout *a, const struct icall_layout *b)
{
    if (!a || !b) return a == b;
    return a->n == b->n && !memcmp(a, b, sizeof *a + a->n * sizeof a->moves[0]);
}

// "Namespace.Outer/Nested" in the loaded assemblies
static Il2CppClass *find_class(char *full)
{
    char *nested = strchr(full, '/');
    if (nested) *nested++ = 0;
    char *dot = strrchr(full, '.');
    const char *ns = dot ? full : "", *name = dot ? dot + 1 : full;
    if (dot) *dot = 0;
    size_t count = 0;
    void **as = il.domain_get_assemblies(il.domain_get(), &count);
    Il2CppClass *k = NULL;
    for (size_t i = 0; i < count && !k; i++) k = il.class_from_name(il.assembly_get_image(as[i]), ns, name);
    while (k && nested) {
        char *next = strchr(nested, '/');
        if (next) *next++ = 0;
        void *iter = NULL;
        Il2CppClass *in;
        while ((in = il.class_get_nested_types(k, &iter)) && strcmp(il.class_get_name(in), nested) != 0) { }
        k = in;
        nested = next;
    }
    return k;
}

// The first call of an internal call (from ua_icall_lazy, on the calling thread, every
// argument register saved): sets the record's handler.
HIDDEN void ua_icall_classify(struct icall_rec *rec)
{
    const char *name = NULL;
    pthread_mutex_lock(&icall_lock);
    for (struct icall_block *b = blocks; b; b = b->next)
        if (rec >= b->recs && rec < b->recs + BLOCK_STUBS) name = b->names[rec - b->recs];
    if (!name) DIE("an internal call's stub with no record\n");
    if (rec->handler != (void *)ua_icall_lazy) { pthread_mutex_unlock(&icall_lock); return; }

    char buf[512];
    if (strlen(name) >= sizeof buf) DIE("internal call %s: the name is too long\n", name);
    strcpy(buf, name);
    char *method = strstr(buf, "::");
    if (!method) DIE("internal call %s: not Class::Method\n", name);
    *method = 0;
    method += 2;
    char *paren = strchr(method, '(');
    if (paren) *paren = 0;
    Il2CppClass *k = find_class(buf);
    if (!k) DIE("internal call %s: the game's code calls it, its metadata has no such class\n", name);

    struct icall_layout *layout = NULL;
    int found = 0;
    void *iter = NULL;
    const MethodInfo *m;
    while ((m = il.class_get_methods(k, &iter))) {
        uint32_t iflags = 0;
        if (strcmp(il.method_get_name(m), method) != 0) continue;
        il.method_get_flags(m, &iflags);
        if (!(iflags & 0x1000)) continue;                   // not an internal call: a managed overload
        struct icall_layout *l = NULL;
        char why[128];
        if (method_layout(m, &l, why, sizeof why) != 0) DIE("internal call %s: %s, which the bridge does not carry\n", name, why);
        if (found && !same_layout(l, layout))
            DIE("internal call %s: overloads that are passed differently share the name\n", name);
        if (found) free(l); else layout = l;
        found++;
    }
    if (!found) DIE("internal call %s: the game's code calls it, its metadata has no such method\n", name);
    rec->layout = layout;
    if (layout) n_thunked++; else n_plain++;
    if (layout && getenv("UA_MACHO_LOG"))
        LOG("internal call %s: result kind %d, %d stack arguments laid out again\n", name, layout->result, layout->n);
    __atomic_store_n(&rec->handler, layout ? (void *)ua_icall_thunk : rec->target, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&icall_lock);
}

// ---- what only the Android engine modules have -------------------------------------------
// Unity's engine modules (UnityEngine.*.dll) differ a little by platform, and the Android
// player finds and calls a few managed methods that only its own modules have, in classes a
// macOS build may not have kept at all. Each is answered here as the macOS module behaves, so
// the game sees what it would on a Mac. (How the set is found, per Unity version:
// docs/macho-route.md.)
//
// The player gets such a class or method as a pointer into ua_standin_space, one page that
// holds nothing else. Every il2cpp function with a class or a method among its arguments
// looks for such a pointer first (bridge.S, APIH) and then calls the function of standin_alts
// instead of the image's; one with no function there stops with its name: the image never
// sees a pointer of ours.
struct sclass {
    const char *image, *ns, *outer, *name;      // of the Android module
    uint32_t flags;
    uint8_t found, said;                        // set once the three below are (outer_is)
    Il2CppClass *outer_k;                       // the image's outer class
    Il2CppClass *real;                          // the image's own class of this name, if the build kept it
};
struct smethod {
    uint8_t klass, argc, result, params[2];     // index of its class; element types (0: void)
    uint16_t flags;
    uint8_t said;
    const char *name, *param_names[2];
    uint64_t (*run)(void **params);             // the result's value
};
enum { N_SCLASS = 1, N_SMETHOD = 2 };
union standin_space {
    struct { struct sclass classes[N_SCLASS]; struct smethod methods[N_SMETHOD]; } s;
    char page[4096];
};
_Static_assert(sizeof(union standin_space) == 4096, "one page: bridge.S tells the pointers by it");

static uint64_t standin_false(void **params) { return 0; }
static uint64_t standin_nothing(void **params) { return 0; }

HIDDEN union standin_space ua_standin_space __attribute__((aligned(4096))) = { .s = {
    .classes = {
        // AudioSettings.Mobile: abstract sealed nested public beforefieldinit
        { "UnityEngine.AudioModule.dll", "UnityEngine", "AudioSettings", "Mobile", 0x100182, 0, 0, NULL, NULL },
    },
    .methods = {
        // internal static bool: AudioSettings.Mobile.stopAudioOutputOnMute, which the macOS
        // module has as the constant false (its setter only warns)
        { 0, 0, T_BOOLEAN, { 0 }, 0x0093, 0, "InvokeIsStopAudioOutputOnMuteEnabled", { 0 }, standin_false },
        // internal static void: Android's mute switch. Keeps muteState and raises
        // OnMuteStateChanged there; in the macOS module muteState is the constant false and
        // nothing ever raises the event
        { 0, 1, 0, { T_BOOLEAN }, 0x0093, 0, "InvokeOnMuteStateChanged", { "mute" }, standin_nothing },
    },
} };
#define SCLASSES ua_standin_space.s.classes
#define SMETHODS ua_standin_space.s.methods

static struct sclass *sclass_of(const void *p)
{
    uintptr_t d = (uintptr_t)p - (uintptr_t)SCLASSES;
    return d < sizeof SCLASSES && d % sizeof(struct sclass) == 0 ? (struct sclass *)p : NULL;
}
static struct smethod *smethod_of(const void *p)
{
    uintptr_t d = (uintptr_t)p - (uintptr_t)SMETHODS;
    return d < sizeof SMETHODS && d % sizeof(struct smethod) == 0 ? (struct smethod *)p : NULL;
}
// For a function of standin_alts: its class or method argument, which has to be ours.
static struct sclass *sclass_arg(const void *p, const char *fn)
{
    struct sclass *c = sclass_of(p);
    if (!c) DIE("%s: called with a method, or with a class of the game's, beside one the bridge stands in for\n", fn);
    return c;
}
static struct smethod *smethod_arg(const void *p, const char *fn)
{
    struct smethod *m = smethod_of(p);
    if (!m) DIE("%s: called with a class, or with a method of the game's, beside one the bridge stands in for\n", fn);
    return m;
}

static void said(uint8_t *flag, const struct sclass *c, const char *method)
{
    if (__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED) || !getenv("UA_MACHO_LOG")) return;
    LOG("%s.%s/%s%s%s: only the Android modules have it, answered by the bridge\n", c->ns, c->outer, c->name, method ? "::" : "",
        method ? method : "");
}

// Whether k is the class c is nested in. The first yes also looks for c among k's own nested
// classes: a build that kept the class has it without the Android methods.
static int outer_is(struct sclass *c, Il2CppClass *k)
{
    if (__atomic_load_n(&c->found, __ATOMIC_ACQUIRE)) return c->outer_k == k;
    if (il.class_get_declaring_type(k) || strcmp(il.class_get_name(k), c->outer) != 0 ||
        strcmp(il.class_get_namespace(k), c->ns) != 0 || strcmp(il.image_get_name(il.class_get_image(k)), c->image) != 0)
        return 0;
    Il2CppClass *n, *real = NULL;
    void *iter = NULL;
    while (!real && (n = il.class_get_nested_types(k, &iter)))
        if (!strcmp(il.class_get_name(n), c->name)) real = n;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    c->outer_k = k;
    c->real = real;
    __atomic_store_n(&c->found, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&lock);
    return 1;
}

// The record of the image's own class k, if it is one whose Android methods are answered here.
static struct sclass *sclass_kept(Il2CppClass *k)
{
    for (struct sclass *c = SCLASSES; c < SCLASSES + N_SCLASS; c++) {
        if (__atomic_load_n(&c->found, __ATOMIC_ACQUIRE)) {
            if (c->real == k) return c;
            continue;
        }
        Il2CppClass *outer;
        if (!strcmp(il.class_get_name(k), c->name) && (outer = il.class_get_declaring_type(k)) && outer_is(c, outer) && c->real == k)
            return c;
    }
    return NULL;
}

// c's next method from SMETHODS[i] on that the image's class does not have itself.
static const MethodInfo *smethod_next(struct sclass *c, size_t i, const char *name, int argc)
{
    for (; i < N_SMETHOD; i++) {
        struct smethod *m = &SMETHODS[i];
        if (&SCLASSES[m->klass] != c || (name && strcmp(name, m->name) != 0) || (argc >= 0 && argc != m->argc)) continue;
        if (c->real && il.class_get_method_from_name(c->real, m->name, m->argc)) continue;
        said(&m->said, c, m->name);
        return (const MethodInfo *)m;
    }
    return NULL;
}

// Where the player meets them: in the image's own answers, after what the image has. (These
// take the image's function's place in its slot: their class argument is one of the image's.)
static Il2CppClass *with_class_get_nested_types(Il2CppClass *k, void **iter)
{
    struct sclass *c = iter ? sclass_of(*iter) : NULL;          // the walk's place: the image's own, then our last
    if (!c) {
        Il2CppClass *n = il.class_get_nested_types(k, iter);
        if (n || !k || !iter) return n;
    }
    for (c = c ? c + 1 : SCLASSES; c < SCLASSES + N_SCLASS; c++)
        if (outer_is(c, k) && !c->real) {
            said(&c->said, c, NULL);
            *iter = c;
            return (Il2CppClass *)c;
        }
    return NULL;
}

static const MethodInfo *with_class_get_methods(Il2CppClass *k, void **iter)
{
    struct smethod *last = iter ? smethod_of(*iter) : NULL;
    if (!last) {
        const MethodInfo *m = il.class_get_methods(k, iter);
        if (m || !k || !iter) return m;
    }
    struct sclass *c = sclass_kept(k);
    const MethodInfo *m = c ? smethod_next(c, last ? (size_t)(last - SMETHODS) + 1 : 0, NULL, -1) : NULL;
    if (m) *iter = (void *)m;
    return m;
}

static const MethodInfo *with_class_get_method_from_name(Il2CppClass *k, const char *name, int argc)
{
    const MethodInfo *m = il.class_get_method_from_name(k, name, argc);
    if (m || !k || !name) return m;
    struct sclass *c = sclass_kept(k);
    return c ? smethod_next(c, 0, name, argc) : NULL;
}

// What the player asks of one of ours.
static Il2CppClass *corlib_class(const char *name)
{
    Il2CppClass *k = il.class_from_name(il.get_corlib(), "System", name);
    if (!k) DIE("the game's metadata has no System.%s\n", name);
    return k;
}
static const Il2CppType *element_type(int code)
{
    return il.class_get_type(corlib_class(code == T_BOOLEAN ? "Boolean" : "Void"));
}
static Il2CppClass *smethod_class(const struct smethod *m)
{
    struct sclass *c = &SCLASSES[m->klass];
    return c->real ? c->real : (Il2CppClass *)c;
}

#define KLASS struct sclass *c = sclass_arg(k, __func__ + 4)
static const char *alt_il2cpp_class_get_name(Il2CppClass *k) { KLASS; return c->name; }
static const char *alt_il2cpp_class_get_namespace(Il2CppClass *k) { KLASS; (void)c; return ""; }     // a nested class has none
static Il2CppClass *alt_il2cpp_class_get_declaring_type(Il2CppClass *k) { KLASS; return c->outer_k; }
static Il2CppClass *alt_il2cpp_class_get_parent(Il2CppClass *k) { KLASS; (void)c; return corlib_class("Object"); }
static void *alt_il2cpp_class_get_image(Il2CppClass *k) { KLASS; return il.class_get_image(c->outer_k); }
static const char *alt_il2cpp_class_get_assemblyname(Il2CppClass *k) { KLASS; return il.class_get_assemblyname(c->outer_k); }
static int alt_il2cpp_class_get_flags(Il2CppClass *k) { KLASS; return (int)c->flags; }
static Il2CppClass *alt_il2cpp_class_get_nested_types(Il2CppClass *k, void **iter) { KLASS; (void)c; return NULL; }
static int alt_il2cpp_class_is_valuetype(Il2CppClass *k) { KLASS; (void)c; return 0; }
static int alt_il2cpp_class_is_enum(Il2CppClass *k) { KLASS; (void)c; return 0; }
static int alt_il2cpp_class_is_interface(Il2CppClass *k) { KLASS; (void)c; return 0; }
static int alt_il2cpp_class_is_abstract(Il2CppClass *k) { KLASS; return (c->flags & 0x80) != 0; }
static int alt_il2cpp_class_is_generic(Il2CppClass *k) { KLASS; (void)c; return 0; }
static int alt_il2cpp_class_is_inflated(Il2CppClass *k) { KLASS; (void)c; return 0; }
static void alt_il2cpp_runtime_class_init(Il2CppClass *k) { KLASS; (void)c; }                       // none has a static constructor
static const MethodInfo *alt_il2cpp_class_get_methods(Il2CppClass *k, void **iter)
{
    KLASS;
    struct smethod *last = iter ? smethod_of(*iter) : NULL;
    if (!iter || (*iter && !last)) DIE("il2cpp_class_get_methods: a walk of %s's methods from a place that is not in it\n", c->name);
    const MethodInfo *m = smethod_next(c, last ? (size_t)(last - SMETHODS) + 1 : 0, NULL, -1);
    if (m) *iter = (void *)m;
    return m;
}
static const MethodInfo *alt_il2cpp_class_get_method_from_name(Il2CppClass *k, const char *name, int argc)
{
    KLASS;
    const MethodInfo *m = smethod_next(c, 0, name, argc);
    if (!m) LOG("the player asked for %s.%s/%s::%s (%d arguments): the bridge has no such method of the Android modules\n",
                c->ns, c->outer, c->name, name, argc);
    return m;
}
#undef KLASS

#define METHOD struct smethod *s = smethod_arg(m, __func__ + 4)
static const char *alt_il2cpp_method_get_name(const MethodInfo *m) { METHOD; return s->name; }
static uint32_t alt_il2cpp_method_get_param_count(const MethodInfo *m) { METHOD; return s->argc; }
static Il2CppClass *alt_il2cpp_method_get_class(const MethodInfo *m) { METHOD; return smethod_class(s); }
static Il2CppClass *alt_il2cpp_method_get_declaring_type(const MethodInfo *m) { METHOD; return smethod_class(s); }
static const Il2CppType *alt_il2cpp_method_get_return_type(const MethodInfo *m) { METHOD; return element_type(s->result); }
static const Il2CppType *alt_il2cpp_method_get_param(const MethodInfo *m, uint32_t i)
{
    METHOD;
    if (i >= s->argc) DIE("il2cpp_method_get_param: %s has no parameter %u\n", s->name, i);
    return element_type(s->params[i]);
}
static const char *alt_il2cpp_method_get_param_name(const MethodInfo *m, uint32_t i)
{
    METHOD;
    if (i >= s->argc) DIE("il2cpp_method_get_param_name: %s has no parameter %u\n", s->name, i);
    return s->param_names[i];
}
static uint32_t alt_il2cpp_method_get_flags(const MethodInfo *m, uint32_t *iflags)
{
    METHOD;
    if (iflags) *iflags = 0;                    // IL, managed
    return s->flags;
}
static int alt_il2cpp_method_is_generic(const MethodInfo *m) { METHOD; (void)s; return 0; }
static int alt_il2cpp_method_is_inflated(const MethodInfo *m) { METHOD; (void)s; return 0; }
static int alt_il2cpp_method_is_instance(const MethodInfo *m) { METHOD; return !(s->flags & 0x10); }
static const MethodInfo *alt_il2cpp_object_get_virtual_method(void *obj, const MethodInfo *m) { METHOD; (void)s; return m; }   // none is virtual
static void *alt_il2cpp_runtime_invoke(const MethodInfo *m, void *obj, void **params, void **exc)
{
    METHOD;
    if (exc) *exc = NULL;
    uint64_t v = s->run(params);
    return s->result ? il.value_box(corlib_class("Boolean"), &v) : NULL;
}
#undef METHOD

HIDDEN void ua_standin_missing(const char *name)
{
    DIE("the player called %s with a class or method only the Android engine modules have: the bridge has no answer to that\n", name);
}

static const struct { const char *name; void *fn; } standin_alts[] = {
#define A(fn) { #fn, (void *)alt_##fn }
    A(il2cpp_class_get_name), A(il2cpp_class_get_namespace), A(il2cpp_class_get_declaring_type), A(il2cpp_class_get_parent),
    A(il2cpp_class_get_image), A(il2cpp_class_get_assemblyname), A(il2cpp_class_get_flags), A(il2cpp_class_get_nested_types),
    A(il2cpp_class_is_valuetype), A(il2cpp_class_is_enum), A(il2cpp_class_is_interface), A(il2cpp_class_is_abstract),
    A(il2cpp_class_is_generic), A(il2cpp_class_is_inflated), A(il2cpp_runtime_class_init), A(il2cpp_class_get_methods),
    A(il2cpp_class_get_method_from_name), A(il2cpp_method_get_name), A(il2cpp_method_get_param_count), A(il2cpp_method_get_class),
    A(il2cpp_method_get_declaring_type), A(il2cpp_method_get_return_type), A(il2cpp_method_get_param),
    A(il2cpp_method_get_param_name), A(il2cpp_method_get_flags), A(il2cpp_method_is_generic), A(il2cpp_method_is_inflated),
    A(il2cpp_method_is_instance), A(il2cpp_object_get_virtual_method), A(il2cpp_runtime_invoke),
#undef A
};
static const struct { const char *name; void *fn; } standin_withs[] = {
    { "il2cpp_class_get_nested_types", (void *)with_class_get_nested_types },
    { "il2cpp_class_get_methods", (void *)with_class_get_methods },
    { "il2cpp_class_get_method_from_name", (void *)with_class_get_method_from_name },
};

// ---- dlopen: the game's plugins are the port's Android or ARM Linux builds ---------------
static void *plugin_open(const char *path, int darwin_mode)
{
    int flags = (darwin_mode & 2 ? 2 : 1) | (darwin_mode & 8 ? 0x100 : 0);      // RTLD_NOW/LAZY, GLOBAL as bionic has them
    void *h = host.dlopen(path, flags);
    if (h) return h;
    // "Plugins/name.bundle", "name.dylib" or "name": libname.so
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char name[256], lib[300];
    snprintf(name, sizeof name, "%s", base);
    char *dot = strrchr(name, '.');
    if (dot && (!strcmp(dot, ".bundle") || !strcmp(dot, ".dylib"))) *dot = 0;
    snprintf(lib, sizeof lib, strncmp(name, "lib", 3) ? "lib%s.so" : "%s.so", name);
    return host.dlopen(lib, flags);
}
static void *plugin_sym(void *h, const char *name) { return host.dlsym(h, name); }
static int plugin_close(void *h) { return host.dlclose ? host.dlclose(h) : 0; }
static const struct darwin_dlopen_host plugin_host = { plugin_open, plugin_sym, plugin_close };

// ---- start -----------------------------------------------------------------------------
__attribute__((visibility("default"))) int ua_macho_setup(const struct ua_macho_host *h)
{
    static const char *const images[] = { "libc++abi.1.dylib", "libc++.1.dylib", "GameAssembly.dylib" };
    static char path[3][600];
    static char *argv[2];
    char err[512];
    if (!h || h->size < sizeof host || !h->dlopen || !h->dlsym || !h->iterate_phdr || !h->thread_start_hook || !h->libdir) return -1;
    host = *h;
    ua_macho_thread_first();
    host.thread_start_hook(ua_macho_thread_first);
    for (int i = 0; i < 3; i++) {
        snprintf(path[i], sizeof path[i], "%s/%s", host.libdir, images[i]);
        ga = darwin_dyld_load(path[i], err, sizeof err);
        if (!ga) { LOG("%s: %s\n", path[i], err); return -1; }
    }
    argv[0] = path[2];
    darwin_dyld_set_args(1, argv, environ);
    if (darwin_dyld_link(err, sizeof err) != 0) { LOG("%s\n", err); return -1; }
    ua_unwind_iterate_phdr = host.iterate_phdr;
    darwin_dyld_set_dlopen(&plugin_host);
    if (host.keep_unblocked) host.keep_unblocked(darwin_thread_stop_signal());
    darwin_dyld_run_initializers();

    int filled = 0, missing = 0;
    for (const struct api_row *r = __start_ua_api_tab; r < __stop_ua_api_tab; r++) {
        char sym[128];
        snprintf(sym, sizeof sym, "_%s", r->name);
        void *p = macho_export(ga->m, sym);
        if (p) { *r->slot = p; filled++; } else missing++;
        if (p && !strcmp(r->name, "il2cpp_add_internal_call")) *r->slot = (void *)add_internal_call;
        for (size_t i = 0; p && i < sizeof standin_withs / sizeof standin_withs[0]; i++)
            if (!strcmp(r->name, standin_withs[i].name)) *r->slot = standin_withs[i].fn;
        for (size_t i = 0; r->alt && i < sizeof standin_alts / sizeof standin_alts[0]; i++)
            if (!strcmp(r->name, standin_alts[i].name)) *r->alt = standin_alts[i].fn;
    }
#define IL(n) il.n = ga_fn("il2cpp_" #n)
    IL(add_internal_call); IL(domain_get); IL(domain_get_assemblies); IL(assembly_get_image); IL(class_from_name);
    IL(class_get_nested_types); IL(class_get_name); IL(class_get_methods); IL(method_get_name); IL(method_get_flags);
    IL(method_get_param_count); IL(method_get_param); IL(method_get_return_type); IL(method_is_instance); IL(type_get_type);
    IL(type_is_byref); IL(class_from_type); IL(class_is_valuetype); IL(class_is_enum); IL(class_enum_basetype);
    IL(class_value_size); IL(class_get_fields); IL(field_get_flags); IL(field_get_type); IL(get_corlib);
    IL(class_get_namespace); IL(class_get_declaring_type); IL(class_get_type); IL(value_box);
    IL(class_get_method_from_name); IL(class_get_image); IL(class_get_assemblyname); IL(image_get_name);
#undef IL
    LOG("GameAssembly.dylib loaded: %d of the il2cpp functions passed on, %d it does not export\n", filled, missing);
    return 0;
}
