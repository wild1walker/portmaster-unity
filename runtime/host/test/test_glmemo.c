// glmemo.c check against fake_memo_gl.c (as libGLESv2.so.2): a pass that invalidates its one colour attachment and
// draws the same thing every frame (program, uniforms, sampled texture, uniform block, vertex data, state) is drawn
// once, then left out together with its invalidate; drawn again whenever anything it reads or its target changed.
// Run by pointer_test.sh: test_glmemo <scenario>
//   skip        the same pass every frame: one draw, one invalidate in 5 frames
//   uniform     a uniform's value changes: drawn again
//   texture     the sampled texture is uploaded to: drawn again
//   param       a texture parameter set to the value it has: still left out; to another value: drawn
//   targetparam a sampling parameter of the target texture changed (it is read with another filter later): still left out
//   target      something else writes the target texture (a clear through another framebuffer): drawn again
//   noinval     a pass that does not begin with an invalidate (its old contents are loaded): never left out
//   blend       blending on: never left out
//   depth       a framebuffer with a depth attachment: never left out
//   ubo         the uniform block rewritten every frame through a mapping with the same bytes: left out from frame 2
//   ubochange   ... with other bytes: drawn
//   ubofresh    the uniform block's data written every frame into another new small buffer (glBufferData through the array
//               buffer binding, then bound as a uniform buffer), the same bytes: left out
//   vbo         the vertex buffer rewritten every frame (glBufferSubData) with the same bytes: left out
//   vbochange   ... with other bytes: drawn
//   second      a second, blended draw after the left-out one in the same pass: drawn, the invalidate not sent
//   clear       a clear between the invalidate and the draw: the invalidate goes first, then the clear, then the draw
//   upload      an upload into the target between the invalidate and the draw: the invalidate goes first
//   feedback    the program samples its own target: never left out
//   recover     a key that changes for 10 frames, then stays: left out again after a while
//   respecify   ... and the target gets new storage at frame 11 (a render texture pool's new texture): keyed at once
//   check       UA_GL_MEMO=check: every draw made, each would-be-left-out one compared: the same pixels
//   checkdiff   ... and an input no key has (the fake's hidden one) changes: reported as other pixels
//   es2         an OpenGL ES 2 context: nothing left out
//   off         UA_GL_MEMO=0: nothing wrapped
//   withrec     UA_GL_REC on (the frame recorder holds the calls until the swap): nothing wrapped
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../glmemo.h"

#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_TRIANGLES 0x0004
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_ARRAY_BUFFER 0x8892
#define GL_FLOAT 0x1406
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_MAP_FLUSH_EXPLICIT_BIT 0x0010

extern char fake_log[4096];
extern int fake_draws, fake_invalidates, fake_reads, fake_blend;
extern unsigned fake_hidden;
extern const char *fake_version;
extern void glUseProgram(unsigned), glUniform4f(int, float, float, float, float), glUniform1i(int, int);
extern void glActiveTexture(unsigned), glBindTexture(unsigned, unsigned), glBindBuffer(unsigned, unsigned);
extern void glEnableVertexAttribArray(unsigned), glVertexAttribPointer(unsigned, int, unsigned, unsigned char, int, const void *);

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void *fn(const so_shim *t, const char *name)
{
    for (; t && t->name; t++) if (!strcmp(t->name, name)) return t->fn;
    return NULL;
}

static void (*Bind)(unsigned, unsigned);
static void (*Invalidate)(unsigned, int, const unsigned *);
static void (*Draw)(unsigned, int, int);
static void (*Clear)(unsigned);
static void (*TexSubImage)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (*TexParameteri)(unsigned, unsigned, int);
static void (*TexImage)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (*BufferData)(unsigned, intptr_t, const void *, unsigned);
static void (*BufferSubData)(unsigned, intptr_t, intptr_t, const void *);
static void *(*MapBufferRange)(unsigned, intptr_t, intptr_t, unsigned);
static void (*FlushMapped)(unsigned, intptr_t, intptr_t);
static unsigned char (*Unmap)(unsigned);
static void (*BindBufferRange)(unsigned, unsigned, unsigned, intptr_t, intptr_t);

