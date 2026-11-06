// Fill census: where a frame's fragments go (any Unity GLES 3 game; a measuring tool, off by default).
//
//   UA_GL_TRACE=<s> UA_GL_TRACE_FILL=1   in each traced frame (gltrace.c), every draw is made twice:
//   first with a counting copy of its program, into a shadow of its target, then as the game made it.
//   The copy runs the game's fragment shader unchanged and then counts, with atomic counters, every
//   fragment it shades, the ones the shader discards, the ones that leave the target unchanged under
//   the draw's blending (alpha 0 with alpha blending: what a tighter sprite mesh would not draw), and
//   the ones that replace what is under them although blending is on (alpha 1: could be drawn
//   opaque). The shadow (an RGBA8 texture per target, with the target's own depth and stencil
//   buffers, their tests as the game set them, nothing written to them) counts, per pixel, the
//   fragments since the last one that covered the pixel completely; a clear, an invalidation or a
//   blit of the target covers it too. What the shadows hold at a clear and at the frame's end gives
//   the fragments drawn over later: written, then never seen.
//     [fill] frame <n>: <frags> fragments (<x> per pixel of the frame), discarded <%>, unchanged <%>,
//            covered later <%>, opaque under blending <%>; <n> draws not counted (<why>)
//     [fill]   target <name> <w>x<h>: <frags> fragments (<x> per pixel) unchanged <%> covered <%> ...
//     [fill]   program <n> fs <hash>: <draws> draws <frags> fragments unchanged <%> discarded <%> opaque <%>
//   The counting copy is made from the program's sources as Unity gave them (a program loaded from a
//   binary has none: its draws are not counted; start without the game's shader cache). The game's
//   draws are made as before, its picture is the same; a traced frame takes much longer.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glfill.h"

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned char GLboolean;
typedef intptr_t GLintptr; typedef intptr_t GLsizeiptr; typedef unsigned GLbitfield;

#define GL_FRAMEBUFFER 0x8D40
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL 0x8CD2
#define GL_TEXTURE 0x1702
#define GL_RENDERBUFFER 0x8D41
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_RGBA8 0x8058
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_COLOR 0x1800
#define GL_VIEWPORT 0x0BA2
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_SCISSOR_TEST 0x0C11
#define GL_BLEND_SRC_RGB 0x80C9
#define GL_BLEND_DST_RGB 0x80C8
#define GL_BLEND_SRC_ALPHA 0x80CB
#define GL_BLEND_DST_ALPHA 0x80CA
#define GL_BLEND_EQUATION_RGB 0x8009
#define GL_BLEND_EQUATION_ALPHA 0x883D
#define GL_FUNC_ADD 0x8006
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_DEPTH_WRITEMASK 0x0B72
#define GL_STENCIL_WRITEMASK 0x0B98
#define GL_STENCIL_BACK_WRITEMASK 0x8CA5
#define GL_ONE 1
#define GL_ZERO 0
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_SHADER_TYPE 0x8B4F
#define GL_SHADER_SOURCE_LENGTH 0x8B88
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_ACTIVE_ATTRIBUTES 0x8B89
#define GL_ACTIVE_UNIFORM_BLOCKS 0x8A36
#define GL_UNIFORM_BLOCK_INDEX 0x8A3A
#define GL_UNIFORM_BLOCK_BINDING 0x8A3F
#define GL_ATOMIC_COUNTER_BUFFER 0x92C0
#define GL_ATOMIC_COUNTER_BUFFER_BINDING 0x92C1
#define GL_DYNAMIC_COPY 0x88EA
#define GL_MAP_READ_BIT 0x0001
#define GL_ALL_BARRIER_BITS 0xFFFFFFFF
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_ACTIVE_TEXTURE 0x84E0
// uniform types
#define T_F 0x1406
#define T_V2 0x8B50
#define T_V3 0x8B51
#define T_V4 0x8B52
#define T_I 0x1404
#define T_IV2 0x8B53
#define T_IV3 0x8B54
#define T_IV4 0x8B55
#define T_B 0x8B56
#define T_BV2 0x8B57
#define T_BV3 0x8B58
#define T_BV4 0x8B59
#define T_U 0x1405
#define T_UV2 0x8DC6
#define T_UV3 0x8DC7
#define T_UV4 0x8DC8
#define T_M2 0x8B5A
#define T_M3 0x8B5B
#define T_M4 0x8B5C
#define T_M23 0x8B65
#define T_M24 0x8B66
#define T_M32 0x8B67
#define T_M34 0x8B68
#define T_M42 0x8B69
#define T_M43 0x8B6A

