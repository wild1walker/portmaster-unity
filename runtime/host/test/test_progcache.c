// glhooks.c's UA_GL_PROGCACHE against fake_progcache_gl.c (as libGLESv2.so.2). Run by pointer_test.sh:
// test_progcache <scenario> <cache dir> (each scenario in a fresh folder)
//   hit      programs linked once are loaded from the cache the second time: the same executable, no link
//   order    the shaders attached in another order: still a hit
//   attribs  other attribute bindings, another source: a link each (never a wrong binary)
//   refused  another driver build: the cached binary is refused, the program linked from source, the file renewed
//   tf       transform-feedback varyings or a program parameter: never cached
//   off      UA_GL_PROGCACHE unset, or UA_SHADER_DUMP set: glLinkProgram not wrapped
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../glhooks.h"
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
unsigned glCreateShader(unsigned), glCreateProgram(void);
void glCompileShader(unsigned);
uint64_t fake_exe(unsigned), fake_exe_of(unsigned);
extern int fake_links, fake_binary_loads, fake_build;
// (glrec.c's notes, which glhooks.c's UA_GL_PROGLOG calls)
void glrec_note_program_binary(unsigned p, const void *b, long l) { (void)p; (void)b; (void)l; }
void glrec_note_get_program_binary(unsigned p, long l, const void *b) { (void)p; (void)b; (void)l; }

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static const so_shim *g_t;
static void *fn(const char *name) { for (const so_shim *s = g_t; s && s->name; s++) if (!strcmp(s->name, name)) return s->fn; return NULL; }
#define CALL(name, type) ((type)fn(name))

// a program of the vertex and fragment source, the attribute named in bind (NULL: none) at 0; vs first or not
static unsigned make(const char *vs, const char *fs, const char *bind, int vs_first)
{
    void (*src)(unsigned, int, const char *const *, const int *) = CALL("glShaderSource", void (*)(unsigned, int, const char *const *, const int *));
    void (*att)(unsigned, unsigned) = CALL("glAttachShader", void (*)(unsigned, unsigned));
    void (*bnd)(unsigned, unsigned, const char *) = CALL("glBindAttribLocation", void (*)(unsigned, unsigned, const char *));
    void (*link)(unsigned) = CALL("glLinkProgram", void (*)(unsigned));
    unsigned v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), p = glCreateProgram();
    src(v, 1, &vs, NULL); src(f, 1, &fs, NULL); glCompileShader(v); glCompileShader(f);
    if (vs_first) { att(p, v); att(p, f); } else { att(p, f); att(p, v); }
    if (bind) bnd(p, 0, bind);
    link(p);
    return p;
}

int main(int argc, char **argv)
{
    const char *sc = argc > 1 ? argv[1] : "hit";
    glhooks_set_cache_dir(argc > 2 ? argv[2] : "/tmp/pc");
    if (strcmp(sc, "off")) setenv("UA_GL_PROGCACHE", "1", 1);
    g_t = glhooks_table();
    const char *VS = "void main(){gl_Position=vec4(0.0);}", *FS = "void main(){}", *FS2 = "void main(){ /*2*/ }";
    if (!strcmp(sc, "off")) {
        CHECK(!fn("glLinkProgram"));
        setenv("UA_GL_PROGCACHE", "1", 1); setenv("UA_SHADER_DUMP", argc > 2 ? argv[2] : "/tmp/pcd", 1);
        g_t = glhooks_table();
        CHECK(!fn("glLinkProgram"));
    } else if (!strcmp(sc, "hit") || !strcmp(sc, "order")) {
        unsigned a = make(VS, FS, "in_POSITION0", 1), b = make(VS, FS2, NULL, 1);
        CHECK(fake_links == 2 && fake_exe(a) && fake_exe(b) && fake_exe(a) != fake_exe(b));
        int order = !strcmp(sc, "order");
        unsigned a2 = make(VS, FS, "in_POSITION0", !order), b2 = make(VS, FS2, NULL, !order);
        CHECK(fake_links == 2 && fake_binary_loads == 2);           // no link: both from the cache
        CHECK(fake_exe(a2) == fake_exe(a) && fake_exe(b2) == fake_exe(b));
        CHECK(fake_exe(a2) == fake_exe_of(a2) && fake_exe(b2) == fake_exe_of(b2));   // what a link would have made
    } else if (!strcmp(sc, "attribs")) {
        unsigned a = make(VS, FS, "in_POSITION0", 1);
        unsigned b = make(VS, FS, "in_NORMAL0", 1), c = make(VS, FS2, "in_POSITION0", 1);
        CHECK(fake_links == 3 && fake_binary_loads == 0);
        CHECK(fake_exe(b) == fake_exe_of(b) && fake_exe(c) == fake_exe_of(c) && fake_exe(a) != fake_exe(b));
    } else if (!strcmp(sc, "refused")) {
        make(VS, FS, NULL, 1);
        fake_build = 8;                                             // a driver update between launches
        unsigned b = make(VS, FS, NULL, 1);
        CHECK(fake_links == 2 && fake_exe(b) == fake_exe_of(b));
        unsigned c = make(VS, FS, NULL, 1);                         // the renewed file: this build's
        CHECK(fake_links == 2 && fake_exe(c) == fake_exe(b));
    } else if (!strcmp(sc, "tf")) {
        void (*tf)(unsigned, int, const char *const *, unsigned) = CALL("glTransformFeedbackVaryings", void (*)(unsigned, int, const char *const *, unsigned));
        void (*pp)(unsigned, unsigned, int) = CALL("glProgramParameteri", void (*)(unsigned, unsigned, int));
        void (*src)(unsigned, int, const char *const *, const int *) = CALL("glShaderSource", void (*)(unsigned, int, const char *const *, const int *));
        void (*att)(unsigned, unsigned) = CALL("glAttachShader", void (*)(unsigned, unsigned));
        void (*link)(unsigned) = CALL("glLinkProgram", void (*)(unsigned));
        for (int round = 0; round < 2; round++)
            for (int kind = 0; kind < 2; kind++) {
                unsigned v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), p = glCreateProgram();
                src(v, 1, &VS, NULL); src(f, 1, &FS, NULL); att(p, v); att(p, f);
                const char *out = "o";
                if (kind == 0) tf(p, 1, &out, 0x8C8C); else pp(p, 0x8258 /* GL_PROGRAM_SEPARABLE */, 1);
                link(p);
            }
        CHECK(fake_links == 4 && fake_binary_loads == 0);
    } else { printf("unknown scenario %s\n", sc); return 2; }
    printf("%s %s (links %d, binary loads %d)\n", failures ? "FAIL" : "PASS", sc, fake_links, fake_binary_loads);
    return failures != 0;
}
