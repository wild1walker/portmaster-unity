// GL state calls that change nothing, counted or left out (UA_GL_STATE=count|1; off by default).
//
// Unity's GLES device keeps its own copy of the GL state, but sets some of it again where nothing
// changed: a texture bound to the unit it is on, a texture's filter or wrap set to what it is, a
// program, a buffer, a capability, a blend, depth or stencil function, a viewport. Each such call
// costs the driver (state marked dirty, descriptors made again at the next draw) for nothing.
//
//   UA_GL_STATE=count  every state call followed in a shadow of what the render thread has set;
//                      "[glstate] ..." every 10 s: per entry point, calls a frame and how many set
//                      what was already set. Nothing is changed.
//   UA_GL_STATE=1      those calls are not made (the shadow says the driver already has it).
//
// Followed on the thread that swaps, from its first swap on; its shadow is forgotten at every swap
// (the host's own drawing then, the scaler, the cursor, screenshots, and a context made current
// start nothing wrong) and at eglMakeCurrent, so a call is left out only when the same thread set
// that very state earlier in the same frame and nothing seen since could have changed it. What a
// call changes as a side effect is followed too: glDeleteTextures and glDeleteBuffers unbind the
// names they delete (and the textures' parameters go), glBindVertexArray brings other vertex
// arrays (the index buffer binding and the attributes are the vertex array's), glBindBufferBase
// and glBindBufferRange set the generic binding. Integer and pointer arguments only (a wrapper of a
// float-taking call would need the player's softfp convention).
#include "glstate.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned char GLboolean;

static so_shim g_shims[40];
static int g_mode;                          // 1 count, 2 leave out
static int g_have_thread, g_frames;
static pthread_t g_thread;
static double g_last_log;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int mine(void) { return g_have_thread && pthread_equal(pthread_self(), g_thread); }

// ---- per entry point: calls and the ones that set what was set
enum { C_ActiveTexture, C_BindTexture, C_TexParameteri, C_UseProgram, C_BindBuffer, C_Enable, C_Disable, C_BlendFuncSeparate,
       C_BlendFunc, C_BlendEquationSeparate, C_BlendEquation, C_ColorMask, C_DepthFunc, C_DepthMask, C_StencilFuncSeparate,
       C_StencilFunc, C_StencilOpSeparate, C_StencilOp, C_StencilMask, C_StencilMaskSeparate, C_Viewport, C_Scissor,
       C_FrontFace, C_CullFace, C_VertexAttribPointer, C_EnableVertexAttribArray, C_DisableVertexAttribArray,
       C_BindVertexArray, C_N };
static const char *const c_name[C_N] = { "glActiveTexture", "glBindTexture", "glTexParameteri", "glUseProgram", "glBindBuffer",
    "glEnable", "glDisable", "glBlendFuncSeparate", "glBlendFunc", "glBlendEquationSeparate", "glBlendEquation", "glColorMask",
    "glDepthFunc", "glDepthMask", "glStencilFuncSeparate", "glStencilFunc", "glStencilOpSeparate", "glStencilOp", "glStencilMask",
    "glStencilMaskSeparate", "glViewport", "glScissor", "glFrontFace", "glCullFace", "glVertexAttribPointer",
    "glEnableVertexAttribArray", "glDisableVertexAttribArray", "glBindVertexArray" };
static unsigned long g_calls[C_N], g_same[C_N];

// ---- the shadow: each piece of state a few words and whether it is known
#define UNITS 32
#define ATTRIBS 16
enum { T_2D, T_CUBE, T_3D, T_2DARRAY, T_EXTERNAL, T_N };
enum { B_ARRAY, B_ELEMENT, B_UNIFORM, B_COPYR, B_COPYW, B_UNPACK, B_N };
enum { K_BLEND, K_CULL, K_DEPTH, K_STENCIL, K_SCISSOR, K_POFFSET, K_DITHER, K_A2C, K_COVERAGE, K_DISCARD, K_RESTART, K_N };
typedef struct { int known; uintptr_t v[6]; } slot;
static struct {
    slot active, prog, vao;
    slot tex[UNITS][T_N];
    slot buf[B_N];
    slot cap[K_N];
    slot blendfunc, blendeq, colormask, depthfunc, depthmask, viewport, scissor, frontface, cullface;
    slot stfunc[2], stop[2], stmask[2];
    slot attrib[ATTRIBS], attrib_on[ATTRIBS];
} S;
// texture parameters: (texture, parameter) -> value, for this frame
#define TP_CAP 1024
static struct { GLuint tex; GLenum pname; GLint v; } g_tp[TP_CAP];
static int g_ntp;