static float g_ua = 0.5f;
static int g_fb = 3, g_inval = 1, g_prog = 7;
static unsigned char g_ubo[16] = { 1, 2, 3, 4 }, g_vbo[36] = { 9, 8, 7 };
static int g_use_ubo, g_use_vbo;
static void (*g_between)(int frame);    // after the invalidate, before the draw
static void (*g_after)(int frame);      // after the draw, in the same pass

static void pass(int frame)
{
    Bind(GL_FRAMEBUFFER, (unsigned)g_fb);
    static const unsigned att[3] = { GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT };
    if (g_inval) Invalidate(GL_FRAMEBUFFER, 3, att);
    glUseProgram((unsigned)g_prog);
    glUniform4f(0, g_ua, 0.25f, 0.125f, 1);
    glUniform1i(1, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_prog == 8 ? 9 : 11);
    if (g_use_ubo == 2) {
        unsigned b = 10 + (unsigned)frame % 5;
        glBindBuffer(GL_ARRAY_BUFFER, b);                    // (written as Unity 2021 does, then bound for the block)
        BufferData(GL_ARRAY_BUFFER, 16, g_ubo, 0);
        BindBufferRange(GL_UNIFORM_BUFFER, 0, b, 0, 16);
    } else if (g_use_ubo) {
        glBindBuffer(GL_UNIFORM_BUFFER, 2);
        unsigned char *p = MapBufferRange(GL_UNIFORM_BUFFER, 0, 16, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
        memcpy(p, g_ubo, 16);
        FlushMapped(GL_UNIFORM_BUFFER, 0, 16);
        Unmap(GL_UNIFORM_BUFFER);
        BindBufferRange(GL_UNIFORM_BUFFER, 0, 2, 0, 16);
    }
    if (g_use_vbo) {
        glBindBuffer(GL_ARRAY_BUFFER, 3);
        BufferSubData(GL_ARRAY_BUFFER, 0, 36, g_vbo);
        glVertexAttribPointer(0, 3, GL_FLOAT, 0, 0, 0);
        glEnableVertexAttribArray(0);
    }
    if (g_between) g_between(frame);
    Draw(GL_TRIANGLES, 0, 3);
    if (g_after) g_after(frame);
    Bind(GL_FRAMEBUFFER, 0);
}
static int frames(int n) { for (int i = 1; i <= n; i++) { pass(i); glmemo_frame(); } return n; }

static void change_uniform(int f) { if (f == 2) g_ua = 0.75f; }
static void upload(int f) { if (f == 2) { static const unsigned char px[4]; TexSubImage(GL_TEXTURE_2D, 0, 0, 0, 1, 1, 0x1908, 0x1401, px); } }
static void param(int f)
{
    if (f == 2) TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 0);          // (the value it has)
    if (f == 3) TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 0x2601);     // another one
}
static void target_param(int f)
{
    glActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, 9);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f & 1 ? 0x2601 : 0x2600);
    glActiveTexture(GL_TEXTURE0);
}
static void ubo_change(int f) { if (f == 1) g_ubo[0] = 1; if (f == 2) { g_ubo[0] = 5; } }
static void vbo_change(int f) { if (f == 2) g_vbo[0] = 1; }
static void second(int f) { if (f == 3) { fake_blend = 1; Draw(GL_TRIANGLES, 0, 3); fake_blend = 0; } }
static void clear_between(int f) { if (f == 2) Clear(GL_COLOR_BUFFER_BIT); }
static void upload_target(int f)
{
    if (f != 2) return;
    glActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, 9);
    static const unsigned char px[4];
    TexSubImage(GL_TEXTURE_2D, 0, 0, 0, 1, 1, 0x1908, 0x1401, px);
    glActiveTexture(GL_TEXTURE0);
}
static void recover(int f) { if (f <= 10) g_ua = (float)f; }
static void respecify(int f)
{
    recover(f);
    if (f != 11) return;
    glActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, 9);
    TexImage(GL_TEXTURE_2D, 0, 0x1908, 32, 1, 0, 0x1908, 0x1401, NULL);
    glActiveTexture(GL_TEXTURE0);
}
static void hidden(int f) { fake_hidden = (unsigned)f; }

