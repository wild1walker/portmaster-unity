// UA_PRESENT_THREAD=1: on the SDL display with a render size (display_sdl.c; SDL's KMSDRM and X11 drivers), Unity's frames
// shown by a present thread of the host's, so that Unity's render thread never waits for the GPU at its swap.
//
// Mali's GBM driver (BattleXP G350 / R36S class, RK3326, r13p0 on a 4.4 kernel, SDL's KMSDRM) waits in
// eglSwapBuffers until the GPU has finished the frame (the display has no fences of the GPU's to wait
// on): Unity's render thread then waits for the GPU at every swap, and the GPU waits for the render
// thread while it builds the next frame. On the G350 in Greenpath that wait was ~10 ms of a ~56 ms frame.
//
// With it on, Unity's contexts are current without a surface (EGL_KHR_surfaceless_context) and Unity's
// "framebuffer 0" is one of three framebuffers of the host's at the render size (a colour texture each,
// one depth-stencil renderbuffer for all): glBindFramebuffer(0) binds the current one, and what Unity
// reads back about framebuffer 0 reads as before (the binding as 0, GL_BACK / GL_DEPTH / GL_STENCIL as
// its attachments, in invalidations, draw/read buffers and attachment queries). At Unity's swap the
// render thread only fences the frame and goes on with the next of the three (waiting, on the GPU,
// until the present thread's copy of it is done); the present thread, with a context of its own that
// shares Unity's, waits for the frame's fence on the GPU, scales it over the window in one
// glBlitFramebuffer (linear, as display.c's scaler; one pass fewer than its copy and scale) and swaps
// (it waits for the GPU there). Unity may run up to two frames ahead of the screen.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glpresent.h"

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned GLbitfield;
typedef unsigned char GLboolean; typedef void *GLsync; typedef uint64_t GLuint64;

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_RENDERBUFFER 0x8D41
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_RGBA8 0x8058
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_LINEAR 0x2601
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_FRAMEBUFFER_DEFAULT 0x8218
#define GL_BACK 0x0405
#define GL_COLOR 0x1800
#define GL_DEPTH 0x1801
#define GL_STENCIL 0x1802
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull

static so_shim g_shims[16];
static int g_want;                              // UA_PRESENT_THREAD=1 (the wrappers are in)
static int g_rw, g_rh, g_dw, g_dh;              // render and window sizes (glpresent_start)
static const glpresent_window *g_win;

