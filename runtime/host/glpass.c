// Passes a GLES frame can do without, on a tiler like the Mali, without changing a pixel.
//
// UA_GL_SAMECLEAR=1 / 0 (default: opt-in) / check: a clear that would write what the buffer already holds.
//   A pass that only clears a colour buffer still costs a tiler a whole pass: every tile is
//   written to memory. Unity clears some render textures every frame that nothing draws into
//   (Hollow Knight: a 720x450 "darkness cutout" texture cleared to 0 each frame), so the clear
//   writes the values the texture already holds. This follows, for each colour image (a texture
//   or renderbuffer attached as a framebuffer's colour buffer), whether it holds only the colour
//   of its last whole clear: glClear of the colour buffer with no scissor (or one that covers the
//   image), all of its channels writable and rasterizer discard off. It stops holding it at
//   anything that may write it: a draw, blit or glClearBuffer into a framebuffer it is attached
//   to, the game's own invalidation, any texture upload or copy into it, its redefinition or
//   deletion, an image binding that can write it. A later whole colour clear of the same image to
//   the same colour (with the same dither state: GL leaves dithering to the window position and
//   the value only) is then not passed on but held back: if the pass goes on with a draw, blit or
//   another clear, the clear is made right before it (so the tiler still starts the pass with a
//   clear, not a load), with the game's state put back afterwards; if the game leaves the
//   framebuffer first, the clear is never made. Either way the image holds the same values.
//   Only the render thread's draws are followed (glpass_frame names it, from the swap hook);
//   uploads and deletions from any thread count.
//
// Measured on the RG34XX-SP (Mali-G31): exact, but nothing to gain in Hollow Knight's King's Pass (its
// clear-only pass costs nothing that shows), so opt-in; docs/profiling.md.
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glpass.h"

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned GLbitfield;
typedef unsigned char GLboolean; typedef intptr_t GLintptr; typedef intptr_t GLsizeiptr;

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_COLOR 0x1800
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL 0x8CD2
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE 0x8CD3
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER 0x8CD4
#define GL_TEXTURE 0x1702
#define GL_RENDERBUFFER 0x8D41
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_WIDTH 0x1000
#define GL_TEXTURE_HEIGHT 0x1001
#define GL_RENDERBUFFER_WIDTH 0x8D42
#define GL_RENDERBUFFER_HEIGHT 0x8D43
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_BINDING_3D 0x806A
#define GL_TEXTURE_BINDING_2D_ARRAY 0x8C1D
#define GL_TEXTURE_BINDING_CUBE_MAP 0x8514
#define GL_TEXTURE_BINDING_EXTERNAL_OES 0x8D67
#define GL_TEXTURE_BINDING_2D_MULTISAMPLE 0x9104
#define GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY 0x9105
#define GL_TEXTURE_BINDING_CUBE_MAP_ARRAY 0x900A
#define GL_TEXTURE_BINDING_BUFFER 0x8C2C
#define GL_TEXTURE_3D 0x806F
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z 0x851A
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#define GL_TEXTURE_2D_MULTISAMPLE 0x9100
#define GL_TEXTURE_2D_MULTISAMPLE_ARRAY 0x9102
#define GL_TEXTURE_CUBE_MAP_ARRAY 0x9009
#define GL_TEXTURE_BUFFER 0x8C2A
#define GL_SCISSOR_TEST 0x0C11
#define GL_SCISSOR_BOX 0x0C10
#define GL_RASTERIZER_DISCARD 0x8C89
#define GL_DITHER 0x0BD0
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_DRAW_BUFFER0 0x8825
#define GL_MAX_COLOR_ATTACHMENTS 0x8CDF
#define GL_NONE 0
#define GL_READ_ONLY 0x88B8
#define GL_EXTENSIONS 0x1F03
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9

static int g_sameclear;              // UA_GL_SAMECLEAR
static pthread_t g_thread;
static int g_have_thread;
static long g_skipped, g_late, g_wipes;  // clears held back and never made; made later, before the pass's next command; foreign_check()s
static int g_srgb_ctl;               // EXT_sRGB_write_control (GL_FRAMEBUFFER_SRGB_EXT can be queried)
static int mine(void) { return g_have_thread && pthread_equal(pthread_self(), g_thread); }
// Another thread's draw, clear, attachment, upload or deletion: what this follows may be wrong
// from then on, so the render thread forgets all of it before it next decides anything.
static volatile int g_foreign;
static int mine_or_note(void) { if (mine()) return 1; g_foreign = 1; return 0; }

// ---- colour images that hold one colour everywhere (their last whole clear's)
#define MAX_UIMG 64
typedef struct { GLenum type; GLuint name; float v[4]; unsigned char dither, srgb; } uimg;
static uimg g_uimg[MAX_UIMG];
static int g_nuimg;                  // (entries in use; name 0 marks a free one below it)
static uimg *uimg_find(GLenum type, GLuint name)
{
    for (int i = 0; i < g_nuimg; i++) if (g_uimg[i].name == name && g_uimg[i].type == type) return &g_uimg[i];
    return NULL;
}
static void uimg_forget(GLenum type, GLuint name)
{
    uimg *u = uimg_find(type, name);
    if (!u) return;
    *u = g_uimg[--g_nuimg];
}
static uimg *uimg_add(GLenum type, GLuint name)
{
    uimg *u = uimg_find(type, name);
    if (u) return u;
    if (g_nuimg >= MAX_UIMG) return NULL;
    u = &g_uimg[g_nuimg++];
    *u = (uimg){ .type = type, .name = name };
    return u;
}