static int g_on;
#define F(ret, name, args) static ret (*name) args;
F(void, gGetIntegerv, (GLenum, GLint *)) F(void, gGetIntegeri_v, (GLenum, GLuint, GLint *))
F(GLboolean, gIsEnabled, (GLenum)) F(void, gEnable, (GLenum)) F(void, gDisable, (GLenum))
F(void, gGetBooleanv, (GLenum, GLboolean *)) F(void, gColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))
F(void, gDepthMask, (GLboolean)) F(void, gStencilMaskSeparate, (GLenum, GLuint))
F(void, gBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) F(void, gBlendEquationSeparate, (GLenum, GLenum))
F(void, gUseProgram, (GLuint)) F(void, gBindFramebuffer, (GLenum, GLuint)) F(void, gGenFramebuffers, (GLsizei, GLuint *))
F(void, gFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
F(void, gFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) F(GLenum, gCheckFramebufferStatus, (GLenum))
F(void, gGetFbAttachment, (GLenum, GLenum, GLenum, GLint *)) F(void, gGenTextures, (GLsizei, GLuint *))
F(void, gBindTexture, (GLenum, GLuint)) F(void, gTexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))
F(void, gDeleteTextures, (GLsizei, const GLuint *))
F(GLuint, gCreateShader, (GLenum)) F(void, gShaderSource, (GLuint, GLsizei, const char *const *, const GLint *))
F(void, gCompileShader, (GLuint)) F(void, gGetShaderiv, (GLuint, GLenum, GLint *))
F(void, gGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, char *)) F(void, gGetShaderSource, (GLuint, GLsizei, GLsizei *, char *))
F(GLuint, gCreateProgram, (void)) F(void, gAttachShader, (GLuint, GLuint)) F(void, gBindAttribLocation, (GLuint, GLuint, const char *))
F(void, gLinkProgram, (GLuint)) F(void, gGetProgramiv, (GLuint, GLenum, GLint *))
F(void, gGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *)) F(void, gDeleteShader, (GLuint))
F(void, gGetActiveAttrib, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *))
F(GLint, gGetAttribLocation, (GLuint, const char *))
F(void, gGetActiveUniform, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *))
F(void, gGetActiveUniformsiv, (GLuint, GLsizei, const GLuint *, GLenum, GLint *))
F(GLint, gGetUniformLocation, (GLuint, const char *))
F(void, gGetUniformfv, (GLuint, GLint, float *)) F(void, gGetUniformiv, (GLuint, GLint, GLint *))
F(void, gGetUniformuiv, (GLuint, GLint, GLuint *))
F(void, gGetActiveUniformBlockiv, (GLuint, GLuint, GLenum, GLint *))
F(void, gGetActiveUniformBlockName, (GLuint, GLuint, GLsizei, GLsizei *, char *))
F(GLuint, gGetUniformBlockIndex, (GLuint, const char *)) F(void, gUniformBlockBinding, (GLuint, GLuint, GLuint))
F(void, gGenBuffers, (GLsizei, GLuint *)) F(void, gBindBuffer, (GLenum, GLuint)) F(void, gBindBufferBase, (GLenum, GLuint, GLuint))
F(void, gBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) F(void, gBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *))
F(void *, gMapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield)) F(GLboolean, gUnmapBuffer, (GLenum))
F(void, gMemoryBarrier, (GLbitfield)) F(void, gReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))
F(void, gPixelStorei, (GLenum, GLint)) F(void, gClearBufferfv, (GLenum, GLint, const float *))
F(void, gProgramUniform1i, (GLuint, GLint, GLint)) F(void, gProgramUniform4i, (GLuint, GLint, GLint, GLint, GLint, GLint))
F(void, gPU1fv, (GLuint, GLint, GLsizei, const float *)) F(void, gPU2fv, (GLuint, GLint, GLsizei, const float *))
F(void, gPU3fv, (GLuint, GLint, GLsizei, const float *)) F(void, gPU4fv, (GLuint, GLint, GLsizei, const float *))
F(void, gPU1iv, (GLuint, GLint, GLsizei, const GLint *)) F(void, gPU2iv, (GLuint, GLint, GLsizei, const GLint *))
F(void, gPU3iv, (GLuint, GLint, GLsizei, const GLint *)) F(void, gPU4iv, (GLuint, GLint, GLsizei, const GLint *))
F(void, gPU1uiv, (GLuint, GLint, GLsizei, const GLuint *)) F(void, gPU2uiv, (GLuint, GLint, GLsizei, const GLuint *))
F(void, gPU3uiv, (GLuint, GLint, GLsizei, const GLuint *)) F(void, gPU4uiv, (GLuint, GLint, GLsizei, const GLuint *))
F(void, gPUM2, (GLuint, GLint, GLsizei, GLboolean, const float *)) F(void, gPUM3, (GLuint, GLint, GLsizei, GLboolean, const float *))
F(void, gPUM4, (GLuint, GLint, GLsizei, GLboolean, const float *)) F(void, gPUM23, (GLuint, GLint, GLsizei, GLboolean, const float *))
F(void, gPUM24, (GLuint, GLint, GLsizei, GLboolean, const float *)) F(void, gPUM32, (GLuint, GLint, GLsizei, GLboolean, const float *))
F(void, gPUM34, (GLuint, GLint, GLsizei, GLboolean, const float *)) F(void, gPUM42, (GLuint, GLint, GLsizei, GLboolean, const float *))
F(void, gPUM43, (GLuint, GLint, GLsizei, GLboolean, const float *))
F(GLenum, gGetError, (void))
#undef F

static void *real(const char *name)
{
    static void *h;
    if (!h) h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    void *p = h ? dlsym(h, name) : NULL;
    if (!p) {
        void *e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        void *(*gpa)(const char *) = e ? (void *(*)(const char *))dlsym(e, "eglGetProcAddress") : NULL;
        if (gpa) p = gpa(name);
    }
    return p;
}

static void drain(void) { for (int i = 0; gGetError && i < 16 && gGetError(); i++) ; }

// ---- programs: the sources Unity gave (copied at glAttachShader), the counting copy
#define MAX_PROG 65536
typedef struct { GLint orig, mine; GLenum type; GLint size; char name[48]; float last[4]; } uni_t;
typedef struct {
    char *vs, *fs;
    int state;                   // 0 not made yet, 1 made, -1 cannot be made
    GLuint prog;
    GLint l_bf, l_mask, l_mode;
    uni_t *u; int nu;
    GLuint *blk_orig, *blk_mine; int nb;
    long draws, frags, discarded, unchanged, opaque;   // this frame's
} prog_t;
static prog_t **g_prog;

void glfill_attach(unsigned prog, unsigned sh)
{
    if (!g_on || prog >= MAX_PROG || !gGetShaderiv || !gGetShaderSource) return;
    GLint type = 0, len = 0;
    gGetShaderiv(sh, GL_SHADER_TYPE, &type);
    gGetShaderiv(sh, GL_SHADER_SOURCE_LENGTH, &len);
    if (len <= 0) return;
    char *src = malloc((size_t)len + 1);
    if (!src) return;
    GLsizei got = 0;
    gGetShaderSource(sh, len + 1, &got, src);
    src[got] = 0;
    if (!g_prog[prog]) g_prog[prog] = calloc(1, sizeof(prog_t));
    prog_t *p = g_prog[prog];
    if (!p) { free(src); return; }
    char **slot = type == GL_VERTEX_SHADER ? &p->vs : type == GL_FRAGMENT_SHADER ? &p->fs : NULL;
    if (!slot) { free(src); return; }
    free(*slot);
    *slot = src;
    p->state = 0;                // (a program linked again: a new copy)
}

