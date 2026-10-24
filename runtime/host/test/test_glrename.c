// glrename.c check against fake_rename_gl.c (as libGLESv2.so.2), which computes the pictures: a
// frame like Hollow Knight's grab pass and blur copy-back, 40 frames, every picture compared with a
// reference computed here. Run by pointer_test.sh: test_glrename <scenario>
//   grab       the camera paints A, Unity copies A into the grab texture C, a sprite in A reads C,
//              an effect reads A: the copy is measured once, then left out; every picture right
//   copyback   an effect's result S copied into the camera's target T (invalidated first), T read,
//              T cleared next frame: left out, and nothing copied at all
//   notcopy    the copy draw scales by 0.5: never left out
//   params     C filters NEAREST, A LINEAR: after the copies are left out each name keeps its own
//   queries    bindings and attachments read back give the game's names
//   delete     C deleted after the copies were left out, a new texture made: no name in use twice,
//              A's picture right
//   context    another context attaches C to a framebuffer of its own: C is never renamed
//   check      UA_GL_RENAME=check: the copies are made and compared, none left out
//   off        UA_GL_RENAME unset: nothing wrapped
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../glrename.h"

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_RGBA8 0x8058
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601

extern int fake_copies, fake_blits, fake_draws;
extern float fake_scale[4], fake_seed;
extern void glGenTextures(int, unsigned *), glTexStorage2D(unsigned, int, unsigned, int, int), glGenFramebuffers(int, unsigned *);
extern void glUseProgram(unsigned), glClearColor(float, float, float, float), glTexParameteri(unsigned, unsigned, int);

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static void *fn(const so_shim *t, const char *name)
{
    for (; t && t->name; t++) if (!strcmp(t->name, name)) return t->fn;
    return NULL;
}
static void (*Bind)(unsigned, unsigned), (*BindTex)(unsigned, unsigned), (*Active)(unsigned), (*Clear)(unsigned);
static void (*Draw)(unsigned, int, unsigned, const void *), (*Attach)(unsigned, unsigned, unsigned, unsigned, int);
static void (*Read)(int, int, int, int, unsigned, unsigned, void *), (*Delete)(int, const unsigned *), (*Invalidate)(unsigned, int, const unsigned *);
static void (*GetInt)(unsigned, int *), (*GetAtt)(unsigned, unsigned, unsigned, int *), (*GetTexParam)(unsigned, unsigned, int *);