static void forget(void) { memset(&S, 0, sizeof S); g_ntp = 0; }

// set: returns 1 when the slot already holds these values (the call changes nothing)
static int set(slot *s, int n, const uintptr_t *v)
{
    if (s->known && !memcmp(s->v, v, n * sizeof *v)) return 1;
    memcpy(s->v, v, n * sizeof *v); s->known = 1;
    return 0;
}
// count, and whether to leave the call out
static int same(int c, slot *s, int n, const uintptr_t *v)
{
    g_calls[c]++;
    if (!set(s, n, v)) return 0;
    g_same[c]++;
    return g_mode == 2;
}
#define SAME(c, s, ...) (mine() && same(c, s, sizeof((uintptr_t[]){ __VA_ARGS__ }) / sizeof(uintptr_t), (uintptr_t[]){ __VA_ARGS__ }))

static int tex_index(GLenum t)
{
    switch (t) {
    case 0x0DE1: return T_2D; case 0x8513: return T_CUBE; case 0x806F: return T_3D; case 0x8C1A: return T_2DARRAY;
    case 0x8D65: return T_EXTERNAL; default: return -1;
    }
}
static int buf_index(GLenum t)
{
    switch (t) {
    case 0x8892: return B_ARRAY; case 0x8893: return B_ELEMENT; case 0x8A11: return B_UNIFORM; case 0x8F36: return B_COPYR;
    case 0x8F37: return B_COPYW; case 0x88EC: return B_UNPACK; default: return -1;   // (not the pixel pack buffer: glmemo.c's readbacks)
    }
}
static int cap_index(GLenum c)
{
    switch (c) {
    case 0x0BE2: return K_BLEND; case 0x0B44: return K_CULL; case 0x0B71: return K_DEPTH; case 0x0B90: return K_STENCIL;
    case 0x0C11: return K_SCISSOR; case 0x8037: return K_POFFSET; case 0x0BD0: return K_DITHER; case 0x809E: return K_A2C;
    case 0x80A0: return K_COVERAGE; case 0x8C89: return K_DISCARD; case 0x8D69: return K_RESTART; default: return -1;
    }
}
static int unit(void) { return S.active.known ? (int)(S.active.v[0] - 0x84C0) : -1; }   // GL_TEXTURE0
static void vao_changed(void) { S.buf[B_ELEMENT].known = 0; memset(S.attrib, 0, sizeof S.attrib); memset(S.attrib_on, 0, sizeof S.attrib_on); }

// ---- the wrappers
static void (*r_ActiveTexture)(GLenum);
static void w_ActiveTexture(GLenum u) { if (SAME(C_ActiveTexture, &S.active, u)) return; r_ActiveTexture(u); }

static void (*r_BindTexture)(GLenum, GLuint);
static void w_BindTexture(GLenum t, GLuint tex)
{
    int i = tex_index(t), u;
    if (mine()) {
        if (i >= 0 && (u = unit()) >= 0 && u < UNITS) { if (same(C_BindTexture, &S.tex[u][i], 1, (uintptr_t[]){ tex })) return; }
        else g_calls[C_BindTexture]++;
    }
    r_BindTexture(t, tex);
}