// ---- framebuffers: their colour attachments (looked up while bound, again after a change)
#define MAX_FB 4096
#define MAX_ATT 8
typedef struct { GLenum type; GLuint name; unsigned char plain; } att_t;     // plain: level 0, layer/face 0, not multisampled
typedef struct {
    unsigned char known, n, simple;  // simple: one colour buffer, attachment 0, drawn to by draw buffer 0
    att_t att[MAX_ATT];
    int w, h;                        // attachment 0's size (0: unknown)
} fbinfo;
static fbinfo g_fb[MAX_FB];
static GLuint g_cur;                 // the draw framebuffer (render thread)
static int g_pending;                // a clear of g_cur's colour buffer held back
static int g_maxatt = 4;

// textures attached as a colour buffer with the GL_TEXTURE_2D target (their size can be read), never
// multisampled-to-texture (EXT_multisampled_render_to_texture: its image is the resolve's)
#define MAX_TEX2D 512
static GLuint g_tex2d[MAX_TEX2D];
static int g_ntex2d;
static int is_tex2d(GLuint t) { for (int i = 0; i < g_ntex2d; i++) if (g_tex2d[i] == t) return 1; return 0; }
static void add_tex2d(GLuint t) { if (t && !is_tex2d(t) && g_ntex2d < MAX_TEX2D) g_tex2d[g_ntex2d++] = t; }
static void drop_tex2d(GLuint t) { for (int i = 0; i < g_ntex2d; i++) if (g_tex2d[i] == t) { g_tex2d[i] = g_tex2d[--g_ntex2d]; return; } }

static void (*r_BindFramebuffer)(GLenum, GLuint);
static void (*r_Clear)(GLbitfield);
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
static void (*r_DrawBuffers)(GLsizei, const GLenum *);
static void (*r_DeleteFramebuffers)(GLsizei, const GLuint *);
static void (*r_DeleteTextures)(GLsizei, const GLuint *);
static void (*r_DeleteRenderbuffers)(GLsizei, const GLuint *);
static void (*r_RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
static void (*r_RenderbufferStorageMS)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void (*r_RenderbufferStorageMSEXT)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void (*r_CopyImageSubData)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei);
static void (*r_BindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
static void (*q_GetIntegerv)(GLenum, GLint *);
static void (*q_GetFloatv)(GLenum, float *);
static GLboolean (*q_IsEnabled)(GLenum);
static void (*q_GetFbAttachment)(GLenum, GLenum, GLenum, GLint *);
static void (*q_GetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *);
static void (*q_GetRenderbufferParameteriv)(GLenum, GLenum, GLint *);
static void (*q_BindTexture)(GLenum, GLuint);
static void (*q_BindRenderbuffer)(GLenum, GLuint);
static void (*q_BindBuffer)(GLenum, GLuint);
static const unsigned char *(*q_GetString)(GLenum);
static void (*s_Enable)(GLenum), (*s_Disable)(GLenum);
static void (*s_ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
static void (*s_ClearColor)(float, float, float, float);

// the bound draw (or read) framebuffer's colour attachments
static fbinfo *info_of(GLenum target, GLuint fb)
{
    if (fb == 0 || fb >= MAX_FB) return NULL;
    fbinfo *f = &g_fb[fb];
    if (f->known) return f;
    *f = (fbinfo){ .known = 1 };
    for (int i = 0; i < g_maxatt && i < MAX_ATT; i++) {
        GLenum a = GL_COLOR_ATTACHMENT0 + (GLenum)i;
        GLint t = 0, n = 0;
        q_GetFbAttachment(target, a, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &t);
        if (t != GL_TEXTURE && t != GL_RENDERBUFFER) continue;
        q_GetFbAttachment(target, a, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &n);
        att_t *e = &f->att[f->n++];
        *e = (att_t){ .type = (GLenum)t, .name = (GLuint)n, .plain = n != 0 };
        if (t == GL_TEXTURE && !is_tex2d((GLuint)n)) e->plain = 0;
        if (t == GL_TEXTURE && e->plain) {
            GLint level = 0, face = 0, layer = 0;
            q_GetFbAttachment(target, a, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
            q_GetFbAttachment(target, a, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE, &face);
            q_GetFbAttachment(target, a, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER, &layer);
            if (level || face || layer) e->plain = 0;
        }
        if (i == 0 && e->plain) {
            GLint old = 0;
            if (t == GL_TEXTURE) {
                q_GetIntegerv(GL_TEXTURE_BINDING_2D, &old);
                q_BindTexture(GL_TEXTURE_2D, (GLuint)n);
                q_GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &f->w);
                q_GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &f->h);
                q_BindTexture(GL_TEXTURE_2D, (GLuint)old);
            } else {
                q_GetIntegerv(GL_RENDERBUFFER_BINDING, &old);
                q_BindRenderbuffer(GL_RENDERBUFFER, (GLuint)n);
                q_GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &f->w);
                q_GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &f->h);
                q_BindRenderbuffer(GL_RENDERBUFFER, (GLuint)old);
            }
        }
    }
    GLint db = GL_NONE;
    q_GetIntegerv(GL_DRAW_BUFFER0, &db);
    f->simple = f->n == 1 && f->att[0].plain && f->w > 0 && f->h > 0 && db == GL_COLOR_ATTACHMENT0 && target != GL_READ_FRAMEBUFFER;
    // (GL_DRAW_BUFFER0 is the draw framebuffer's: an info looked up through the read binding is
    // never simple; draws only need its attachments)
    if (target == GL_READ_FRAMEBUFFER) f->known = 0;
    return f;
}
static fbinfo *cur_info(void) { return info_of(GL_DRAW_FRAMEBUFFER, g_cur); }

