// GL frame tracer: what one frame asks of the GPU, pass by pass (any Unity GLES game).
//
//   UA_GL_TRACE=<s>   every <s> seconds one whole frame (swap to swap) is recorded and logged:
//     [gl] frame <n>: <passes> passes, <draws> draws, <verts> vertices, <programs> program switches;
//          uploads: textures <KB>, buffers <KB>
//     [gl]   fb<id> <w>x<h> clear=<C|D|S> draws <n> verts <n> inval=<start|end>:<C|D|S> blit/read
//   A pass begins at glBindFramebuffer; its size is the viewport set for it. "clear" is a glClear
//   before the pass's first draw (the old contents need not be loaded), "inval" a
//   glInvalidateFramebuffer/glDiscardFramebufferEXT (contents need not be stored, or loaded).
//   On a tiler like the Mali every pass costs a store of its tiles, plus a load unless cleared or
//   invalidated first: the log shows which full-screen passes a frame could do without.
//
//   UA_GL_TRACE_DRAWS=<n>   in each traced frame also log its first <n> draws, clears and blits
//   one by one, as they happen (what draws a wrong picture: which program, into what, from what):
//     [gl]   fb<id> = tex<t> <w>x<h> <format>        (a pass's colour buffer, once per pass)
//     [gl]   #<i> draw fb<id> prog<p> vs <hash> fs <hash> <verts>[x<instances>] <sampler>=<unit>:tex<t> ...
//                -> <centre pixel> <pixel at 1/4,1/4>
//     [gl]   #<i> clear fb<id> <C|D|S> colour (<r>,<g>,<b>,<a>) -> <the two pixels>
//     [gl]   #<i> blit fb<r> -> fb<d> (<x0>,<y0>,<x1>,<y1>) -> (<x0>,<y0>,<x1>,<y1>) <C|D|S> -> <the two pixels>
//   The pixels are read back from the pass's colour buffer 0 after the call, in the viewport
//   (floats for float buffers; "ms" for a multisampled one, "-" for none or an integer one), and
//   Unity's bindings put back. <hash> is the FNV-1a 64 of the shader's source, the name
//   UA_SHADER_DUMP gives its file: run with UA_SHADER_DUMP (which also turns Unity's program
//   binary cache off: a program loaded from that cache has no shaders, "vs ?"; with
//   UA_SHADER_OVERRIDE, the hash of the source compiled). Each read waits for the GPU: a traced
//   frame takes much longer.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gltrace.h"

#define GL_FRAMEBUFFER 0x8D40
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_COLOR 0x1800
#define GL_DEPTH 0x1801
#define GL_STENCIL 0x1802

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei;
typedef unsigned GLbitfield; typedef intptr_t GLintptr; typedef intptr_t GLsizeiptr;

#define MAX_PASS 256
typedef struct {
    GLuint fb; int w, h; unsigned clear, inval_start, inval_end; int draws, blits, reads; long verts;
} pass_t;

static void size_of(pass_t *p);
static double g_every;
static double g_next;
static int g_on;                 // recording this frame
static long g_frame;
static pass_t g_pass[MAX_PASS];
static int g_npass, g_overflow;
static long g_draws, g_verts, g_programs, g_tex_bytes, g_buf_bytes;
static GLuint g_cur_fb;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static pass_t *cur(void)
{
    if (g_npass == 0) { g_pass[0] = (pass_t){ .fb = g_cur_fb }; g_npass = 1; }
    return &g_pass[g_npass - 1];
}

static void new_pass(GLuint fb)
{
    if (g_npass && g_pass[g_npass - 1].draws == 0 && g_pass[g_npass - 1].blits == 0 && !g_pass[g_npass - 1].clear) {
        g_pass[g_npass - 1].fb = fb;            // nothing happened in the last one: reuse it
        return;
    }
    if (g_npass >= MAX_PASS) { g_overflow++; return; }
    g_pass[g_npass++] = (pass_t){ .fb = fb };
}