static void (*r_TexParameteri)(GLenum, GLenum, GLint);
static void w_TexParameteri(GLenum t, GLenum pname, GLint v)
{
    int i = tex_index(t), u;
    if (mine()) {
        g_calls[C_TexParameteri]++;
        if (i >= 0 && (u = unit()) >= 0 && u < UNITS && S.tex[u][i].known) {
            GLuint tex = (GLuint)S.tex[u][i].v[0];
            int k;
            for (k = 0; k < g_ntp; k++) if (g_tp[k].tex == tex && g_tp[k].pname == pname) break;
            if (k < g_ntp && g_tp[k].v == v) { g_same[C_TexParameteri]++; if (g_mode == 2) return; }
            else if (k < g_ntp) g_tp[k].v = v;
            else if (g_ntp < TP_CAP) { g_tp[g_ntp].tex = tex; g_tp[g_ntp].pname = pname; g_tp[g_ntp].v = v; g_ntp++; }
        }
    }
    r_TexParameteri(t, pname, v);
}

static void (*r_DeleteTextures)(GLsizei, const GLuint *);
static void w_DeleteTextures(GLsizei n, const GLuint *names)
{
    if (mine())
        for (GLsizei j = 0; j < n; j++) {
            for (int u = 0; u < UNITS; u++)
                for (int i = 0; i < T_N; i++)
                    if (S.tex[u][i].known && S.tex[u][i].v[0] == names[j]) S.tex[u][i].v[0] = 0;   // (unbound: 0)
            for (int k = 0; k < g_ntp; k++) if (g_tp[k].tex == names[j]) g_tp[k--] = g_tp[--g_ntp];
        }
    r_DeleteTextures(n, names);
}

static void (*r_UseProgram)(GLuint);
static void w_UseProgram(GLuint p) { if (SAME(C_UseProgram, &S.prog, p)) return; r_UseProgram(p); }

static void (*r_BindBuffer)(GLenum, GLuint);
static void w_BindBuffer(GLenum t, GLuint b)
{
    int i = buf_index(t);
    if (mine()) {
        if (i >= 0) { if (same(C_BindBuffer, &S.buf[i], 1, (uintptr_t[]){ b })) return; }
        else g_calls[C_BindBuffer]++;
    }
    r_BindBuffer(t, b);
}
static void (*r_BindBufferBase)(GLenum, GLuint, GLuint);
static void w_BindBufferBase(GLenum t, GLuint idx, GLuint b)
{
    int i = buf_index(t);
    if (mine() && i >= 0) { S.buf[i].v[0] = b; S.buf[i].known = 1; }   // (also the generic binding)
    r_BindBufferBase(t, idx, b);
}
static void (*r_BindBufferRange)(GLenum, GLuint, GLuint, intptr_t, intptr_t);
static void w_BindBufferRange(GLenum t, GLuint idx, GLuint b, intptr_t off, intptr_t size)
{
    int i = buf_index(t);
    if (mine() && i >= 0) { S.buf[i].v[0] = b; S.buf[i].known = 1; }
    r_BindBufferRange(t, idx, b, off, size);
}
static void (*r_DeleteBuffers)(GLsizei, const GLuint *);
static void w_DeleteBuffers(GLsizei n, const GLuint *names)
{
    if (mine())
        for (GLsizei j = 0; j < n; j++) {
            for (int i = 0; i < B_N; i++) if (S.buf[i].known && S.buf[i].v[0] == names[j]) S.buf[i].v[0] = 0;
            for (int a = 0; a < ATTRIBS; a++) if (S.attrib[a].known && S.attrib[a].v[0] == names[j]) S.attrib[a].known = 0;
        }
    r_DeleteBuffers(n, names);
}
static void (*r_BindVertexArray)(GLuint);
static void w_BindVertexArray(GLuint a)
{
    if (mine()) {
        if (same(C_BindVertexArray, &S.vao, 1, (uintptr_t[]){ a })) return;
        vao_changed();
    }
    r_BindVertexArray(a);
}
static void (*r_DeleteVertexArrays)(GLsizei, const GLuint *);
static void w_DeleteVertexArrays(GLsizei n, const GLuint *a)
{
    if (mine()) { S.vao.known = 0; vao_changed(); }   // (deleting the bound one binds 0)
    r_DeleteVertexArrays(n, a);
}