static unsigned g_C;
static void *other_context(void *a) { (void)a; Bind(GL_FRAMEBUFFER, 3); Attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_C, 0); return NULL; }
static unsigned tex(void)
{
    unsigned t;
    glGenTextures(1, &t);
    BindTex(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
    return t;
}
static unsigned fbo(unsigned t)
{
    unsigned f;
    glGenFramebuffers(1, &f);
    Bind(GL_FRAMEBUFFER, f);
    Attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    return f;
}
static int read_is(unsigned fb, const unsigned char *want, const char *what, int frame)
{
    unsigned char got[64];
    Bind(GL_READ_FRAMEBUFFER, fb);
    Read(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, got);
    if (memcmp(got, want, sizeof got)) { printf("frame %d: %s: got %d,%d,%d,%d want %d,%d,%d,%d\n", frame, what, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]); return 0; }
    return 1;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "grab";
    if (strcmp(mode, "off")) setenv("UA_GL_RENAME", !strcmp(mode, "check") ? "check" : "1", 1);
    const so_shim *t = glrename_table(NULL);
    Bind = fn(t, "glBindFramebuffer"); BindTex = fn(t, "glBindTexture"); Active = fn(t, "glActiveTexture"); Clear = fn(t, "glClear");
    Draw = fn(t, "glDrawElements"); Attach = fn(t, "glFramebufferTexture2D"); Read = fn(t, "glReadPixels");
    Delete = fn(t, "glDeleteTextures"); Invalidate = fn(t, "glInvalidateFramebuffer"); GetInt = fn(t, "glGetIntegerv");
    GetAtt = fn(t, "glGetFramebufferAttachmentParameteriv");
    if (!strcmp(mode, "off")) {
        CHECK(!Bind && !BindTex && !Draw);
        printf("test_glrename %s: %s\n", mode, failures ? "FAILED" : "ok");
        return failures != 0;
    }
    CHECK(Bind && BindTex && Active && Clear && Draw && Attach && Read && Delete && Invalidate && GetInt && GetAtt);
    if (failures) return 1;
    extern void glGetTexParameteriv(unsigned, unsigned, int *);
    GetTexParam = glGetTexParameteriv;

    Active(GL_TEXTURE0);
    unsigned A = tex(), C = tex(), D = tex(), S = tex(), T = tex();
    if (!strcmp(mode, "params")) {
        BindTex(GL_TEXTURE_2D, C); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        BindTex(GL_TEXTURE_2D, A); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    unsigned fbA = fbo(A), fbC = fbo(C), fbD = fbo(D), fbS = fbo(S), fbT = fbo(T);
    if (!strcmp(mode, "notcopy")) fake_scale[0] = fake_scale[1] = fake_scale[2] = fake_scale[3] = 0.5f;
    int copyback = !strcmp(mode, "copyback");
    if (!strcmp(mode, "context")) { g_C = C; pthread_t th; pthread_create(&th, NULL, other_context, NULL); pthread_join(th, NULL); }
    glrename_frame();                            // the first swap: this thread renders
    int copies_at[64] = { 0 };
    int deleted = 0;
    for (int f = 1; f <= 45; f++) {
        unsigned char pat[64], grab[64], camera[64], effect[64];
        for (int i = 0; i < 16; i++) for (int c = 0; c < 4; c++) {
            int x = i % 4, y = i / 4, v = (x + 16 * y + f) & 255;
            pat[4 * i + c] = (unsigned char)v;
            grab[4 * i + c] = (unsigned char)(!strcmp(mode, "notcopy") ? (int)(v * 0.5f + 0.5f) : v);
            camera[4 * i + c] = (unsigned char)(grab[4 * i + c] + 1);
            effect[4 * i + c] = (unsigned char)(255 - camera[4 * i + c]);
        }
        fake_seed = (float)f;
        if (copyback) {
            // the effect's result S, then copied into T (invalidated first), then T read into D
            Bind(GL_FRAMEBUFFER, fbT); glClearColor(0, 0, 0, 0); Clear(GL_COLOR_BUFFER_BIT);   // (the camera's clear)
            Bind(GL_FRAMEBUFFER, fbS); glUseProgram(7); Draw(4, 6, 0x1403, 0);
            Bind(GL_FRAMEBUFFER, fbT);
            static const unsigned c0[1] = { GL_COLOR_ATTACHMENT0 };
            Invalidate(GL_FRAMEBUFFER, 1, c0);
            BindTex(GL_TEXTURE_2D, S); glUseProgram(5); Draw(4, 6, 0x1403, 0);
            Bind(GL_FRAMEBUFFER, fbD); BindTex(GL_TEXTURE_2D, T); glUseProgram(6); Draw(4, 6, 0x1403, 0);
            unsigned char inv[64];
            for (int i = 0; i < 64; i++) inv[i] = (unsigned char)(255 - pat[i]);
            if (!read_is(fbD, inv, "the effect of the copy", f)) failures++;
            if (!read_is(fbT, pat, "the copy's target", f)) failures++;
            if (!read_is(fbS, pat, "the copy's source", f)) failures++;
        } else {
            // the camera paints A; the grab copies A into C; a sprite in A reads C; an effect reads A
            Bind(GL_FRAMEBUFFER, fbA); glClearColor(0, 0, 0, 0); Clear(GL_COLOR_BUFFER_BIT); glUseProgram(7); Draw(4, 6, 0x1403, 0);
            if (!deleted) {
                Bind(GL_FRAMEBUFFER, fbC); BindTex(GL_TEXTURE_2D, A); glUseProgram(5); Draw(4, 3, 0x1403, 0);
                Bind(GL_FRAMEBUFFER, fbA); BindTex(GL_TEXTURE_2D, C); glUseProgram(8); Draw(4, 6, 0x1403, 0);
            } else {
                // (C is gone: the camera's picture is the paint, and a texture made now has a fresh name)
                for (int i = 0; i < 64; i++) { camera[i] = pat[i]; effect[i] = (unsigned char)(255 - pat[i]); }
                unsigned tmp = tex();
                CHECK(tmp != A && tmp != D && tmp != S && tmp != T);
                unsigned d1[1] = { tmp };
                Delete(1, d1);
            }
            Bind(GL_FRAMEBUFFER, fbD); BindTex(GL_TEXTURE_2D, A); glUseProgram(6); Draw(4, 6, 0x1403, 0);
            if (!read_is(fbA, camera, "the camera's picture", f)) failures++;
            if (!read_is(fbD, effect, "the effect of the camera's picture", f)) failures++;
            if (!deleted && !read_is(fbC, grab, "the grab", f)) failures++;
            if (!strcmp(mode, "params") && f > 35) {
                int v = 0;
                BindTex(GL_TEXTURE_2D, C); GetTexParam(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &v); CHECK(v == GL_NEAREST);
                BindTex(GL_TEXTURE_2D, A); GetTexParam(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &v); CHECK(v == GL_LINEAR);
            }
            if (!strcmp(mode, "queries") && f > 35) {
                int v = 0;
                BindTex(GL_TEXTURE_2D, C); GetInt(0x8069, &v); CHECK(v == (int)C);
                Bind(GL_FRAMEBUFFER, fbC); GetAtt(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0x8CD1, &v); CHECK(v == (int)C);
                Bind(GL_FRAMEBUFFER, fbA); GetAtt(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0x8CD1, &v); CHECK(v == (int)A);
            }
            if (!strcmp(mode, "delete") && f == 38) {
                unsigned d1[1] = { C };
                Delete(1, d1);
                deleted = 1;
                unsigned n = tex();                    // a name the driver gives: never A's
                CHECK(n != A && n != D && n != S && n != T);
                unsigned char want[64];
                for (int i = 0; i < 64; i++) want[i] = camera[i];
                if (!read_is(fbA, want, "the camera's picture after C's deletion", f)) failures++;
            }
        }
        copies_at[f] = fake_copies;
        glrename_frame();
    }
    int late = copies_at[45] - copies_at[40];              // copy draws made in the last frames
    if (!strcmp(mode, "notcopy") || !strcmp(mode, "check") || !strcmp(mode, "context")) CHECK(late >= 5);
    else if (strcmp(mode, "delete")) CHECK(late == 0);
    CHECK(copies_at[45] >= 30);                              // (made until measured)
    if (copyback) CHECK(fake_blits == 0);
    (void)fbS; (void)fbT; (void)S; (void)T; (void)fake_draws;
    printf("test_glrename %s: %s\n", mode, failures ? "FAILED" : "ok");
    return failures != 0;
}
