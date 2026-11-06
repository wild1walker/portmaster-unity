// glspec.c against fake_spec_gl.c (as libGLESv2.so.2). Run by pointer_test.sh: test_glspec <scenario>
//   source   a lit program from source: its twin (_LightColor0 a constant 0) is bound while the game's value
//            is 0, the program for any other; the game reads its own program back; a uniform set while
//            the twin is bound is set on both
//   binary   the program's binary as Unity caches it carries the twin's; loaded into a new program, the
//            twin comes back with it
//   stale    a binary cached without the twin's (before): loads as it is, without a twin
//   cache    glspec_cache: turned on, the cache emptied and marked; again: kept; turned off: emptied, mark gone
//   unlit    a program without the uniform: no twin
//   wcomp    the shader reads _LightColor0.w: (0, 0, 0, 1) binds the program
//   off      UA_GL_SPEC unset: nothing wrapped
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../glspec.h"

#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_LINK_STATUS 0x8B82
#define GL_PROGRAM_BINARY_LENGTH 0x8741

extern int fake_draw_prog;
float *fake_value(unsigned p, const char *name);
const char *fake_fs(unsigned p);
unsigned glCreateShader(unsigned), glCreateProgram(void);
void glCompileShader(unsigned), glAttachShader(unsigned, unsigned);

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static const so_shim *g_t;
static void *fn(const char *name) { for (const so_shim *s = g_t; s && s->name; s++) if (!strcmp(s->name, name)) return s->fn; return NULL; }

static const char *VS = "#version 320 es\nuniform \tvec4 hlslcc_mtx4x4unity_MatrixVP[4];\nvoid main() { gl_Position = vec4(0.0); }\n";
static const char *LIT =
    "#version 320 es\nprecision highp float;\nuniform \tvec4 _WorldSpaceLightPos0;\nuniform \tvec4 _LightColor0;\nuniform \tvec4 _Color;\n"
    "layout(location = 0) out vec4 SV_Target0;\nvoid main()\n{\n    vec3 l = _Color.xyz * _LightColor0.xyz;\n    SV_Target0 = vec4(l, 1.0);\n}\n";
static const char *LITW =
    "#version 320 es\nprecision highp float;\nuniform \tvec4 _LightColor0;\nuniform \tvec4 _Color;\n"
    "layout(location = 0) out vec4 SV_Target0;\nvoid main()\n{\n    SV_Target0 = _Color * _LightColor0.w + vec4(_LightColor0.xyz, 0.0);\n}\n";
