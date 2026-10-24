// GL frame tracer: what one frame asks of the GPU, pass by pass (any Unity GLES game).
//
//   UA_GL_TRACE=<s>   every <s> seconds one whole frame (swap to swap) is recorded and logged
//   (UA_GL_TRACE_START=<s>: the first one <s> seconds after the first frame, e.g. past the menus):
//     [gl] frame <n>: <passes> passes, <draws> draws, <verts> vertices, <programs> program switches;
//          uploads: textures <KB>, buffers <KB>[; GPU <ms> ms]
//     [gl]   calls: glDrawElements <n>, glDrawElementsBaseVertex <n>, ...   (the draw entry points used)
//     [gl]   fb<id> <w>x<h> clear=<C|D|S> draws <n> verts <n> inval=<start|end>:<C|D|S> [blits <n>]
//            [copies <n>] [reads <n>] [indirect <n>] [dispatches <n>] [gpu <ms> ms]
//   A pass begins at glBindFramebuffer; its size is the viewport set for it. "clear" is a glClear
//   before the pass's first draw (the old contents need not be loaded), "inval" a
//   glInvalidateFramebuffer/glDiscardFramebufferEXT (contents need not be stored, or loaded).
//   On a tiler like the Mali every pass costs a store of its tiles, plus a load unless cleared or
//   invalidated first: the log shows which full-screen passes a frame could do without.
//   Every draw entry point of GLES 3.2 and its extensions is counted (DrawArrays/Elements, their
//   Instanced, Range, BaseVertex (core, OES, EXT), BaseInstance, MultiDraw and Indirect forms;
//   indirect draws with 0 vertices: their counts are in a buffer), also when Unity gets them
//   through eglGetProcAddress (the EGL glue hands out the host's shims first); "copies" are
//   glCopyTex(Sub)Image (a framebuffer copied into a texture), "dispatches" compute.
//
//   GPU time per pass (UA_GL_TRACE_GPU; not with UA_GL_TRACE_DRAWS, whose reads split the passes):
//     3 (default)  the host waits for the GPU (glFinish) at the end of each pass of a traced frame and
//                  times that wait on the CPU's clock: the passes run one at a time, each with the
//                  driver's submission and the GPU's start-up in it (about 0.3 ms on the Mali-G31),
//                  and the frame's first pass with what the previous frame left (the vsync wait).
//     1            EXT_disjoint_timer_query, when the driver has it: each pass inside a
//                  GL_TIME_ELAPSED_EXT query, begun with the pass's first command and ended when the
//                  next pass begins (glFlush first, so the pass's work is submitted inside its
//                  query, as a tiler does at a framebuffer change anyway) or at the swap; the frame is
//                  logged a frame later, when the results are in; "(disjoint)": the driver lost time,
//                  don't trust the frame. If the game times the GPU itself (its own GL_TIME_ELAPSED_EXT
//                  query), that frame has no times. The Mali-G31's r20p0 driver answers these queries
//                  with numbers that cannot be right (0 for most of a scene's passes): use 3 there.
//     2            the same without the flush;  0  off.
//   "gpu <ms> ms" per pass, "GPU <ms> ms" for the frame.
#include <dlfcn.h>
#include <pthread.h>
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
    GLuint fb; int w, h; unsigned clear, inval_start, inval_end; int draws, blits, copies, reads, indirect, dispatches;
    long verts; int timed; double gpu_ms;
} pass_t;

static void size_of(pass_t *p);
static double g_every, g_start;
static double g_next;
static int g_on;                 // recording this frame
static long g_frame, g_traced;   // frames swapped; the traced one's number
static pass_t g_pass[MAX_PASS];
static int g_npass, g_overflow;
static long g_draws, g_verts, g_programs, g_tex_bytes, g_buf_bytes;
static GLuint g_cur_fb;
static pthread_t g_thread;       // the thread that swaps (Unity's render thread): the traced one
static int g_have_thread;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

// ---- GPU time per pass: EXT_disjoint_timer_query
#define GL_EXTENSIONS 0x1F03
#define GL_TIME_ELAPSED_EXT 0x88BF
#define GL_QUERY_COUNTER_BITS_EXT 0x8864
#define GL_QUERY_RESULT_EXT 0x8866
#define GL_QUERY_RESULT_AVAILABLE_EXT 0x8867
#define GL_GPU_DISJOINT_EXT 0x8FBB

static int g_gpu_mode = 3;       // UA_GL_TRACE_GPU: 3 glFinish (CPU clock), 1 timer queries + glFlush at pass ends, 2 without, 0 off
static int g_gpu;                // 0 not checked yet (needs a context), 1 timing, -1 none
static GLuint g_query[MAX_PASS];
static int g_q_active = -1;      // the pass whose query runs
static int g_q_wanted = -1;      // the frame's first pass: its query begins with its first command
static int g_q_lost;             // the game timed the GPU itself in this frame: no times
static int g_game_query;         // the game's own GL_TIME_ELAPSED_EXT query is running
static int g_pending, g_waited;  // a traced frame waiting for its GPU times; frames waited
static int g_disjoint;
static const unsigned char *(*r_GetString)(GLenum);
static void (*r_Flush)(void);
static void (*r_Finish)(void);
static void (*r_GenQueries)(GLsizei, GLuint *);
static void (*r_BeginQueryEXT)(GLenum, GLuint);
static void (*r_EndQueryEXT)(GLenum);
static void (*r_BeginQuery)(GLenum, GLuint);
static void (*r_EndQuery)(GLenum);
static void (*r_GetQueryiv)(GLenum, GLenum, GLint *);
static void (*r_GetQueryObjectuiv)(GLuint, GLenum, GLuint *);
static void (*r_GetQueryObjectui64v)(GLuint, GLenum, uint64_t *);
static void (*r_GetIntegerv)(GLenum, GLint *);

static int has_ext(const char *list, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return 1;
    return 0;
}