// the name of the fragment shader's output at location 0
static int output_name(const char *s, char *name, size_t n)
{
    const char *best = NULL;
    for (const char *p = s; (p = strstr(p, "out ")); p += 4) {
        if (p > s && !(p[-1] == ' ' || p[-1] == '\n' || p[-1] == ')' || p[-1] == '\t' || (p >= s + 2 && !strncmp(p - 2, "in", 2)))) continue;
        const char *line = p;
        while (line > s && line[-1] != '\n') line--;
        const char *semi = strchr(p, ';');
        if (!semi || memchr(p, '(', (size_t)(semi - p))) continue;
        int loc0 = 0, located = 0;
        const char *lay = strstr(line, "location");
        if (lay && lay < p) { located = 1; const char *e = strchr(lay, '='); if (e && atoi(e + 1) == 0) loc0 = 1; }
        if (!strstr(p, "vec4") || strstr(p, "vec4") > semi) continue;
        if (loc0 || (!located && !best)) best = p;
        if (loc0) break;
    }
    if (!best) return 0;
    const char *semi = strchr(best, ';'), *e = semi;
    while (e > best && (e[-1] == ' ' || e[-1] == ']' || (e[-1] >= '0' && e[-1] <= '9') || e[-1] == '[')) {
        if (e[-1] == '[') { e--; break; }
        if (e[-1] == ' ') e--; else break;
    }
    const char *b = e;
    while (b > best && (b[-1] == '_' || (b[-1] >= 'a' && b[-1] <= 'z') || (b[-1] >= 'A' && b[-1] <= 'Z') || (b[-1] >= '0' && b[-1] <= '9'))) b--;
    if (e - b <= 0 || (size_t)(e - b) >= n) return 0;
    memcpy(name, b, (size_t)(e - b));
    name[e - b] = 0;
    return 1;
}

static const char g_tail[] =
    "layout(early_fragment_tests) in;\n"
    "layout(binding = 0, offset = 0) uniform atomic_uint ua_fill_n;\n"
    "layout(binding = 0, offset = 4) uniform atomic_uint ua_fill_kept;\n"
    "layout(binding = 0, offset = 8) uniform atomic_uint ua_fill_same;\n"
    "layout(binding = 0, offset = 12) uniform atomic_uint ua_fill_opaque;\n"
    "uniform ivec4 ua_fill_bf;\n"        // blend factors: src rgb, dst rgb, src alpha, dst alpha (codes below)
    "uniform ivec4 ua_fill_mask;\n"      // the colour channels written
    "uniform int ua_fill_mode;\n"        // 0 blending off, 1 on (add), 2 another equation
    "highp float ua_fill_f(int c, highp vec4 s, int i) {\n"
    "  if (c == 0) return 0.0; if (c == 1) return 1.0; if (c == 2) return s[i]; if (c == 3) return 1.0 - s[i];\n"
    "  if (c == 4) return s.a; if (c == 5) return 1.0 - s.a; return -1.0; }\n"
    "void main() {\n"
    "  atomicCounterIncrement(ua_fill_n);\n"
    "  ua_fill_main();\n"
    "  atomicCounterIncrement(ua_fill_kept);\n"
    "  highp vec4 s = clamp(vec4(%s), 0.0, 1.0);\n"
    "  bool same = ua_fill_mode == 1, cover = ua_fill_mode == 0;\n"
    "  if (ua_fill_mode == 1) {\n"
    "    cover = true;\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "      if (ua_fill_mask[i] == 0) continue;\n"
    "      int sc = i < 3 ? ua_fill_bf.x : ua_fill_bf.z, dc = i < 3 ? ua_fill_bf.y : ua_fill_bf.w;\n"
    "      highp float sf = ua_fill_f(sc, s, i), df = ua_fill_f(dc, s, i);\n"
    "      highp float term = sf < 0.0 ? s[i] : s[i] * sf;\n"
    "      if (!(abs(term) < 0.5 / 255.0 && df >= 0.0 && abs(df - 1.0) < 0.5 / 255.0)) same = false;\n"
    "      if (!(df >= 0.0 && df < 0.5 / 255.0 && sf >= 0.0)) cover = false;\n"
    "    }\n"
    "  }\n"
    "  if (same) atomicCounterIncrement(ua_fill_same);\n"
    "  if (cover && ua_fill_mode == 1) atomicCounterIncrement(ua_fill_opaque);\n"
    "  %s = vec4(1.0 / 255.0, 0.0, 0.0, cover ? 1.0 : 0.0);\n"
    "}\n";

static char *fill_source(const char *fs)
{
    char out[64];
    // (GLSL ES 3.00 has no atomic counters: 3.10 is the same language with them; 3.10 and 3.20 stay)
    const char *v = strstr(fs, "#version 3");
    int ver = v ? atoi(v + 9) : 0;
    const char *m = strstr(fs, "void main()");
    if (!v || ver < 300 || ver > 320 || !strstr(v, " es") || !m || !output_name(fs, out, sizeof out)) return NULL;
    size_t n = strlen(fs) + sizeof g_tail + 256;
    char *s = malloc(n);
    if (!s) return NULL;
    size_t o = 0;
    memcpy(s, fs, (size_t)(v - fs)); o = (size_t)(v - fs);
    o += (size_t)sprintf(s + o, "#version %d es", ver < 310 ? 310 : ver);
    const char *rest = v + strlen("#version 300 es");
    memcpy(s + o, rest, (size_t)(m - rest)); o += (size_t)(m - rest);
    o += (size_t)sprintf(s + o, "void ua_fill_main()");
    const char *after = m + strlen("void main()");
    size_t al = strlen(after);
    memcpy(s + o, after, al); o += al;
    o += (size_t)snprintf(s + o, n - o, "\n");
    o += (size_t)snprintf(s + o, n - o, g_tail, out, out);
    return s;
}

static GLuint compile(GLenum type, const char *src, int *ok, char *log, size_t logn)
{
    GLuint sh = gCreateShader(type);
    gShaderSource(sh, 1, &src, NULL);
    gCompileShader(sh);
    GLint st = 0;
    gGetShaderiv(sh, GL_COMPILE_STATUS, &st);
    *ok = st != 0;
    if (!st && log) { GLsizei l = 0; gGetShaderInfoLog(sh, (GLsizei)logn, &l, log); }
    return sh;
}

static long g_failed_logged;