static unsigned att_bits(GLsizei n, const GLenum *a)
{
    unsigned b = 0;
    for (int i = 0; a && i < n; i++) {
        if (a[i] == GL_COLOR || (a[i] >= GL_COLOR_ATTACHMENT0 && a[i] < GL_COLOR_ATTACHMENT0 + 16)) b |= GL_COLOR_BUFFER_BIT;
        if (a[i] == GL_DEPTH || a[i] == GL_DEPTH_ATTACHMENT || a[i] == GL_DEPTH_STENCIL_ATTACHMENT) b |= GL_DEPTH_BUFFER_BIT;
        if (a[i] == GL_STENCIL || a[i] == GL_STENCIL_ATTACHMENT || a[i] == GL_DEPTH_STENCIL_ATTACHMENT) b |= GL_STENCIL_BUFFER_BIT;
    }
    return b;
}

static const char *bits(unsigned b, char *s)
{
    char *p = s;
    if (b & GL_COLOR_BUFFER_BIT) *p++ = 'C';
    if (b & GL_DEPTH_BUFFER_BIT) *p++ = 'D';
    if (b & GL_STENCIL_BUFFER_BIT) *p++ = 'S';
    if (p == s) *p++ = '-';
    *p = 0;
    return s;
}

// ---- UA_GL_TRACE_DRAWS: draws, clears and blits one by one
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_BINDING_3D 0x806A
#define GL_TEXTURE_BINDING_CUBE_MAP 0x8514
#define GL_TEXTURE_BINDING_2D_ARRAY 0x8C1D
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_SAMPLE_BUFFERS 0x80A8
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE 0x8211
#define GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE 0x8212
#define GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE 0x8213
#define GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE 0x8214
#define GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE 0x8215
#define GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING 0x8210
#define GL_SRGB 0x8C40
#define GL_TEXTURE 0x1702
#define GL_RENDERBUFFER 0x8D41
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RGBA 0x1908
#define GL_UNSIGNED_NORMALIZED 0x8C17
#define GL_SIGNED_NORMALIZED 0x8F9C
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_SHADER_TYPE 0x8B4F
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_SHADER_SOURCE_LENGTH 0x8B88
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_SAMPLER_2D 0x8B5E
#define GL_SAMPLER_3D 0x8B5F
#define GL_SAMPLER_CUBE 0x8B60
#define GL_SAMPLER_2D_SHADOW 0x8B62
#define GL_SAMPLER_2D_ARRAY 0x8DC1
#define GL_SAMPLER_2D_ARRAY_SHADOW 0x8DC4
#define GL_SAMPLER_CUBE_SHADOW 0x8DC5
#define GL_INT_SAMPLER_2D 0x8DCA
#define GL_UNSIGNED_INT_SAMPLER_2D 0x8DD2
#define GL_SAMPLER_2D_MULTISAMPLE 0x9108
#define GL_SAMPLER_EXTERNAL_OES 0x8D66
#define GL_VIEWPORT 0x0BA2

static int g_draws_max;          // UA_GL_TRACE_DRAWS
static int g_events;             // draws, clears, blits logged in this frame
static int g_fb_described;       // the current pass's colour buffer was logged
static void (*r_GetIntegerv)(GLenum, GLint *);
static void (*r_GetFloatv)(GLenum, float *);
static void (*r_GetFbAttachment)(GLenum, GLenum, GLenum, GLint *);
static void (*r_ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*r_BindFramebuffer)(GLenum, GLuint);
static void (*r_BindBuffer)(GLenum, GLuint);
static void (*r_ActiveTexture)(GLenum);
static GLenum (*r_GetError)(void);
static void (*r_AttachShader)(GLuint, GLuint);
static void (*r_GetShaderiv)(GLuint, GLenum, GLint *);
static void (*r_GetShaderSource)(GLuint, GLsizei, GLsizei *, char *);
static void (*r_GetProgramiv)(GLuint, GLenum, GLint *);
static void (*r_GetActiveUniform)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *);
static GLint (*r_GetUniformLocation)(GLuint, const char *);
static void (*r_GetUniformiv)(GLuint, GLint, GLint *);

// program -> its shaders' source hashes (as glhooks.c names the UA_SHADER_DUMP files)
#define MAX_PROG 65536
static uint64_t *g_vs_hash, *g_fs_hash;

