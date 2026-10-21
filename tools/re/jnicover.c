/* jnicover: which of the Java classes, methods and fields a Unity player binds does runtime/jni
 * define? Reads jnibindings.py rows (class TAB member TAB signature TAB kind ...) on stdin and
 * prints one status per row: ok, NO-CLASS (class unknown or a placeholder), NO-MEMBER.
 * Natives (rows of kind "native", e.g. from jnitables.py) are looked up as Java methods too.
 *
 * It includes fakejni.c to query its tables directly; build it on any glibc host, e.g. in the
 * ua-build-arm64 image:
 *   gcc -O1 -w -Iruntime/jni -Iruntime/jni/include tools/re/jnicover.c runtime/jni/unityplayer.c \
 *       runtime/jni/up_android.c runtime/jni/up_input.c runtime/jni/up_tls.c runtime/jni/up_fmod.c \
 *       runtime/jni/up_http.c runtime/jni/up_unity6.c runtime/jni/up_legacy.c -lz -ldl -lpthread -lm -o /tmp/jnicover
 *   /tmp/jnicover < bindings.tsv
 */
#include "fakejni.c"
#include "unityplayer.h"

int main(void)
{
    struct up_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.files_dir = "/tmp/jnicover/files";
    cfg.cache_dir = "/tmp/jnicover/cache";
    cfg.external_files_dir = "/tmp/jnicover/ext";
    up_register_classes(&cfg);
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        line[strcspn(line, "\n")] = 0;
        char *f[6] = {0};
        int n = 0;
        for (char *p = line, *t; n < 6 && (t = strsep(&p, "\t")); ) f[n++] = t;
        if (n < 4) continue;
        const char *cls = f[0], *mem = f[1], *sig = f[2], *kind = f[3];
        if (strstr(cls, "/__Proxy")) {              /* interfaces Unity implements via JNIBridge */
            printf("proxy\t%s\t%s\t%s\t%s\n", cls, mem, sig, kind);
            continue;
        }
        fj_class *c = class_lookup_raw(cls);
        const char *st;
        if (!c || c->placeholder) st = "NO-CLASS";
        else if (!strcmp(kind, "field") || !strcmp(kind, "static-field"))
            st = (field_find(c, mem, sig, 0, 0) || field_find(c, mem, sig, 1, 0)) ? "ok" : "NO-MEMBER";
        else
            st = (method_find(c, mem, sig, 0, 0) || method_find(c, mem, sig, 1, 0)) ? "ok" : "NO-MEMBER";
        printf("%s\t%s\t%s\t%s\t%s\n", st, cls, mem, sig, kind);
    }
    return 0;
}