static int make(GLuint orig, prog_t *p)
{
    p->state = -1;
    if (!p->vs || !p->fs) return 0;
    char *fs = fill_source(p->fs);
    if (!fs) { if (g_failed_logged++ < 5) fprintf(stderr, "[fill] program %u: no #version 3x0 es / main / vec4 output\n", orig); return 0; }
    char log[400] = "";
    int ok1 = 0, ok2 = 0;
    GLuint vs = compile(GL_VERTEX_SHADER, p->vs, &ok1, log, sizeof log);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs, &ok2, log, sizeof log);
    free(fs);
    if (!ok1 || !ok2) {
        if (g_failed_logged++ < 5) fprintf(stderr, "[fill] program %u: its counting copy does not compile: %.300s\n", orig, log);
        gDeleteShader(vs); gDeleteShader(f);
        return 0;
    }
    GLuint q = gCreateProgram();
    gAttachShader(q, vs); gAttachShader(q, f);
    GLint na = 0;
    gGetProgramiv(orig, GL_ACTIVE_ATTRIBUTES, &na);
    for (GLint i = 0; i < na; i++) {
        char name[128]; GLsizei l = 0; GLint sz = 0; GLenum t = 0;
        gGetActiveAttrib(orig, (GLuint)i, sizeof name, &l, &sz, &t, name);
        GLint loc = gGetAttribLocation(orig, name);
        if (loc >= 0) gBindAttribLocation(q, (GLuint)loc, name);
    }
    gLinkProgram(q);
    gDeleteShader(vs); gDeleteShader(f);
    GLint st = 0;
    gGetProgramiv(q, GL_LINK_STATUS, &st);
    if (!st) {
        GLsizei l = 0; gGetProgramInfoLog(q, sizeof log, &l, log);
        if (g_failed_logged++ < 5) fprintf(stderr, "[fill] program %u: its counting copy does not link: %.300s\n", orig, log);
        return 0;
    }
    p->prog = q;
    p->l_bf = gGetUniformLocation(q, "ua_fill_bf");
    p->l_mask = gGetUniformLocation(q, "ua_fill_mask");
    p->l_mode = gGetUniformLocation(q, "ua_fill_mode");
    // the game's uniforms outside blocks, element by element
    GLint nu = 0;
    gGetProgramiv(orig, GL_ACTIVE_UNIFORMS, &nu);
    int cap = 0;
    for (GLint i = 0; i < nu; i++) {
        char name[160]; GLsizei l = 0; GLint sz = 0; GLenum t = 0; GLuint idx = (GLuint)i; GLint blk = -1;
        gGetActiveUniform(orig, idx, sizeof name - 8, &l, &sz, &t, name);
        gGetActiveUniformsiv(orig, 1, &idx, GL_UNIFORM_BLOCK_INDEX, &blk);
        if (blk >= 0 || !strncmp(name, "gl_", 3)) continue;
        char *br = strchr(name, '[');
        if (br) *br = 0;
        for (GLint e = 0; e < sz; e++) {
            char el[176];
            if (sz > 1) snprintf(el, sizeof el, "%s[%d]", name, e); else snprintf(el, sizeof el, "%s", name);
            GLint lo = gGetUniformLocation(orig, el), lm = gGetUniformLocation(q, el);
            if (lo < 0 || lm < 0) continue;
            if (p->nu == cap) { cap = cap ? cap * 2 : 16; p->u = realloc(p->u, (size_t)cap * sizeof *p->u); }
            p->u[p->nu] = (uni_t){ lo, lm, t, 1, "", { 0, 0, 0, 0 } };
            snprintf(p->u[p->nu].name, sizeof p->u[p->nu].name, "%.47s", el);
            p->nu++;
        }
    }
    GLint nb = 0;
    gGetProgramiv(orig, GL_ACTIVE_UNIFORM_BLOCKS, &nb);
    if (nb > 0) { p->blk_orig = calloc((size_t)nb, sizeof(GLuint)); p->blk_mine = calloc((size_t)nb, sizeof(GLuint)); }
    for (GLint i = 0; i < nb && p->blk_orig; i++) {
        char name[160]; GLsizei l = 0;
        gGetActiveUniformBlockName(orig, (GLuint)i, sizeof name, &l, name);
        GLuint m = gGetUniformBlockIndex(q, name);
        if (m == 0xFFFFFFFFu) continue;
        p->blk_orig[p->nb] = (GLuint)i; p->blk_mine[p->nb] = m; p->nb++;
    }
    drain();
    p->state = 1;
    return 1;
}

static void copy_uniforms(GLuint orig, prog_t *p)
{
    for (int i = 0; i < p->nu; i++) {
        uni_t *u = &p->u[i];
        float f[16]; GLint iv[4]; GLuint uv[4];
        switch (u->type) {
        case T_F: gGetUniformfv(orig, u->orig, f); gPU1fv(p->prog, u->mine, 1, f); u->last[0] = f[0]; break;
        case T_V2: gGetUniformfv(orig, u->orig, f); gPU2fv(p->prog, u->mine, 1, f); break;
        case T_V3: gGetUniformfv(orig, u->orig, f); gPU3fv(p->prog, u->mine, 1, f); break;
        case T_V4: gGetUniformfv(orig, u->orig, f); gPU4fv(p->prog, u->mine, 1, f); memcpy(u->last, f, sizeof u->last); break;
        case T_M2: gGetUniformfv(orig, u->orig, f); gPUM2(p->prog, u->mine, 1, 0, f); break;
        case T_M3: gGetUniformfv(orig, u->orig, f); gPUM3(p->prog, u->mine, 1, 0, f); break;
        case T_M4: gGetUniformfv(orig, u->orig, f); gPUM4(p->prog, u->mine, 1, 0, f); break;
        case T_M23: gGetUniformfv(orig, u->orig, f); gPUM23(p->prog, u->mine, 1, 0, f); break;
        case T_M24: gGetUniformfv(orig, u->orig, f); gPUM24(p->prog, u->mine, 1, 0, f); break;
        case T_M32: gGetUniformfv(orig, u->orig, f); gPUM32(p->prog, u->mine, 1, 0, f); break;
        case T_M34: gGetUniformfv(orig, u->orig, f); gPUM34(p->prog, u->mine, 1, 0, f); break;
        case T_M42: gGetUniformfv(orig, u->orig, f); gPUM42(p->prog, u->mine, 1, 0, f); break;
        case T_M43: gGetUniformfv(orig, u->orig, f); gPUM43(p->prog, u->mine, 1, 0, f); break;
        case T_IV2: case T_BV2: gGetUniformiv(orig, u->orig, iv); gPU2iv(p->prog, u->mine, 1, iv); break;
        case T_IV3: case T_BV3: gGetUniformiv(orig, u->orig, iv); gPU3iv(p->prog, u->mine, 1, iv); break;
        case T_IV4: case T_BV4: gGetUniformiv(orig, u->orig, iv); gPU4iv(p->prog, u->mine, 1, iv); break;
        case T_U: gGetUniformuiv(orig, u->orig, uv); gPU1uiv(p->prog, u->mine, 1, uv); break;
        case T_UV2: gGetUniformuiv(orig, u->orig, uv); gPU2uiv(p->prog, u->mine, 1, uv); break;
        case T_UV3: gGetUniformuiv(orig, u->orig, uv); gPU3uiv(p->prog, u->mine, 1, uv); break;
        case T_UV4: gGetUniformuiv(orig, u->orig, uv); gPU4uiv(p->prog, u->mine, 1, uv); break;
        default: gGetUniformiv(orig, u->orig, iv); gPU1iv(p->prog, u->mine, 1, iv); break;   // int, bool, samplers
        }
    }
    for (int i = 0; i < p->nb; i++) {
        GLint b = 0;
        gGetActiveUniformBlockiv(orig, p->blk_orig[i], GL_UNIFORM_BLOCK_BINDING, &b);
        gUniformBlockBinding(p->prog, p->blk_mine[i], (GLuint)b);
    }
}