// the next layer's functions (what Unity's calls go on to) and the driver's own (the present thread's)
static void (*r_BindFramebuffer)(GLenum, GLuint), (*r_GetIntegerv)(GLenum, GLint *);
static void (*r_GetFramebufferAttachmentParameteriv)(GLenum, GLenum, GLenum, GLint *);
static void (*r_InvalidateFramebuffer)(GLenum, GLsizei, const GLenum *), (*r_DiscardFramebufferEXT)(GLenum, GLsizei, const GLenum *);
static void (*r_DrawBuffers)(GLsizei, const GLenum *), (*r_ReadBuffer)(GLenum);
static void (*r_GenTextures)(GLsizei, GLuint *), (*r_BindTexture)(GLenum, GLuint);
static void (*r_TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei), (*r_TexParameteri)(GLenum, GLenum, GLint);
static void (*r_GenRenderbuffers)(GLsizei, GLuint *), (*r_BindRenderbuffer)(GLenum, GLuint);
static void (*r_RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
static void (*r_GenFramebuffers)(GLsizei, GLuint *), (*r_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static void (*r_FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static GLenum (*r_CheckFramebufferStatus)(GLenum);
static GLsync (*r_FenceSync)(GLenum, GLbitfield);
static void (*r_WaitSync)(GLsync, GLbitfield, GLuint64), (*r_DeleteSync)(GLsync), (*r_Flush)(void);
static struct {                                 // the driver's, on the present thread
    void (*BindFramebuffer)(GLenum, GLuint), (*GenFramebuffers)(GLsizei, GLuint *);
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    void (*BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void (*InvalidateFramebuffer)(GLenum, GLsizei, const GLenum *);
    GLsync (*FenceSync)(GLenum, GLbitfield);
    void (*WaitSync)(GLsync, GLbitfield, GLuint64), (*DeleteSync)(GLsync), (*Flush)(void);
} D;

// ---- Unity's framebuffer 0: three frames per context of Unity's (framebuffers are not shared)
#define SLOTS 3
typedef struct { void *ctx; GLuint fbo[SLOTS], tex[SLOTS], ds; int cur, ok; } ring;
static ring g_rings[4];
static int g_nrings;
static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread ring *t_ring;                   // the calling thread's current context's (NULL: off here)
static __thread int t_draw0, t_read0;           // Unity has "framebuffer 0" bound to draw / to read

static int is_ours(const ring *r, GLuint fb) { for (int i = 0; i < SLOTS; i++) if (r->fbo[i] == fb) return 1; return 0; }
static int target_is0(GLenum t) { return t == GL_READ_FRAMEBUFFER ? t_read0 : t_draw0; }
static GLenum att0(GLenum a)                    // a default framebuffer's buffer as our framebuffer's attachment
{
    switch (a) {
    case GL_BACK: case GL_COLOR: return GL_COLOR_ATTACHMENT0;
    case GL_DEPTH: return GL_DEPTH_ATTACHMENT;
    case GL_STENCIL: return GL_STENCIL_ATTACHMENT;
    default: return a;
    }
}

static void w_BindFramebuffer(GLenum t, GLuint fb)
{
    ring *r = t_ring;
    if (r) {
        int zero = fb == 0;
        if (zero) fb = r->fbo[r->cur];
        if (t == GL_FRAMEBUFFER) t_draw0 = t_read0 = zero;
        else if (t == GL_DRAW_FRAMEBUFFER) t_draw0 = zero;
        else if (t == GL_READ_FRAMEBUFFER) t_read0 = zero;
    }
    r_BindFramebuffer(t, fb);
}
static void w_GetIntegerv(GLenum p, GLint *v)
{
    r_GetIntegerv(p, v);
    ring *r = t_ring;
    if (r && v && (p == GL_DRAW_FRAMEBUFFER_BINDING || p == GL_READ_FRAMEBUFFER_BINDING) && *v > 0 && is_ours(r, (GLuint)*v)) *v = 0;
}
static void w_GetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum p, GLint *v)
{
    if (!t_ring || !target_is0(t)) { r_GetFramebufferAttachmentParameteriv(t, a, p, v); return; }
    r_GetFramebufferAttachmentParameteriv(t, att0(a), p, v);
    if (v && p == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE) *v = GL_FRAMEBUFFER_DEFAULT;
    else if (v && p == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) *v = 0;
}
static void invalidate(void (*f)(GLenum, GLsizei, const GLenum *), GLenum t, GLsizei n, const GLenum *a)
{
    if (!t_ring || !target_is0(t) || n <= 0 || n > 8) { f(t, n, a); return; }
    GLenum m[8];
    for (GLsizei i = 0; i < n; i++) m[i] = att0(a[i]);
    f(t, n, m);
}
static void w_InvalidateFramebuffer(GLenum t, GLsizei n, const GLenum *a) { invalidate(r_InvalidateFramebuffer, t, n, a); }
static void w_DiscardFramebufferEXT(GLenum t, GLsizei n, const GLenum *a) { invalidate(r_DiscardFramebufferEXT, t, n, a); }
static void w_DrawBuffers(GLsizei n, const GLenum *b)
{
    if (!t_ring || !t_draw0 || n != 1 || !b || b[0] != GL_BACK) { r_DrawBuffers(n, b); return; }
    GLenum c = GL_COLOR_ATTACHMENT0;
    r_DrawBuffers(1, &c);
}
static void w_ReadBuffer(GLenum b) { r_ReadBuffer(t_ring && t_read0 && b == GL_BACK ? GL_COLOR_ATTACHMENT0 : b); }

// the three frames of Unity's context ctx, made on it (current on this thread)
static ring *ring_for(void *ctx)
{
    pthread_mutex_lock(&g_ring_lock);
    ring *r = NULL;
    for (int i = 0; i < g_nrings; i++) if (g_rings[i].ctx == ctx) r = &g_rings[i];
    if (!r && g_nrings < (int)(sizeof g_rings / sizeof *g_rings)) {
        r = &g_rings[g_nrings++];
        memset(r, 0, sizeof *r);
        r->ctx = ctx;
        GLint tex0 = 0, rb0 = 0;
        r_GetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
        r_GetIntegerv(GL_RENDERBUFFER_BINDING, &rb0);
        r_GenTextures(SLOTS, r->tex);
        r_GenRenderbuffers(1, &r->ds);
        r_BindRenderbuffer(GL_RENDERBUFFER, r->ds);
        r_RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, g_rw, g_rh);
        r_GenFramebuffers(SLOTS, r->fbo);
        r->ok = 1;
        for (int i = 0; i < SLOTS; i++) {
            r_BindTexture(GL_TEXTURE_2D, r->tex[i]);
            r_TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, g_rw, g_rh);
            r_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            r_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            r_BindFramebuffer(GL_FRAMEBUFFER, r->fbo[i]);
            r_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex[i], 0);
            r_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, r->ds);
            if (r_CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) r->ok = 0;
        }
        r_BindTexture(GL_TEXTURE_2D, (GLuint)tex0);
        r_BindRenderbuffer(GL_RENDERBUFFER, (GLuint)rb0);
        fprintf(stderr, "[present] Unity's context %p: framebuffer 0 is %d frames of %dx%d of the host's%s\n", ctx, SLOTS, g_rw, g_rh,
                r->ok ? "" : " (INCOMPLETE: off)");
    }
    pthread_mutex_unlock(&g_ring_lock);
    return r && r->ok ? r : NULL;
}