static uint64_t gltrace_fnv64(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t k = 0; k < n; k++) { h ^= (unsigned char)s[k]; h *= 1099511628211ull; }
    return h;
}

static void w_AttachShader(GLuint prog, GLuint sh)
{
    r_AttachShader(prog, sh);
    if (prog >= MAX_PROG || !g_vs_hash || !r_GetShaderiv || !r_GetShaderSource) return;
    GLint type = 0, len = 0;
    r_GetShaderiv(sh, GL_SHADER_TYPE, &type);
    r_GetShaderiv(sh, GL_SHADER_SOURCE_LENGTH, &len);
    if (len <= 0) return;
    char *src = malloc((size_t)len + 1);
    if (!src) return;
    GLsizei got = 0;
    r_GetShaderSource(sh, len + 1, &got, src);
    uint64_t h = gltrace_fnv64(src, (size_t)got);
    free(src);
    if (type == GL_VERTEX_SHADER) g_vs_hash[prog] = h;
    else if (type == GL_FRAGMENT_SHADER) g_fs_hash[prog] = h;
}

static void drain_errors(void) { for (int i = 0; r_GetError && i < 16 && r_GetError(); i++) ; }

// the draw framebuffer's colour buffer 0: what reads it back, and a description
typedef struct { GLenum type; int ms, none; GLint name, objtype, comp, bits[4], srgb; } colour_t;

static colour_t colour_of(GLuint fb)
{
    colour_t c = { .type = GL_UNSIGNED_BYTE };
    GLint sb = 0;
    if (r_GetIntegerv) r_GetIntegerv(GL_SAMPLE_BUFFERS, &sb);
    c.ms = sb > 0;
    if (fb == 0 || !r_GetFbAttachment) return c;
    r_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &c.objtype);
    if (c.objtype == 0) { c.none = 1; drain_errors(); return c; }
    r_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &c.name);
    r_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &c.comp);
    static const GLenum sizes[4] = { GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE,
                                     GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE, GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE };
    for (int i = 0; i < 4; i++) r_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, sizes[i], &c.bits[i]);
    GLint enc = 0;
    r_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &enc);
    c.srgb = enc == GL_SRGB;
    if (c.comp == GL_FLOAT) c.type = GL_FLOAT;
    else if (c.comp == GL_INT || c.comp == GL_UNSIGNED_INT) c.type = 0;     // integer buffers: not read
    drain_errors();
    return c;
}

static const char *format_of(const colour_t *c, char *s, size_t n)
{
    if (c->none) { snprintf(s, n, "no colour buffer"); return s; }
    const char *k = c->comp == GL_FLOAT ? "f" : c->comp == GL_INT ? "i" : c->comp == GL_UNSIGNED_INT ? "ui" :
                    c->comp == GL_SIGNED_NORMALIZED ? "snorm" : "unorm";
    snprintf(s, n, "%s R%dG%dB%dA%d%s%s", k, c->bits[0], c->bits[1], c->bits[2], c->bits[3], c->srgb ? " sRGB" : "",
             c->ms ? " ms" : "");
    return s;
}

// one pixel of the draw framebuffer's colour buffer 0, as text
static void pixel(GLuint fb, const colour_t *c, int x, int y, char *s, size_t n)
{
    if (c->ms) { snprintf(s, n, "ms"); return; }
    if (c->none || !c->type || !r_ReadPixels || !r_BindFramebuffer) { snprintf(s, n, "-"); return; }
    GLint rfb = 0, pbo = 0;
    r_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    r_GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    if ((GLuint)rfb != fb) r_BindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    if (pbo && r_BindBuffer) r_BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    drain_errors();
    if (c->type == GL_FLOAT) {
        float v[4] = { -1, -1, -1, -1 };
        r_ReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, v);
        snprintf(s, n, "(%.3g,%.3g,%.3g,%.3g)", v[0], v[1], v[2], v[3]);
    } else {
        unsigned char v[4] = { 0, 0, 0, 0 };
        r_ReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, v);
        snprintf(s, n, "(%d,%d,%d,%d)", v[0], v[1], v[2], v[3]);
    }
    if (r_GetError && r_GetError()) snprintf(s, n, "?");
    drain_errors();
    if ((GLuint)rfb != fb) r_BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    if (pbo && r_BindBuffer) r_BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pbo);
}