static void (*r_Enable)(GLenum);
static void w_Enable(GLenum c)
{
    int i = cap_index(c);
    if (mine()) { if (i >= 0) { if (same(C_Enable, &S.cap[i], 1, (uintptr_t[]){ 1 })) return; } else g_calls[C_Enable]++; }
    r_Enable(c);
}
static void (*r_Disable)(GLenum);
static void w_Disable(GLenum c)
{
    int i = cap_index(c);
    if (mine()) { if (i >= 0) { if (same(C_Disable, &S.cap[i], 1, (uintptr_t[]){ 0 })) return; } else g_calls[C_Disable]++; }
    r_Disable(c);
}

static void (*r_BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
static void w_BlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d)
{ if (SAME(C_BlendFuncSeparate, &S.blendfunc, a, b, c, d)) return; r_BlendFuncSeparate(a, b, c, d); }
static void (*r_BlendFunc)(GLenum, GLenum);
static void w_BlendFunc(GLenum a, GLenum b) { if (SAME(C_BlendFunc, &S.blendfunc, a, b, a, b)) return; r_BlendFunc(a, b); }
static void (*r_BlendEquationSeparate)(GLenum, GLenum);
static void w_BlendEquationSeparate(GLenum a, GLenum b) { if (SAME(C_BlendEquationSeparate, &S.blendeq, a, b)) return; r_BlendEquationSeparate(a, b); }
static void (*r_BlendEquation)(GLenum);
static void w_BlendEquation(GLenum a) { if (SAME(C_BlendEquation, &S.blendeq, a, a)) return; r_BlendEquation(a); }
static void (*r_ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
static void w_ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{ if (SAME(C_ColorMask, &S.colormask, r != 0, g != 0, b != 0, a != 0)) return; r_ColorMask(r, g, b, a); }
static void (*r_DepthFunc)(GLenum);
static void w_DepthFunc(GLenum f) { if (SAME(C_DepthFunc, &S.depthfunc, f)) return; r_DepthFunc(f); }
static void (*r_DepthMask)(GLboolean);
static void w_DepthMask(GLboolean m) { if (SAME(C_DepthMask, &S.depthmask, m != 0)) return; r_DepthMask(m); }

// stencil: per face (0 front, 1 back); a both-faces call is left out only when both faces hold it
static int faces(GLenum face, int *f0, int *f1)
{
    switch (face) { case 0x0404: *f0 = *f1 = 0; return 1; case 0x0405: *f0 = *f1 = 1; return 1; case 0x0408: *f0 = 0; *f1 = 1; return 1; }
    return 0;
}
static int stencil_same(int c, slot *st, GLenum face, int n, const uintptr_t *v)
{
    int f0, f1;
    if (!mine()) return 0;
    g_calls[c]++;
    if (!faces(face, &f0, &f1)) return 0;
    int s0 = set(&st[f0], n, v), s1 = f1 != f0 ? set(&st[f1], n, v) : s0;
    if (!(s0 && s1)) return 0;
    g_same[c]++;
    return g_mode == 2;
}
static void (*r_StencilFuncSeparate)(GLenum, GLenum, GLint, GLuint);
static void w_StencilFuncSeparate(GLenum face, GLenum f, GLint ref, GLuint mask)
{ if (stencil_same(C_StencilFuncSeparate, S.stfunc, face, 3, (uintptr_t[]){ f, (uintptr_t)ref, mask })) return; r_StencilFuncSeparate(face, f, ref, mask); }
static void (*r_StencilFunc)(GLenum, GLint, GLuint);
static void w_StencilFunc(GLenum f, GLint ref, GLuint mask)
{ if (stencil_same(C_StencilFunc, S.stfunc, 0x0408, 3, (uintptr_t[]){ f, (uintptr_t)ref, mask })) return; r_StencilFunc(f, ref, mask); }
static void (*r_StencilOpSeparate)(GLenum, GLenum, GLenum, GLenum);
static void w_StencilOpSeparate(GLenum face, GLenum a, GLenum b, GLenum c)
{ if (stencil_same(C_StencilOpSeparate, S.stop, face, 3, (uintptr_t[]){ a, b, c })) return; r_StencilOpSeparate(face, a, b, c); }
static void (*r_StencilOp)(GLenum, GLenum, GLenum);
static void w_StencilOp(GLenum a, GLenum b, GLenum c)
{ if (stencil_same(C_StencilOp, S.stop, 0x0408, 3, (uintptr_t[]){ a, b, c })) return; r_StencilOp(a, b, c); }
static void (*r_StencilMask)(GLuint);
static void w_StencilMask(GLuint m) { if (stencil_same(C_StencilMask, S.stmask, 0x0408, 1, (uintptr_t[]){ m })) return; r_StencilMask(m); }
static void (*r_StencilMaskSeparate)(GLenum, GLuint);
static void w_StencilMaskSeparate(GLenum face, GLuint m)
{ if (stencil_same(C_StencilMaskSeparate, S.stmask, face, 1, (uintptr_t[]){ m })) return; r_StencilMaskSeparate(face, m); }

static void (*r_Viewport)(GLint, GLint, GLsizei, GLsizei);
static void w_Viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{ if (SAME(C_Viewport, &S.viewport, (uintptr_t)x, (uintptr_t)y, (uintptr_t)w, (uintptr_t)h)) return; r_Viewport(x, y, w, h); }
static void (*r_Scissor)(GLint, GLint, GLsizei, GLsizei);
static void w_Scissor(GLint x, GLint y, GLsizei w, GLsizei h)
{ if (SAME(C_Scissor, &S.scissor, (uintptr_t)x, (uintptr_t)y, (uintptr_t)w, (uintptr_t)h)) return; r_Scissor(x, y, w, h); }
static void (*r_FrontFace)(GLenum);
static void w_FrontFace(GLenum m) { if (SAME(C_FrontFace, &S.frontface, m)) return; r_FrontFace(m); }
static void (*r_CullFace)(GLenum);
static void w_CullFace(GLenum m) { if (SAME(C_CullFace, &S.cullface, m)) return; r_CullFace(m); }

// vertex attributes: the vertex array's, with the array buffer bound at the call
static void (*r_VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
static void w_VertexAttribPointer(GLuint a, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p)
{
    if (mine()) {
        if (a < ATTRIBS && S.buf[B_ARRAY].known) {
            if (same(C_VertexAttribPointer, &S.attrib[a], 6,
                     (uintptr_t[]){ S.buf[B_ARRAY].v[0], (uintptr_t)size, type, norm != 0, (uintptr_t)stride, (uintptr_t)p })) return;
        } else {
            g_calls[C_VertexAttribPointer]++;
            if (a < ATTRIBS) S.attrib[a].known = 0;
        }
    }
    r_VertexAttribPointer(a, size, type, norm, stride, p);
}
static void (*r_VertexAttribIPointer)(GLuint, GLint, GLenum, GLsizei, const void *);
static void w_VertexAttribIPointer(GLuint a, GLint size, GLenum type, GLsizei stride, const void *p)
{
    if (mine() && a < ATTRIBS) S.attrib[a].known = 0;
    r_VertexAttribIPointer(a, size, type, stride, p);
}
static void (*r_EnableVertexAttribArray)(GLuint);
static void w_EnableVertexAttribArray(GLuint a)
{
    if (mine()) { if (a < ATTRIBS) { if (same(C_EnableVertexAttribArray, &S.attrib_on[a], 1, (uintptr_t[]){ 1 })) return; } else g_calls[C_EnableVertexAttribArray]++; }
    r_EnableVertexAttribArray(a);
}
static void (*r_DisableVertexAttribArray)(GLuint);
static void w_DisableVertexAttribArray(GLuint a)
{
    if (mine()) { if (a < ATTRIBS) { if (same(C_DisableVertexAttribArray, &S.attrib_on[a], 1, (uintptr_t[]){ 0 })) return; } else g_calls[C_DisableVertexAttribArray]++; }
    r_DisableVertexAttribArray(a);
}

// a context made current: its state is not this shadow's
static void *(*g_next)(const char *);
static unsigned (*r_MakeCurrent)(void *, void *, void *, void *);
static unsigned w_eglMakeCurrent(void *d, void *dr, void *rd, void *c)
{
    if (mine()) forget();
    if (!r_MakeCurrent) {
        *(void **)&r_MakeCurrent = g_next ? g_next("eglMakeCurrent") : NULL;
        if (!r_MakeCurrent) { void *e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL); if (e) *(void **)&r_MakeCurrent = dlsym(e, "eglMakeCurrent"); }
        if (!r_MakeCurrent) return 0;
    }
    return r_MakeCurrent(d, dr, rd, c);
}

void glstate_frame(void)
{
    if (!g_mode) return;
    if (!g_have_thread) { g_thread = pthread_self(); g_have_thread = 1; g_last_log = now(); }
    if (!mine()) return;
    forget();
    g_frames++;
    double t = now();
    if (t - g_last_log < 10) return;
    g_last_log = t;
    char line[2048]; int o = 0; unsigned long all = 0, all_same = 0;
    for (int c = 0; c < C_N; c++) {
        all += g_calls[c]; all_same += g_same[c];
        if (g_calls[c] && o < (int)sizeof line - 80)
            o += snprintf(line + o, sizeof line - o, "%s%s %.1f/%.1f", o ? ", " : "", c_name[c], (double)g_calls[c] / g_frames,
                          (double)g_same[c] / g_frames);
    }
    fprintf(stderr, "[glstate] %d frames: %.1f state calls a frame, %.1f of them set what was set (%s)\n", g_frames,
            (double)all / g_frames, (double)all_same / g_frames, g_mode == 2 ? "left out" : "counted");
    fprintf(stderr, "[glstate]   calls/same a frame: %s\n", line);
    memset(g_calls, 0, sizeof g_calls); memset(g_same, 0, sizeof g_same); g_frames = 0;
}

static void *gl(void *(*next)(const char *), const char *name)
{
    void *p = next ? next(name) : NULL;
    if (p) return p;
    static void *h;
    if (!h) h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    p = h ? dlsym(h, name) : NULL;
    if (!p) {
        void *e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        void *(*gpa)(const char *) = e ? (void *(*)(const char *))dlsym(e, "eglGetProcAddress") : NULL;
        p = gpa ? gpa(name) : NULL;
    }
    return p;
}

const so_shim *glstate_table(void *(*next)(const char *name))
{
    memset(g_shims, 0, sizeof g_shims);
    const char *e = getenv("UA_GL_STATE");
    g_mode = !e ? 0 : !strcmp(e, "count") ? 1 : !strcmp(e, "1") ? 2 : 0;
    if (!g_mode) return g_shims;
    g_next = next;
    int k = 0, missing = 0;
#define W(f) do { *(void **)&r_##f = gl(next, "gl" #f); if (r_##f) g_shims[k++] = (so_shim){ "gl" #f, (void *)w_##f }; else missing++; } while (0)
    W(ActiveTexture); W(BindTexture); W(TexParameteri); W(DeleteTextures); W(UseProgram); W(BindBuffer); W(BindBufferBase);
    W(BindBufferRange); W(DeleteBuffers); W(BindVertexArray); W(DeleteVertexArrays); W(Enable); W(Disable); W(BlendFuncSeparate);
    W(BlendFunc); W(BlendEquationSeparate); W(BlendEquation); W(ColorMask); W(DepthFunc); W(DepthMask); W(StencilFuncSeparate);
    W(StencilFunc); W(StencilOpSeparate); W(StencilOp); W(StencilMask); W(StencilMaskSeparate); W(Viewport); W(Scissor);
    W(FrontFace); W(CullFace); W(VertexAttribPointer); W(VertexAttribIPointer); W(EnableVertexAttribArray);
    W(DisableVertexAttribArray);
#undef W
    // the side effects must all be followed: without one of these, nothing is left out
    if (missing && (!r_DeleteTextures || !r_DeleteBuffers || !r_BindVertexArray || !r_BindBufferBase || !r_BindBufferRange ||
                    !r_DeleteVertexArrays || !r_VertexAttribIPointer)) {
        fprintf(stderr, "[glstate] GL functions missing: off\n");
        memset(g_shims, 0, sizeof g_shims); g_mode = 0;
        return g_shims;
    }
    g_shims[k++] = (so_shim){ "eglMakeCurrent", (void *)w_eglMakeCurrent };
    fprintf(stderr, "[glstate] %s state calls that change nothing (UA_GL_STATE)\n", g_mode == 2 ? "leaving out" : "counting");
    return g_shims;
}