// ---- the present thread
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static ring *g_shown;                           // the ring whose frames are shown (the swapping thread's)
static GLsync g_fence[SLOTS], g_read[SLOTS];    // a frame's end (Unity's context); the present thread's copy of it
static int g_busy[SLOTS], g_q[SLOTS], g_qn;
static int g_started, g_running;
static void *g_pctx;
static long g_frames;
static int g_interval = -1, g_interval_set = -2;

static void *present_main(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "ua_present");
    if (g_win->make_current(g_win->user, g_pctx) != 0) {
        fprintf(stderr, "[present] the present context could not be made current with the window: frames are not shown\n");
        return NULL;
    }
    GLuint fbo[SLOTS];
    D.GenFramebuffers(SLOTS, fbo);
    for (int i = 0; i < SLOTS; i++) {
        D.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo[i]);
        D.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_shown->tex[i], 0);
    }
    D.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    static const GLenum all[3] = { GL_COLOR, GL_DEPTH, GL_STENCIL };
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_qn) pthread_cond_wait(&g_cond, &g_lock);
        int s = g_q[0];
        memmove(g_q, g_q + 1, (size_t)--g_qn * sizeof *g_q);
        GLsync f = g_fence[s];
        g_fence[s] = NULL;
        int iv = g_interval;
        pthread_mutex_unlock(&g_lock);
        if (iv != g_interval_set) { g_win->swap_interval(g_win->user, iv < 0 ? 1 : iv); g_interval_set = iv; }
        if (f) { D.WaitSync(f, 0, GL_TIMEOUT_IGNORED); D.DeleteSync(f); }
        D.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo[s]);
        D.InvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 3, all);    // (the whole window is drawn: nothing of it loaded)
        D.BlitFramebuffer(0, 0, g_rw, g_rh, 0, 0, g_dw, g_dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        GLsync rd = D.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        D.Flush();
        pthread_mutex_lock(&g_lock);
        g_read[s] = rd;
        g_busy[s] = 0;
        pthread_cond_broadcast(&g_cond);
        pthread_mutex_unlock(&g_lock);
        g_win->swap(g_win->user);
        g_frames++;
    }
    return NULL;
}

// ---- the display's calls (display_sdl.c)
int glpresent_wanted(void) { return g_want; }

void glpresent_start(int rw, int rh, int dw, int dh, const glpresent_window *w)
{
    g_rw = rw; g_rh = rh; g_dw = dw; g_dh = dh; g_win = w;
    fprintf(stderr, "[present] UA_PRESENT_THREAD: Unity's %dx%d frames shown by a present thread, scaled to %dx%d\n", rw, rh, dw, dh);
}

int glpresent_active(void) { return g_win != NULL; }

void glpresent_made_current(void *ctx)
{
    if (!g_win || !ctx) { t_ring = NULL; t_draw0 = t_read0 = 0; return; }
    ring *r = ring_for(ctx);
    t_ring = r;
    t_draw0 = t_read0 = r != NULL;
    if (r) r_BindFramebuffer(GL_FRAMEBUFFER, r->fbo[r->cur]);   // (a context made current reads framebuffer 0 as bound)
}

void glpresent_swap_interval(int interval)
{
    pthread_mutex_lock(&g_lock);
    g_interval = interval;
    pthread_mutex_unlock(&g_lock);
}

