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

#define MAX_PASS 48
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

// ---- wrappers (none of these take floats: no softfp thunk needed)
static void (*r_BindFramebuffer)(GLenum, GLuint);
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
static void (*r_ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);

static void w_BindFramebuffer(GLenum t, GLuint fb)
{
    r_BindFramebuffer(t, fb);
    if (t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER) { g_cur_fb = fb; if (g_on) new_pass(fb); }
}
static void w_Viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    r_Viewport(x, y, w, h);
    if (g_on) { pass_t *p = cur(); if (!p->draws || !p->w) { p->w = w; p->h = h; } }
}
static void w_Clear(GLbitfield m)
{
    r_Clear(m);
    if (g_on) { pass_t *p = cur(); size_of(p); if (!p->draws) p->clear |= m; }
}
static void inval(GLenum t, GLsizei n, const GLenum *a)
{
    if (!g_on || t == GL_READ_FRAMEBUFFER) return;
    pass_t *p = cur();
    if (p->draws || p->blits) p->inval_end |= att_bits(n, a); else p->inval_start |= att_bits(n, a);
}
static void w_Invalidate(GLenum t, GLsizei n, const GLenum *a) { r_Invalidate(t, n, a); inval(t, n, a); }
static void w_Discard(GLenum t, GLsizei n, const GLenum *a) { r_Discard(t, n, a); inval(t, n, a); }
static void (*r_GetIntegerv)(GLenum, GLint *);
#define GL_VIEWPORT 0x0BA2
static void size_of(pass_t *p)
{
    // Unity skips glViewport when the value does not change: ask GL for the one in effect
    if (p->w || !r_GetIntegerv) return;
    GLint v[4] = { 0, 0, 0, 0 };
    r_GetIntegerv(GL_VIEWPORT, v);
    p->w = v[2]; p->h = v[3];
}
static void draw(long verts) { if (g_on) { pass_t *p = cur(); size_of(p); p->draws++; p->verts += verts; g_draws++; g_verts += verts; } }
static void w_DrawArrays(GLenum m, GLint f, GLsizei c) { r_DrawArrays(m, f, c); draw(c); }
static void w_DrawElements(GLenum m, GLsizei c, GLenum t, const void *i) { r_DrawElements(m, c, t, i); draw(c); }
static void w_DrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n) { r_DrawElementsInstanced(m, c, t, i, n); draw((long)c * n); }
static void w_DrawArraysInstanced(GLenum m, GLint f, GLsizei c, GLsizei n) { r_DrawArraysInstanced(m, f, c, n); draw((long)c * n); }
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
{ r_BlitFramebuffer(a, b, c, d, e, f, g, h, m, fl); if (g_on) cur()->blits++; }
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
        new_pass(g_cur_fb);                     // the pass the frame starts in (often the window, fb 0)
    }
}

static so_shim g_shims[24];

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
#undef W
        r_GetIntegerv = real("glGetIntegerv");
        fprintf(stderr, "[gl] tracing one frame every %.0f s (%d entry points)\n", g_every, k);
    }
    g_shims[k] = (so_shim){ NULL, NULL };
    return g_shims;
}
