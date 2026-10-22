// gltrace.c check against fake_trace_gl.c (as libGLESv2.so.2): a traced frame's pass summary, and
// with UA_GL_TRACE_DRAWS its clears, draws and blits one by one (colour buffer, program and shader
// hashes, samplers, pixels read back), with Unity's bindings as they were after each read; without
// it no such lines and no glAttachShader wrapper; UA_GL_TRACE_DRAWS=<n> logs the first n only.
// Run by pointer_test.sh: test_gltrace draws | off | limit
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../gltrace.h"

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_2D 0x0DE1

extern int fake_read_fb, fake_draw_fb, fake_active, fake_pbo, fake_reads;
extern void glActiveTexture(unsigned);
extern void glBindTexture(unsigned, unsigned);
extern void glBindBuffer(unsigned, unsigned);

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stdout, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void *fn(const so_shim *t, const char *name)
{
    for (; t && t->name; t++) if (!strcmp(t->name, name)) return t->fn;
    return NULL;
}

static uint64_t fnv(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h;
}

static int has(const char *log, const char *s)
{
    if (strstr(log, s)) return 1;
    fprintf(stdout, "missing: %s\n", s);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "draws";
    setenv("UA_GL_TRACE", "1000", 1);
    if (strcmp(mode, "off")) setenv("UA_GL_TRACE_DRAWS", strcmp(mode, "limit") ? "8" : "1", 1);
    char path[] = "/tmp/gltrace-XXXXXX";
    int fd = mkstemp(path);
    fflush(stderr); int saved = dup(2); dup2(fd, 2);

    const so_shim *t = gltrace_table();
    void (*Attach)(unsigned, unsigned) = fn(t, "glAttachShader");
    void (*Bind)(unsigned, unsigned) = fn(t, "glBindFramebuffer");
    void (*Viewport)(int, int, int, int) = fn(t, "glViewport");
    void (*Clear)(unsigned) = fn(t, "glClear");
    void (*Use)(unsigned) = fn(t, "glUseProgram");
    void (*Draw)(unsigned, int, int) = fn(t, "glDrawArrays");
    void (*DrawEl)(unsigned, int, unsigned, const void *) = fn(t, "glDrawElements");
    void (*Blit)(int, int, int, int, int, int, int, int, unsigned, unsigned) = fn(t, "glBlitFramebuffer");
    CHECK(Bind && Viewport && Clear && Use && Draw && DrawEl && Blit);
    if (!strcmp(mode, "off")) CHECK(!Attach);
    else CHECK(Attach);
    if (!(Bind && Viewport && Clear && Use && Draw && DrawEl && Blit)) return 1;

    if (Attach) { Attach(5, 1); Attach(5, 2); }      // at link time, before the traced frame
    glActiveTexture(GL_TEXTURE0 + 2); glBindTexture(GL_TEXTURE_2D, 11);
    glActiveTexture(GL_TEXTURE0 + 1);                // Unity's unit, not the sampler's
    gltrace_frame();                                 // the traced frame starts
    Bind(GL_FRAMEBUFFER, 3); Viewport(0, 0, 64, 32);
    Clear(GL_COLOR_BUFFER_BIT);
    Use(5); Draw(4, 0, 3);
    CHECK(fake_read_fb == 3 && fake_draw_fb == 3 && fake_active == GL_TEXTURE0 + 1 && fake_pbo == 4);
    Bind(GL_DRAW_FRAMEBUFFER, 0); Viewport(0, 0, 720, 480);      // read 3, draw 0: as a resolve leaves them
    Blit(0, 0, 64, 32, 0, 0, 720, 480, GL_COLOR_BUFFER_BIT, 0x2601);
    CHECK(fake_read_fb == 3 && fake_draw_fb == 0 && fake_pbo == 4);
    Bind(GL_FRAMEBUFFER, 0);
    DrawEl(4, 6, 0x1403, NULL);
    gltrace_frame();                                 // its report
    fflush(stderr); dup2(saved, 2);

    char log[8192] = "";
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(log, 1, sizeof log - 1, f) : 0;
    log[n] = 0;
    if (f) fclose(f);
    unlink(path);

    CHECK(has(log, "[gl] frame 2: 3 passes, 2 draws, 9 vertices, 1 program switches"));
    CHECK(has(log, "fb3 64x32 clear=C draws 1 verts 3"));
    char want[256];
    if (!strcmp(mode, "off")) {
        CHECK(!strstr(log, " draw fb") && !strstr(log, " clear fb") && fake_reads == 0);
    } else {
        CHECK(has(log, "[gl]   fb3 = tex9 64x32 f R16G16B16A16"));
        CHECK(has(log, "#0 clear fb3 C colour (0,1,0,1) -> (0.25,0.5,0.75,1) (0.25,0.5,0.75,1)"));
        if (!strcmp(mode, "limit")) {
            CHECK(!strstr(log, "#1 "));
        } else {
            snprintf(want, sizeof want, "#1 draw fb3 prog5 vs %016llx fs %016llx 3 _MainTex=2:tex11 -> (0.25,0.5,0.75,1)",
                     (unsigned long long)fnv("void main() { gl_Position = vec4(0.0); }"), (unsigned long long)fnv("void main() {}"));
            CHECK(has(log, want));
            CHECK(has(log, "#2 blit fb3 -> fb0 (0,0,64,32) -> (0,0,720,480) C -> (10,20,30,255)"));
            CHECK(has(log, "#3 draw fb0 prog5"));
            CHECK(has(log, "6 _MainTex=2:tex11 -> (10,20,30,255) (10,20,30,255)"));
        }
    }
    if (failures) fputs(log, stdout);
    printf("test_gltrace %s: %s\n", mode, failures ? "FAILED" : "ok");
    return failures != 0;
}