static char *read_log(const char *path)
{
    static char buf[65536];
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;
    return buf;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "skip";
    char logpath[64];
    snprintf(logpath, sizeof logpath, "/tmp/test_glmemo.%d.log", (int)getpid());
    if (!freopen(logpath, "w", stderr)) return 1;
    if (!strcmp(mode, "off")) setenv("UA_GL_MEMO", "0", 1);
    if (!strcmp(mode, "withrec")) setenv("UA_GL_REC", "1", 1);
    if (!strncmp(mode, "check", 5)) setenv("UA_GL_MEMO", "check", 1);
    if (!strcmp(mode, "es2")) fake_version = "OpenGL ES 2.0 fake";
    const so_shim *t = glmemo_table(NULL);
    Bind = fn(t, "glBindFramebuffer"); Invalidate = fn(t, "glInvalidateFramebuffer"); Draw = fn(t, "glDrawArrays");
    Clear = fn(t, "glClear"); TexSubImage = fn(t, "glTexSubImage2D"); TexParameteri = fn(t, "glTexParameteri"); TexImage = fn(t, "glTexImage2D");
    BufferData = fn(t, "glBufferData"); BufferSubData = fn(t, "glBufferSubData"); MapBufferRange = fn(t, "glMapBufferRange");
    FlushMapped = fn(t, "glFlushMappedBufferRange"); Unmap = fn(t, "glUnmapBuffer"); BindBufferRange = fn(t, "glBindBufferRange");
    if (!strcmp(mode, "off") || !strcmp(mode, "withrec")) {
        CHECK(!Bind && !Draw && !Invalidate);
        printf("test_glmemo %s: %s\n", mode, failures ? "FAILED" : "ok");
        return failures != 0;
    }
    CHECK(Bind && Invalidate && Draw && Clear && TexSubImage && TexParameteri && BufferData && BufferSubData && MapBufferRange &&
          FlushMapped && Unmap && BindBufferRange);
    if (failures) { printf("test_glmemo %s: FAILED (wrappers)\n", mode); return 1; }
    glmemo_frame();                              // (the first swap: this thread renders)
    // the buffers exist before the frames (one write each: the next write makes a shadow)
    glBindBuffer(GL_UNIFORM_BUFFER, 2); BufferData(GL_UNIFORM_BUFFER, 16, g_ubo, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 3); BufferData(GL_ARRAY_BUFFER, 36, g_vbo, 0);

    int n = 5;
    if (!strcmp(mode, "skip")) {
        frames(n);
        CHECK(fake_draws == 1 && fake_invalidates == 1);
        CHECK(!strcmp(fake_log, "ID"));
    } else if (!strcmp(mode, "uniform")) {
        g_between = change_uniform; frames(n);
        CHECK(fake_draws == 2 && fake_invalidates == 2);
    } else if (!strcmp(mode, "texture")) {
        g_between = upload; frames(n);
        CHECK(fake_draws == 2);
        CHECK(!strcmp(fake_log, "IDUID"));             // (the held invalidate goes after an upload of another texture)
    } else if (!strcmp(mode, "param")) {
        g_between = param; frames(n);
        CHECK(fake_draws == 2);                  // frames 1 and 3
    } else if (!strcmp(mode, "targetparam")) {
        g_between = target_param; frames(n);
        CHECK(fake_draws == 1);
    } else if (!strcmp(mode, "target")) {
        pass(1); glmemo_frame();
        Bind(GL_FRAMEBUFFER, 4); Clear(GL_COLOR_BUFFER_BIT); Bind(GL_FRAMEBUFFER, 0);
        frames(3);
        CHECK(fake_draws == 2);
    } else if (!strcmp(mode, "noinval")) {
        g_inval = 0; frames(n);
        CHECK(fake_draws == n);
    } else if (!strcmp(mode, "blend")) {
        fake_blend = 1; frames(n);
        CHECK(fake_draws == n);
    } else if (!strcmp(mode, "depth")) {
        g_fb = 4; frames(n);
        CHECK(fake_draws == n && fake_invalidates == n);
    } else if (!strcmp(mode, "ubo")) {
        g_use_ubo = 1; g_prog = 6; frames(n);
        CHECK(fake_draws == 1);
    } else if (!strcmp(mode, "ubochange")) {
        g_use_ubo = 1; g_prog = 6; g_between = ubo_change; frames(n);
        CHECK(fake_draws == 2);
    } else if (!strcmp(mode, "ubofresh")) {
        g_use_ubo = 2; g_prog = 6; frames(n);
        CHECK(fake_draws == 1);
    } else if (!strcmp(mode, "vbo")) {
        g_use_vbo = 1; frames(n);
        CHECK(fake_draws == 1);
    } else if (!strcmp(mode, "vbochange")) {
        g_use_vbo = 1; g_between = vbo_change; frames(n);
        CHECK(fake_draws == 2);
    } else if (!strcmp(mode, "second")) {
        g_after = second; frames(4);
        // frame 1 drawn, 2 left out, 3 left out and then the second draw (on the contents the first would have written,
        // without the invalidate), 4 drawn (the target's last write was the second draw)
        CHECK(fake_draws == 3 && fake_invalidates == 2);
        CHECK(!strcmp(fake_log, "IDDID"));
    } else if (!strcmp(mode, "clear")) {
        g_between = clear_between; frames(3);
        CHECK(!strcmp(fake_log, "IDICDID"));              // frame 3: drawn (frame 2 cleared the target: the key was not made)
    } else if (!strcmp(mode, "upload")) {
        g_between = upload_target; frames(3);
        CHECK(!strncmp(fake_log, "IDIUD", 5));
    } else if (!strcmp(mode, "feedback")) {
        g_prog = 8; frames(n);
        CHECK(fake_draws == n);
    } else if (!strcmp(mode, "recover")) {
        g_between = recover; frames(60);
        CHECK(fake_draws < 30);
    } else if (!strcmp(mode, "respecify")) {
        g_between = respecify; frames(60);
        CHECK(fake_draws == 11);                 // frames 1 - 11 drawn, then left out
    } else if (!strcmp(mode, "check") || !strcmp(mode, "checkdiff")) {
        if (!strcmp(mode, "checkdiff")) g_between = hidden;
        frames(601);
        CHECK(fake_draws == 601);
        fflush(stderr);
        const char *log = read_log(logpath);
        if (!strcmp(mode, "check")) {
            CHECK(strstr(log, " 0 other ones") != NULL);
            CHECK(strstr(log, "wrote the same pixels") != NULL);
        } else {
            CHECK(strstr(log, "the draw wrote other pixels") != NULL);
        }
    } else if (!strcmp(mode, "es2")) {
        frames(n);
        CHECK(fake_draws == n);
    } else {
        printf("unknown scenario %s\n", mode);
        return 2;
    }
    fflush(stderr);
    if (failures) printf("%s", read_log(logpath));
    remove(logpath);
    printf("test_glmemo %s: %s (draws %d, invalidates %d, log %.40s)\n", mode, failures ? "FAILED" : "ok", fake_draws, fake_invalidates, fake_log);
    return failures != 0;
}
