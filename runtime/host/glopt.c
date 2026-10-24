// Tile-memory traffic a GLES frame can do without, on a tiler like the Mali, without changing a
// pixel. UA_GL_DSCLEAR=1 (default) / 0.
//
// A tiler renders a pass (the commands between two framebuffer changes) tile by tile in on-chip
// memory: at the pass's start it loads each attachment from memory unless the pass begins by
// clearing or invalidating it, and at its end stores it. A camera that leaves its framebuffer in
// the middle of its rendering and comes back (a grab pass: Unity copies the picture so far into a
// texture for an object that reads it, through another framebuffer; or an image effect drawn into
// the camera's target again) makes the driver store and load all its attachments each time. For
// the depth/stencil buffer that is often wasted: a 2D game clears it and then never writes it (its
// sprites test depth against the cleared value with depth writes off, or keep the stencil value),
// so it still holds exactly what the clear wrote.
//
// So for each depth/stencil buffer (a texture or renderbuffer attached to a framebuffer, the state
// kept per buffer: framebuffers may share one) this follows whether it holds only the values of
// its last whole clear: a glClear of all its components (depth and/or stencil) with depth writes
// on, all stencil bits writable, and no scissor (or one that covers the attachment). It stops
// holding them at a draw that may write it (the depth test on with depth writes on; the stencil
// test on with a writable bit and an operation other than GL_KEEP, front or back), a clear that is
// not whole, glClearBuffer*, a blit that writes depth or stencil into it, the game's own
// invalidation, a change of the framebuffer's attachments, or the buffer redefined or deleted.
// When a pass on a framebuffer whose buffer is in that state begins (its first draw, clear or blit
// after glBindFramebuffer), the buffer is cleared to those same values first, so the driver does
// not load it: nothing the GPU tests or reads changes. Unity's state (scissor, masks, clear values)
// is put back after that clear. Only the render thread's calls (the one that swaps) are followed;
// glopt_frame() names it, from the swap hook.
//
// Measured on the RG34XX-SP (Mali-G31): Hollow Knight 6000.0 (64-bit), King's Pass 30.6 -> 32.4 fps,
// its main menu 36.1 -> 38.7 (its cameras leave their framebuffer 3 - 6 times a frame for grab
// passes; docs/profiling.md).
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glopt.h"

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned GLbitfield;
typedef unsigned char GLboolean;

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_COLOR 0x1800
#define GL_DEPTH 0x1801
#define GL_STENCIL 0x1802
#define GL_DEPTH_STENCIL 0x84F9
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_TEXTURE 0x1702
#define GL_RENDERBUFFER 0x8D41
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_WIDTH 0x1000
#define GL_TEXTURE_HEIGHT 0x1001
#define GL_RENDERBUFFER_WIDTH 0x8D42
#define GL_RENDERBUFFER_HEIGHT 0x8D43
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_SCISSOR_TEST 0x0C11
#define GL_SCISSOR_BOX 0x0C10
#define GL_RASTERIZER_DISCARD 0x8C89
#define GL_DEPTH_WRITEMASK 0x0B72
#define GL_STENCIL_WRITEMASK 0x0B98
#define GL_STENCIL_BACK_WRITEMASK 0x8CA5
#define GL_STENCIL_FAIL 0x0B94
#define GL_STENCIL_PASS_DEPTH_FAIL 0x0B95
#define GL_STENCIL_PASS_DEPTH_PASS 0x0B96
#define GL_STENCIL_BACK_FAIL 0x8801
#define GL_STENCIL_BACK_PASS_DEPTH_FAIL 0x8802
#define GL_STENCIL_BACK_PASS_DEPTH_PASS 0x8803
#define GL_DEPTH_CLEAR_VALUE 0x0B73
#define GL_STENCIL_CLEAR_VALUE 0x0B91
#define GL_KEEP 0x1E00
#define GL_FRONT 0x0404
#define GL_BACK 0x0405

static int g_on;
static pthread_t g_thread;
static int g_have_thread;
static long g_restored, g_dropped;
static int g_sameclear;              // UA_GL_SAMECLEAR=1 (opt-in): a clear of what the buffer holds is left out (glpass.c for colour)