// at the first traced frame, on the render thread (its context is current)
static void gpu_check(int draws_max)
{
    if (g_gpu) return;
    g_gpu = -1;
    if (g_gpu_mode <= 0) return;
    if (draws_max > 0) { fprintf(stderr, "[gl] no GPU times with UA_GL_TRACE_DRAWS (its reads split the passes)\n"); return; }
    if (g_gpu_mode == 3) {
        if (!r_Finish) return;
        g_gpu = 1;
        fprintf(stderr, "[gl] GPU time per pass: glFinish at each pass's end (the CPU's clock; the passes run one at a time)\n");
        return;
    }
    const char *ext = r_GetString ? (const char *)r_GetString(GL_EXTENSIONS) : NULL;
    GLint bits = 0;
    if (ext && has_ext(ext, "GL_EXT_disjoint_timer_query") && r_GenQueries && r_BeginQueryEXT && r_EndQueryEXT &&
        r_GetQueryObjectui64v && r_GetQueryObjectuiv && r_GetQueryiv && r_Flush)
        r_GetQueryiv(GL_TIME_ELAPSED_EXT, GL_QUERY_COUNTER_BITS_EXT, &bits);
    if (bits <= 0) { fprintf(stderr, "[gl] no GPU times: the driver has no EXT_disjoint_timer_query\n"); return; }
    r_GenQueries(MAX_PASS, g_query);
    g_gpu = 1;
    fprintf(stderr, "[gl] GPU time per pass: EXT_disjoint_timer_query (%d-bit counter)%s\n", bits,
            g_gpu_mode == 2 ? ", no flush at pass ends" : "");
}

static void q_begin(int i)
{
    if (g_gpu <= 0 || g_q_lost || i < 0 || i >= MAX_PASS || g_q_active >= 0) return;
    if (g_game_query) { g_q_lost = 1; return; }
    if (g_gpu_mode == 3) r_Finish();            // (the GPU idle: what came before is not this pass's)
    else r_BeginQueryEXT(GL_TIME_ELAPSED_EXT, g_query[i]);
    g_q_active = i;
    g_pass[i].timed = 1;
}

static void q_end(void)
{
    if (g_q_active < 0) return;
    if (g_gpu_mode == 3) {                      // what the GPU still had to do once the pass was issued
        double t = now();
        r_Finish();
        g_pass[g_q_active].gpu_ms += (now() - t) * 1000;
        g_q_active = -1;
        return;
    }
    if (g_gpu_mode == 1) r_Flush();             // the pass's work submitted inside its query
    r_EndQueryEXT(GL_TIME_ELAPSED_EXT);
    g_q_active = -1;
}

// the game's own queries: a GL_TIME_ELAPSED_EXT query of its own cannot run inside ours
static void game_begin(GLenum t)
{
    if (t != GL_TIME_ELAPSED_EXT) return;
    if (g_q_active >= 0) q_end();
    if (g_on && !g_q_lost) { g_q_lost = 1; fprintf(stderr, "[gl] the game times the GPU itself: frame %ld has no GPU times\n", g_traced); }
    g_game_query = 1;
}
static void w_BeginQueryEXT(GLenum t, GLuint q) { if (pthread_equal(pthread_self(), g_thread)) game_begin(t); r_BeginQueryEXT(t, q); }
static void w_EndQueryEXT(GLenum t) { r_EndQueryEXT(t); if (t == GL_TIME_ELAPSED_EXT) g_game_query = 0; }
static void w_BeginQuery(GLenum t, GLuint q) { if (pthread_equal(pthread_self(), g_thread)) game_begin(t); r_BeginQuery(t, q); }
static void w_EndQuery(GLenum t) { r_EndQuery(t); if (t == GL_TIME_ELAPSED_EXT) g_game_query = 0; }

// the traced frame's results, a frame (or a few) after it: 0 while the GPU is not done yet
static int gpu_results(int wait)
{
    if (g_gpu_mode == 3) return 1;              // (measured as the frame ran)
    int last = -1;
    for (int i = 0; i < g_npass; i++) if (g_pass[i].timed) last = i;
    if (last < 0) return 1;
    GLuint avail = 0;
    if (!wait) { r_GetQueryObjectuiv(g_query[last], GL_QUERY_RESULT_AVAILABLE_EXT, &avail); if (!avail) return 0; }
    for (int i = 0; i <= last; i++) {
        if (!g_pass[i].timed) continue;
        uint64_t ns = 0;
        r_GetQueryObjectui64v(g_query[i], GL_QUERY_RESULT_EXT, &ns);
        g_pass[i].gpu_ms = ns / 1e6;
    }
    GLint d = 0;
    r_GetIntegerv(GL_GPU_DISJOINT_EXT, &d);
    g_disjoint = d != 0;
    return 1;
}

static pass_t *cur(void)
{
    if (g_npass == 0) { g_pass[0] = (pass_t){ .fb = g_cur_fb }; g_npass = 1; }
    if (g_q_wanted >= 0) { int i = g_q_wanted; g_q_wanted = -1; q_begin(i); }   // (after the swap)
    return &g_pass[g_npass - 1];
}

static void new_pass(GLuint fb)
{
    pass_t *l = g_npass ? &g_pass[g_npass - 1] : NULL;
    if (l && !l->draws && !l->blits && !l->copies && !l->clear && !l->dispatches) {
        l->fb = fb;                             // nothing happened in the last one: reuse it (and its query)
        l->inval_start = l->inval_end = 0;      // (an invalidation of that framebuffer is not this one's)
        l->w = l->h = 0;
        return;
    }
    if (g_npass >= MAX_PASS) { g_overflow++; return; }
    q_end(); g_q_wanted = -1;
    g_pass[g_npass++] = (pass_t){ .fb = fb };
    q_begin(g_npass - 1);
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
#define GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE 0x8216
#define GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE 0x8217
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
        else {
            char ds[64] = "";
            GLint t = 0, nm = 0, db = 0, sb = 0;
            r_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &t);
            if (t) {
                r_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &nm);
                r_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &db);
            }
            GLint st = 0;
            r_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &st);
            if (st) r_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &sb);
            if (t || st) snprintf(ds, sizeof ds, ", depth %s%d D%dS%d", t == GL_RENDERBUFFER ? "rb" : "tex", nm, db, sb);
            drain_errors();
            fprintf(stderr, "[gl]   fb%u = %s%d %dx%d %s%s\n", g_cur_fb, c.objtype == GL_RENDERBUFFER ? "rb" : "tex", c.name,
                    vp[2], vp[3], format_of(&c, f, sizeof f), ds);
        }
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

