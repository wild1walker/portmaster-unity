// fihost (tools/mono64/fitest/run.sh): every no-argument instance method of FiTest.dll's Base called on a fresh object through
// mono_runtime_invoke and then through the runtime-invoke wrapper mono_unity_runtime_invoke_info gives (or, with a
// 6th argument, through mono_method_get_unmanaged_thunk); the side effects (the log, the field)
// and the exception (type, message, stack trace, inner) printed for both and compared
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef void *P;
static P (*jit_init)(const char *, const char *); static void (*set_path)(const char *);
static P (*asm_open)(P, const char *); static P (*get_image)(P); static P (*class_from_name)(P, const char *, const char *);
static P (*get_method)(P, const char *, int); static P (*obj_new)(P, P); static void (*obj_init)(P);
static P (*invoke)(P, P, void **, P *); static P (*get_thunk)(P); static char *(*to_utf8)(P);
static unsigned (*gch_new)(P, int);  static int (*info)(P, P *, P *); static P (*str_new)(P, const char *);
static P dom, img, kbase, kder, krep, mget, mdesc;
static char *rep(P ex) {
    P e = NULL; void *a[1] = { ex };
    P s1 = invoke(mget, NULL, NULL, &e); P s2 = invoke(mdesc, NULL, a, &e);
    char *r1 = to_utf8(s1), *r2 = to_utf8(s2); char *o = malloc(strlen(r1) + strlen(r2) + 32);
    sprintf(o, "log=[%s] ex=[%s]", r1, r2); return o;
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    P h = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL); if (!h) { puts(dlerror()); return 1; }
#define S(v, n) *(void **)&v = dlsym(h, n); if (!v) { printf("no %s\n", n); return 1; }
    S(jit_init, "mono_jit_init_version") S(set_path, "mono_set_assemblies_path") S(asm_open, "mono_domain_assembly_open")
    S(get_image, "mono_assembly_get_image") S(class_from_name, "mono_class_from_name") S(get_method, "mono_class_get_method_from_name")
    S(obj_new, "mono_object_new") S(obj_init, "mono_runtime_object_init") S(invoke, "mono_runtime_invoke")
    S(get_thunk, "mono_method_get_unmanaged_thunk") S(to_utf8, "mono_string_to_utf8") S(gch_new, "mono_gchandle_new") S(str_new, "mono_string_new") S(info, "mono_unity_runtime_invoke_info")
    set_path(argv[2]);
    dom = jit_init("fitest", "v4.0.30319");
    P as = asm_open(dom, argv[3]); img = get_image(as);
    kbase = class_from_name(img, "", "Base"); kder = class_from_name(img, "", "Derived"); krep = class_from_name(img, "", "Report");
    mget = get_method(krep, "Get", 0); mdesc = get_method(krep, "Desc", 1);
    const char *names[] = { "V", "Ok", "Throw", "ThrowDeep", "NullRef", "DivZero", "Finally", "ThrowInFinally", "Caught", "Rethrow",
        "Wrapped", "Cast", "Index", "Alloc", "Generic", "ThrowNull", "TypeInit", "Recurse" };
    int n = sizeof names / sizeof *names, bad = 0;
    if (argc > 4) n = atoi(argv[4]);
    for (int k = 0; k < 2; k++) for (int i = 0; i < n; i++) {
        P m = get_method(kbase, names[i], 0);
        char *r[2];
        for (int way = 0; way < 2; way++) {
            P o = obj_new(dom, k ? kder : kbase); obj_init(o); gch_new(o, 1);
            P ex = NULL;
            if (way == 0) invoke(m, o, NULL, &ex);
            else if (argc > 5) { void (*t)(P, P *) = (void (*)(P, P *))get_thunk(m); t(o, &ex); }
            else { P f = NULL, code = NULL; if (!info(m, &f, &code)) { printf("  (no info for %s: the bridge's general path)\n", names[i]); invoke(m, o, NULL, &ex); r[way] = rep(ex); continue; }
                   ex = NULL; ((P (*)(P, void **, P *, P))f)(o, NULL, &ex, code); }
            r[way] = rep(ex);
        }
        int same = !strcmp(r[0], r[1]); bad += !same;
        printf("%s %s.%s\n  invoke: %s\n%s", same ? "SAME" : "DIFF", k ? "Derived" : "Base", names[i], r[0], same ? "" : "");
        if (!same) printf("  thunk:  %s\n", r[1]);
    }
    {   // a return value (boxed), a static method, arguments
        P ex = NULL, f, code; static P (*unbox)(P); *(void **)&unbox = dlsym(h, "mono_object_unbox");
        P o = obj_new(dom, kbase); obj_init(o); gch_new(o, 1);
        P mn = get_method(kbase, "Next", 0), ms = get_method(kbase, "Stat", 0), ma = get_method(kbase, "Args", 2), mr = get_method(kbase, "Ret", 0);
        int a = 7; P sarg = str_new(dom, "x"); void *args[2] = { &a, sarg };
        P r0 = invoke(mn, o, NULL, &ex); int b0 = *(unsigned char *)unbox(r0);
        invoke(ms, NULL, NULL, &ex); invoke(ma, o, args, &ex); P q0 = invoke(mr, o, NULL, &ex); float f0 = *(float *)unbox(q0);
        char *l0 = rep(NULL);
        o = obj_new(dom, kbase); obj_init(o); gch_new(o, 1);
        int ok = info(mn, &f, &code); P r1 = ((P (*)(P, void **, P *, P))f)(o, NULL, &ex, code); int b1 = *(unsigned char *)unbox(r1);
        ok &= info(ms, &f, &code); ((P (*)(P, void **, P *, P))f)(NULL, NULL, &ex, code);
        ok &= info(ma, &f, &code); ((P (*)(P, void **, P *, P))f)(o, args, &ex, code);
        ok &= info(mr, &f, &code); P q1 = ((P (*)(P, void **, P *, P))f)(o, NULL, &ex, code); float f1 = *(float *)unbox(q1);
        char *l1 = rep(NULL);
        int same = ok && b0 == b1 && f0 == f1 && !strcmp(l0, l1); bad += !same;
        printf("%s return/static/args: %d %d %g %g\n  %s\n  %s\n", same ? "SAME" : "DIFF", b0, b1, f0, f1, l0, l1);
    }
    printf("RESULT %d differ\n", bad);
    return 0;
}