// ---- depth/stencil buffers: what they hold
#define MAX_DS 256
typedef struct {
    GLenum type; GLuint name;        // GL_TEXTURE / GL_RENDERBUFFER and its name (0: a free slot)
    int clean;                       // holds only its last whole clear's values
    float depth; GLint stencil;
} dsbuf;
static dsbuf g_ds[MAX_DS];
// textures attached as a depth/stencil buffer with the GL_TEXTURE_2D target (their size can be read)
#define MAX_TEX2D 256
static GLuint g_tex2d[MAX_TEX2D];
static int g_ntex2d;
static int is_tex2d(GLuint t) { for (int i = 0; i < g_ntex2d; i++) if (g_tex2d[i] == t) return 1; return 0; }
static void add_tex2d(GLuint t)
{
    if (!t || is_tex2d(t)) return;
    if (g_ntex2d < MAX_TEX2D) g_tex2d[g_ntex2d++] = t;
}

static dsbuf *ds_find(GLenum type, GLuint name, int add)
{
    dsbuf *free_slot = NULL;
    for (int i = 0; i < MAX_DS; i++) {
        if (g_ds[i].name == name && g_ds[i].type == type) return &g_ds[i];
        if (!g_ds[i].name && !free_slot) free_slot = &g_ds[i];
    }
    if (!add || !free_slot) return NULL;
    *free_slot = (dsbuf){ .type = type, .name = name };
    return free_slot;
}
static void ds_forget(GLenum type, GLuint name) { dsbuf *b = ds_find(type, name, 0); if (b) b->clean = 0; }

// ---- framebuffers: their depth/stencil buffer (looked up while bound)
#define MAX_FB 4096
typedef struct {
    unsigned char known;
    unsigned char has_d, has_s;      // depth / stencil components
    unsigned char usable;            // one buffer for both (or only one of them): followed
    GLenum type; GLuint name;        // that buffer
    int w, h;                        // its size (0: unknown)
} fbinfo;
static fbinfo g_fb[MAX_FB];
static GLuint g_cur;                 // the draw framebuffer
static int g_pass_started;           // a draw, clear or blit since it was bound

static void (*r_BindFramebuffer)(GLenum, GLuint);
static void (*r_Clear)(GLbitfield);
static void (*r_ClearBufferfi)(GLenum, GLint, float, GLint);
static void (*r_ClearBufferfv)(GLenum, GLint, const float *);
static void (*r_ClearBufferiv)(GLenum, GLint, const GLint *);
static void (*r_ClearBufferuiv)(GLenum, GLint, const GLuint *);
static void (*r_Invalidate)(GLenum, GLsizei, const GLenum *);
static void (*r_InvalidateSub)(GLenum, GLsizei, const GLenum *, GLint, GLint, GLsizei, GLsizei);
static void (*r_Discard)(GLenum, GLsizei, const GLenum *);
static void (*r_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
static void (*r_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static void (*r_FramebufferTextureLayer)(GLenum, GLenum, GLuint, GLint, GLint);
static void (*r_FramebufferTexture)(GLenum, GLenum, GLuint, GLint);
static void (*r_FramebufferTexture2DMS)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
static void (*r_FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static void (*r_DeleteFramebuffers)(GLsizei, const GLuint *);
static void (*r_DeleteTextures)(GLsizei, const GLuint *);
static void (*r_DeleteRenderbuffers)(GLsizei, const GLuint *);
static void (*r_TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (*r_TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
static void (*r_TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void (*r_RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
static void (*r_RenderbufferStorageMS)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void (*r_RenderbufferStorageMSEXT)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void (*r_CopyImageSubData)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei);
static void (*q_GetIntegerv)(GLenum, GLint *);
static void (*q_GetFloatv)(GLenum, float *);
static GLboolean (*q_IsEnabled)(GLenum);
static void (*q_GetFbAttachment)(GLenum, GLenum, GLenum, GLint *);
static void (*q_GetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *);
static void (*q_GetRenderbufferParameteriv)(GLenum, GLenum, GLint *);
static void (*q_BindTexture)(GLenum, GLuint);
static void (*q_BindRenderbuffer)(GLenum, GLuint);
static void (*s_Enable)(GLenum), (*s_Disable)(GLenum);
static void (*s_DepthMask)(GLboolean);
static void (*s_StencilMaskSeparate)(GLenum, GLuint);
static void (*s_ClearDepthf)(float);
static void (*s_ClearStencil)(GLint);

static int mine(void) { return g_have_thread && pthread_equal(pthread_self(), g_thread); }

// the bound draw framebuffer's depth/stencil buffer
static fbinfo *info(void)
{
    if (g_cur == 0 || g_cur >= MAX_FB) return NULL;
    fbinfo *f = &g_fb[g_cur];
    if (f->known) return f;
    *f = (fbinfo){ .known = 1 };
    GLint dt = 0, dn = 0, st = 0, sn = 0;
    q_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &dt);
    if (dt) q_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &dn);
    q_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &st);
    if (st) q_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &sn);
    f->has_d = dt == GL_TEXTURE || dt == GL_RENDERBUFFER;
    f->has_s = st == GL_TEXTURE || st == GL_RENDERBUFFER;
    if (!f->has_d && !f->has_s) return f;
    if (f->has_d && f->has_s && (dt != st || dn != sn)) return f;     // two buffers: not followed
    f->type = (GLenum)(f->has_d ? dt : st); f->name = (GLuint)(f->has_d ? dn : sn);
    f->usable = f->name != 0;
    // its size (whether a scissored clear covers it)
    GLint old = 0;
    if (f->type == GL_TEXTURE && is_tex2d(f->name)) {
        q_GetIntegerv(GL_TEXTURE_BINDING_2D, &old);
        q_BindTexture(GL_TEXTURE_2D, f->name);
        q_GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &f->w);
        q_GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &f->h);
        q_BindTexture(GL_TEXTURE_2D, (GLuint)old);
    } else if (f->type == GL_RENDERBUFFER) {
        q_GetIntegerv(GL_RENDERBUFFER_BINDING, &old);
        q_BindRenderbuffer(GL_RENDERBUFFER, f->name);
        q_GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &f->w);
        q_GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &f->h);
        q_BindRenderbuffer(GL_RENDERBUFFER, (GLuint)old);
    }
    return f;
}

