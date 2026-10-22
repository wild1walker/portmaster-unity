// MSAA present of Unity 2017.1 - 2017.3 players: show the frame Unity drew.
//
// When a game's quality level uses MSAA, these players render through their "blit" present path
// whatever PlayerSettings.androidBlitType says (CheckAutoBlitType): a multisampled framebuffer of
// Unity's own that is resolved into a texture and drawn over the window at present. But the window
// setup creates that framebuffer before it turns the path on (the display manager turns it on
// right away only for androidBlitType Always), and only a framebuffer created with the path on
// becomes Unity's default framebuffer (2017.4 turns it on first and sets the buffers up again).
// Until the rendering size or the sample count changes (a second surface callback, or the surface
// destroyed and created again, does not do it), Unity draws each frame into the window itself
// (framebuffer 0), then resolves its own framebuffer, which nothing drew into, and draws that over
// the window: every frame black (2017.2, Shiver).
//
// So at that present resolve (a whole-window colour glBlitFramebuffer from a framebuffer whose
// colour buffer is a multisampled renderbuffer, into another framebuffer, while nothing was drawn
// into the read framebuffer since the last swap and something was drawn into framebuffer 0),
// framebuffer 0 is read instead: the resolve's texture gets the frame Unity drew, and the present
// draws it back. The frame has no MSAA (Unity drew it into the single-sampled window), but it is
// the game's. Once Unity draws into its own framebuffer, its blits are left alone. With the fix on,
// the window's EGL config is single-sampled as well (egl_glue_set_single_sample_window): these
// players take a multisampled config for the window when the quality level uses MSAA (2017.2 took a
// 4x one in the rig from a request that named no samples), where MSAA is lost anyway, and a resolve
// of a multisampled window into the copy texture would need identical formats.
//   UA_MSAA_FIX=0|1   off / on; default: on for Unity 2017.1 - 2017.3 (ua_host.c)
// glBindFramebuffer, glBlitFramebuffer, glClear and the draw calls are wrapped, only when on.
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "msaafix.h"

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned GLbitfield;

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_RENDERBUFFER 0x8D41
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_RENDERBUFFER_SAMPLES 0x8CAB
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_SAMPLES 0x80A9

static void (*r_BindFramebuffer)(GLenum, GLuint);
static void (*r_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
static void (*r_GetIntegerv)(GLenum, GLint *);
static void (*r_GetFramebufferAttachmentParameteriv)(GLenum, GLenum, GLenum, GLint *);
static void (*r_GetRenderbufferParameteriv)(GLenum, GLenum, GLint *);
static void (*r_BindRenderbuffer)(GLenum, GLuint);

static int g_on, g_width, g_height;
static long g_fixed;

// framebuffers drawn into (draws, clears) since the last swap (Unity's render thread only)
#define MAX_DRAWN 32
static GLuint g_drawn[MAX_DRAWN], g_draw_fb;
static int g_ndrawn, g_full, g_last_drawn = -1;

static void drawn(void)
{
    GLuint fb = g_draw_fb;
    if (g_last_drawn >= 0 && g_drawn[g_last_drawn] == fb) return;     // (the usual case: the same as the last draw)
    for (int i = 0; i < g_ndrawn; i++) if (g_drawn[i] == fb) { g_last_drawn = i; return; }
    if (g_ndrawn < MAX_DRAWN) { g_last_drawn = g_ndrawn; g_drawn[g_ndrawn++] = fb; }
    else g_full = 1;
}

static int was_drawn(GLuint fb)
{
    if (g_full) return 1;              // lost count: as if drawn (leave the blit alone)
    for (int i = 0; i < g_ndrawn; i++) if (g_drawn[i] == fb) return 1;
    return 0;
}

// the read framebuffer's colour buffer is a multisampled renderbuffer
static int read_multisampled(void)
{
    GLint type = 0, name = 0, samples = 0, rb = 0;
    r_GetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    if (type != GL_RENDERBUFFER) return 0;
    r_GetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
    r_GetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
    r_BindRenderbuffer(GL_RENDERBUFFER, (GLuint)name);
    r_GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &samples);
    r_BindRenderbuffer(GL_RENDERBUFFER, (GLuint)rb);
    return samples > 1;
}

static void w_BindFramebuffer(GLenum target, GLuint fb)
{
    r_BindFramebuffer(target, fb);
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) g_draw_fb = fb;
}

static void (*r_Clear)(GLbitfield);
static void (*r_DrawArrays)(GLenum, GLint, GLsizei);
static void (*r_DrawElements)(GLenum, GLsizei, GLenum, const void *);
static void (*r_DrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei);
static void (*r_DrawElementsInstanced)(GLenum, GLsizei, GLenum, const void *, GLsizei);
static void (*r_DrawRangeElements)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
static void w_Clear(GLbitfield m) { r_Clear(m); drawn(); }
static void w_DrawArrays(GLenum m, GLint f, GLsizei c) { r_DrawArrays(m, f, c); drawn(); }
static void w_DrawElements(GLenum m, GLsizei c, GLenum t, const void *i) { r_DrawElements(m, c, t, i); drawn(); }
static void w_DrawArraysInstanced(GLenum m, GLint f, GLsizei c, GLsizei n) { r_DrawArraysInstanced(m, f, c, n); drawn(); }
static void w_DrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n) { r_DrawElementsInstanced(m, c, t, i, n); drawn(); }
static void w_DrawRangeElements(GLenum m, GLuint a, GLuint b, GLsizei c, GLenum t, const void *i) { r_DrawRangeElements(m, a, b, c, t, i); drawn(); }