// the pass's colour buffer, once per pass; then the pixels after a call
static void after(char *out, size_t n)
{
    GLint vp[4] = { 0, 0, 0, 0 };
    r_GetIntegerv(GL_VIEWPORT, vp);
    colour_t c = colour_of(g_cur_fb);
    if (!g_fb_described) {
        char f[64];
        g_fb_described = 1;
        if (g_cur_fb == 0) fprintf(stderr, "[gl]   fb0 = the window %dx%d%s\n", vp[2], vp[3], c.ms ? " ms" : "");
        else fprintf(stderr, "[gl]   fb%u = %s%d %dx%d %s\n", g_cur_fb, c.objtype == GL_RENDERBUFFER ? "rb" : "tex", c.name,
                     vp[2], vp[3], format_of(&c, f, sizeof f));
    }
    char a[64], b[64];
    pixel(g_cur_fb, &c, vp[0] + vp[2] / 2, vp[1] + vp[3] / 2, a, sizeof a);
    pixel(g_cur_fb, &c, vp[0] + vp[2] / 4, vp[1] + vp[3] / 4, b, sizeof b);
    snprintf(out, n, "-> %s %s", a, b);
}

static int sampler_type(GLenum t)
{
    switch (t) {
    case GL_SAMPLER_2D: case GL_SAMPLER_2D_SHADOW: case GL_INT_SAMPLER_2D: case GL_UNSIGNED_INT_SAMPLER_2D:
    case GL_SAMPLER_2D_MULTISAMPLE: case GL_SAMPLER_EXTERNAL_OES: return GL_TEXTURE_BINDING_2D;
    case GL_SAMPLER_3D: return GL_TEXTURE_BINDING_3D;
    case GL_SAMPLER_CUBE: case GL_SAMPLER_CUBE_SHADOW: return GL_TEXTURE_BINDING_CUBE_MAP;
    case GL_SAMPLER_2D_ARRAY: case GL_SAMPLER_2D_ARRAY_SHADOW: return GL_TEXTURE_BINDING_2D_ARRAY;
    default: return 0;
    }
}

// the current program's samplers: name=unit:texture
static void samplers(GLuint prog, char *out, size_t n)
{
    size_t o = 0;
    out[0] = 0;
    if (!prog || !r_GetProgramiv || !r_GetActiveUniform || !r_GetUniformLocation || !r_GetUniformiv || !r_ActiveTexture) return;
    GLint nu = 0, active = 0;
    r_GetProgramiv(prog, GL_ACTIVE_UNIFORMS, &nu);
    r_GetIntegerv(GL_ACTIVE_TEXTURE, &active);
    for (GLint i = 0; i < nu && i < 256 && o + 48 < n; i++) {
        char name[64]; GLsizei len = 0; GLint size = 0; GLenum type = 0;
        r_GetActiveUniform(prog, (GLuint)i, sizeof name, &len, &size, &type, name);
        GLenum binding = (GLenum)sampler_type(type);
        if (!binding) continue;
        GLint loc = r_GetUniformLocation(prog, name), unit = -1, tex = 0;
        if (loc >= 0) r_GetUniformiv(prog, loc, &unit);
        if (unit >= 0 && unit < 32) { r_ActiveTexture(GL_TEXTURE0 + (GLenum)unit); r_GetIntegerv(binding, &tex); }
        o += (size_t)snprintf(out + o, n - o, " %s=%d:tex%d", name, unit, tex);
    }
    r_ActiveTexture((GLenum)active);
    drain_errors();
}