// ---- targets: a shadow per colour buffer the frame draws into
#define MAX_TGT 64
typedef struct {
    GLint type, name;            // the game's colour buffer 0 (type 0: the window)
    GLuint tex; int w, h;        // the shadow
    int dirty;                   // fragments since its last read
    long frags, visible, unchanged, discarded, opaque, uses;
} tgt_t;
static tgt_t g_tgt[MAX_TGT];
static int g_ntgt;
static GLuint g_fbo, g_counters;
static unsigned char *g_pix;
static size_t g_pixn;
static long g_draws, g_skipped, g_skip_prog, g_skip_fb, g_frame_no;

static void flush(tgt_t *t, int clear);
static tgt_t *target(GLint type, GLint name, int w, int h)
{
    tgt_t *t = NULL;
    for (int i = 0; i < g_ntgt; i++) if (g_tgt[i].type == type && g_tgt[i].name == name) { t = &g_tgt[i]; break; }
    if (!t) {
        if (g_ntgt == MAX_TGT) return NULL;
        t = &g_tgt[g_ntgt++];
        memset(t, 0, sizeof *t);
        t->type = type; t->name = name;
    }
    if (!t->tex || w > t->w || h > t->h) {
        if (t->tex && t->dirty && t->visible >= 0) flush(t, 0);     // (what it counted so far)
        GLint unit_tex = 0;
        gGetIntegerv(GL_TEXTURE_BINDING_2D, &unit_tex);
        if (t->tex) gDeleteTextures(1, &t->tex);
        t->w = w > t->w ? w : t->w; t->h = h > t->h ? h : t->h;
        gGenTextures(1, &t->tex);
        gBindTexture(GL_TEXTURE_2D, t->tex);
        gTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, t->w, t->h);
        gBindTexture(GL_TEXTURE_2D, (GLuint)unit_tex);
        t->dirty = 1;            // (cleared before its first draw below)
        t->visible = -1;
    }
    return t;
}

// the shadow's per-pixel counts added up: the fragments not covered since; then it starts over
static void flush(tgt_t *t, int clear)
{
    if (!t->tex || !t->dirty) return;
    size_t n = (size_t)t->w * t->h * 4;
    if (t->visible >= 0) {
        if (n > g_pixn) { free(g_pix); g_pix = malloc(n); g_pixn = g_pix ? n : 0; }
        if (!g_pix) return;
        GLint rfb = 0, pbo = 0, al = 4, rl = 0;
        gGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
        gGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
        gGetIntegerv(GL_PACK_ALIGNMENT, &al);
        gGetIntegerv(GL_PACK_ROW_LENGTH, &rl);
        gBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
        gFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
        if (pbo) gBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        gPixelStorei(GL_PACK_ALIGNMENT, 4); gPixelStorei(GL_PACK_ROW_LENGTH, 0);
        gReadPixels(0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, g_pix);
        long s = 0;
        for (size_t i = 0; i < n; i += 4) s += g_pix[i];
        t->visible += s;
        gPixelStorei(GL_PACK_ALIGNMENT, al); gPixelStorei(GL_PACK_ROW_LENGTH, rl);
        if (pbo) gBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pbo);
        gBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    } else t->visible = 0;
    if (clear) {
        GLint dfb = 0;
        gGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
        GLboolean cm[4]; gGetBooleanv(GL_COLOR_WRITEMASK, cm);
        GLboolean sc = gIsEnabled(GL_SCISSOR_TEST);
        gBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo);
        gFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
        gFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        gColorMask(1, 1, 1, 1);
        if (sc) gDisable(GL_SCISSOR_TEST);
        static const float zero[4] = { 0, 0, 0, 0 };
        gClearBufferfv(GL_COLOR, 0, zero);
        if (sc) gEnable(GL_SCISSOR_TEST);
        gColorMask(cm[0], cm[1], cm[2], cm[3]);
        gBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dfb);
    }
    t->dirty = 0;
    drain();
}

// the draw framebuffer's colour buffer 0: its target (NULL: not counted)
static tgt_t *current_target(unsigned fb, GLint vp[4], GLint *dtype, GLint *dname, GLint *dlevel, GLint *stype, GLint *sname, GLint *slevel)
{
    GLint type = 0, name = 0;
    *dtype = *dname = *stype = *sname = 0; *dlevel = *slevel = 0;
    gGetIntegerv(GL_VIEWPORT, vp);
    int w = vp[0] + vp[2], h = vp[1] + vp[3];
    if (fb) {
        gGetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        if (!type) { drain(); return NULL; }
        gGetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
        gGetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, dtype);
        if (*dtype) {
            gGetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, dname);
            if (*dtype == GL_TEXTURE) gGetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, dlevel);
        }
        gGetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, stype);
        if (*stype) {
            gGetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, sname);
            if (*stype == GL_TEXTURE) gGetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, slevel);
        }
        drain();
    }
    if (w <= 0 || h <= 0) return NULL;
    return target(type, name, w, h);
}