// what another thread did: forget everything (on the render thread)
static void foreign_check(void)
{
    if (!g_foreign) return;
    g_foreign = 0; g_wipes++;
    g_nuimg = 0; g_ntex2d = 0; g_pending = 0;
    for (int i = 0; i < MAX_FB; i++) g_fb[i].known = 0;
}

// something may write the colour buffers of the draw framebuffer (or of fb through target)
static void written(GLenum target, GLuint fb)
{
    if (!g_nuimg) return;
    fbinfo *f = info_of(target, fb);
    for (int i = 0; f && i < f->n; i++) uimg_forget(f->att[i].type, f->att[i].name);
}

// the held-back clear, made now (the pass goes on)
static void make_pending(void)
{
    if (!g_pending) return;
    g_pending = 0;
    uimg *u = NULL;
    fbinfo *f = cur_info();
    if (f && f->simple) u = uimg_find(f->att[0].type, f->att[0].name);
    if (!u) return;                  // (cannot happen: the image changes only through this thread's calls)
    float cc[4] = { 0, 0, 0, 0 };
    GLint m[4] = { 1, 1, 1, 1 };
    q_GetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    q_GetIntegerv(GL_COLOR_WRITEMASK, m);
    int sc = q_IsEnabled(GL_SCISSOR_TEST), di = q_IsEnabled(GL_DITHER), rd = q_IsEnabled(GL_RASTERIZER_DISCARD);
    int sr = g_srgb_ctl ? q_IsEnabled(GL_FRAMEBUFFER_SRGB_EXT) : 0;
    if (sc) s_Disable(GL_SCISSOR_TEST);
    if (rd) s_Disable(GL_RASTERIZER_DISCARD);
    if (di != u->dither) (u->dither ? s_Enable : s_Disable)(GL_DITHER);
    if (g_srgb_ctl && sr != u->srgb) (u->srgb ? s_Enable : s_Disable)(GL_FRAMEBUFFER_SRGB_EXT);
    s_ColorMask(1, 1, 1, 1);
    s_ClearColor(u->v[0], u->v[1], u->v[2], u->v[3]);
    r_Clear(GL_COLOR_BUFFER_BIT);
    s_ClearColor(cc[0], cc[1], cc[2], cc[3]);
    s_ColorMask((GLboolean)(m[0] != 0), (GLboolean)(m[1] != 0), (GLboolean)(m[2] != 0), (GLboolean)(m[3] != 0));
    if (g_srgb_ctl && sr != u->srgb) (sr ? s_Enable : s_Disable)(GL_FRAMEBUFFER_SRGB_EXT);
    if (di != u->dither) (di ? s_Enable : s_Disable)(GL_DITHER);
    if (rd) s_Enable(GL_RASTERIZER_DISCARD);
    if (sc) s_Enable(GL_SCISSOR_TEST);
    g_skipped--; g_late++;
}