// the fixed-function state of a draw: "| depth <func>[ write] stencil <func> blend <src>,<dst>[ mask <rgba>]
// [scissor]" (off when off; what decides whether a pass's depth/stencil or old colour matter)
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_SCISSOR_TEST 0x0C11
#define GL_DEPTH_WRITEMASK 0x0B72
#define GL_DEPTH_FUNC 0x0B74
#define GL_STENCIL_FUNC 0x0B92
#define GL_STENCIL_WRITEMASK 0x0B98
#define GL_STENCIL_REF 0x0B97
#define GL_STENCIL_FAIL 0x0B94
#define GL_STENCIL_PASS_DEPTH_FAIL 0x0B95
#define GL_STENCIL_PASS_DEPTH_PASS 0x0B96
#define GL_BLEND_SRC_RGB 0x80C9
#define GL_BLEND_DST_RGB 0x80C8
#define GL_COLOR_WRITEMASK 0x0C23
static unsigned char (*r_IsEnabled)(GLenum);
static const char *gl_name(GLint e, char *b)
{
    switch (e) {
    case 0: return "0"; case 1: return "1";
    case 0x0300: return "sc"; case 0x0301: return "1-sc"; case 0x0302: return "sa"; case 0x0303: return "1-sa";
    case 0x0304: return "da"; case 0x0305: return "1-da"; case 0x0306: return "dc"; case 0x0307: return "1-dc";
    case 0x0200: return "never"; case 0x0201: return "less"; case 0x0202: return "equal"; case 0x0203: return "lequal";
    case 0x0204: return "greater"; case 0x0205: return "notequal"; case 0x0206: return "gequal"; case 0x0207: return "always";
    case 0x1E00: return "keep"; case 0x1E01: return "replace"; case 0x1E02: return "incr"; case 0x1E03: return "decr";
    case 0x150A: return "invert"; case 0x8507: return "incrwrap"; case 0x8508: return "decrwrap";
    default: sprintf(b, "%04x", (unsigned)e); return b;
    }
}
static void draw_state(char *out, size_t n)
{
    out[0] = 0;
    if (!r_IsEnabled) return;
    char b1[8], b2[8];
    size_t o = 0;
    GLint v = 0, w = 0, m[4] = { 1, 1, 1, 1 };
    if (r_IsEnabled(GL_DEPTH_TEST)) {
        r_GetIntegerv(GL_DEPTH_FUNC, &v); r_GetIntegerv(GL_DEPTH_WRITEMASK, &w);
        o += (size_t)snprintf(out + o, n - o, " | depth %s%s", gl_name(v, b1), w ? " write" : "");
    } else o += (size_t)snprintf(out + o, n - o, " | depth off");
    if (r_IsEnabled(GL_STENCIL_TEST)) {
        GLint f = 0, zf = 0, zp = 0, ref = 0;
        r_GetIntegerv(GL_STENCIL_FUNC, &v); r_GetIntegerv(GL_STENCIL_WRITEMASK, &w); r_GetIntegerv(GL_STENCIL_REF, &ref);
        r_GetIntegerv(GL_STENCIL_FAIL, &f); r_GetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &zf); r_GetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &zp);
        char b3[8], b4[8];
        o += (size_t)snprintf(out + o, n - o, " stencil %s ref %d %s/%s/%s%s", gl_name(v, b1), ref, gl_name(f, b2), gl_name(zf, b3),
                              gl_name(zp, b4), w ? " write" : "");
    } else o += (size_t)snprintf(out + o, n - o, " stencil off");
    if (r_IsEnabled(GL_BLEND)) {
        r_GetIntegerv(GL_BLEND_SRC_RGB, &v); r_GetIntegerv(GL_BLEND_DST_RGB, &w);
        o += (size_t)snprintf(out + o, n - o, " blend %s,%s", gl_name(v, b1), gl_name(w, b2));
    } else o += (size_t)snprintf(out + o, n - o, " blend off");
    r_GetIntegerv(GL_COLOR_WRITEMASK, m);
    if (!(m[0] && m[1] && m[2] && m[3]))
        o += (size_t)snprintf(out + o, n - o, " mask %s%s%s%s", m[0] ? "r" : "", m[1] ? "g" : "", m[2] ? "b" : "", m[3] ? "a" : "");
    if (r_IsEnabled(GL_SCISSOR_TEST)) snprintf(out + o, n - o, " scissor");
    drain_errors();
}

// the draw's buffers: " | vao <n> a<loc>=b<buffer>+<offset> ... e=b<buffer> u<block>=b<buffer>+<offset>/<size>" (vertex
// attributes of the program that are arrays, the element buffer, the uniform blocks' ranges): whether the vertex and
// constant data of a draw are the same buffers and offsets from frame to frame
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_ACTIVE_ATTRIBUTES 0x8B89
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED 0x8622
#define GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING 0x889F
#define GL_VERTEX_ATTRIB_ARRAY_POINTER 0x8645
#define GL_ACTIVE_UNIFORM_BLOCKS 0x8A36
#define GL_UNIFORM_BLOCK_BINDING 0x8A3F
#define GL_UNIFORM_BUFFER_BINDING 0x8A28
#define GL_UNIFORM_BUFFER_START 0x8A29
#define GL_UNIFORM_BUFFER_SIZE 0x8A2A
static void (*r_GetActiveAttrib)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *);
static GLint (*r_GetAttribLocation)(GLuint, const char *);
static void (*r_GetVertexAttribiv)(GLuint, GLenum, GLint *);
static void (*r_GetVertexAttribPointerv)(GLuint, GLenum, void **);
static void (*r_GetActiveUniformBlockiv)(GLuint, GLuint, GLenum, GLint *);
static void (*r_GetIntegeri_v)(GLenum, GLuint, GLint *);
static void (*r_GetInteger64i_v)(GLenum, GLuint, int64_t *);
static void buffers(GLuint prog, char *out, size_t n)
{
    size_t o = 0;
    out[0] = 0;
    if (!prog || !r_GetActiveAttrib || !r_GetAttribLocation || !r_GetVertexAttribiv || !r_GetVertexAttribPointerv) return;
    GLint vao = 0, na = 0, eb = 0;
    r_GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    o += (size_t)snprintf(out + o, n - o, " | vao %d", vao);
    r_GetProgramiv(prog, GL_ACTIVE_ATTRIBUTES, &na);
    for (GLint i = 0; i < na && i < 16 && o + 40 < n; i++) {
        char name[64]; GLsizei len = 0; GLint size = 0; GLenum type = 0;
        r_GetActiveAttrib(prog, (GLuint)i, sizeof name, &len, &size, &type, name);
        GLint loc = r_GetAttribLocation(prog, name), en = 0, b = 0;
        if (loc < 0) continue;                  // (gl_VertexID and the like)
        r_GetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en);
        if (!en) { o += (size_t)snprintf(out + o, n - o, " a%d=const", loc); continue; }
        void *p = NULL;
        r_GetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &b);
        r_GetVertexAttribPointerv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_POINTER, &p);
        o += (size_t)snprintf(out + o, n - o, " a%d=b%d+%ld", loc, b, (long)(intptr_t)p);
    }
    r_GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eb);
    if (eb) o += (size_t)snprintf(out + o, n - o, " e=b%d", eb);
    GLint nb = 0;
    if (r_GetActiveUniformBlockiv && r_GetIntegeri_v) r_GetProgramiv(prog, GL_ACTIVE_UNIFORM_BLOCKS, &nb);
    for (GLint i = 0; i < nb && i < 16 && o + 40 < n; i++) {
        GLint bind = 0, b = 0;
        int64_t start = 0, size = 0;
        r_GetActiveUniformBlockiv(prog, (GLuint)i, GL_UNIFORM_BLOCK_BINDING, &bind);
        r_GetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, (GLuint)bind, &b);
        if (r_GetInteger64i_v) {
            r_GetInteger64i_v(GL_UNIFORM_BUFFER_START, (GLuint)bind, &start);
            r_GetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, (GLuint)bind, &size);
        }
        o += (size_t)snprintf(out + o, n - o, " u%d=b%d+%lld/%lld", i, b, (long long)start, (long long)size);
    }
    drain_errors();
}