static dsbuf *cur_ds(int add)
{
    fbinfo *f = info();
    return f && f->usable ? ds_find(f->type, f->name, add) : NULL;
}
static void dirty(void) { dsbuf *b = cur_ds(0); if (b) b->clean = 0; }

// the pass's first command: a buffer holding its clear's values is cleared to them (not loaded)
static void pass_start(GLbitfield unity_clears)
{
    if (g_pass_started) return;
    g_pass_started = 1;
    fbinfo *f = info();
    dsbuf *b = cur_ds(0);
    if (!b || !b->clean) return;
    GLbitfield m = (f->has_d ? GL_DEPTH_BUFFER_BIT : 0) | (f->has_s ? GL_STENCIL_BUFFER_BIT : 0);
    if ((unity_clears & m) == m || q_IsEnabled(GL_RASTERIZER_DISCARD)) return;   // (the game clears it itself)
    GLint sc = q_IsEnabled(GL_SCISSOR_TEST), dm = 0, sm = 0, sbm = 0, cs = 0;
    float cd = 1;
    q_GetIntegerv(GL_DEPTH_WRITEMASK, &dm);
    q_GetIntegerv(GL_STENCIL_WRITEMASK, &sm);
    q_GetIntegerv(GL_STENCIL_BACK_WRITEMASK, &sbm);
    q_GetFloatv(GL_DEPTH_CLEAR_VALUE, &cd);
    q_GetIntegerv(GL_STENCIL_CLEAR_VALUE, &cs);
    if (sc) s_Disable(GL_SCISSOR_TEST);
    s_DepthMask(1);
    s_StencilMaskSeparate(GL_FRONT, 0xffffffffu); s_StencilMaskSeparate(GL_BACK, 0xffffffffu);
    s_ClearDepthf(b->depth); s_ClearStencil(b->stencil);
    r_Clear(m);
    s_ClearDepthf(cd); s_ClearStencil(cs);
    s_DepthMask((GLboolean)(dm != 0));
    s_StencilMaskSeparate(GL_FRONT, (GLuint)sm); s_StencilMaskSeparate(GL_BACK, (GLuint)sbm);
    if (sc) s_Enable(GL_SCISSOR_TEST);
    g_restored++;
}

static void w_BindFramebuffer(GLenum t, GLuint fb)
{
    r_BindFramebuffer(t, fb);
    if (!mine() || (t != GL_FRAMEBUFFER && t != GL_DRAW_FRAMEBUFFER)) return;
    if (fb != g_cur) g_pass_started = 0;
    g_cur = fb;
}