// UA_GL_SAMECLEAR=check: the clears this would leave out are made, and the image read back before
// and after (as RGBA8): a clear that changes a pixel would have been wrong to leave out
static int g_check;
static long g_checked, g_check_bad;
static unsigned char *g_check_buf[2];
static size_t g_check_size;
static void (*q_ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*q_BindFramebuffer)(GLenum, GLuint);
static void (*q_PixelStorei)(GLenum, GLint);
static void check_clear(fbinfo *f)
{
    size_t n = (size_t)f->w * (size_t)f->h * 4;
    if (n > g_check_size) {
        for (int i = 0; i < 2; i++) { free(g_check_buf[i]); g_check_buf[i] = malloc(n); }
        g_check_size = g_check_buf[0] && g_check_buf[1] ? n : 0;
    }
    if (n > g_check_size) { r_Clear(GL_COLOR_BUFFER_BIT); return; }
    GLint rfb = 0, pbo = 0;
    q_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    q_GetIntegerv(0x88ED, &pbo);                                    // GL_PIXEL_PACK_BUFFER_BINDING
    q_BindFramebuffer(GL_READ_FRAMEBUFFER, g_cur);
    if (pbo) q_BindBuffer(0x88EB, 0);
    // (the pack state the game may have changed: row length, skips, alignment)
    static const GLenum pk[4] = { 0x0D05, 0x0D02, 0x0D03, 0x0D04 };
    GLint ps[4];
    for (int i = 0; i < 4; i++) { q_GetIntegerv(pk[i], &ps[i]); q_PixelStorei(pk[i], i ? 0 : 4); }
    q_ReadPixels(0, 0, f->w, f->h, 0x1908, 0x1401, g_check_buf[0]);  // GL_RGBA, GL_UNSIGNED_BYTE
    r_Clear(GL_COLOR_BUFFER_BIT);
    q_ReadPixels(0, 0, f->w, f->h, 0x1908, 0x1401, g_check_buf[1]);
    for (int i = 0; i < 4; i++) q_PixelStorei(pk[i], ps[i]);
    if (pbo) q_BindBuffer(0x88EB, (GLuint)pbo);
    q_BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    g_checked++;
    if (memcmp(g_check_buf[0], g_check_buf[1], n)) {
        size_t d = 0;
        for (size_t i = 0; i < n; i += 4) d += memcmp(g_check_buf[0] + i, g_check_buf[1] + i, 4) != 0;
        g_check_bad++;
        fprintf(stderr, "[glpass] check: the clear of fb%u (%dx%d) changed %zu pixels: it would have been wrong to leave it out\n",
                g_cur, f->w, f->h, d);
    }
}

// ---- glClear: the colour part's effect on what the image holds
static void w_Clear(GLbitfield m)
{
    if (!mine_or_note()) { r_Clear(m); return; }
    foreign_check();
    if (g_cur == 0 || !(m & GL_COLOR_BUFFER_BIT)) {
        make_pending();
        r_Clear(m);
        return;
    }
    fbinfo *f = cur_info();
    int whole = 0, di = 0, sr = 0;
    float v[4] = { 0, 0, 0, 0 };
    if (f && f->simple && !q_IsEnabled(GL_RASTERIZER_DISCARD)) {
        GLint mask[4] = { 0, 0, 0, 0 }, box[4] = { 0, 0, 0, 0 };
        q_GetIntegerv(GL_COLOR_WRITEMASK, mask);
        int sc = q_IsEnabled(GL_SCISSOR_TEST);
        if (sc) q_GetIntegerv(GL_SCISSOR_BOX, box);
        whole = mask[0] && mask[1] && mask[2] && mask[3] &&
                (!sc || (box[0] <= 0 && box[1] <= 0 && box[0] + box[2] >= f->w && box[1] + box[3] >= f->h));
        q_GetFloatv(GL_COLOR_CLEAR_VALUE, v);
        di = q_IsEnabled(GL_DITHER);
        sr = g_srgb_ctl ? q_IsEnabled(GL_FRAMEBUFFER_SRGB_EXT) : 0;
    }
    uimg *u = whole ? uimg_find(f->att[0].type, f->att[0].name) : NULL;
    if (u && !memcmp(u->v, v, sizeof v) && u->dither == di && u->srgb == sr) {
        if (g_check) { check_clear(f); if (m & ~GL_COLOR_BUFFER_BIT) r_Clear(m & ~GL_COLOR_BUFFER_BIT); return; }
        if (!g_pending) g_skipped++;
        g_pending = 1;               // (it holds these values already: made only if the pass goes on)
        if (m & ~GL_COLOR_BUFFER_BIT) r_Clear(m & ~GL_COLOR_BUFFER_BIT);   // (depth/stencil: glopt.c's)
        return;
    }
    make_pending();
    r_Clear(m);
    written(GL_DRAW_FRAMEBUFFER, g_cur);
    if (whole && (u = uimg_add(f->att[0].type, f->att[0].name))) {
        memcpy(u->v, v, sizeof v);
        u->dither = (unsigned char)di; u->srgb = (unsigned char)sr;
    }
}

// before the pass's next draw, blit or clear: the held-back clear first, and what it writes
static void before_write(void)
{
    if (!mine_or_note()) return;
    make_pending();
    written(GL_DRAW_FRAMEBUFFER, g_cur);
}

static void w_BindFramebuffer(GLenum t, GLuint fb)
{
    r_BindFramebuffer(t, fb);
    if (!mine() || (t != GL_FRAMEBUFFER && t != GL_DRAW_FRAMEBUFFER)) return;
    if (fb != g_cur) g_pending = 0;  // the pass ends: a clear still held back is not needed
    g_cur = fb;
}