static void log_draw(long verts, int inst)
{
    if (g_events >= g_draws_max) return;
    int i = g_events++;
    GLint prog = 0;
    r_GetIntegerv(GL_CURRENT_PROGRAM, &prog);
    char smp[1024], px[160], st[128], bf[512];
    draw_state(st, sizeof st);
    buffers((GLuint)prog, bf, sizeof bf);
    samplers((GLuint)prog, smp, sizeof smp);
    after(px, sizeof px);
    uint64_t vs = prog >= 0 && prog < MAX_PROG && g_vs_hash ? g_vs_hash[prog] : 0;
    uint64_t fs = prog >= 0 && prog < MAX_PROG && g_fs_hash ? g_fs_hash[prog] : 0;
    char vh[20] = "?", fh[20] = "?", ni[16] = "";
    if (vs) snprintf(vh, sizeof vh, "%016llx", (unsigned long long)vs);
    if (fs) snprintf(fh, sizeof fh, "%016llx", (unsigned long long)fs);
    if (inst > 1) snprintf(ni, sizeof ni, "x%d", inst);
    fprintf(stderr, "[gl]   #%d draw fb%u prog%d vs %s fs %s %ld%s%s %s%s%s\n", i, g_cur_fb, prog, vh, fh, verts, ni, smp, px, st, bf);
}