static void log_draw(long verts, int inst)
{
    if (g_events >= g_draws_max) return;
    int i = g_events++;
    GLint prog = 0;
    r_GetIntegerv(GL_CURRENT_PROGRAM, &prog);
    char smp[1024], px[160];
    samplers((GLuint)prog, smp, sizeof smp);
    after(px, sizeof px);
    uint64_t vs = prog >= 0 && prog < MAX_PROG && g_vs_hash ? g_vs_hash[prog] : 0;
    uint64_t fs = prog >= 0 && prog < MAX_PROG && g_fs_hash ? g_fs_hash[prog] : 0;
    char vh[20] = "?", fh[20] = "?", ni[16] = "";
    if (vs) snprintf(vh, sizeof vh, "%016llx", (unsigned long long)vs);
    if (fs) snprintf(fh, sizeof fh, "%016llx", (unsigned long long)fs);
    if (inst > 1) snprintf(ni, sizeof ni, "x%d", inst);
    fprintf(stderr, "[gl]   #%d draw fb%u prog%d vs %s fs %s %ld%s%s %s\n", i, g_cur_fb, prog, vh, fh, verts, ni, smp, px);
}

static void log_clear(GLbitfield m)
{
    if (g_events >= g_draws_max) return;
    int i = g_events++;
    float c[4] = { 0, 0, 0, 0 };
    if (r_GetFloatv) r_GetFloatv(GL_COLOR_CLEAR_VALUE, c);
    char px[160], b[4];
    after(px, sizeof px);
    fprintf(stderr, "[gl]   #%d clear fb%u %s colour (%.3g,%.3g,%.3g,%.3g) %s\n", i, g_cur_fb, bits(m, b), c[0], c[1], c[2], c[3], px);
}

static void log_blit(const GLint r[8], GLbitfield m)
{
    if (g_events >= g_draws_max) return;
    int i = g_events++;
    GLint rfb = 0;
    r_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    char px[160], b[4];
    after(px, sizeof px);
    fprintf(stderr, "[gl]   #%d blit fb%d -> fb%u (%d,%d,%d,%d) -> (%d,%d,%d,%d) %s %s\n", i, rfb, g_cur_fb,
            r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], bits(m, b), px);
}