static void attachments_changed(GLenum t)
{
    if (!mine()) return;
    GLint fb = 0;
    q_GetIntegerv(t == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER_BINDING : GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    if (fb > 0 && fb < MAX_FB) g_fb[fb].known = 0;      // looked up again
}
static int ds_attachment(GLenum a) { return a == GL_DEPTH_ATTACHMENT || a == GL_STENCIL_ATTACHMENT || a == GL_DEPTH_STENCIL_ATTACHMENT; }
static void w_FramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l)
{
    r_FramebufferTexture2D(t, a, tt, tex, l);
    if (mine() && tt == GL_TEXTURE_2D && l == 0 && ds_attachment(a)) add_tex2d(tex);
    attachments_changed(t);
}
static void w_FramebufferTextureLayer(GLenum t, GLenum a, GLuint tex, GLint l, GLint layer)
{ r_FramebufferTextureLayer(t, a, tex, l, layer); attachments_changed(t); }
static void w_FramebufferTexture(GLenum t, GLenum a, GLuint tex, GLint l) { r_FramebufferTexture(t, a, tex, l); attachments_changed(t); }
static void w_FramebufferTexture2DMS(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l, GLsizei s)
{
    r_FramebufferTexture2DMS(t, a, tt, tex, l, s);
    if (mine() && tt == GL_TEXTURE_2D && l == 0 && ds_attachment(a)) add_tex2d(tex);
    attachments_changed(t);
}
static void w_FramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint rb)
{ r_FramebufferRenderbuffer(t, a, rt, rb); attachments_changed(t); }
static void w_DeleteFramebuffers(GLsizei n, const GLuint *fbs)
{
    r_DeleteFramebuffers(n, fbs);
    if (!mine()) return;
    for (GLsizei i = 0; fbs && i < n; i++) if (fbs[i] < MAX_FB) g_fb[fbs[i]].known = 0;
}
// a buffer redefined or deleted: what it held is gone (its name may come back as another one)
static void deleted(GLenum type, GLuint name)
{
    dsbuf *b = ds_find(type, name, 0);
    if (b) b->name = 0;
    for (int i = 0; i < g_ntex2d; i++) if (type == GL_TEXTURE && g_tex2d[i] == name) g_tex2d[i] = g_tex2d[--g_ntex2d];
    for (int i = 1; i < MAX_FB; i++) if (g_fb[i].known && g_fb[i].usable && g_fb[i].type == type && g_fb[i].name == name) g_fb[i].known = 0;
}
static void w_DeleteTextures(GLsizei n, const GLuint *t)
{
    r_DeleteTextures(n, t);
    if (mine()) for (GLsizei i = 0; t && i < n; i++) if (ds_find(GL_TEXTURE, t[i], 0) || is_tex2d(t[i])) deleted(GL_TEXTURE, t[i]);
}
static void w_DeleteRenderbuffers(GLsizei n, const GLuint *r)
{
    r_DeleteRenderbuffers(n, r);
    if (mine()) for (GLsizei i = 0; r && i < n; i++) deleted(GL_RENDERBUFFER, r[i]);
}
static void bound_texture_changed(void)
{
    GLint t = 0;
    q_GetIntegerv(GL_TEXTURE_BINDING_2D, &t);
    if (t > 0) ds_forget(GL_TEXTURE, (GLuint)t);
}
static void w_TexImage2D(GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLint b, GLenum fm, GLenum ty, const void *p)
{ r_TexImage2D(t, l, f, w, h, b, fm, ty, p); if (t == GL_TEXTURE_2D) bound_texture_changed(); }
static void w_TexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *p)
{ r_TexSubImage2D(t, l, x, y, w, h, f, ty, p); if (t == GL_TEXTURE_2D) bound_texture_changed(); }
static void w_TexStorage2D(GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h)
{ r_TexStorage2D(t, l, f, w, h); if (t == GL_TEXTURE_2D) bound_texture_changed(); }
static void bound_renderbuffer_changed(void)
{
    GLint r = 0;
    q_GetIntegerv(GL_RENDERBUFFER_BINDING, &r);
    if (r > 0) ds_forget(GL_RENDERBUFFER, (GLuint)r);
}
static void w_RenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h) { r_RenderbufferStorage(t, f, w, h); bound_renderbuffer_changed(); }
static void w_RenderbufferStorageMS(GLenum t, GLsizei s, GLenum f, GLsizei w, GLsizei h)
{ r_RenderbufferStorageMS(t, s, f, w, h); bound_renderbuffer_changed(); }
static void w_RenderbufferStorageMSEXT(GLenum t, GLsizei s, GLenum f, GLsizei w, GLsizei h)
{ r_RenderbufferStorageMSEXT(t, s, f, w, h); bound_renderbuffer_changed(); }
static void w_CopyImageSubData(GLuint s, GLenum st, GLint sl, GLint sx, GLint sy, GLint sz, GLuint d, GLenum dt, GLint dl,
                               GLint dx, GLint dy, GLint dz, GLsizei w, GLsizei h, GLsizei dp)
{
    r_CopyImageSubData(s, st, sl, sx, sy, sz, d, dt, dl, dx, dy, dz, w, h, dp);
    ds_forget(dt == GL_RENDERBUFFER ? GL_RENDERBUFFER : GL_TEXTURE, d);
}