int glpresent_swap(void)
{
    ring *r = t_ring;
    if (!r) return -1;
    if (!g_started) {                           // the first swap: the present thread, its context sharing this one
        g_started = 1;
        g_shown = r;
        g_pctx = g_win->create_context(g_win->user, r->ctx);
        pthread_t t;
        if (!g_pctx || pthread_create(&t, NULL, present_main, NULL) != 0) {
            fprintf(stderr, "[present] no present context or thread: frames are not shown\n");
            return -1;
        }
        pthread_detach(t);
        g_running = 1;
    }
    if (r != g_shown || !g_running) return -1;
    int s = r->cur, next = (s + 1) % SLOTS;
    GLsync f = r_FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    r_Flush();
    pthread_mutex_lock(&g_lock);
    g_fence[s] = f;
    g_busy[s] = 1;
    g_q[g_qn++] = s;
    pthread_cond_broadcast(&g_cond);
    while (g_busy[next]) pthread_cond_wait(&g_cond, &g_lock);   // (two frames ahead of the screen: the oldest not copied yet)
    GLsync rd = g_read[next];
    g_read[next] = NULL;
    pthread_mutex_unlock(&g_lock);
    if (rd) { r_WaitSync(rd, 0, GL_TIMEOUT_IGNORED); r_DeleteSync(rd); }   // (on the GPU: the copy reads it before this frame draws)
    r->cur = next;
    if (t_draw0 && t_read0) r_BindFramebuffer(GL_FRAMEBUFFER, r->fbo[next]);
    else if (t_draw0) r_BindFramebuffer(GL_DRAW_FRAMEBUFFER, r->fbo[next]);
    else if (t_read0) r_BindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo[next]);
    return 0;
}

long glpresent_frames(void) { return g_frames; }

unsigned glpresent_fb0(void) { ring *r = t_ring; return r ? r->fbo[r->cur] : 0; }

static void *gl_driver(const char *name)
{
    static void *h;
    if (!h) h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    return h ? dlsym(h, name) : NULL;
}

const so_shim *glpresent_table(void *(*next)(const char *name))
{
    memset(g_shims, 0, sizeof g_shims);
    const char *e = getenv("UA_PRESENT_THREAD");
    if (!e || strcmp(e, "1")) return g_shims;
#define N(f) do { void *p = next ? next("gl" #f) : NULL; *(void **)&r_##f = p ? p : gl_driver("gl" #f); } while (0)
    N(BindFramebuffer); N(GetIntegerv); N(GetFramebufferAttachmentParameteriv); N(InvalidateFramebuffer); N(DiscardFramebufferEXT);
    N(DrawBuffers); N(ReadBuffer); N(GenTextures); N(BindTexture); N(TexStorage2D); N(TexParameteri); N(GenRenderbuffers);
    N(BindRenderbuffer); N(RenderbufferStorage); N(GenFramebuffers); N(FramebufferTexture2D); N(FramebufferRenderbuffer);
    N(CheckFramebufferStatus); N(FenceSync); N(WaitSync); N(DeleteSync); N(Flush);
#undef N
#define L(f) *(void **)&D.f = gl_driver("gl" #f)
    L(BindFramebuffer); L(GenFramebuffers); L(FramebufferTexture2D); L(BlitFramebuffer); L(InvalidateFramebuffer);
    L(FenceSync); L(WaitSync); L(DeleteSync); L(Flush);
#undef L
    if (!r_BindFramebuffer || !r_GetIntegerv || !r_GetFramebufferAttachmentParameteriv || !r_InvalidateFramebuffer || !r_DrawBuffers ||
        !r_ReadBuffer || !r_GenTextures || !r_BindTexture || !r_TexStorage2D || !r_TexParameteri || !r_GenRenderbuffers ||
        !r_BindRenderbuffer || !r_RenderbufferStorage || !r_GenFramebuffers || !r_FramebufferTexture2D || !r_FramebufferRenderbuffer ||
        !r_CheckFramebufferStatus || !r_FenceSync || !r_WaitSync || !r_DeleteSync || !r_Flush || !D.BindFramebuffer ||
        !D.GenFramebuffers || !D.FramebufferTexture2D || !D.BlitFramebuffer || !D.InvalidateFramebuffer || !D.FenceSync || !D.WaitSync ||
        !D.DeleteSync || !D.Flush) {
        fprintf(stderr, "[present] GLES 3 functions missing: UA_PRESENT_THREAD off\n");
        return g_shims;
    }
    int k = 0;
    g_shims[k++] = (so_shim){ "glBindFramebuffer", (void *)w_BindFramebuffer };
    g_shims[k++] = (so_shim){ "glGetIntegerv", (void *)w_GetIntegerv };
    g_shims[k++] = (so_shim){ "glGetFramebufferAttachmentParameteriv", (void *)w_GetFramebufferAttachmentParameteriv };
    g_shims[k++] = (so_shim){ "glInvalidateFramebuffer", (void *)w_InvalidateFramebuffer };
    if (r_DiscardFramebufferEXT) g_shims[k++] = (so_shim){ "glDiscardFramebufferEXT", (void *)w_DiscardFramebufferEXT };
    g_shims[k++] = (so_shim){ "glDrawBuffers", (void *)w_DrawBuffers };
    g_shims[k++] = (so_shim){ "glReadBuffer", (void *)w_ReadBuffer };
    g_want = 1;
    return g_shims;
}