static void attachments_changed(GLenum t)
{
    if (!mine_or_note()) return;    // (another context's framebuffers: not these)
    GLint fb = 0;
    q_GetIntegerv(t == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER_BINDING : GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    if (fb > 0 && fb < MAX_FB) g_fb[fb].known = 0;
    if ((GLuint)fb == g_cur) make_pending();
}
static void w_FramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l)
{
    attachments_changed(t);
    r_FramebufferTexture2D(t, a, tt, tex, l);
    attachments_changed(t);
    if (mine() && tt == GL_TEXTURE_2D && l == 0) add_tex2d(tex);
}
static void w_FramebufferTextureLayer(GLenum t, GLenum a, GLuint tex, GLint l, GLint layer)
{ attachments_changed(t); r_FramebufferTextureLayer(t, a, tex, l, layer); attachments_changed(t); }
static void w_FramebufferTexture(GLenum t, GLenum a, GLuint tex, GLint l)
{ attachments_changed(t); r_FramebufferTexture(t, a, tex, l); attachments_changed(t); }
static void w_FramebufferTexture2DMS(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l, GLsizei s)
{
    attachments_changed(t);
    r_FramebufferTexture2DMS(t, a, tt, tex, l, s);
    attachments_changed(t);
    if (!mine()) return;
    drop_tex2d(tex); uimg_forget(GL_TEXTURE, tex);
    for (int i = 0; i < MAX_FB; i++) g_fb[i].known = 0;      // (wherever else it is attached)
}
static void w_FramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint rb)
{ attachments_changed(t); r_FramebufferRenderbuffer(t, a, rt, rb); attachments_changed(t); }
static void w_DrawBuffers(GLsizei n, const GLenum *b) { if (mine()) make_pending(); r_DrawBuffers(n, b); attachments_changed(GL_DRAW_FRAMEBUFFER); }
static void w_DeleteFramebuffers(GLsizei n, const GLuint *fbs)
{
    for (GLsizei i = 0; mine_or_note() && fbs && i < n; i++) {
        if (fbs[i] < MAX_FB) g_fb[fbs[i]].known = 0;
        if (mine() && fbs[i] == g_cur) { g_pending = 0; g_cur = 0; }   // (deleting the bound one binds 0)
    }
    r_DeleteFramebuffers(n, fbs);
}
// an image redefined or deleted: what it held is gone (its name may come back as another one)
static void gone(GLenum type, GLuint name)
{
    if (!mine_or_note()) return;
    uimg_forget(type, name);
    if (type == GL_TEXTURE) drop_tex2d(name);
    for (int i = 1; i < MAX_FB; i++) {
        fbinfo *f = &g_fb[i];
        if (!f->known) continue;
        for (int k = 0; k < f->n; k++) if (f->att[k].type == type && f->att[k].name == name) { f->known = 0; break; }
    }
}
static void w_DeleteTextures(GLsizei n, const GLuint *t)
{
    if (mine()) make_pending();
    r_DeleteTextures(n, t);
    for (GLsizei i = 0; t && i < n; i++) gone(GL_TEXTURE, t[i]);
}
static void w_DeleteRenderbuffers(GLsizei n, const GLuint *r)
{
    if (mine()) make_pending();
    r_DeleteRenderbuffers(n, r);
    for (GLsizei i = 0; r && i < n; i++) gone(GL_RENDERBUFFER, r[i]);
}
static void bound_renderbuffer_changed(void)
{
    GLint r = 0;
    q_GetIntegerv(GL_RENDERBUFFER_BINDING, &r);
    if (r > 0) gone(GL_RENDERBUFFER, (GLuint)r);
}
static void w_RenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h)
{ if (mine()) make_pending(); r_RenderbufferStorage(t, f, w, h); bound_renderbuffer_changed(); }
static void w_RenderbufferStorageMS(GLenum t, GLsizei s, GLenum f, GLsizei w, GLsizei h)
{ if (mine()) make_pending(); r_RenderbufferStorageMS(t, s, f, w, h); bound_renderbuffer_changed(); }
static void w_RenderbufferStorageMSEXT(GLenum t, GLsizei s, GLenum f, GLsizei w, GLsizei h)
{ if (mine()) make_pending(); r_RenderbufferStorageMSEXT(t, s, f, w, h); bound_renderbuffer_changed(); }

// ---- texture uploads and copies: the texture bound to that target (on the calling thread's context)
static GLenum binding_of(GLenum target)
{
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) return GL_TEXTURE_BINDING_CUBE_MAP;
    switch (target) {
    case GL_TEXTURE_2D: return GL_TEXTURE_BINDING_2D;
    case GL_TEXTURE_3D: return GL_TEXTURE_BINDING_3D;
    case GL_TEXTURE_2D_ARRAY: return GL_TEXTURE_BINDING_2D_ARRAY;
    case GL_TEXTURE_CUBE_MAP: return GL_TEXTURE_BINDING_CUBE_MAP;
    case GL_TEXTURE_EXTERNAL_OES: return GL_TEXTURE_BINDING_EXTERNAL_OES;
    case GL_TEXTURE_2D_MULTISAMPLE: return GL_TEXTURE_BINDING_2D_MULTISAMPLE;
    case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: return GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY;
    case GL_TEXTURE_CUBE_MAP_ARRAY: return GL_TEXTURE_BINDING_CUBE_MAP_ARRAY;
    case GL_TEXTURE_BUFFER: return GL_TEXTURE_BINDING_BUFFER;
    default: return 0;
    }
}
static void tex_changed(GLenum target, int redefined)
{
    if (!mine_or_note()) return;
    if (!g_nuimg && !redefined) return;
    GLenum b = binding_of(target);
    GLint t = 0;
    if (b) q_GetIntegerv(b, &t);
    if (t <= 0) return;
    if (redefined) gone(GL_TEXTURE, (GLuint)t);
    else uimg_forget(GL_TEXTURE, (GLuint)t);
}
#define TEX(f, args, call, target, redefined) static void (*r_##f) args; \
    static void w_##f args { if (mine()) make_pending(); r_##f call; tex_changed(target, redefined); }
