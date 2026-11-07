// gltexdump.c: a 2D texture as the GPU samples it, written to files (gltrace.c's UA_GL_TRACE_DUMPTEX).
//
// Compressed textures cannot be read back in GLES; the texture is drawn instead, texel for texel, into
// an RGBA8 texture of its size (a full-screen triangle, nearest filtering on level 0), read back and
// written as <dir>/tex<id>.ppm (colour) and <dir>/tex<id>.a.pgm (alpha). Called from the traced frame's
// draw log; every piece of GL state it touches is read first and put back after, through the driver's
// own functions (no layer sees these calls).
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gltexdump.h"

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned char GLboolean;
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_FRAMEBUFFER 0x8D40
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_VIEWPORT 0x0BA2
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_SAMPLER_BINDING 0x8919
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_SCISSOR_TEST 0x0C11
#define GL_CULL_FACE 0x0B44
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_NEAREST 0x2600
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_TRIANGLES 0x0004
#define GL_LINK_STATUS 0x8B82

static void *gl(const char *n)
{
    static void *h;
    if (!h) h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    return h ? dlsym(h, n) : NULL;
}
#define F(ret, name, args) static ret (*name) args
F(void, glGetIntegerv, (GLenum, GLint *)); F(GLboolean, glIsEnabled, (GLenum)); F(void, glEnable, (GLenum)); F(void, glDisable, (GLenum));
F(void, glGetBooleanv, (GLenum, GLboolean *)); F(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean));
F(void, glGenTextures, (GLsizei, GLuint *)); F(void, glDeleteTextures, (GLsizei, const GLuint *)); F(void, glBindTexture, (GLenum, GLuint));
F(void, glTexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)); F(void, glActiveTexture, (GLenum));
F(void, glGetTexParameteriv, (GLenum, GLenum, GLint *)); F(void, glTexParameteri, (GLenum, GLenum, GLint));
F(void, glGenFramebuffers, (GLsizei, GLuint *)); F(void, glDeleteFramebuffers, (GLsizei, const GLuint *)); F(void, glBindFramebuffer, (GLenum, GLuint));
F(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)); F(void, glViewport, (GLint, GLint, GLsizei, GLsizei));
F(GLuint, glCreateShader, (GLenum)); F(void, glShaderSource, (GLuint, GLsizei, const char *const *, const GLint *)); F(void, glCompileShader, (GLuint));
F(GLuint, glCreateProgram, (void)); F(void, glAttachShader, (GLuint, GLuint)); F(void, glLinkProgram, (GLuint)); F(void, glGetProgramiv, (GLuint, GLenum, GLint *));
F(void, glUseProgram, (GLuint)); F(GLint, glGetUniformLocation, (GLuint, const char *)); F(void, glUniform1i, (GLint, GLint));
F(void, glGenVertexArrays, (GLsizei, GLuint *)); F(void, glBindVertexArray, (GLuint)); F(void, glBindSampler, (GLuint, GLuint));
F(void, glBindBuffer, (GLenum, GLuint)); F(void, glDrawArrays, (GLenum, GLint, GLsizei));
F(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)); F(GLenum, glGetError, (void));
#undef F

static int load(void)
{
    static int ok = -1;
    if (ok >= 0) return ok;
#define L(name) if (!(*(void **)&name = gl(#name))) return ok = 0
    L(glGetIntegerv); L(glIsEnabled); L(glEnable); L(glDisable); L(glGetBooleanv); L(glColorMask); L(glGenTextures); L(glDeleteTextures);
    L(glBindTexture); L(glTexStorage2D); L(glActiveTexture); L(glGetTexParameteriv); L(glTexParameteri); L(glGenFramebuffers);
    L(glDeleteFramebuffers); L(glBindFramebuffer); L(glFramebufferTexture2D); L(glViewport); L(glCreateShader); L(glShaderSource);
    L(glCompileShader); L(glCreateProgram); L(glAttachShader); L(glLinkProgram); L(glGetProgramiv); L(glUseProgram); L(glGetUniformLocation);
    L(glUniform1i); L(glGenVertexArrays); L(glBindVertexArray); L(glBindSampler); L(glBindBuffer); L(glDrawArrays); L(glReadPixels); L(glGetError);
#undef L
    return ok = 1;
}

static GLuint g_prog, g_vao;
static int program(void)
{
    if (g_prog) return 1;
    static const char *vs = "#version 300 es\nout vec2 uv;\nvoid main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                            "uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
    static const char *fs = "#version 300 es\nprecision highp float;\nuniform sampler2D t;\nin vec2 uv;\nout vec4 o;\nvoid main() { o = texture(t, uv); }\n";
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &vs, NULL); glCompileShader(v);
    glShaderSource(f, 1, &fs, NULL); glCompileShader(f);
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    GLint okk = 0; glGetProgramiv(p, GL_LINK_STATUS, &okk);
    if (!okk) return 0;
    g_prog = p;
    glGenVertexArrays(1, &g_vao);
    return 1;
}

static void write_files(const char *dir, GLuint tex, int w, int h, const unsigned char *px)
{
    char path[512];
    snprintf(path, sizeof path, "%s/tex%u.ppm", dir, tex);
    FILE *c = fopen(path, "wb");
    snprintf(path, sizeof path, "%s/tex%u.a.pgm", dir, tex);
    FILE *a = fopen(path, "wb");
    if (c) fprintf(c, "P6\n%d %d\n255\n", w, h);
    if (a) fprintf(a, "P5\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--)                // (GL's rows bottom up)
        for (int x = 0; x < w; x++) {
            const unsigned char *p = px + ((size_t)y * w + x) * 4;
            if (c) fwrite(p, 1, 3, c);
            if (a) fputc(p[3], a);
        }
    if (c) fclose(c);
    if (a) fclose(a);
    fprintf(stderr, "[gl]   tex%u dumped to %s/tex%u.ppm (%dx%d)\n", tex, dir, tex, w, h);
}

void gltexdump(GLuint tex, int w, int h, const char *dir)
{
    if (!load() || w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    // what is touched, read first
    GLint prog, dfb, rfb, vp[4], vao, active, sampler0, pbo, tex0, minf, magf, base;
    GLboolean mask[4];
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    glGetIntegerv(GL_VIEWPORT, vp); glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao); glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo); glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    static const GLenum caps[] = { GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_CULL_FACE };
    GLboolean on[5];
    for (int i = 0; i < 5; i++) on[i] = glIsEnabled(caps[i]);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0); glGetIntegerv(GL_SAMPLER_BINDING, &sampler0);
    if (!program()) { glActiveTexture((GLenum)active); return; }
    // the texture's own filtering, set to level 0 nearest for the draw and put back
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minf); glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magf);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &base);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint rt = 0, fb = 0;
    glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    for (int i = 0; i < 5; i++) glDisable(caps[i]);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, w, h);
    glBindTexture(GL_TEXTURE_2D, tex); glBindSampler(0, 0);
    glUseProgram(g_prog); glUniform1i(glGetUniformLocation(g_prog, "t"), 0);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    unsigned char *px = malloc((size_t)w * h * 4);
    if (px) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        write_files(dir, tex, w, h, px);
        free(px);
    }
    // everything put back
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, base);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dfb); glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    glDeleteFramebuffers(1, &fb); glDeleteTextures(1, &rt);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex0); glBindSampler(0, (GLuint)sampler0); glActiveTexture((GLenum)active);
    glBindVertexArray((GLuint)vao); glUseProgram((GLuint)prog); glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pbo);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    for (int i = 0; i < 5; i++) if (on[i]) glEnable(caps[i]);
    while (glGetError()) {}
}