// a clear of the depth/stencil buffer that writes the values it already holds (whole or not): left out
static int redundant(GLbitfield ds)
{
    dsbuf *b = cur_ds(0);
    fbinfo *f = info();
    if (!b || !b->clean || !f) return 0;
    if ((ds & GL_DEPTH_BUFFER_BIT) && f->has_d) {
        GLint dm = 0;
        float cd = 1;
        q_GetIntegerv(GL_DEPTH_WRITEMASK, &dm);
        q_GetFloatv(GL_DEPTH_CLEAR_VALUE, &cd);
        if (dm && cd != b->depth) return 0;
    }
    if ((ds & GL_STENCIL_BUFFER_BIT) && f->has_s) {
        GLint sm = 0, cs = 0;
        q_GetIntegerv(GL_STENCIL_WRITEMASK, &sm);
        q_GetIntegerv(GL_STENCIL_CLEAR_VALUE, &cs);
        if ((cs ^ b->stencil) & sm & 0xff) return 0;
    }
    return 1;
}

static void w_Clear(GLbitfield m)
{
    if (!mine()) { r_Clear(m); return; }
    GLbitfield ds = m & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (ds && g_sameclear && redundant(ds)) {
        m &= ~ds;
        g_dropped++;
        if (!m) return;              // (nothing else in it: as if it had not been made; a pass that goes on restores it)
    }
    pass_start(m);
    r_Clear(m);
    if (!(m & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))) return;
    fbinfo *f = info();
    if (!f || !f->usable) return;
    dsbuf *b = ds_find(f->type, f->name, 1);
    if (!b) return;
    GLint dm = 0, sm = 0, sbm = 0, box[4] = { 0, 0, 0, 0 };
    q_GetIntegerv(GL_DEPTH_WRITEMASK, &dm);
    q_GetIntegerv(GL_STENCIL_WRITEMASK, &sm);
    q_GetIntegerv(GL_STENCIL_BACK_WRITEMASK, &sbm);
    int sc = q_IsEnabled(GL_SCISSOR_TEST);
    if (sc) q_GetIntegerv(GL_SCISSOR_BOX, box);
    int whole = !sc || (box[0] <= 0 && box[1] <= 0 && f->w > 0 && f->h > 0 && box[0] + box[2] >= f->w && box[1] + box[3] >= f->h);
    int d_ok = !f->has_d || ((m & GL_DEPTH_BUFFER_BIT) && dm);
    int s_ok = !f->has_s || ((m & GL_STENCIL_BUFFER_BIT) && (sm & 0xff) == 0xff && (sbm & 0xff) == 0xff);
    b->clean = whole && d_ok && s_ok && !q_IsEnabled(GL_RASTERIZER_DISCARD);
    if (b->clean) { q_GetFloatv(GL_DEPTH_CLEAR_VALUE, &b->depth); q_GetIntegerv(GL_STENCIL_CLEAR_VALUE, &b->stencil); }
}
static void cleared_buffer(GLenum buffer)
{
    if (!mine()) return;
    pass_start(0);
    if (buffer == GL_DEPTH || buffer == GL_STENCIL || buffer == GL_DEPTH_STENCIL) dirty();
}
// (a float argument: on armhf the player calls with softfp, SO_ABI, so_loader.h)
static SO_ABI void w_ClearBufferfi(GLenum b, GLint d, float v, GLint s) { if (mine()) pass_start(0); r_ClearBufferfi(b, d, v, s); cleared_buffer(b); }
static void w_ClearBufferfv(GLenum b, GLint d, const float *v) { if (mine()) pass_start(0); r_ClearBufferfv(b, d, v); cleared_buffer(b); }
static void w_ClearBufferiv(GLenum b, GLint d, const GLint *v) { if (mine()) pass_start(0); r_ClearBufferiv(b, d, v); cleared_buffer(b); }
static void w_ClearBufferuiv(GLenum b, GLint d, const GLuint *v) { if (mine()) pass_start(0); r_ClearBufferuiv(b, d, v); cleared_buffer(b); }
// the game's own invalidation: the contents are undefined from then on (nothing to keep)
static void invalidated(GLenum t, GLsizei n, const GLenum *a)
{
    if (!mine() || t == GL_READ_FRAMEBUFFER) return;
    for (GLsizei i = 0; a && i < n; i++)
        if (a[i] == GL_DEPTH || a[i] == GL_STENCIL || a[i] == GL_DEPTH_ATTACHMENT || a[i] == GL_STENCIL_ATTACHMENT ||
            a[i] == GL_DEPTH_STENCIL_ATTACHMENT) dirty();
}
static void w_Invalidate(GLenum t, GLsizei n, const GLenum *a) { r_Invalidate(t, n, a); invalidated(t, n, a); }
static void w_InvalidateSub(GLenum t, GLsizei n, const GLenum *a, GLint x, GLint y, GLsizei w, GLsizei h)
{ r_InvalidateSub(t, n, a, x, y, w, h); invalidated(t, n, a); }
static void w_Discard(GLenum t, GLsizei n, const GLenum *a) { r_Discard(t, n, a); invalidated(t, n, a); }
static void w_BlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield m, GLenum fl)
{
    if (mine()) pass_start(0);
    r_BlitFramebuffer(a, b, c, d, e, f, g, h, m, fl);
    if (mine() && (m & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))) dirty();
}