// ---- wrappers (none of these take floats: no softfp thunk needed)
static void (*r_Viewport)(GLint, GLint, GLsizei, GLsizei);
static void (*r_Clear)(GLbitfield);
static void (*r_Invalidate)(GLenum, GLsizei, const GLenum *);
static void (*r_Discard)(GLenum, GLsizei, const GLenum *);
static void (*r_DrawArrays)(GLenum, GLint, GLsizei);
static void (*r_DrawElements)(GLenum, GLsizei, GLenum, const void *);
static void (*r_DrawElementsInstanced)(GLenum, GLsizei, GLenum, const void *, GLsizei);
static void (*r_DrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei);
static void (*r_BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void (*r_BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
static void *(*r_MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
static void (*r_TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (*r_TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
static void (*r_CompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
static void (*r_CompressedTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);
static void (*r_UseProgram)(GLuint);
static void (*r_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

static void w_BindFramebuffer(GLenum t, GLuint fb)
{
    r_BindFramebuffer(t, fb);
    if (t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER) {
        if (fb != g_cur_fb) g_fb_described = 0;
        g_cur_fb = fb;
        if (g_on) new_pass(fb);
    }
}
static void w_Viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    r_Viewport(x, y, w, h);
    if (g_on) { pass_t *p = cur(); if (!p->draws || !p->w) { p->w = w; p->h = h; } }
}
static void w_Clear(GLbitfield m)
{
    r_Clear(m);
    if (g_on) { pass_t *p = cur(); size_of(p); if (!p->draws) p->clear |= m; if (g_draws_max) log_clear(m); }
}
static void inval(GLenum t, GLsizei n, const GLenum *a)
{
    if (!g_on || t == GL_READ_FRAMEBUFFER) return;
    pass_t *p = cur();
    if (p->draws || p->blits) p->inval_end |= att_bits(n, a); else p->inval_start |= att_bits(n, a);
}
static void w_Invalidate(GLenum t, GLsizei n, const GLenum *a) { r_Invalidate(t, n, a); inval(t, n, a); }
static void w_Discard(GLenum t, GLsizei n, const GLenum *a) { r_Discard(t, n, a); inval(t, n, a); }
static void size_of(pass_t *p)
{
    // Unity skips glViewport when the value does not change: ask GL for the one in effect
    if (p->w || !r_GetIntegerv) return;
    GLint v[4] = { 0, 0, 0, 0 };
    r_GetIntegerv(GL_VIEWPORT, v);
    p->w = v[2]; p->h = v[3];
}
static void draw(long verts, int inst)
{
    if (!g_on) return;
    pass_t *p = cur(); size_of(p); p->draws++; p->verts += verts * inst; g_draws++; g_verts += verts * inst;
    if (g_draws_max) log_draw(verts, inst);
}
static void w_DrawArrays(GLenum m, GLint f, GLsizei c) { r_DrawArrays(m, f, c); draw(c, 1); }
static void w_DrawElements(GLenum m, GLsizei c, GLenum t, const void *i) { r_DrawElements(m, c, t, i); draw(c, 1); }
static void w_DrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n) { r_DrawElementsInstanced(m, c, t, i, n); draw(c, n); }
static void w_DrawArraysInstanced(GLenum m, GLint f, GLsizei c, GLsizei n) { r_DrawArraysInstanced(m, f, c, n); draw(c, n); }
static void w_BufferData(GLenum t, GLsizeiptr s, const void *d, GLenum u) { r_BufferData(t, s, d, u); if (g_on && d) g_buf_bytes += s; }
static void w_BufferSubData(GLenum t, GLintptr o, GLsizeiptr s, const void *d) { r_BufferSubData(t, o, s, d); if (g_on) g_buf_bytes += s; }
static void *w_MapBufferRange(GLenum t, GLintptr o, GLsizeiptr l, GLbitfield a) { if (g_on) g_buf_bytes += l; return r_MapBufferRange(t, o, l, a); }
static void w_TexImage2D(GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLint b, GLenum fm, GLenum ty, const void *p)
{ r_TexImage2D(t, l, f, w, h, b, fm, ty, p); if (g_on && p) g_tex_bytes += (long)w * h * 4; }
static void w_TexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *p)
{ r_TexSubImage2D(t, l, x, y, w, h, f, ty, p); if (g_on) g_tex_bytes += (long)w * h * 4; }
static void w_CompressedTexImage2D(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei s, const void *d)
{ r_CompressedTexImage2D(t, l, f, w, h, b, s, d); if (g_on) g_tex_bytes += s; }
static void w_CompressedTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLsizei s, const void *d)
{ r_CompressedTexSubImage2D(t, l, x, y, w, h, f, s, d); if (g_on) g_tex_bytes += s; }
static void w_UseProgram(GLuint p) { r_UseProgram(p); if (g_on) g_programs++; }
static void w_BlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield m, GLenum fl)
{
    r_BlitFramebuffer(a, b, c, d, e, f, g, h, m, fl);
    if (!g_on) return;
    cur()->blits++;
    if (g_draws_max) { GLint r[8] = { a, b, c, d, e, f, g, h }; log_blit(r, m); }
}
static void w_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p)
{ r_ReadPixels(x, y, w, h, f, t, p); if (g_on) cur()->reads++; }

static void report(void)
{
    char a[4], b[4], c[4];
    fprintf(stderr, "[gl] frame %ld: %d passes%s, %ld draws, %ld vertices, %ld program switches; uploads: textures %ld KB, buffers %ld KB\n",
            g_frame, g_npass, g_overflow ? "+" : "", g_draws, g_verts, g_programs, g_tex_bytes >> 10, g_buf_bytes >> 10);
    for (int i = 0; i < g_npass; i++) {
        pass_t *p = &g_pass[i];
        fprintf(stderr, "[gl]   fb%u %dx%d clear=%s draws %d verts %ld inval=start:%s end:%s%s%s\n", p->fb, p->w, p->h,
                bits(p->clear, a), p->draws, p->verts, bits(p->inval_start, b), bits(p->inval_end, c),
                p->blits ? " blit" : "", p->reads ? " read" : "");
    }
}

void gltrace_frame(void)
{
    if (g_every <= 0) return;
    g_frame++;
    if (g_on) { report(); g_on = 0; }
    double t = now();
    if (t >= g_next) {
        g_next = t + g_every;
        g_on = 1; g_npass = 0; g_overflow = 0; g_draws = g_verts = g_programs = g_tex_bytes = g_buf_bytes = 0;
        g_events = 0; g_fb_described = 0;
        new_pass(g_cur_fb);                     // the pass the frame starts in (often the window, fb 0)
    }
}

static so_shim g_shims[28];

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

void *gltrace_wrapper(const char *name)
{
    for (const so_shim *s = g_shims; s->name; s++) if (!strcmp(s->name, name)) return s->fn;
    return NULL;
}

const so_shim *gltrace_table(void)
{
    int k = 0;
    const char *e = getenv("UA_GL_TRACE");
    g_every = e ? atof(e) : 0;
    if (g_every > 0) {
#define W(name, var, fn) if ((var = real(name))) g_shims[k++] = (so_shim){ name, (void *)fn };
        W("glBindFramebuffer", r_BindFramebuffer, w_BindFramebuffer);
        W("glViewport", r_Viewport, w_Viewport);
        W("glClear", r_Clear, w_Clear);
        W("glInvalidateFramebuffer", r_Invalidate, w_Invalidate);
        W("glDiscardFramebufferEXT", r_Discard, w_Discard);
        W("glDrawArrays", r_DrawArrays, w_DrawArrays);
        W("glDrawElements", r_DrawElements, w_DrawElements);
        W("glDrawElementsInstanced", r_DrawElementsInstanced, w_DrawElementsInstanced);
        W("glDrawArraysInstanced", r_DrawArraysInstanced, w_DrawArraysInstanced);
        W("glBufferData", r_BufferData, w_BufferData);
        W("glBufferSubData", r_BufferSubData, w_BufferSubData);
        W("glMapBufferRange", r_MapBufferRange, w_MapBufferRange);
        W("glTexImage2D", r_TexImage2D, w_TexImage2D);
        W("glTexSubImage2D", r_TexSubImage2D, w_TexSubImage2D);
        W("glCompressedTexImage2D", r_CompressedTexImage2D, w_CompressedTexImage2D);
        W("glCompressedTexSubImage2D", r_CompressedTexSubImage2D, w_CompressedTexSubImage2D);
        W("glUseProgram", r_UseProgram, w_UseProgram);
        W("glBlitFramebuffer", r_BlitFramebuffer, w_BlitFramebuffer);
        W("glReadPixels", r_ReadPixels, w_ReadPixels);
        r_GetIntegerv = real("glGetIntegerv");
        const char *d = getenv("UA_GL_TRACE_DRAWS");
        g_draws_max = d ? atoi(d) : 0;
        if (g_draws_max > 0) {
            r_GetFloatv = real("glGetFloatv");
            r_GetFbAttachment = real("glGetFramebufferAttachmentParameteriv");
            r_BindBuffer = real("glBindBuffer");
            r_ActiveTexture = real("glActiveTexture");
            r_GetError = real("glGetError");
            r_GetShaderiv = real("glGetShaderiv");
            r_GetShaderSource = real("glGetShaderSource");
            r_GetProgramiv = real("glGetProgramiv");
            r_GetActiveUniform = real("glGetActiveUniform");
            r_GetUniformLocation = real("glGetUniformLocation");
            r_GetUniformiv = real("glGetUniformiv");
            g_vs_hash = calloc(MAX_PROG, sizeof *g_vs_hash);
            g_fs_hash = calloc(MAX_PROG, sizeof *g_fs_hash);
            W("glAttachShader", r_AttachShader, w_AttachShader);
            if (!r_GetIntegerv || !r_ReadPixels || !g_vs_hash || !g_fs_hash) g_draws_max = 0;
        }
#undef W
        fprintf(stderr, "[gl] tracing one frame every %.0f s (%d entry points)%s\n", g_every, k,
                g_draws_max > 0 ? ", its draws one by one" : "");
    }
    g_shims[k] = (so_shim){ NULL, NULL };
    return g_shims;
}