static void attach(GLenum att, GLint type, GLint name, GLint level)
{
    if (type == GL_RENDERBUFFER) gFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, att, GL_RENDERBUFFER, (GLuint)name);
    else if (type == GL_TEXTURE) gFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att, GL_TEXTURE_2D, (GLuint)name, level);
    else gFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, att, GL_RENDERBUFFER, 0);
}

static int code(GLint f)
{
    switch (f) {
    case GL_ZERO: return 0; case GL_ONE: return 1; case GL_SRC_COLOR: return 2; case GL_ONE_MINUS_SRC_COLOR: return 3;
    case GL_SRC_ALPHA: return 4; case GL_ONE_MINUS_SRC_ALPHA: return 5; default: return 6;
    }
}

// the state the counting draw changes, kept to be put back
static struct {
    GLint dfb, ac_generic, ac0, src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a, sw, sbw;
    GLboolean blend, depth_test, stencil_test, cm[4], dw;
    prog_t *p; tgt_t *t; GLuint orig; int window;
} g_s;

int glfill_begin(unsigned fb)
{
    if (!g_on) return 0;
    g_draws++;
    GLint prog = 0;
    gGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    prog_t *p = prog > 0 && prog < MAX_PROG ? g_prog[prog] : NULL;
    if (p && p->state == 0) make((GLuint)prog, p);
    if (!p || p->state != 1) { g_skipped++; g_skip_prog++; return 0; }
    GLint vp[4], dt, dn, dl, st, sn, sl;
    tgt_t *t = current_target(fb, vp, &dt, &dn, &dl, &st, &sn, &sl);
    if (!t) { g_skipped++; g_skip_fb++; return 0; }
    if (t->dirty && t->visible < 0) flush(t, 1);         // (a new shadow: zeroed)
    g_s.p = p; g_s.t = t; g_s.orig = (GLuint)prog; g_s.window = fb == 0;
    gGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &g_s.dfb);
    g_s.blend = gIsEnabled(GL_BLEND);
    gGetIntegerv(GL_BLEND_SRC_RGB, &g_s.src_rgb); gGetIntegerv(GL_BLEND_DST_RGB, &g_s.dst_rgb);
    gGetIntegerv(GL_BLEND_SRC_ALPHA, &g_s.src_a); gGetIntegerv(GL_BLEND_DST_ALPHA, &g_s.dst_a);
    gGetIntegerv(GL_BLEND_EQUATION_RGB, &g_s.eq_rgb); gGetIntegerv(GL_BLEND_EQUATION_ALPHA, &g_s.eq_a);
    gGetBooleanv(GL_COLOR_WRITEMASK, g_s.cm);
    gGetBooleanv(GL_DEPTH_WRITEMASK, &g_s.dw);
    gGetIntegerv(GL_STENCIL_WRITEMASK, &g_s.sw); gGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &g_s.sbw);
    g_s.depth_test = gIsEnabled(GL_DEPTH_TEST); g_s.stencil_test = gIsEnabled(GL_STENCIL_TEST);
    gGetIntegerv(GL_ATOMIC_COUNTER_BUFFER_BINDING, &g_s.ac_generic);
    gGetIntegeri_v(GL_ATOMIC_COUNTER_BUFFER_BINDING, 0, &g_s.ac0);
    // the counting copy, its uniforms, the shadow with the game's depth and stencil buffers
    copy_uniforms((GLuint)prog, p);
    int mode = !g_s.blend ? 0 : g_s.eq_rgb == GL_FUNC_ADD && g_s.eq_a == GL_FUNC_ADD ? 1 : 2;
    gProgramUniform4i(p->prog, p->l_bf, code(g_s.src_rgb), code(g_s.dst_rgb), code(g_s.src_a), code(g_s.dst_a));
    gProgramUniform4i(p->prog, p->l_mask, g_s.cm[0], g_s.cm[1], g_s.cm[2], g_s.cm[3]);
    gProgramUniform1i(p->prog, p->l_mode, mode);
    gBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo);
    gFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    if (dt && dt == st && dn == sn) { attach(GL_DEPTH_STENCIL_ATTACHMENT, dt, dn, dl); }
    else { attach(GL_DEPTH_STENCIL_ATTACHMENT, 0, 0, 0); attach(GL_DEPTH_ATTACHMENT, dt, dn, dl); attach(GL_STENCIL_ATTACHMENT, st, sn, sl); }
    if (gCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        gBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)g_s.dfb);
        drain();
        g_skipped++; g_skip_fb++;
        return 0;
    }
    if (g_s.window) { gDisable(GL_DEPTH_TEST); gDisable(GL_STENCIL_TEST); }   // (the window's buffers cannot be shared)
    gUseProgram(p->prog);
    static const unsigned zero[4] = { 0, 0, 0, 0 };
    gBindBuffer(GL_ATOMIC_COUNTER_BUFFER, g_counters);
    gBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof zero, zero);
    gBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, g_counters);
    gColorMask(1, 1, 1, 1);
    gDepthMask(0);
    gStencilMaskSeparate(0x0404 /* GL_FRONT */, 0); gStencilMaskSeparate(0x0405 /* GL_BACK */, 0);
    gEnable(GL_BLEND);
    gBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    gBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);   // r: 1 + r * (1 - covers)
    return 1;
}