// before a draw: does it write the depth/stencil buffer?
static void before_draw(void)
{
    if (!mine()) return;
    pass_start(0);
    dsbuf *b = cur_ds(0);
    if (!b || !b->clean) return;
    fbinfo *f = info();
    if (f->has_d && q_IsEnabled(GL_DEPTH_TEST)) {
        GLint dm = 0;
        q_GetIntegerv(GL_DEPTH_WRITEMASK, &dm);
        if (dm) { b->clean = 0; return; }
    }
    if (f->has_s && q_IsEnabled(GL_STENCIL_TEST)) {
        static const GLenum ops[6] = { GL_STENCIL_FAIL, GL_STENCIL_PASS_DEPTH_FAIL, GL_STENCIL_PASS_DEPTH_PASS,
                                       GL_STENCIL_BACK_FAIL, GL_STENCIL_BACK_PASS_DEPTH_FAIL, GL_STENCIL_BACK_PASS_DEPTH_PASS };
        GLint sm = 0, sbm = 0;
        q_GetIntegerv(GL_STENCIL_WRITEMASK, &sm); q_GetIntegerv(GL_STENCIL_BACK_WRITEMASK, &sbm);
        for (int i = 0; i < 6; i++) {
            GLint op = GL_KEEP;
            if (!((i < 3 ? sm : sbm) & 0xff)) continue;
            q_GetIntegerv(ops[i], &op);
            if (op != GL_KEEP) { b->clean = 0; return; }
        }
    }
}

// ---- every draw entry point (as gltrace.c counts them)
#define DRAW(f, args, call) static void (*r_##f) args; static void w_##f args { before_draw(); r_##f call; }
DRAW(DrawArrays, (GLenum m, GLint a, GLsizei c), (m, a, c))
DRAW(DrawElements, (GLenum m, GLsizei c, GLenum t, const void *i), (m, c, t, i))
DRAW(DrawArraysInstanced, (GLenum m, GLint a, GLsizei c, GLsizei n), (m, a, c, n))
DRAW(DrawElementsInstanced, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n), (m, c, t, i, n))
DRAW(DrawArraysInstancedEXT, (GLenum m, GLint a, GLsizei c, GLsizei n), (m, a, c, n))
DRAW(DrawElementsInstancedEXT, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n), (m, c, t, i, n))
DRAW(DrawRangeElements, (GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i), (m, s, e, c, t, i))
DRAW(DrawElementsBaseVertex, (GLenum m, GLsizei c, GLenum t, const void *i, GLint b), (m, c, t, i, b))
DRAW(DrawElementsBaseVertexOES, (GLenum m, GLsizei c, GLenum t, const void *i, GLint b), (m, c, t, i, b))
DRAW(DrawElementsBaseVertexEXT, (GLenum m, GLsizei c, GLenum t, const void *i, GLint b), (m, c, t, i, b))
DRAW(DrawRangeElementsBaseVertex, (GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i, GLint b), (m, s, e, c, t, i, b))
DRAW(DrawRangeElementsBaseVertexOES, (GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i, GLint b), (m, s, e, c, t, i, b))
DRAW(DrawRangeElementsBaseVertexEXT, (GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i, GLint b), (m, s, e, c, t, i, b))
DRAW(DrawElementsInstancedBaseVertex, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint b), (m, c, t, i, n, b))
DRAW(DrawElementsInstancedBaseVertexOES, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint b), (m, c, t, i, n, b))
DRAW(DrawElementsInstancedBaseVertexEXT, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint b), (m, c, t, i, n, b))
DRAW(DrawArraysInstancedBaseInstanceEXT, (GLenum m, GLint a, GLsizei c, GLsizei n, GLuint b), (m, a, c, n, b))
DRAW(DrawElementsInstancedBaseInstanceEXT, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLuint b), (m, c, t, i, n, b))
DRAW(DrawElementsInstancedBaseVertexBaseInstanceEXT, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint v, GLuint b),
     (m, c, t, i, n, v, b))