static const char *UNLIT = "#version 320 es\nuniform \tvec4 _Color;\nlayout(location = 0) out vec4 SV_Target0;\nvoid main()\n{\n    SV_Target0 = _Color;\n}\n";

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "source";
    if (!strcmp(mode, "cache")) {
        char dir[] = "/tmp/test_glspec_XXXXXX", f[256];
        if (!mkdtemp(dir)) return 1;
        snprintf(f, sizeof f, "%s/UnityShaderCache", dir); mkdir(f, 0755);
        snprintf(f, sizeof f, "%s/UnityShaderCache/abc", dir); fclose(fopen(f, "w"));
        struct stat st;
        setenv("UA_GL_SPEC", "1", 1);
        glspec_cache(dir);
        CHECK(stat(f, &st) != 0);
        snprintf(f, sizeof f, "%s/uaspec.on", dir); CHECK(stat(f, &st) == 0);
        snprintf(f, sizeof f, "%s/UnityShaderCache", dir); mkdir(f, 0755);
        snprintf(f, sizeof f, "%s/UnityShaderCache/def", dir); fclose(fopen(f, "w"));
        glspec_cache(dir);                      // (still on: kept)
        CHECK(stat(f, &st) == 0);
        unsetenv("UA_GL_SPEC");
        glspec_cache(dir);
        CHECK(stat(f, &st) != 0);
        snprintf(f, sizeof f, "%s/uaspec.on", dir); CHECK(stat(f, &st) != 0);
        printf("test_glspec %s: %s\n", mode, failures ? "FAILED" : "ok");
        return failures != 0;
    }
    if (strcmp(mode, "off")) setenv("UA_GL_SPEC", "1", 1);
    g_t = glspec_table(NULL);
    if (!strcmp(mode, "off")) { CHECK(!g_t[0].name); printf("test_glspec %s: %s\n", mode, failures ? "FAILED" : "ok"); return failures != 0; }
    void (*Source)(unsigned, int, const char *const *, const int *) = fn("glShaderSource");
    void (*Link)(unsigned) = fn("glLinkProgram");
    void (*Use)(unsigned) = fn("glUseProgram");
    void (*Get)(unsigned, int *) = fn("glGetIntegerv");
    void (*U4)(int, int, const float *) = fn("glUniform4fv");
    void (*Progiv)(unsigned, unsigned, int *) = fn("glGetProgramiv");
    void (*GetBin)(unsigned, int, int *, unsigned *, void *) = fn("glGetProgramBinary");
    void (*Bin)(unsigned, unsigned, const void *, int) = fn("glProgramBinary");
    int (*Loc)(unsigned, const char *) = NULL;
    extern int glGetUniformLocation(unsigned, const char *);
    Loc = glGetUniformLocation;
    void (*Draw)(unsigned, int, unsigned, const void *) = fn("glDrawElements") ? fn("glDrawElements") : NULL;
    extern void glDrawElements(unsigned, int, unsigned, const void *);
    if (!Draw) Draw = glDrawElements;
    CHECK(Source && Link && Use && Get && U4 && Progiv && GetBin && Bin);
    glspec_frame();                             // (this thread renders)

    const char *fsrc = !strcmp(mode, "unlit") ? UNLIT : !strcmp(mode, "wcomp") ? LITW : LIT;
    unsigned vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
    Source(vs, 1, &VS, NULL); Source(fs, 1, &fsrc, NULL);
    glCompileShader(vs); glCompileShader(fs);
    unsigned p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    Link(p);
    Use(p);
    int lc = Loc(p, "_LightColor0"), col = Loc(p, "_Color"), cur = 0;
    static const float zero[4] = { 0, 0, 0, 0 }, one[4] = { 1, 1, 1, 0 }, w1[4] = { 0, 0, 0, 1 }, red[4] = { 1, 0, 0, 1 };

    if (!strcmp(mode, "unlit")) {
        U4(col, 1, red); Draw(4, 6, 0x1403, NULL);
        CHECK(fake_draw_prog == (int)p);
    } else if (!strcmp(mode, "wcomp")) {
        U4(lc, 1, w1); Draw(4, 6, 0x1403, NULL);
        CHECK(fake_draw_prog == (int)p);         // (w read: not 0)
        U4(lc, 1, zero); Draw(4, 6, 0x1403, NULL);
        CHECK(fake_draw_prog != (int)p && strstr(fake_fs((unsigned)fake_draw_prog), "const vec4 _LightColor0"));
    } else {
        U4(lc, 1, zero); Draw(4, 6, 0x1403, NULL);
        unsigned twin = (unsigned)fake_draw_prog;
        CHECK(twin != p && strstr(fake_fs(twin), "const vec4 _LightColor0 = vec4(0.0);") && !strstr(fake_fs(twin), "uniform \tvec4 _LightColor0"));
        Get(GL_CURRENT_PROGRAM, &cur);
        CHECK(cur == (int)p);
        U4(col, 1, red);                        // (set while the twin is bound: both have it)
        CHECK(fake_value(p, "_Color")[0] == 1 && fake_value(twin, "_Color")[0] == 1 && fake_value(twin, "_Color")[3] == 1);
        U4(lc, 1, one); Draw(4, 6, 0x1403, NULL);
        CHECK(fake_draw_prog == (int)p);
        CHECK(fake_value(p, "_LightColor0")[0] == 1);
        U4(lc, 1, zero); Draw(4, 6, 0x1403, NULL);
        CHECK(fake_draw_prog == (int)twin);
        Use(0); Use(p); Draw(4, 6, 0x1403, NULL);
        CHECK(fake_draw_prog == (int)twin);
        if (!strcmp(mode, "binary") || !strcmp(mode, "stale")) {
            int len = 0; unsigned fmt = 0;
            Progiv(p, GL_PROGRAM_BINARY_LENGTH, &len);
            char *b = malloc((size_t)len);
            int got = 0;
            GetBin(p, len, &got, &fmt, b);
            CHECK(got == len && !memcmp(b, "UASP", 4));
            unsigned q = glCreateProgram();
            int ok = 0;
            if (!strcmp(mode, "binary")) {
                Bin(q, fmt, b, got);
                Progiv(q, GL_LINK_STATUS, &ok);
                CHECK(ok);
                Use(q);
                U4(Loc(q, "_LightColor0"), 1, zero); Draw(4, 6, 0x1403, NULL);
                CHECK(fake_draw_prog != (int)q && strstr(fake_fs((unsigned)fake_draw_prog), "const vec4 _LightColor0"));
                U4(Loc(q, "_LightColor0"), 1, one); Draw(4, 6, 0x1403, NULL);
                CHECK(fake_draw_prog == (int)q);
            } else {
                // what the driver's own binary looks like (Unity's cache from before): no header
                extern void glGetProgramBinary(unsigned, int, int *, unsigned *, void *);
                char raw[512]; int rl = 0; unsigned rf = 0;
                glGetProgramBinary(p, sizeof raw, &rl, &rf, raw);
                Bin(q, rf, raw, rl);
                Progiv(q, GL_LINK_STATUS, &ok);
                CHECK(ok);
                Use(q);
                U4(Loc(q, "_LightColor0"), 1, zero); Draw(4, 6, 0x1403, NULL);
                CHECK(fake_draw_prog == (int)q);
            }
            free(b);
        }
    }
    printf("test_glspec %s: %s\n", mode, failures ? "FAILED" : "ok");
    return failures != 0;
}