void glfill_end(void)
{
    prog_t *p = g_s.p; tgt_t *t = g_s.t;
    gMemoryBarrier(GL_ALL_BARRIER_BITS);
    unsigned c[4] = { 0, 0, 0, 0 };
    const unsigned *m = gMapBufferRange(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof c, GL_MAP_READ_BIT);
    if (m) { memcpy(c, m, sizeof c); gUnmapBuffer(GL_ATOMIC_COUNTER_BUFFER); }
    // put back what the game had
    gUseProgram(g_s.orig);
    gBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)g_s.dfb);
    gBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, (GLuint)g_s.ac0);
    gBindBuffer(GL_ATOMIC_COUNTER_BUFFER, (GLuint)g_s.ac_generic);
    gColorMask(g_s.cm[0], g_s.cm[1], g_s.cm[2], g_s.cm[3]);
    gDepthMask(g_s.dw);
    gStencilMaskSeparate(0x0404, (GLuint)g_s.sw); gStencilMaskSeparate(0x0405, (GLuint)g_s.sbw);
    if (!g_s.blend) gDisable(GL_BLEND);
    gBlendEquationSeparate((GLenum)g_s.eq_rgb, (GLenum)g_s.eq_a);
    gBlendFuncSeparate((GLenum)g_s.src_rgb, (GLenum)g_s.dst_rgb, (GLenum)g_s.src_a, (GLenum)g_s.dst_a);
    if (g_s.window) { if (g_s.depth_test) gEnable(GL_DEPTH_TEST); if (g_s.stencil_test) gEnable(GL_STENCIL_TEST); }
    drain();
    long n = c[0], kept = c[1];
    p->draws++; p->frags += n; p->discarded += n - kept; p->unchanged += c[2]; p->opaque += c[3];
    t->frags += n; t->discarded += n - kept; t->unchanged += c[2]; t->opaque += c[3]; t->uses++;
    t->dirty = 1;
}

// a whole write of the draw framebuffer's colour buffer (a clear, an invalidation, a blit into it):
// what its shadow counted is either seen by now or covered
void glfill_covered(unsigned fb)
{
    if (!g_on) return;
    GLint vp[4], dt, dn, dl, st, sn, sl;
    gGetIntegerv(GL_VIEWPORT, vp);
    GLint type = 0, name = 0;
    if (fb) {
        gGetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        if (type) gGetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
        drain();
    }
    (void)dt; (void)dn; (void)dl; (void)st; (void)sn; (void)sl;
    for (int i = 0; i < g_ntgt; i++)
        if (g_tgt[i].type == type && g_tgt[i].name == name) { flush(&g_tgt[i], 1); return; }
}

static int pct(long a, long b) { return b > 0 ? (int)((double)a * 1000 / (double)b) : 0; }   // (32-bit long: no a * 1000)
#define P(a, b) pct(a, b) / 10, pct(a, b) % 10

void glfill_frame_start(void)
{
    if (!g_on) return;
    g_draws = g_skipped = g_skip_prog = g_skip_fb = 0;
    for (int i = 0; i < g_ntgt; i++) {
        tgt_t *t = &g_tgt[i];
        t->frags = t->unchanged = t->discarded = t->opaque = t->uses = 0;
        t->dirty = 1; t->visible = -1;               // zeroed before its first draw
    }
    for (int i = 0; i < MAX_PROG; i++) if (g_prog[i]) g_prog[i]->draws = g_prog[i]->frags = g_prog[i]->discarded = g_prog[i]->unchanged = g_prog[i]->opaque = 0;
}

static uint64_t fnv(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h;
}

void glfill_frame_end(long frame)
{
    if (!g_on) return;
    g_frame_no = frame;
    long frags = 0, visible = 0, unchanged = 0, discarded = 0, opaque = 0, px = 0, win_px = 0;
    for (int i = 0; i < g_ntgt; i++) {
        tgt_t *t = &g_tgt[i];
        if (!t->uses) continue;
        flush(t, 1);
        frags += t->frags; visible += t->visible; unchanged += t->unchanged; discarded += t->discarded; opaque += t->opaque;
        px += (long)t->w * t->h;
        if ((long)t->w * t->h > win_px) win_px = (long)t->w * t->h;
    }
    long kept = frags - discarded, covered = kept - visible;
    fprintf(stderr, "[fill] frame %ld: %ld fragments (%.1f per pixel of the %ld-pixel frame), discarded %d.%d%%, unchanged %d.%d%%, "
                    "covered later %d.%d%%, opaque under blending %d.%d%%; %ld of %ld draws not counted (%ld program, %ld target)\n",
            frame, frags, win_px ? (double)frags / win_px : 0, win_px, P(discarded, frags), P(unchanged, frags), P(covered, frags),
            P(opaque, frags), g_skipped, g_draws, g_skip_prog, g_skip_fb);
    for (int i = 0; i < g_ntgt; i++) {
        tgt_t *t = &g_tgt[i];
        if (!t->uses) continue;
        long k = t->frags - t->discarded;
        fprintf(stderr, "[fill]   target %s%d %dx%d: %ld draws, %ld fragments (%.1f per pixel), discarded %d.%d%%, unchanged %d.%d%%, "
                        "covered later %d.%d%%, opaque under blending %d.%d%%\n",
                t->type == GL_RENDERBUFFER ? "rb" : t->type ? "tex" : "window", t->name, t->w, t->h, t->uses, t->frags,
                (double)t->frags / ((long)t->w * t->h), P(t->discarded, t->frags), P(t->unchanged, t->frags),
                P(k - t->visible, t->frags), P(t->opaque, t->frags));
    }
    // the programs that shade the most
    for (int r = 0; r < 12; r++) {
        int best = -1;
        for (int i = 0; i < MAX_PROG; i++)
            if (g_prog[i] && g_prog[i]->frags > 0 && (best < 0 || g_prog[i]->frags > g_prog[best]->frags)) best = i;
        if (best < 0) break;
        prog_t *p = g_prog[best];
        fprintf(stderr, "[fill]   program %d fs %016llx: %ld draws, %ld fragments (%d.%d%%), discarded %d.%d%%, unchanged %d.%d%%, "
                        "opaque under blending %d.%d%%\n", best, (unsigned long long)fnv(p->fs), p->draws, p->frags, P(p->frags, frags),
                P(p->discarded, p->frags), P(p->unchanged, p->frags), P(p->opaque, p->frags));
        if (r < 3) {                 // the vec4 and float uniforms outside blocks at its last draw
            char v[1200]; size_t o = 0; v[0] = 0;
            for (int i = 0; i < p->nu && o + 80 < sizeof v; i++)
                if ((p->u[i].type == T_V4 || p->u[i].type == T_F) && !strstr(p->u[i].name, "mtx"))
                    o += (size_t)snprintf(v + o, sizeof v - o, " %s=(%g,%g,%g,%g)", p->u[i].name, p->u[i].last[0], p->u[i].last[1],
                                          p->u[i].last[2], p->u[i].last[3]);
            if (o) fprintf(stderr, "[fill]     uniforms:%s\n", v);
        }
        p->frags = -p->frags;        // (listed)
    }
    for (int i = 0; i < MAX_PROG; i++) if (g_prog[i] && g_prog[i]->frags < 0) g_prog[i]->frags = -g_prog[i]->frags;
    (void)px;
}