DRAW(MultiDrawArraysEXT, (GLenum m, const GLint *a, const GLsizei *c, GLsizei n), (m, a, c, n))
DRAW(MultiDrawElementsEXT, (GLenum m, const GLsizei *c, GLenum t, const void *const *i, GLsizei n), (m, c, t, i, n))
DRAW(MultiDrawElementsBaseVertexEXT, (GLenum m, const GLsizei *c, GLenum t, const void *const *i, GLsizei n, const GLint *b),
     (m, c, t, i, n, b))
DRAW(MultiDrawElementsBaseVertexOES, (GLenum m, const GLsizei *c, GLenum t, const void *const *i, GLsizei n, const GLint *b),
     (m, c, t, i, n, b))
DRAW(DrawArraysIndirect, (GLenum m, const void *p), (m, p))
DRAW(DrawElementsIndirect, (GLenum m, GLenum t, const void *p), (m, t, p))

void glopt_frame(void)
{
    if (!g_on) return;
    if (!g_have_thread) {                       // the swapping thread, and the framebuffer it draws into
        GLint fb = 0;
        q_GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
        g_cur = (GLuint)fb; g_pass_started = 1;
        g_thread = pthread_self(); g_have_thread = 1;
    }
    static long frames;
    if (++frames % 3600 == 0)
        fprintf(stderr, "[glopt] %ld frames: %ld depth/stencil buffers cleared instead of loaded, %ld clears of them left out\n", frames, g_restored, g_dropped);
}

static so_shim g_shims[64];

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
        if (gpa) p = gpa(name);
    }
    return p;
}

