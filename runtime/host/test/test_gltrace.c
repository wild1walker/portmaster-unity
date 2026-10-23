// gltrace.c check against fake_trace_gl.c (as libGLESv2.so.2): a traced frame's pass summary, and
// with UA_GL_TRACE_DRAWS its clears, draws and blits one by one (colour buffer, program and shader
// hashes, samplers, pixels read back), with Unity's bindings as they were after each read; without
// it no such lines and no glAttachShader wrapper; UA_GL_TRACE_DRAWS=<n> logs the first n only.
// Every draw entry point counts (Range, BaseVertex OES, Instanced BaseVertex, MultiDraw, Indirect),
// copies too. GPU times (EXT_disjoint_timer_query in the fake's extension string): one query per
// pass, begun after the swap that starts the frame, the report a frame later (gpu); without a
// flush at pass ends (gpu2); off (gpuoff); none when the game times the GPU itself (game).
// Run by pointer_test.sh: test_gltrace draws | off | limit | gpu | gpu2 | gpuoff | game
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

extern int fake_read_fb, fake_draw_fb, fake_active, fake_pbo, fake_reads, fake_begins, fake_query_errors, fake_flushes, fake_finishes;
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
    int gpu = !strncmp(mode, "gpu", 3) || !strcmp(mode, "game"), timed = !strcmp(mode, "gpu") || !strcmp(mode, "gpu2");
    setenv("UA_GL_TRACE", "1000", 1);
    if (strcmp(mode, "off") && !gpu) setenv("UA_GL_TRACE_DRAWS", strcmp(mode, "limit") ? "8" : "1", 1);
    if (gpu) setenv("FAKE_GL_TIMER", "1", 1);
    if (!strcmp(mode, "gpu") || !strcmp(mode, "game")) setenv("UA_GL_TRACE_GPU", "1", 1);
    if (!strcmp(mode, "gpu2")) setenv("UA_GL_TRACE_GPU", "2", 1);
    if (!strcmp(mode, "gpuoff")) setenv("UA_GL_TRACE_GPU", "0", 1);
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
    void (*Range)(unsigned, unsigned, unsigned, int, unsigned, const void *) = fn(t, "glDrawRangeElements");
    void (*BV)(unsigned, int, unsigned, const void *, int) = fn(t, "glDrawElementsBaseVertex");
    void (*BVOES)(unsigned, int, unsigned, const void *, int) = fn(t, "glDrawElementsBaseVertexOES");
    void (*InstBV)(unsigned, int, unsigned, const void *, int, int) = fn(t, "glDrawElementsInstancedBaseVertex");
    void (*Multi)(unsigned, const int *, const int *, int) = fn(t, "glMultiDrawArraysEXT");
    void (*Indirect)(unsigned, const void *) = fn(t, "glDrawArraysIndirect");
    void (*Copy)(unsigned, int, int, int, int, int, int, int) = fn(t, "glCopyTexSubImage2D");
    void (*BeginQ)(unsigned, unsigned) = fn(t, "glBeginQueryEXT");
    void (*EndQ)(unsigned) = fn(t, "glEndQueryEXT");
    CHECK(Bind && Viewport && Clear && Use && Draw && DrawEl && Blit && Range && BV && BVOES && InstBV && Multi && Indirect && Copy);
    if (!strcmp(mode, "off") || gpu) CHECK(!Attach);
    else CHECK(Attach);
    if ((gpu && strcmp(mode, "gpuoff")) || !strcmp(mode, "off")) CHECK(BeginQ && EndQ);
    else CHECK(!BeginQ && !EndQ);
    if (!(Bind && Viewport && Clear && Use && Draw && DrawEl && Blit && Range && BV && BVOES && InstBV && Multi && Indirect && Copy)) return 1;

    if (Attach) { Attach(5, 1); Attach(5, 2); }      // at link time, before the traced frame
    glActiveTexture(GL_TEXTURE0 + 2); glBindTexture(GL_TEXTURE_2D, 11);
    glActiveTexture(GL_TEXTURE0 + 1);                // Unity's unit, not the sampler's
    gltrace_frame();                                 // the traced frame starts (before the swap)
    CHECK(fake_begins == 0);                         // its first query: not before that swap
    Bind(GL_FRAMEBUFFER, 3); Viewport(0, 0, 64, 32);
    Clear(GL_COLOR_BUFFER_BIT);
    Use(5); Draw(4, 0, 3);
    CHECK(fake_read_fb == 3 && fake_draw_fb == 3 && fake_active == GL_TEXTURE0 + 1 && fake_pbo == 4);
    if (!strcmp(mode, "game")) { BeginQ(0x88BF, 900); EndQ(0x88BF); }   // the game's own time query
    else if (BeginQ) { BeginQ(0x8C2F, 901); EndQ(0x8C2F); }          // an occlusion query: no matter
    Bind(GL_DRAW_FRAMEBUFFER, 0); Viewport(0, 0, 720, 480);      // read 3, draw 0: as a resolve leaves them
    Blit(0, 0, 64, 32, 0, 0, 720, 480, GL_COLOR_BUFFER_BIT, 0x2601);
    CHECK(fake_read_fb == 3 && fake_draw_fb == 0 && fake_pbo == 4);
    Bind(GL_FRAMEBUFFER, 0);
    DrawEl(4, 6, 0x1403, NULL);
    Range(4, 0, 5, 6, 0x1403, NULL);
    BV(4, 12, 0x1403, NULL, 4);
    BVOES(4, 6, 0x1403, NULL, 0);
    InstBV(4, 6, 0x1403, NULL, 3, 0);
    static const int first[2] = { 0, 3 }, count[2] = { 3, 3 };
    Multi(4, first, count, 2);
    Indirect(4, NULL);
    Bind(GL_FRAMEBUFFER, 7);
    Copy(0x0DE1, 0, 0, 0, 0, 0, 16, 16);
    gltrace_frame();                                 // its report (with GPU times: a frame later)
    if (timed) CHECK(!fake_query_errors);
    gltrace_frame();
    fflush(stderr); dup2(saved, 2);

    char log[16384] = "";
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(log, 1, sizeof log - 1, f) : 0;
    log[n] = 0;
    if (f) fclose(f);
    unlink(path);

    CHECK(has(log, "[gl] frame 2: 4 passes, 9 draws, 57 vertices, 1 program switches"));
    CHECK(has(log, "fb3 64x32 clear=C draws 1 verts 3"));
    CHECK(has(log, "[gl]   calls: glDrawArrays 1, glDrawElements 1, glDrawRangeElements 1, glDrawElementsBaseVertex 1, "
                   "glDrawElementsBaseVertexOES 1, glDrawElementsInstancedBaseVertex 1, glMultiDrawArraysEXT 1, glDrawArraysIndirect 1\n"));
    CHECK(has(log, "fb0 720x480 clear=- draws 8 verts 54 inval=start:- end:- indirect 1"));
    CHECK(has(log, "fb7 720x480 clear=- draws 0 verts 0 inval=start:- end:- copies 1"));
    CHECK(!fake_query_errors);
    char want[256];
    if (timed) {
        CHECK(has(log, "GPU time per pass: EXT_disjoint_timer_query (32-bit counter)"));
        CHECK(has(log, "buffers 0 KB; GPU 10.75 ms\n"));
        CHECK(has(log, "fb3 64x32 clear=C draws 1 verts 3 inval=start:- end:- gpu 1.50 ms\n"));
        CHECK(has(log, "fb0 720x480 clear=- draws 0 verts 0 inval=start:- end:- blits 1 gpu 2.00 ms\n"));
        CHECK(has(log, "indirect 1 gpu 7.00 ms\n"));
        CHECK(has(log, "copies 1 gpu 0.25 ms\n"));
        CHECK(fake_begins == 4);
        CHECK(fake_flushes == (!strcmp(mode, "gpu") ? 4 : 0));
    } else if (gpu) {
        CHECK(!strstr(log, " gpu ") && !strstr(log, "; GPU "));
        if (!strcmp(mode, "game")) CHECK(has(log, "the game times the GPU itself: frame 2 has no GPU times"));
        if (!strcmp(mode, "gpuoff")) CHECK(fake_begins == 0);
    } else if (!strcmp(mode, "off")) {              // (the default GPU times: glFinish at each pass's end)
        CHECK(has(log, "GPU time per pass: glFinish at each pass's end"));
        CHECK(has(log, " ms (glFinish)\n") && has(log, "fb3 64x32 clear=C draws 1 verts 3 inval=start:- end:- gpu "));
        CHECK(fake_finishes == 8 && fake_begins == 0);      // (one before and one after each of the 4 passes)
        CHECK(!strstr(log, " draw fb") && !strstr(log, " clear fb") && fake_reads == 0);
    } else {
        CHECK(has(log, "no GPU times with UA_GL_TRACE_DRAWS"));
        CHECK(has(log, "[gl]   fb3 = tex9 64x32 f R16G16B16A16"));
        CHECK(has(log, "#0 clear fb3 C colour (0,1,0,1) -> (0.25,0.5,0.75,1) (0.25,0.5,0.75,1)"));
        if (!strcmp(mode, "limit")) {
            CHECK(!strstr(log, "#1 "));
        } else {
            snprintf(want, sizeof want, "#1 draw fb3 prog5 vs %016llx fs %016llx 3 _MainTex=2:tex11 -> (0.25,0.5,0.75,1)",
                     (unsigned long long)fnv("void main() { gl_Position = vec4(0.0); }"), (unsigned long long)fnv("void main() {}"));
            CHECK(has(log, want));
            CHECK(has(log, "(0.25,0.5,0.75,1) | depth off stencil off blend sa,1-sa\n"));
            CHECK(has(log, "#2 blit fb3 -> fb0 (0,0,64,32) -> (0,0,720,480) C -> (10,20,30,255)"));
            CHECK(has(log, "#3 draw fb0 prog5"));
            CHECK(has(log, "6 _MainTex=2:tex11 -> (10,20,30,255) (10,20,30,255)"));
            CHECK(has(log, "#7 draw fb0 prog5 vs") && has(log, " 6x3 _MainTex=2:tex11"));   // instanced, base vertex
            CHECK(!strstr(log, "#8 "));
        }
    }
    if (strcmp(mode, "off") && !gpu) CHECK(fake_begins == 0);
    if (failures) fputs(log, stdout);
    printf("test_gltrace %s: %s\n", mode, failures ? "FAILED" : "ok");
    return failures != 0;
}