// at the first traced frame (the render thread, its context current)
static int g_ready;
static void ready(void)
{
    if (g_ready) return;
    g_ready = 1;
    gGenFramebuffers(1, &g_fbo);
    gGenBuffers(1, &g_counters);
    GLint ac = 0;
    gGetIntegerv(GL_ATOMIC_COUNTER_BUFFER_BINDING, &ac);
    gBindBuffer(GL_ATOMIC_COUNTER_BUFFER, g_counters);
    gBufferData(GL_ATOMIC_COUNTER_BUFFER, 16, NULL, GL_DYNAMIC_COPY);
    gBindBuffer(GL_ATOMIC_COUNTER_BUFFER, (GLuint)ac);
    drain();
}
void glfill_ready(void) { if (g_on) ready(); }

int glfill_init(void)
{
    const char *e = getenv("UA_GL_TRACE_FILL");
    if (!e || strcmp(e, "1")) return 0;
#define R(var, name) if (!(*(void **)&var = real(name))) { fprintf(stderr, "[fill] no %s: off\n", name); return 0; }
    R(gGetIntegerv, "glGetIntegerv"); R(gGetIntegeri_v, "glGetIntegeri_v"); R(gIsEnabled, "glIsEnabled"); R(gEnable, "glEnable");
    R(gDisable, "glDisable"); R(gGetBooleanv, "glGetBooleanv"); R(gColorMask, "glColorMask"); R(gDepthMask, "glDepthMask");
    R(gStencilMaskSeparate, "glStencilMaskSeparate"); R(gBlendFuncSeparate, "glBlendFuncSeparate");
    R(gBlendEquationSeparate, "glBlendEquationSeparate"); R(gUseProgram, "glUseProgram"); R(gBindFramebuffer, "glBindFramebuffer");
    R(gGenFramebuffers, "glGenFramebuffers"); R(gFramebufferTexture2D, "glFramebufferTexture2D");
    R(gFramebufferRenderbuffer, "glFramebufferRenderbuffer"); R(gCheckFramebufferStatus, "glCheckFramebufferStatus");
    R(gGetFbAttachment, "glGetFramebufferAttachmentParameteriv"); R(gGenTextures, "glGenTextures"); R(gBindTexture, "glBindTexture");
    R(gTexStorage2D, "glTexStorage2D"); R(gDeleteTextures, "glDeleteTextures"); R(gCreateShader, "glCreateShader");
    R(gShaderSource, "glShaderSource"); R(gCompileShader, "glCompileShader"); R(gGetShaderiv, "glGetShaderiv");
    R(gGetShaderInfoLog, "glGetShaderInfoLog"); R(gGetShaderSource, "glGetShaderSource"); R(gCreateProgram, "glCreateProgram");
    R(gAttachShader, "glAttachShader"); R(gBindAttribLocation, "glBindAttribLocation"); R(gLinkProgram, "glLinkProgram");
    R(gGetProgramiv, "glGetProgramiv"); R(gGetProgramInfoLog, "glGetProgramInfoLog"); R(gDeleteShader, "glDeleteShader");
    R(gGetActiveAttrib, "glGetActiveAttrib"); R(gGetAttribLocation, "glGetAttribLocation"); R(gGetActiveUniform, "glGetActiveUniform");
    R(gGetActiveUniformsiv, "glGetActiveUniformsiv"); R(gGetUniformLocation, "glGetUniformLocation");
    R(gGetUniformfv, "glGetUniformfv"); R(gGetUniformiv, "glGetUniformiv"); R(gGetUniformuiv, "glGetUniformuiv");
    R(gGetActiveUniformBlockiv, "glGetActiveUniformBlockiv"); R(gGetActiveUniformBlockName, "glGetActiveUniformBlockName");
    R(gGetUniformBlockIndex, "glGetUniformBlockIndex"); R(gUniformBlockBinding, "glUniformBlockBinding");
    R(gGenBuffers, "glGenBuffers"); R(gBindBuffer, "glBindBuffer"); R(gBindBufferBase, "glBindBufferBase");
    R(gBufferData, "glBufferData"); R(gBufferSubData, "glBufferSubData"); R(gMapBufferRange, "glMapBufferRange");
    R(gUnmapBuffer, "glUnmapBuffer"); R(gMemoryBarrier, "glMemoryBarrier"); R(gReadPixels, "glReadPixels");
    R(gPixelStorei, "glPixelStorei"); R(gClearBufferfv, "glClearBufferfv"); R(gProgramUniform1i, "glProgramUniform1i");
    R(gProgramUniform4i, "glProgramUniform4i"); R(gPU1fv, "glProgramUniform1fv"); R(gPU2fv, "glProgramUniform2fv");
    R(gPU3fv, "glProgramUniform3fv"); R(gPU4fv, "glProgramUniform4fv"); R(gPU1iv, "glProgramUniform1iv");
    R(gPU2iv, "glProgramUniform2iv"); R(gPU3iv, "glProgramUniform3iv"); R(gPU4iv, "glProgramUniform4iv");
    R(gPU1uiv, "glProgramUniform1uiv"); R(gPU2uiv, "glProgramUniform2uiv"); R(gPU3uiv, "glProgramUniform3uiv");
    R(gPU4uiv, "glProgramUniform4uiv"); R(gPUM2, "glProgramUniformMatrix2fv"); R(gPUM3, "glProgramUniformMatrix3fv");
    R(gPUM4, "glProgramUniformMatrix4fv"); R(gPUM23, "glProgramUniformMatrix2x3fv"); R(gPUM24, "glProgramUniformMatrix2x4fv");
    R(gPUM32, "glProgramUniformMatrix3x2fv"); R(gPUM34, "glProgramUniformMatrix3x4fv"); R(gPUM42, "glProgramUniformMatrix4x2fv");
    R(gPUM43, "glProgramUniformMatrix4x3fv"); R(gGetError, "glGetError");
#undef R
    g_prog = calloc(MAX_PROG, sizeof *g_prog);
    if (!g_prog) return 0;
    g_on = 1;
    fprintf(stderr, "[fill] counting each traced frame's fragments (UA_GL_TRACE_FILL)\n");
    return 1;
}