const so_shim *glopt_table(void *(*next)(const char *name))
{
    memset(g_shims, 0, sizeof g_shims);
    memset(g_ds, 0, sizeof g_ds);
    memset(g_fb, 0, sizeof g_fb);
    g_ntex2d = 0;
    g_on = 0; g_have_thread = 0; g_cur = 0; g_pass_started = 0; g_restored = 0; g_dropped = 0;
    const char *sc = getenv("UA_GL_SAMECLEAR");
    g_sameclear = sc && (!strcmp(sc, "1") || !strcmp(sc, "check"));
    const char *e = getenv("UA_GL_DSCLEAR");
    if (e && !strcmp(e, "0")) return g_shims;
#define Q(var, name) *(void **)&var = gl(NULL, name)
    Q(q_GetIntegerv, "glGetIntegerv"); Q(q_GetFloatv, "glGetFloatv"); Q(q_IsEnabled, "glIsEnabled");
    Q(q_GetFbAttachment, "glGetFramebufferAttachmentParameteriv"); Q(q_GetTexLevelParameteriv, "glGetTexLevelParameteriv");
    Q(q_GetRenderbufferParameteriv, "glGetRenderbufferParameteriv"); Q(q_BindTexture, "glBindTexture");
    Q(q_BindRenderbuffer, "glBindRenderbuffer"); Q(s_Enable, "glEnable"); Q(s_Disable, "glDisable"); Q(s_DepthMask, "glDepthMask");
    Q(s_StencilMaskSeparate, "glStencilMaskSeparate"); Q(s_ClearDepthf, "glClearDepthf"); Q(s_ClearStencil, "glClearStencil");
#undef Q
    int k = 0, ok = 1;
    // (required: wrapped, the real one found)
#define REQ(f, n) do { *(void **)&r_##f = gl(next, n); if (r_##f) g_shims[k++] = (so_shim){ n, (void *)w_##f }; else ok = 0; } while (0)
#define OPT(f, n) do { *(void **)&r_##f = gl(next, n); if (r_##f) g_shims[k++] = (so_shim){ n, (void *)w_##f }; } while (0)
    REQ(BindFramebuffer, "glBindFramebuffer"); REQ(Clear, "glClear"); REQ(BlitFramebuffer, "glBlitFramebuffer");
    REQ(Invalidate, "glInvalidateFramebuffer"); REQ(FramebufferTexture2D, "glFramebufferTexture2D");
    REQ(FramebufferRenderbuffer, "glFramebufferRenderbuffer"); REQ(DeleteFramebuffers, "glDeleteFramebuffers");
    REQ(DeleteTextures, "glDeleteTextures"); REQ(DeleteRenderbuffers, "glDeleteRenderbuffers"); REQ(TexImage2D, "glTexImage2D");
    REQ(TexSubImage2D, "glTexSubImage2D"); REQ(RenderbufferStorage, "glRenderbufferStorage");
    REQ(ClearBufferfi, "glClearBufferfi"); REQ(ClearBufferfv, "glClearBufferfv"); REQ(ClearBufferiv, "glClearBufferiv");
    REQ(ClearBufferuiv, "glClearBufferuiv"); REQ(DrawArrays, "glDrawArrays"); REQ(DrawElements, "glDrawElements");
    OPT(InvalidateSub, "glInvalidateSubFramebuffer"); OPT(Discard, "glDiscardFramebufferEXT");
    OPT(FramebufferTextureLayer, "glFramebufferTextureLayer"); OPT(FramebufferTexture, "glFramebufferTexture");
    OPT(FramebufferTexture2DMS, "glFramebufferTexture2DMultisampleEXT"); OPT(TexStorage2D, "glTexStorage2D");
    OPT(RenderbufferStorageMS, "glRenderbufferStorageMultisample");
    OPT(RenderbufferStorageMSEXT, "glRenderbufferStorageMultisampleEXT"); OPT(CopyImageSubData, "glCopyImageSubData");
    OPT(DrawArraysInstanced, "glDrawArraysInstanced"); OPT(DrawElementsInstanced, "glDrawElementsInstanced");
    OPT(DrawArraysInstancedEXT, "glDrawArraysInstancedEXT"); OPT(DrawElementsInstancedEXT, "glDrawElementsInstancedEXT");
    OPT(DrawRangeElements, "glDrawRangeElements"); OPT(DrawElementsBaseVertex, "glDrawElementsBaseVertex");
    OPT(DrawElementsBaseVertexOES, "glDrawElementsBaseVertexOES"); OPT(DrawElementsBaseVertexEXT, "glDrawElementsBaseVertexEXT");
    OPT(DrawRangeElementsBaseVertex, "glDrawRangeElementsBaseVertex");
    OPT(DrawRangeElementsBaseVertexOES, "glDrawRangeElementsBaseVertexOES");
    OPT(DrawRangeElementsBaseVertexEXT, "glDrawRangeElementsBaseVertexEXT");
    OPT(DrawElementsInstancedBaseVertex, "glDrawElementsInstancedBaseVertex");
    OPT(DrawElementsInstancedBaseVertexOES, "glDrawElementsInstancedBaseVertexOES");
    OPT(DrawElementsInstancedBaseVertexEXT, "glDrawElementsInstancedBaseVertexEXT");
    OPT(DrawArraysInstancedBaseInstanceEXT, "glDrawArraysInstancedBaseInstanceEXT");
    OPT(DrawElementsInstancedBaseInstanceEXT, "glDrawElementsInstancedBaseInstanceEXT");
    OPT(DrawElementsInstancedBaseVertexBaseInstanceEXT, "glDrawElementsInstancedBaseVertexBaseInstanceEXT");
    OPT(MultiDrawArraysEXT, "glMultiDrawArraysEXT"); OPT(MultiDrawElementsEXT, "glMultiDrawElementsEXT");
    OPT(MultiDrawElementsBaseVertexEXT, "glMultiDrawElementsBaseVertexEXT");
    OPT(MultiDrawElementsBaseVertexOES, "glMultiDrawElementsBaseVertexOES");
    OPT(DrawArraysIndirect, "glDrawArraysIndirect"); OPT(DrawElementsIndirect, "glDrawElementsIndirect");
#undef REQ
#undef OPT
    if (!ok || !(q_GetIntegerv && q_GetFloatv && q_IsEnabled && q_GetFbAttachment && q_GetTexLevelParameteriv &&
                 q_GetRenderbufferParameteriv && q_BindTexture && q_BindRenderbuffer && s_Enable && s_Disable && s_DepthMask &&
                 s_StencilMaskSeparate && s_ClearDepthf && s_ClearStencil)) {
        memset(g_shims, 0, sizeof g_shims);     // (a GLES 2 driver: nothing wrapped)
        return g_shims;
    }
    g_on = 1;
    return g_shims;
}