static void w_BlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                              GLbitfield mask, GLenum filter)
{
    if (mask == GL_COLOR_BUFFER_BIT && !sx0 && !sy0 && sx1 == g_width && sy1 == g_height &&
        !dx0 && !dy0 && dx1 == g_width && dy1 == g_height && was_drawn(0)) {
        GLint rf = 0, df = 0;
        r_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rf);
        r_GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &df);
        if (rf && df && rf != df && !was_drawn((GLuint)rf) && read_multisampled()) {
            if (!g_fixed++) {
                GLint ws = 0;
                r_BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                r_GetIntegerv(GL_SAMPLES, &ws);
                r_BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)df);
                fprintf(stderr, "[host] MSAA present: Unity drew the frame into the window (%d samples), not into its "
                        "multisampled framebuffer %d; the window is presented (UA_MSAA_FIX, msaafix.c)\n", ws, rf);
            }
            r_BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            r_BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
            r_BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rf);
            return;
        }
    }
    r_BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}

void msaafix_set_size(int width, int height) { g_width = width; g_height = height; }

void msaafix_frame(void)
{
    if (!g_on) return;
    g_ndrawn = 0; g_full = 0; g_last_drawn = -1;
    GLint df = 0;
    r_GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &df);     // what the next frame starts drawing into
    g_draw_fb = (GLuint)df;
}

static so_shim g_shims[9];

static void *gl(void *(*next)(const char *), const char *name)
{
    void *p = next ? next(name) : NULL;
    if (p) return p;
    static void *h;
    if (!h) h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    return h ? dlsym(h, name) : NULL;
}

const so_shim *msaafix_table(int on, void *(*next)(const char *name))
{
    memset(g_shims, 0, sizeof g_shims);
    g_on = 0;
    if (!on) return g_shims;                     // nothing looked up, nothing wrapped
    *(void **)&r_BindFramebuffer = gl(next, "glBindFramebuffer");
    *(void **)&r_BlitFramebuffer = gl(next, "glBlitFramebuffer");
    *(void **)&r_Clear = gl(next, "glClear");
    *(void **)&r_DrawArrays = gl(next, "glDrawArrays");
    *(void **)&r_DrawElements = gl(next, "glDrawElements");
    *(void **)&r_DrawArraysInstanced = gl(next, "glDrawArraysInstanced");
    *(void **)&r_DrawElementsInstanced = gl(next, "glDrawElementsInstanced");
    *(void **)&r_DrawRangeElements = gl(next, "glDrawRangeElements");
    *(void **)&r_GetIntegerv = gl(NULL, "glGetIntegerv");
    *(void **)&r_GetFramebufferAttachmentParameteriv = gl(NULL, "glGetFramebufferAttachmentParameteriv");
    *(void **)&r_GetRenderbufferParameteriv = gl(NULL, "glGetRenderbufferParameteriv");
    *(void **)&r_BindRenderbuffer = gl(NULL, "glBindRenderbuffer");
    g_on = r_BindFramebuffer && r_BlitFramebuffer && r_GetIntegerv && r_GetFramebufferAttachmentParameteriv &&
           r_GetRenderbufferParameteriv && r_BindRenderbuffer && r_Clear && r_DrawArrays && r_DrawElements;
    if (!g_on) fprintf(stderr, "[host] UA_MSAA_FIX: no GLES 3 framebuffer functions, off\n");
    if (g_on) {
        int k = 0;
        g_shims[k++] = (so_shim){ "glBindFramebuffer", (void *)w_BindFramebuffer };
        g_shims[k++] = (so_shim){ "glBlitFramebuffer", (void *)w_BlitFramebuffer };
        g_shims[k++] = (so_shim){ "glClear", (void *)w_Clear };
        g_shims[k++] = (so_shim){ "glDrawArrays", (void *)w_DrawArrays };
        g_shims[k++] = (so_shim){ "glDrawElements", (void *)w_DrawElements };
        if (r_DrawArraysInstanced) g_shims[k++] = (so_shim){ "glDrawArraysInstanced", (void *)w_DrawArraysInstanced };
        if (r_DrawElementsInstanced) g_shims[k++] = (so_shim){ "glDrawElementsInstanced", (void *)w_DrawElementsInstanced };
        if (r_DrawRangeElements) g_shims[k++] = (so_shim){ "glDrawRangeElements", (void *)w_DrawRangeElements };
    }
    return g_shims;
}