// a buffer upload of the traced frame: "#<n> upload <call> b<buffer> +<offset> <bytes>[ access <bits>]"
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_COPY_READ_BUFFER 0x8F36
#define GL_COPY_WRITE_BUFFER 0x8F37
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_COPY_READ_BUFFER_BINDING 0x8F36
#define GL_COPY_WRITE_BUFFER_BINDING 0x8F37
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
static GLint bound_buffer(GLenum t)
{
    GLenum q = t == GL_ARRAY_BUFFER ? GL_ARRAY_BUFFER_BINDING : t == GL_ELEMENT_ARRAY_BUFFER ? GL_ELEMENT_ARRAY_BUFFER_BINDING :
               t == GL_UNIFORM_BUFFER ? GL_UNIFORM_BUFFER_BINDING : t == GL_COPY_READ_BUFFER ? GL_COPY_READ_BUFFER_BINDING :
               t == GL_COPY_WRITE_BUFFER ? GL_COPY_WRITE_BUFFER_BINDING : t == GL_PIXEL_UNPACK_BUFFER ? GL_PIXEL_UNPACK_BUFFER_BINDING :
               t == GL_PIXEL_PACK_BUFFER ? GL_PIXEL_PACK_BUFFER_BINDING : 0;
    GLint b = -1;
    if (q) r_GetIntegerv(q, &b);
    return b;
}
static void log_upload(const char *call, GLenum t, long off, long len, long access)
{
    if (!g_draws_max || g_events >= g_draws_max) return;
    int i = g_events++;
    char a[32] = "";
    if (access >= 0) snprintf(a, sizeof a, " access %lx", access);
    fprintf(stderr, "[gl]   #%d upload %s b%d +%ld %ld%s\n", i, call, bound_buffer(t), off, len, a);
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
// only the swapping thread's calls make the traced frame (another context's, e.g. an upload thread's, do not)
#define ON (g_on && pthread_equal(pthread_self(), g_thread))
static void (*r_Viewport)(GLint, GLint, GLsizei, GLsizei);
static void (*r_Clear)(GLbitfield);
static void (*r_Invalidate)(GLenum, GLsizei, const GLenum *);
static void (*r_InvalidateSub)(GLenum, GLsizei, const GLenum *, GLint, GLint, GLsizei, GLsizei);
static void (*r_Discard)(GLenum, GLsizei, const GLenum *);
static void (*r_BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void (*r_BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
static void *(*r_MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
static void (*r_TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (*r_TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
static void (*r_CompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
static void (*r_CompressedTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);
static void (*r_UseProgram)(GLuint);
static void (*r_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
static void (*r_CopyTexSubImage2D)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
static void (*r_CopyTexImage2D)(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint);
static void (*r_CopyTexSubImage3D)(GLenum, GLint, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
static void (*r_DispatchCompute)(GLuint, GLuint, GLuint);
static void (*r_DispatchComputeIndirect)(GLintptr);

static void w_BindFramebuffer(GLenum t, GLuint fb)
{
    r_BindFramebuffer(t, fb);
    if ((t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER) && (!g_have_thread || pthread_equal(pthread_self(), g_thread))) {
        if (fb != g_cur_fb) g_fb_described = 0;
        g_cur_fb = fb;
        if (g_on) new_pass(fb);
    }
}
static void w_Viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    r_Viewport(x, y, w, h);
    if (ON) { pass_t *p = cur(); if (!p->draws || !p->w) { p->w = w; p->h = h; } }
}
static void w_Clear(GLbitfield m)
{
    r_Clear(m);
    if (ON) { pass_t *p = cur(); size_of(p); if (!p->draws) p->clear |= m; if (g_draws_max) log_clear(m); }
}
static void inval(GLenum t, GLsizei n, const GLenum *a)
{
    if (!ON || t == GL_READ_FRAMEBUFFER) return;
    pass_t *p = cur();
    if (p->draws || p->blits || p->copies) p->inval_end |= att_bits(n, a); else p->inval_start |= att_bits(n, a);
}
static void w_Invalidate(GLenum t, GLsizei n, const GLenum *a) { r_Invalidate(t, n, a); inval(t, n, a); }
static void w_InvalidateSub(GLenum t, GLsizei n, const GLenum *a, GLint x, GLint y, GLsizei w, GLsizei h)
{ r_InvalidateSub(t, n, a, x, y, w, h); inval(t, n, a); }
static void w_Discard(GLenum t, GLsizei n, const GLenum *a) { r_Discard(t, n, a); inval(t, n, a); }
static void size_of(pass_t *p)
{
    // Unity skips glViewport when the value does not change: ask GL for the one in effect
    if (p->w || !r_GetIntegerv) return;
    GLint v[4] = { 0, 0, 0, 0 };
    r_GetIntegerv(GL_VIEWPORT, v);
    p->w = v[2]; p->h = v[3];
}

// ---- draws: every entry point of GLES 3.2 and its extensions, counted by name
#define DRAWS(X) \
    X(DrawArrays, "glDrawArrays") X(DrawElements, "glDrawElements") \
    X(DrawArraysInstanced, "glDrawArraysInstanced") X(DrawElementsInstanced, "glDrawElementsInstanced") \
    X(DrawArraysInstancedEXT, "glDrawArraysInstancedEXT") X(DrawElementsInstancedEXT, "glDrawElementsInstancedEXT") \
    X(DrawRangeElements, "glDrawRangeElements") \
    X(DrawElementsBaseVertex, "glDrawElementsBaseVertex") X(DrawElementsBaseVertexOES, "glDrawElementsBaseVertexOES") \
    X(DrawElementsBaseVertexEXT, "glDrawElementsBaseVertexEXT") \
    X(DrawRangeElementsBaseVertex, "glDrawRangeElementsBaseVertex") \
    X(DrawRangeElementsBaseVertexOES, "glDrawRangeElementsBaseVertexOES") \
    X(DrawRangeElementsBaseVertexEXT, "glDrawRangeElementsBaseVertexEXT") \
    X(DrawElementsInstancedBaseVertex, "glDrawElementsInstancedBaseVertex") \
    X(DrawElementsInstancedBaseVertexOES, "glDrawElementsInstancedBaseVertexOES") \
    X(DrawElementsInstancedBaseVertexEXT, "glDrawElementsInstancedBaseVertexEXT") \
    X(DrawArraysInstancedBaseInstanceEXT, "glDrawArraysInstancedBaseInstanceEXT") \
    X(DrawElementsInstancedBaseInstanceEXT, "glDrawElementsInstancedBaseInstanceEXT") \
    X(DrawElementsInstancedBaseVertexBaseInstanceEXT, "glDrawElementsInstancedBaseVertexBaseInstanceEXT") \
    X(MultiDrawArraysEXT, "glMultiDrawArraysEXT") X(MultiDrawElementsEXT, "glMultiDrawElementsEXT") \
    X(MultiDrawElementsBaseVertexEXT, "glMultiDrawElementsBaseVertexEXT") \
    X(MultiDrawElementsBaseVertexOES, "glMultiDrawElementsBaseVertexOES") \
    X(DrawArraysIndirect, "glDrawArraysIndirect") X(DrawElementsIndirect, "glDrawElementsIndirect")
#define ENUM(f, n) D_##f,
enum { DRAWS(ENUM) D_N };
#define NAME(f, n) n,
static const char *const g_draw_name[D_N] = { DRAWS(NAME) };
static long g_calls[D_N];

static void draw(int which, long verts, long inst, int n, int indirect)
{
    if (!ON) return;
    pass_t *p = cur(); size_of(p); p->draws += n; p->verts += verts * inst; g_draws += n; g_verts += verts * inst;
    if (indirect) p->indirect += n;
    g_calls[which]++;
    if (g_draws_max) log_draw(verts, (int)inst);
}
static long sum(const GLsizei *c, GLsizei n) { long s = 0; for (GLsizei i = 0; c && i < n; i++) s += c[i]; return s; }

static void (*r_DrawArrays)(GLenum, GLint, GLsizei);
static void w_DrawArrays(GLenum m, GLint f, GLsizei c) { r_DrawArrays(m, f, c); draw(D_DrawArrays, c, 1, 1, 0); }
static void (*r_DrawElements)(GLenum, GLsizei, GLenum, const void *);
static void w_DrawElements(GLenum m, GLsizei c, GLenum t, const void *i) { r_DrawElements(m, c, t, i); draw(D_DrawElements, c, 1, 1, 0); }
#define ARRAYS_INST(f) static void (*r_##f)(GLenum, GLint, GLsizei, GLsizei); \
    static void w_##f(GLenum m, GLint a, GLsizei c, GLsizei n) { r_##f(m, a, c, n); draw(D_##f, c, n, 1, 0); }
ARRAYS_INST(DrawArraysInstanced) ARRAYS_INST(DrawArraysInstancedEXT)
#define ELEMENTS_INST(f) static void (*r_##f)(GLenum, GLsizei, GLenum, const void *, GLsizei); \
    static void w_##f(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n) { r_##f(m, c, t, i, n); draw(D_##f, c, n, 1, 0); }
ELEMENTS_INST(DrawElementsInstanced) ELEMENTS_INST(DrawElementsInstancedEXT)
static void (*r_DrawRangeElements)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
static void w_DrawRangeElements(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i)
{ r_DrawRangeElements(m, s, e, c, t, i); draw(D_DrawRangeElements, c, 1, 1, 0); }
#define ELEMENTS_BV(f) static void (*r_##f)(GLenum, GLsizei, GLenum, const void *, GLint); \
    static void w_##f(GLenum m, GLsizei c, GLenum t, const void *i, GLint b) { r_##f(m, c, t, i, b); draw(D_##f, c, 1, 1, 0); }
ELEMENTS_BV(DrawElementsBaseVertex) ELEMENTS_BV(DrawElementsBaseVertexOES) ELEMENTS_BV(DrawElementsBaseVertexEXT)
#define RANGE_BV(f) static void (*r_##f)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *, GLint); \
    static void w_##f(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i, GLint b) \
    { r_##f(m, s, e, c, t, i, b); draw(D_##f, c, 1, 1, 0); }
RANGE_BV(DrawRangeElementsBaseVertex) RANGE_BV(DrawRangeElementsBaseVertexOES) RANGE_BV(DrawRangeElementsBaseVertexEXT)
#define INST_BV(f) static void (*r_##f)(GLenum, GLsizei, GLenum, const void *, GLsizei, GLint); \
    static void w_##f(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint b) { r_##f(m, c, t, i, n, b); draw(D_##f, c, n, 1, 0); }
INST_BV(DrawElementsInstancedBaseVertex) INST_BV(DrawElementsInstancedBaseVertexOES) INST_BV(DrawElementsInstancedBaseVertexEXT)
static void (*r_DrawArraysInstancedBaseInstanceEXT)(GLenum, GLint, GLsizei, GLsizei, GLuint);
static void w_DrawArraysInstancedBaseInstanceEXT(GLenum m, GLint f, GLsizei c, GLsizei n, GLuint b)
{ r_DrawArraysInstancedBaseInstanceEXT(m, f, c, n, b); draw(D_DrawArraysInstancedBaseInstanceEXT, c, n, 1, 0); }
static void (*r_DrawElementsInstancedBaseInstanceEXT)(GLenum, GLsizei, GLenum, const void *, GLsizei, GLuint);
static void w_DrawElementsInstancedBaseInstanceEXT(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLuint b)
{ r_DrawElementsInstancedBaseInstanceEXT(m, c, t, i, n, b); draw(D_DrawElementsInstancedBaseInstanceEXT, c, n, 1, 0); }
static void (*r_DrawElementsInstancedBaseVertexBaseInstanceEXT)(GLenum, GLsizei, GLenum, const void *, GLsizei, GLint, GLuint);
static void w_DrawElementsInstancedBaseVertexBaseInstanceEXT(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint v, GLuint b)
{ r_DrawElementsInstancedBaseVertexBaseInstanceEXT(m, c, t, i, n, v, b); draw(D_DrawElementsInstancedBaseVertexBaseInstanceEXT, c, n, 1, 0); }
static void (*r_MultiDrawArraysEXT)(GLenum, const GLint *, const GLsizei *, GLsizei);
static void w_MultiDrawArraysEXT(GLenum m, const GLint *f, const GLsizei *c, GLsizei n)
{ r_MultiDrawArraysEXT(m, f, c, n); draw(D_MultiDrawArraysEXT, sum(c, n), 1, n, 0); }
static void (*r_MultiDrawElementsEXT)(GLenum, const GLsizei *, GLenum, const void *const *, GLsizei);
static void w_MultiDrawElementsEXT(GLenum m, const GLsizei *c, GLenum t, const void *const *i, GLsizei n)
{ r_MultiDrawElementsEXT(m, c, t, i, n); draw(D_MultiDrawElementsEXT, sum(c, n), 1, n, 0); }
#define MULTI_BV(f) static void (*r_##f)(GLenum, const GLsizei *, GLenum, const void *const *, GLsizei, const GLint *); \
    static void w_##f(GLenum m, const GLsizei *c, GLenum t, const void *const *i, GLsizei n, const GLint *b) \
    { r_##f(m, c, t, i, n, b); draw(D_##f, sum(c, n), 1, n, 0); }
MULTI_BV(MultiDrawElementsBaseVertexEXT) MULTI_BV(MultiDrawElementsBaseVertexOES)
// (the vertex counts are in a buffer: 0 here)
static void (*r_DrawArraysIndirect)(GLenum, const void *);
static void w_DrawArraysIndirect(GLenum m, const void *p) { r_DrawArraysIndirect(m, p); draw(D_DrawArraysIndirect, 0, 1, 1, 1); }
static void (*r_DrawElementsIndirect)(GLenum, GLenum, const void *);
static void w_DrawElementsIndirect(GLenum m, GLenum t, const void *p) { r_DrawElementsIndirect(m, t, p); draw(D_DrawElementsIndirect, 0, 1, 1, 1); }

static void w_BufferData(GLenum t, GLsizeiptr s, const void *d, GLenum u)
{ r_BufferData(t, s, d, u); if (ON && d) g_buf_bytes += s; if (ON) log_upload(d ? "BufferData" : "BufferData(null)", t, 0, (long)s, -1); }
static void w_BufferSubData(GLenum t, GLintptr o, GLsizeiptr s, const void *d)
{ r_BufferSubData(t, o, s, d); if (ON) { g_buf_bytes += s; log_upload("BufferSubData", t, (long)o, (long)s, -1); } }
static void *w_MapBufferRange(GLenum t, GLintptr o, GLsizeiptr l, GLbitfield a)
{ if (ON) { g_buf_bytes += l; log_upload("MapBufferRange", t, (long)o, (long)l, (long)a); } return r_MapBufferRange(t, o, l, a); }
static void (*r_FlushMappedBufferRange)(GLenum, GLintptr, GLsizeiptr);
static void w_FlushMappedBufferRange(GLenum t, GLintptr o, GLsizeiptr l)
{ r_FlushMappedBufferRange(t, o, l); if (ON) log_upload("FlushMappedBufferRange", t, (long)o, (long)l, -1); }
static unsigned char (*r_UnmapBuffer)(GLenum);
static unsigned char w_UnmapBuffer(GLenum t) { if (ON) log_upload("UnmapBuffer", t, 0, 0, -1); return r_UnmapBuffer(t); }
static void (*r_BufferStorageEXT)(GLenum, GLsizeiptr, const void *, GLbitfield);
static void w_BufferStorageEXT(GLenum t, GLsizeiptr s, const void *d, GLbitfield f)
{ r_BufferStorageEXT(t, s, d, f); if (ON) log_upload("BufferStorageEXT", t, 0, (long)s, (long)f); }
static void (*r_CopyBufferSubData)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr);
static void w_CopyBufferSubData(GLenum rt, GLenum wt, GLintptr ro, GLintptr wo, GLsizeiptr s)
{ r_CopyBufferSubData(rt, wt, ro, wo, s); if (ON) log_upload("CopyBufferSubData", wt, (long)wo, (long)s, -1); }
static void w_TexImage2D(GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLint b, GLenum fm, GLenum ty, const void *p)
{ r_TexImage2D(t, l, f, w, h, b, fm, ty, p); if (ON && p) g_tex_bytes += (long)w * h * 4; }
static void w_TexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *p)
{ r_TexSubImage2D(t, l, x, y, w, h, f, ty, p); if (ON) g_tex_bytes += (long)w * h * 4; }
static void w_CompressedTexImage2D(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei s, const void *d)
{ r_CompressedTexImage2D(t, l, f, w, h, b, s, d); if (ON) g_tex_bytes += s; }
static void w_CompressedTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLsizei s, const void *d)
{ r_CompressedTexSubImage2D(t, l, x, y, w, h, f, s, d); if (ON) g_tex_bytes += s; }
static void w_UseProgram(GLuint p) { r_UseProgram(p); if (ON) g_programs++; }
static void w_BlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield m, GLenum fl)
{
    r_BlitFramebuffer(a, b, c, d, e, f, g, h, m, fl);
    if (!ON) return;
    cur()->blits++;
    if (g_draws_max) { GLint r[8] = { a, b, c, d, e, f, g, h }; log_blit(r, m); }
}
// the read framebuffer copied into a texture (a grab pass's way on GLES)
static void copied(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (!ON) return;
    pass_t *p = cur(); size_of(p); p->copies++;
    if (g_draws_max && g_events < g_draws_max) {
        int i = g_events++;
        GLint rfb = 0, tex = 0;
        r_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
        r_GetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
        fprintf(stderr, "[gl]   #%d copy fb%d (%d,%d %dx%d) -> tex%d\n", i, rfb, x, y, w, h, tex);
    }
}
static void w_CopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{ r_CopyTexSubImage2D(t, l, xo, yo, x, y, w, h); copied(x, y, w, h); }
static void w_CopyTexImage2D(GLenum t, GLint l, GLenum f, GLint x, GLint y, GLsizei w, GLsizei h, GLint b)
{ r_CopyTexImage2D(t, l, f, x, y, w, h, b); copied(x, y, w, h); }
static void w_CopyTexSubImage3D(GLenum t, GLint l, GLint xo, GLint yo, GLint zo, GLint x, GLint y, GLsizei w, GLsizei h)
{ r_CopyTexSubImage3D(t, l, xo, yo, zo, x, y, w, h); copied(x, y, w, h); }
static void w_DispatchCompute(GLuint x, GLuint y, GLuint z) { r_DispatchCompute(x, y, z); if (ON) cur()->dispatches++; }
static void w_DispatchComputeIndirect(GLintptr o) { r_DispatchComputeIndirect(o); if (ON) cur()->dispatches++; }
static void w_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p)
{ r_ReadPixels(x, y, w, h, f, t, p); if (ON) cur()->reads++; }

static void report(void)
{
    char a[4], b[4], c[4], g[48] = "";
    double gpu = 0;
    int timed = 0;
    for (int i = 0; i < g_npass; i++) if (g_pass[i].timed) { gpu += g_pass[i].gpu_ms; timed = 1; }
    if (timed) snprintf(g, sizeof g, "; GPU %.2f ms%s", gpu, g_disjoint ? " (disjoint)" : g_gpu_mode == 3 ? " (glFinish)" : "");
    fprintf(stderr, "[gl] frame %ld: %d passes%s, %ld draws, %ld vertices, %ld program switches; uploads: textures %ld KB, buffers %ld KB%s\n",
            g_traced, g_npass, g_overflow ? "+" : "", g_draws, g_verts, g_programs, g_tex_bytes >> 10, g_buf_bytes >> 10, g);
    char calls[1024];
    size_t o = 0;
    calls[0] = 0;
    for (int i = 0; i < D_N && o + 80 < sizeof calls; i++)
        if (g_calls[i]) o += (size_t)snprintf(calls + o, sizeof calls - o, "%s%s %ld", o ? ", " : "", g_draw_name[i], g_calls[i]);
    if (o) fprintf(stderr, "[gl]   calls: %s\n", calls);
    for (int i = 0; i < g_npass; i++) {
        pass_t *p = &g_pass[i];
        char x[160];
        size_t k = 0;
        x[0] = 0;
        if (p->blits) k += (size_t)snprintf(x + k, sizeof x - k, " blits %d", p->blits);
        if (p->copies) k += (size_t)snprintf(x + k, sizeof x - k, " copies %d", p->copies);
        if (p->reads) k += (size_t)snprintf(x + k, sizeof x - k, " reads %d", p->reads);
        if (p->indirect) k += (size_t)snprintf(x + k, sizeof x - k, " indirect %d", p->indirect);
        if (p->dispatches) k += (size_t)snprintf(x + k, sizeof x - k, " dispatches %d", p->dispatches);
        if (p->timed) k += (size_t)snprintf(x + k, sizeof x - k, " gpu %.2f ms", p->gpu_ms);
        fprintf(stderr, "[gl]   fb%u %dx%d clear=%s draws %d verts %ld inval=start:%s end:%s%s\n", p->fb, p->w, p->h,
                bits(p->clear, a), p->draws, p->verts, bits(p->inval_start, b), bits(p->inval_end, c), x);
    }
}

void gltrace_frame(void)
{
    if (g_every <= 0) return;
    g_frame++;
    g_thread = pthread_self(); g_have_thread = 1;
    if (g_frame == 1 && g_start > 0) { g_next = now() + g_start; return; }
    if (!g_gpu) gpu_check(g_draws_max);          // (the render thread, its context current)
    if (g_on) {                                  // the traced frame ends (before its swap)
        g_on = 0;
        q_end(); g_q_wanted = -1;
        int timed = 0;
        for (int i = 0; i < g_npass; i++) { if (g_q_lost) g_pass[i].timed = 0; timed |= g_pass[i].timed; }
        if (timed && g_gpu_mode != 3) { g_pending = 1; g_waited = 0; }
        else report();
    } else if (g_pending) {                      // its GPU times, once the GPU is done (else wait at most 4 frames)
        if (!gpu_results(++g_waited >= 4)) return;
        g_pending = 0;
        report();
    }
    double t = now();
    if (!g_pending && t >= g_next) {
        g_next = t + g_every;
        g_on = 1; g_npass = 0; g_overflow = 0; g_draws = g_verts = g_programs = g_tex_bytes = g_buf_bytes = 0;
        g_events = 0; g_fb_described = 0; g_traced = g_frame + 1; g_q_lost = 0; g_disjoint = 0;
        memset(g_calls, 0, sizeof g_calls);
        // the pass the frame starts in (often the window, fb 0); its query begins with its first
        // command, after this swap
        g_pass[0] = (pass_t){ .fb = g_cur_fb }; g_npass = 1; g_q_wanted = 0;
    }
}

static so_shim g_shims[96];

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
        const char *d = getenv("UA_GL_TRACE_DRAWS");
        g_draws_max = d ? atoi(d) : 0;
        const char *st = getenv("UA_GL_TRACE_START");
        g_start = st ? atof(st) : 0;
        const char *gm = getenv("UA_GL_TRACE_GPU");
        g_gpu_mode = !gm || !*gm ? 3 : atoi(gm) >= 1 && atoi(gm) <= 3 ? atoi(gm) : 0;
#define W(name, var, fn) if ((var = real(name))) g_shims[k++] = (so_shim){ name, (void *)fn };
        W("glBindFramebuffer", r_BindFramebuffer, w_BindFramebuffer);
        W("glViewport", r_Viewport, w_Viewport);
        W("glClear", r_Clear, w_Clear);
        W("glInvalidateFramebuffer", r_Invalidate, w_Invalidate);
        W("glInvalidateSubFramebuffer", r_InvalidateSub, w_InvalidateSub);
        W("glDiscardFramebufferEXT", r_Discard, w_Discard);
#define WD(f, n) W(n, r_##f, w_##f)
        DRAWS(WD)
#undef WD
        W("glBufferData", r_BufferData, w_BufferData);
        W("glBufferSubData", r_BufferSubData, w_BufferSubData);
        W("glMapBufferRange", r_MapBufferRange, w_MapBufferRange);
        W("glTexImage2D", r_TexImage2D, w_TexImage2D);
        W("glTexSubImage2D", r_TexSubImage2D, w_TexSubImage2D);
        W("glCompressedTexImage2D", r_CompressedTexImage2D, w_CompressedTexImage2D);
        W("glCompressedTexSubImage2D", r_CompressedTexSubImage2D, w_CompressedTexSubImage2D);
        W("glUseProgram", r_UseProgram, w_UseProgram);
        W("glBlitFramebuffer", r_BlitFramebuffer, w_BlitFramebuffer);
        W("glCopyTexSubImage2D", r_CopyTexSubImage2D, w_CopyTexSubImage2D);
        W("glCopyTexImage2D", r_CopyTexImage2D, w_CopyTexImage2D);
        W("glCopyTexSubImage3D", r_CopyTexSubImage3D, w_CopyTexSubImage3D);
        W("glDispatchCompute", r_DispatchCompute, w_DispatchCompute);
        W("glDispatchComputeIndirect", r_DispatchComputeIndirect, w_DispatchComputeIndirect);
        W("glReadPixels", r_ReadPixels, w_ReadPixels);
        r_GetIntegerv = real("glGetIntegerv");
        if (g_gpu_mode > 0 && g_draws_max <= 0) {
            // GPU times: the queries are ours; the game's own time queries are watched for
            W("glBeginQueryEXT", r_BeginQueryEXT, w_BeginQueryEXT);
            W("glEndQueryEXT", r_EndQueryEXT, w_EndQueryEXT);
            W("glBeginQuery", r_BeginQuery, w_BeginQuery);
            W("glEndQuery", r_EndQuery, w_EndQuery);
            if (!r_BeginQueryEXT || !r_EndQueryEXT) { r_BeginQueryEXT = r_BeginQuery; r_EndQueryEXT = r_EndQuery; }
            if (!(r_GenQueries = real("glGenQueriesEXT"))) r_GenQueries = real("glGenQueries");
            if (!(r_GetQueryiv = real("glGetQueryivEXT"))) r_GetQueryiv = real("glGetQueryiv");
            if (!(r_GetQueryObjectuiv = real("glGetQueryObjectuivEXT"))) r_GetQueryObjectuiv = real("glGetQueryObjectuiv");
            r_GetQueryObjectui64v = real("glGetQueryObjectui64vEXT");
            r_GetString = real("glGetString");
            r_Flush = real("glFlush");
            r_Finish = real("glFinish");
        }
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
            r_IsEnabled = real("glIsEnabled");
            r_GetActiveAttrib = real("glGetActiveAttrib");
            r_GetAttribLocation = real("glGetAttribLocation");
            r_GetVertexAttribiv = real("glGetVertexAttribiv");
            r_GetVertexAttribPointerv = real("glGetVertexAttribPointerv");
            r_GetActiveUniformBlockiv = real("glGetActiveUniformBlockiv");
            r_GetIntegeri_v = real("glGetIntegeri_v");
            r_GetInteger64i_v = real("glGetInteger64i_v");
            W("glFlushMappedBufferRange", r_FlushMappedBufferRange, w_FlushMappedBufferRange);
            W("glUnmapBuffer", r_UnmapBuffer, w_UnmapBuffer);
            W("glBufferStorageEXT", r_BufferStorageEXT, w_BufferStorageEXT);
            W("glCopyBufferSubData", r_CopyBufferSubData, w_CopyBufferSubData);
            g_vs_hash = calloc(MAX_PROG, sizeof *g_vs_hash);
            g_fs_hash = calloc(MAX_PROG, sizeof *g_fs_hash);
            W("glAttachShader", r_AttachShader, w_AttachShader);
            if (!r_GetIntegerv || !r_ReadPixels || !g_vs_hash || !g_fs_hash) g_draws_max = 0;
        }
#undef W
        char from[80] = "";
        if (g_start > 0) snprintf(from, sizeof from, ", the first %g s after the first frame", g_start);
        fprintf(stderr, "[gl] tracing one frame every %g s%s (%d entry points)%s\n", g_every, from, k,
                g_draws_max > 0 ? ", its draws one by one" : "");
    }
    g_shims[k] = (so_shim){ NULL, NULL };
    return g_shims;
}