TEX(TexImage2D, (GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLint b, GLenum fm, GLenum ty, const void *p),
    (t, l, f, w, h, b, fm, ty, p), t, 1)
TEX(TexSubImage2D, (GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *p),
    (t, l, x, y, w, h, f, ty, p), t, 0)
TEX(TexStorage2D, (GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h), (t, l, f, w, h), t, 1)
TEX(TexStorage2DMS, (GLenum t, GLsizei s, GLenum f, GLsizei w, GLsizei h, GLboolean fx), (t, s, f, w, h, fx), t, 1)
TEX(CompressedTexImage2D, (GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei s, const void *d),
    (t, l, f, w, h, b, s, d), t, 1)
TEX(CompressedTexSubImage2D, (GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLsizei s, const void *d),
    (t, l, x, y, w, h, f, s, d), t, 0)
TEX(CopyTexImage2D, (GLenum t, GLint l, GLenum f, GLint x, GLint y, GLsizei w, GLsizei h, GLint b), (t, l, f, x, y, w, h, b), t, 1)
TEX(CopyTexSubImage2D, (GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h), (t, l, xo, yo, x, y, w, h), t, 0)
TEX(TexImage3D, (GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLsizei d, GLint b, GLenum fm, GLenum ty, const void *p),
    (t, l, f, w, h, d, b, fm, ty, p), t, 1)
TEX(TexSubImage3D, (GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum f, GLenum ty, const void *p),
    (t, l, x, y, z, w, h, d, f, ty, p), t, 0)
TEX(TexStorage3D, (GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h, GLsizei d), (t, l, f, w, h, d), t, 1)
TEX(CopyTexSubImage3D, (GLenum t, GLint l, GLint xo, GLint yo, GLint zo, GLint x, GLint y, GLsizei w, GLsizei h),
    (t, l, xo, yo, zo, x, y, w, h), t, 0)
TEX(CompressedTexImage3D, (GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLsizei d, GLint b, GLsizei s, const void *p),
    (t, l, f, w, h, d, b, s, p), t, 1)
TEX(CompressedTexSubImage3D, (GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum f, GLsizei s, const void *p),
    (t, l, x, y, z, w, h, d, f, s, p), t, 0)
TEX(GenerateMipmap, (GLenum t), (t), t, 0)
TEX(EGLImageTargetTexture2DOES, (GLenum t, void *img), (t, img), t, 1)
TEX(TexBuffer, (GLenum t, GLenum f, GLuint b), (t, f, b), t, 1)
TEX(TexBufferEXT, (GLenum t, GLenum f, GLuint b), (t, f, b), t, 1)
TEX(TexBufferOES, (GLenum t, GLenum f, GLuint b), (t, f, b), t, 1)
TEX(TexBufferRange, (GLenum t, GLenum f, GLuint b, GLintptr o, GLsizeiptr s), (t, f, b, o, s), t, 1)
#undef TEX
static void (*r_EGLImageTargetRenderbufferStorageOES)(GLenum, void *);
static void w_EGLImageTargetRenderbufferStorageOES(GLenum t, void *img)
{ if (mine()) make_pending(); r_EGLImageTargetRenderbufferStorageOES(t, img); bound_renderbuffer_changed(); }
static void w_CopyImageSubData(GLuint s, GLenum st, GLint sl, GLint sx, GLint sy, GLint sz, GLuint d, GLenum dt, GLint dl,
                               GLint dx, GLint dy, GLint dz, GLsizei w, GLsizei h, GLsizei dp)
{
    if (mine()) make_pending();
    r_CopyImageSubData(s, st, sl, sx, sy, sz, d, dt, dl, dx, dy, dz, w, h, dp);
    if (mine_or_note()) uimg_forget(dt == GL_RENDERBUFFER ? GL_RENDERBUFFER : GL_TEXTURE, d);
}
static void w_BindImageTexture(GLuint u, GLuint t, GLint l, GLboolean ly, GLint layer, GLenum a, GLenum f)
{
    r_BindImageTexture(u, t, l, ly, layer, a, f);
    if (a != GL_READ_ONLY && mine_or_note()) uimg_forget(GL_TEXTURE, t);   // (a shader may write it from now on)
}

// ---- clears, blits and the game's invalidation
static void cleared_buffer(GLenum buffer)
{
    if (!mine_or_note()) return;
    make_pending();
    if (buffer == GL_COLOR) written(GL_DRAW_FRAMEBUFFER, g_cur);
}
// (glClearBufferfi clears depth and stencil only: not wrapped here; it takes a float, and a host
// wrapper calling another one would need the softfp convention on armhf, SO_ABI)
static void w_ClearBufferfv(GLenum b, GLint d, const float *v) { cleared_buffer(b); r_ClearBufferfv(b, d, v); }
static void w_ClearBufferiv(GLenum b, GLint d, const GLint *v) { cleared_buffer(b); r_ClearBufferiv(b, d, v); }
static void w_ClearBufferuiv(GLenum b, GLint d, const GLuint *v) { cleared_buffer(b); r_ClearBufferuiv(b, d, v); }
// (the held-back clear is made first: the game's sequence, clear then invalidate, as it was)
static void invalidated(GLenum t, GLsizei n, const GLenum *a)
{
    if (!mine_or_note()) return;
    int colour = 0;
    for (GLsizei i = 0; a && i < n; i++) if (a[i] == GL_COLOR || (a[i] >= GL_COLOR_ATTACHMENT0 && a[i] < GL_COLOR_ATTACHMENT0 + 32)) colour = 1;
    if (!colour) return;
    if (t == GL_READ_FRAMEBUFFER) {
        GLint fb = 0;
        q_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &fb);
        written(GL_READ_FRAMEBUFFER, (GLuint)fb);
        return;
    }
    written(GL_DRAW_FRAMEBUFFER, g_cur);
}
static void w_Invalidate(GLenum t, GLsizei n, const GLenum *a) { if (mine()) make_pending(); r_Invalidate(t, n, a); invalidated(t, n, a); }
static void w_InvalidateSub(GLenum t, GLsizei n, const GLenum *a, GLint x, GLint y, GLsizei w, GLsizei h)
{ if (mine()) make_pending(); r_InvalidateSub(t, n, a, x, y, w, h); invalidated(t, n, a); }
static void w_Discard(GLenum t, GLsizei n, const GLenum *a) { if (mine()) make_pending(); r_Discard(t, n, a); invalidated(t, n, a); }
static void w_BlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield m, GLenum fl)
{
    if (mine_or_note()) {
        make_pending();
        if (m & GL_COLOR_BUFFER_BIT) written(GL_DRAW_FRAMEBUFFER, g_cur);
    }
    r_BlitFramebuffer(a, b, c, d, e, f, g, h, m, fl);
}

// ---- every draw entry point (as gltrace.c counts them)
#define DRAW(f, args, call) static void (*r_##f) args; static void w_##f args { before_write(); r_##f call; }
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
#undef DRAW

void glpass_frame(void)
{
    if (!g_sameclear) return;
    if (!g_have_thread) {                       // the swapping thread, and the framebuffer it draws into
        GLint fb = 0;
        q_GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
        g_cur = (GLuint)fb;
        GLint n = 0;
        q_GetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &n);
        if (n > 0) g_maxatt = n < MAX_ATT ? n : MAX_ATT;
        const char *ext = q_GetString ? (const char *)q_GetString(GL_EXTENSIONS) : NULL;
        g_srgb_ctl = ext && strstr(ext, "GL_EXT_sRGB_write_control") != NULL;
        g_thread = pthread_self(); g_have_thread = 1;
    }
    g_pending = 0;                              // (a clear held back over the swap: its pass is over)
    foreign_check();
    static long frames;
    if (++frames % 1200 == 0)
        fprintf(stderr, "[glpass] %ld frames: %ld clears left out (the buffer held their colour), %ld made later, %ld times forgotten (another thread)\n", frames, g_skipped, g_late, g_wipes);
    if (g_check && frames % 600 == 0)
        fprintf(stderr, "[glpass] check, %ld frames: %ld clears that would be left out made and read back, %ld changed the image\n", frames,
                g_checked, g_check_bad);
}

static so_shim g_shims[96];

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

const so_shim *glpass_table(void *(*next)(const char *name))
{
    memset(g_shims, 0, sizeof g_shims);
    memset(g_fb, 0, sizeof g_fb);
    g_nuimg = 0; g_ntex2d = 0; g_have_thread = 0; g_cur = 0; g_pending = 0; g_skipped = g_late = 0; g_foreign = 0;
    const char *e = getenv("UA_GL_SAMECLEAR");
    g_sameclear = e && (!strcmp(e, "1") || !strcmp(e, "check"));     // (opt-in: it gains nothing measurable in King's Pass)
    g_check = e && !strcmp(e, "check");
    if (!g_sameclear) return g_shims;
#define Q(var, name) *(void **)&var = gl(NULL, name)
    Q(q_GetIntegerv, "glGetIntegerv"); Q(q_GetFloatv, "glGetFloatv"); Q(q_IsEnabled, "glIsEnabled");
    Q(q_GetFbAttachment, "glGetFramebufferAttachmentParameteriv"); Q(q_GetTexLevelParameteriv, "glGetTexLevelParameteriv");
    Q(q_GetRenderbufferParameteriv, "glGetRenderbufferParameteriv"); Q(q_BindTexture, "glBindTexture");
    Q(q_BindRenderbuffer, "glBindRenderbuffer"); Q(q_GetString, "glGetString");
    Q(q_ReadPixels, "glReadPixels"); Q(q_BindFramebuffer, "glBindFramebuffer"); Q(q_BindBuffer, "glBindBuffer");
    Q(q_PixelStorei, "glPixelStorei");
    Q(s_Enable, "glEnable"); Q(s_Disable, "glDisable"); Q(s_ColorMask, "glColorMask"); Q(s_ClearColor, "glClearColor");
#undef Q
    int k = 0, ok = 1;
    // (required: wrapped, the real one found)
#define REQ(f, n) do { *(void **)&r_##f = gl(next, n); if (r_##f) g_shims[k++] = (so_shim){ n, (void *)w_##f }; else ok = 0; } while (0)
#define OPT(f, n) do { *(void **)&r_##f = gl(next, n); if (r_##f) g_shims[k++] = (so_shim){ n, (void *)w_##f }; } while (0)
    REQ(BindFramebuffer, "glBindFramebuffer"); REQ(Clear, "glClear"); REQ(BlitFramebuffer, "glBlitFramebuffer");
    REQ(Invalidate, "glInvalidateFramebuffer"); REQ(FramebufferTexture2D, "glFramebufferTexture2D");
    REQ(FramebufferTextureLayer, "glFramebufferTextureLayer"); REQ(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    REQ(DrawBuffers, "glDrawBuffers"); REQ(DeleteFramebuffers, "glDeleteFramebuffers");
    REQ(DeleteTextures, "glDeleteTextures"); REQ(DeleteRenderbuffers, "glDeleteRenderbuffers");
    REQ(RenderbufferStorage, "glRenderbufferStorage"); REQ(RenderbufferStorageMS, "glRenderbufferStorageMultisample");
    REQ(TexImage2D, "glTexImage2D"); REQ(TexSubImage2D, "glTexSubImage2D"); REQ(TexStorage2D, "glTexStorage2D");
    REQ(CompressedTexImage2D, "glCompressedTexImage2D"); REQ(CompressedTexSubImage2D, "glCompressedTexSubImage2D");
    REQ(CopyTexImage2D, "glCopyTexImage2D"); REQ(CopyTexSubImage2D, "glCopyTexSubImage2D");
    REQ(TexImage3D, "glTexImage3D"); REQ(TexSubImage3D, "glTexSubImage3D"); REQ(TexStorage3D, "glTexStorage3D");
    REQ(CopyTexSubImage3D, "glCopyTexSubImage3D"); REQ(CompressedTexImage3D, "glCompressedTexImage3D");
    REQ(CompressedTexSubImage3D, "glCompressedTexSubImage3D"); REQ(GenerateMipmap, "glGenerateMipmap");
    REQ(ClearBufferfv, "glClearBufferfv"); REQ(ClearBufferiv, "glClearBufferiv");
    REQ(ClearBufferuiv, "glClearBufferuiv"); REQ(DrawArrays, "glDrawArrays"); REQ(DrawElements, "glDrawElements");
    REQ(DrawArraysInstanced, "glDrawArraysInstanced"); REQ(DrawElementsInstanced, "glDrawElementsInstanced");
    REQ(DrawRangeElements, "glDrawRangeElements");
    OPT(InvalidateSub, "glInvalidateSubFramebuffer"); OPT(Discard, "glDiscardFramebufferEXT");
    OPT(FramebufferTexture, "glFramebufferTexture"); OPT(FramebufferTexture2DMS, "glFramebufferTexture2DMultisampleEXT");
    OPT(RenderbufferStorageMSEXT, "glRenderbufferStorageMultisampleEXT"); OPT(CopyImageSubData, "glCopyImageSubData");
    OPT(BindImageTexture, "glBindImageTexture"); OPT(TexStorage2DMS, "glTexStorage2DMultisample");
    OPT(EGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES");
    OPT(EGLImageTargetRenderbufferStorageOES, "glEGLImageTargetRenderbufferStorageOES");
    OPT(TexBuffer, "glTexBuffer"); OPT(TexBufferEXT, "glTexBufferEXT"); OPT(TexBufferOES, "glTexBufferOES");
    OPT(TexBufferRange, "glTexBufferRange");
    OPT(DrawArraysInstancedEXT, "glDrawArraysInstancedEXT"); OPT(DrawElementsInstancedEXT, "glDrawElementsInstancedEXT");
    OPT(DrawElementsBaseVertex, "glDrawElementsBaseVertex");
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
                 q_GetRenderbufferParameteriv && q_BindTexture && q_BindRenderbuffer && s_Enable && s_Disable &&
                 s_ColorMask && s_ClearColor)) {
        memset(g_shims, 0, sizeof g_shims);     // (a GLES 2 driver: nothing wrapped)
        g_sameclear = 0;
        return g_shims;
    }
    return g_shims;
}
