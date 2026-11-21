// Display backend selection (display.h): UA_DISPLAY=auto|fbdev|sdl.
#define _GNU_SOURCE
#include "display.h"
#include "fbdev.h"
#include "glpresent.h"
#include "gldriver.h"
#include "../android/android_shims.h"
#include "../android/egl_glue.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EGL_EXTENSIONS 0x3055

// ---- the GPU scaler (render thread, Unity's context current, before the swap) -------------------
// Two glBlitFramebuffer: the rw x rh frame into a texture (the same size: also resolves a
// multisampled surface), then the texture (linear) over the whole surface (a blit within one
// framebuffer may not overlap), or with UA_ASPECT in the middle of it, black around it. GLES 3.
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA8 0x8058
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_LINEAR 0x2601
#define GL_NEAREST 0x2600
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_SCISSOR_TEST 0x0C11
#define GL_RASTERIZER_DISCARD 0x8C89
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_DEPTH 0x1801
#define GL_STENCIL 0x1802
#define GL_SCISSOR_BOX 0x0C10
static struct {
    void (*GetIntegerv)(unsigned, int *);
    void (*GetBooleanv)(unsigned, unsigned char *);
    unsigned char (*IsEnabled)(unsigned);
    void (*Enable)(unsigned), (*Disable)(unsigned);
    void (*ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
    void (*GenTextures)(int, unsigned *);
    void (*BindTexture)(unsigned, unsigned);
    void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (*TexParameteri)(unsigned, unsigned, int);
    void (*GenFramebuffers)(int, unsigned *);
    void (*BindFramebuffer)(unsigned, unsigned);
    void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    unsigned (*CheckFramebufferStatus)(unsigned);
    void (*BlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
    void (*GetFloatv)(unsigned, float *);
    void (*ClearColor)(float, float, float, float);
    void (*Clear)(unsigned);
    void (*InvalidateFramebuffer)(unsigned, int, const unsigned *);
    void (*Scissor)(int, int, int, int);
} G;
static int g_srw, g_srh, g_sdw, g_sdh;          // set: display_gpu_scale
static int g_sx, g_sy, g_sfw, g_sfh, g_sbars;   // where the frame goes on the surface; bars: black around it
static int g_spanned;                           // UA_ASPECT placed by the pans: only the bars to clear
static unsigned g_sfbo;
static int g_sstate;                            // 0 not set up, 1 on, -1 failed (frames stay unscaled)

void display_gpu_scale(int rw, int rh, int dw, int dh)
{
    g_srw = rw; g_srh = rh; g_sdw = dw; g_sdh = dh;
    g_sx = g_sy = g_sbars = g_spanned = 0;
    g_sfw = dw; g_sfh = dh;
    const char *a = getenv("UA_ASPECT");
    if (!a || !*a) return;
    int fw = dw, fh = (int)((double)dw * rh / rw + 0.5);        // the frame's own aspect over the surface
    if (fh > dh) { fh = dh; fw = (int)((double)dh * rw / rh + 0.5); }
    if (fw >= dw - 1 && fh >= dh - 1) return;                   // the surface's aspect (within a pixel)
    g_sfw = fw; g_sfh = fh;
    g_sx = (dw - fw) / 2; g_sy = (dh - fh) / 2;
    g_sbars = 1;
}

static int gl_load(void)
{
    void *h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    void *(*gpa)(const char *) = (void *(*)(const char *))egl_glue_real("eglGetProcAddress");
#define L(f) do { *(void **)&G.f = h ? dlsym(h, "gl" #f) : NULL; if (!G.f && gpa) *(void **)&G.f = gpa("gl" #f); } while (0)
    L(GetIntegerv); L(GetBooleanv); L(IsEnabled); L(Enable); L(Disable); L(ColorMask); L(GenTextures);
    L(BindTexture); L(TexImage2D); L(TexParameteri); L(GenFramebuffers); L(BindFramebuffer);
    L(FramebufferTexture2D); L(CheckFramebufferStatus); L(BlitFramebuffer); L(GetFloatv); L(ClearColor); L(Clear);
    L(InvalidateFramebuffer); L(Scissor);
#undef L
    void **f = (void **)&G;
    for (unsigned i = 0; i < sizeof G / sizeof *f; i++) if (!f[i]) return 0;
    return 1;
}

static int scale_setup(void)
{
    if (!gl_load()) return 0;
    int tex0 = 0, rfb = 0, dfb = 0;
    unsigned tex;
    G.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    G.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    G.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
    G.GenTextures(1, &tex);
    G.BindTexture(GL_TEXTURE_2D, tex);
    G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_srw, g_srh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    G.GenFramebuffers(1, &g_sfbo);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_sfbo);
    G.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    int ok = G.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    G.BindTexture(GL_TEXTURE_2D, (unsigned)tex0);
    G.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)rfb);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned)dfb);
    return ok;
}

static int g_srot;                              // degrees the panel is turned (display_gpu_rotate)

void display_frame_rect(int *x, int *y, int *w, int *h)
{
    if (g_sbars && g_sstate > 0 && !g_srot) { *x = g_sx; *y = g_sy; *w = g_sfw; *h = g_sfh; }
}

// The surface's lines above Unity's frame, black: the bar above the picture. In the frame's own
// pass (no pass of its own, nothing loaded).
static void clear_bars(void)
{
    int dfb = 0, box[4];
    float c[4];
    unsigned char mask[4];
    G.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
    G.GetIntegerv(GL_SCISSOR_BOX, box);
    G.GetFloatv(GL_COLOR_CLEAR_VALUE, c);
    G.GetBooleanv(GL_COLOR_WRITEMASK, mask);
    unsigned char sc = G.IsEnabled(GL_SCISSOR_TEST);
    if (dfb) G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (!sc) G.Enable(GL_SCISSOR_TEST);
    G.Scissor(0, g_srh, g_sdw, g_sdh - g_srh);
    G.ColorMask(1, 1, 1, 1);
    G.ClearColor(0, 0, 0, 1);
    G.Clear(GL_COLOR_BUFFER_BIT);
    G.ClearColor(c[0], c[1], c[2], c[3]);
    G.ColorMask(mask[0], mask[1], mask[2], mask[3]);
    G.Scissor(box[0], box[1], box[2], box[3]);
    if (!sc) G.Disable(GL_SCISSOR_TEST);
    if (dfb) G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned)dfb);
}

void display_gpu_spanned(void) { g_spanned = 1; g_sbars = 0; }

void display_present_scaled(void)
{
    if (!g_srw || g_sstate < 0 || g_srot || glpresent_active()) return;   // (UA_PRESENT_THREAD: the present thread scales;
                                                                          // a turned panel: display_present_rotated)
    if (g_spanned) {
        if (!g_sstate) {
            g_sstate = gl_load() ? 1 : -1;
            fprintf(stderr, g_sstate > 0 ? "[display] render size %dx%d, at the bottom of a %dx%d surface: the middle of the panel\n"
                                         : "[display] render size %dx%d in %dx%d: no GL for its bars\n", g_srw, g_srh, g_sdw, g_sdh);
            if (g_sstate < 0) return;
        }
        clear_bars();
        return;
    }
    if (!g_sstate) {
        g_sstate = scale_setup() ? 1 : -1;
        fprintf(stderr, g_sstate > 0 ? "[display] render size %dx%d, scaled to %dx%d by the GPU\n"
                                     : "[display] render size %dx%d: no GLES 3 framebuffer copy, the frame stays in the corner of %dx%d\n",
                g_srw, g_srh, g_sfw, g_sfh);
        if (g_sstate < 0) return;
    }
    int rfb = 0, dfb = 0;
    unsigned char mask[4];
    G.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    G.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
    G.GetBooleanv(GL_COLOR_WRITEMASK, mask);
    unsigned char sc = G.IsEnabled(GL_SCISSOR_TEST), rd = G.IsEnabled(GL_RASTERIZER_DISCARD);
    if (sc) G.Disable(GL_SCISSOR_TEST);
    if (rd) G.Disable(GL_RASTERIZER_DISCARD);
    G.ColorMask(1, 1, 1, 1);
    G.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_sfbo);
    G.BlitFramebuffer(0, 0, g_srw, g_srh, 0, 0, g_srw, g_srh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    G.BindFramebuffer(GL_READ_FRAMEBUFFER, g_sfbo);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (g_sbars) {                              // a new pass of the window: nothing of it loaded, bars black
        static const unsigned ds[2] = { GL_DEPTH, GL_STENCIL };
        float c[4];
        G.InvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 2, ds);
        G.GetFloatv(GL_COLOR_CLEAR_VALUE, c);
        G.ClearColor(0, 0, 0, 1);
        G.Clear(GL_COLOR_BUFFER_BIT);
        G.ClearColor(c[0], c[1], c[2], c[3]);
    }
    G.BlitFramebuffer(0, 0, g_srw, g_srh, g_sx, g_sy, g_sx + g_sfw, g_sy + g_sfh, GL_COLOR_BUFFER_BIT,
                      g_sfw == g_srw && g_sfh == g_srh ? GL_NEAREST : GL_LINEAR);
    G.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)rfb);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned)dfb);
    G.ColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (sc) G.Enable(GL_SCISSOR_TEST);
    if (rd) G.Enable(GL_RASTERIZER_DISCARD);
}

// ---- a panel turned a quarter (fbdev.c's UA_ROTATE; render thread, Unity's context, at the swap) ---
// Unity's frame is glpresent.c's framebuffer 0 (a texture of the host's); one quad draws it turned
// over the window, after the host's own drawing into the frame (the on-screen keyboard, the cursor,
// screenshots). Positions and texture coordinates come from gl_VertexID: no buffer or vertex state
// of Unity's is used. Put back after it: the program, vertex array, active unit and its 2D texture
// and sampler, viewport, draw framebuffer, colour mask and clear colour, and the scissor, blend,
// depth, stencil, cull, rasterizer-discard, polygon-offset and coverage switches. GLES 3.
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE0 0x84C0
#define GL_SAMPLER_BINDING 0x8919
#define GL_VIEWPORT 0x0BA2
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_CULL_FACE 0x0B44
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#define GL_SAMPLE_COVERAGE 0x80A0
#define GL_TRIANGLE_STRIP 0x0005
#define GL_COLOR 0x1800
static struct {
    unsigned (*CreateShader)(unsigned);
    void (*ShaderSource)(unsigned, int, const char *const *, const int *);
    void (*CompileShader)(unsigned), (*DeleteShader)(unsigned);
    void (*GetShaderiv)(unsigned, unsigned, int *), (*GetProgramiv)(unsigned, unsigned, int *);
    unsigned (*CreateProgram)(void);
    void (*AttachShader)(unsigned, unsigned), (*LinkProgram)(unsigned), (*UseProgram)(unsigned);
    int (*GetUniformLocation)(unsigned, const char *);
    void (*Uniform1i)(int, int);
    void (*GenVertexArrays)(int, unsigned *), (*BindVertexArray)(unsigned);
    void (*ActiveTexture)(unsigned), (*BindSampler)(unsigned, unsigned);
    void (*Viewport)(int, int, int, int), (*DrawArrays)(unsigned, int, int);
} R;
static int g_rx, g_ry, g_rfw, g_rfh, g_rbars;   // the frame's place on the window; black around it
static unsigned g_rprog, g_rvao;
static int g_rstate;                            // 0 not set up, 1 on, -1 failed (frames not shown)

void display_gpu_rotate(int deg)
{
    if (!g_srw || glpresent_start_sync(g_srw, g_srh) != 0) {
        fprintf(stderr, "[display] UA_ROTATE=%d: no frame of the host's (GL wrappers missing): the picture is not turned\n", deg);
        return;
    }
    int q = deg == 90 || deg == 270;
    int lw = q ? g_sdh : g_sdw, lh = q ? g_sdw : g_sdh;          // the window as the handheld is held
    int fw = lw, fh = (int)((double)lw * g_srh / g_srw + 0.5);   // the frame at its own aspect
    if (fh > lh) { fh = lh; fw = (int)((double)lh * g_srw / g_srh + 0.5); }
    if (fw >= lw - 1 && fh >= lh - 1) { fw = lw; fh = lh; }
    int fx = (lw - fw) / 2, fy = (lh - fh) / 2;
    g_rbars = fw < lw || fh < lh;
    if (deg == 270) { g_rx = g_sdw - (fy + fh); g_ry = fx; g_rfw = fh; g_rfh = fw; }
    else if (deg == 90) { g_rx = fy; g_ry = g_sdh - (fx + fw); g_rfw = fh; g_rfh = fw; }
    else { g_rx = g_sdw - (fx + fw); g_ry = g_sdh - (fy + fh); g_rfw = fw; g_rfh = fh; }
    g_srot = deg;
}

static unsigned rotate_shader(unsigned type, const char *src)
{
    unsigned sh = R.CreateShader(type);
    int ok = 0;
    R.ShaderSource(sh, 1, &src, NULL);
    R.CompileShader(sh);
    R.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { R.DeleteShader(sh); return 0; }
    return sh;
}

static int rotate_setup(void)
{
    if (!gl_load()) return 0;
    void *h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    void *(*gpa)(const char *) = (void *(*)(const char *))egl_glue_real("eglGetProcAddress");
#define L(f) do { *(void **)&R.f = h ? dlsym(h, "gl" #f) : NULL; if (!R.f && gpa) *(void **)&R.f = gpa("gl" #f); } while (0)
    L(CreateShader); L(ShaderSource); L(CompileShader); L(DeleteShader); L(GetShaderiv); L(GetProgramiv); L(CreateProgram);
    L(AttachShader); L(LinkProgram); L(UseProgram); L(GetUniformLocation); L(Uniform1i); L(GenVertexArrays);
    L(BindVertexArray); L(ActiveTexture); L(BindSampler); L(Viewport); L(DrawArrays);
#undef L
    void **f = (void **)&R;
    for (unsigned i = 0; i < sizeof R / sizeof *f; i++) if (!f[i]) return 0;
    // p: the corner of the window's quad (0..1 over the frame's place); uv: where it is in the frame
    char vs[512];
    snprintf(vs, sizeof vs, "#version 300 es\nout highp vec2 uv;\nvoid main() {\n"
             "    highp vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
             "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n    uv = %s;\n}\n",
             g_srot == 270 ? "vec2(p.y, 1.0 - p.x)" : g_srot == 90 ? "vec2(1.0 - p.y, p.x)" : "vec2(1.0 - p.x, 1.0 - p.y)");
    static const char fs[] = "#version 300 es\nprecision highp float;\nuniform sampler2D t;\nin highp vec2 uv;\n"
                             "out vec4 c;\nvoid main() { c = texture(t, uv); }\n";
    unsigned v = rotate_shader(GL_VERTEX_SHADER, vs), fr = v ? rotate_shader(GL_FRAGMENT_SHADER, fs) : 0;
    if (!fr) { if (v) R.DeleteShader(v); return 0; }
    int ok = 0, prog0 = 0;
    g_rprog = R.CreateProgram();
    R.AttachShader(g_rprog, v);
    R.AttachShader(g_rprog, fr);
    R.LinkProgram(g_rprog);
    R.DeleteShader(v);
    R.DeleteShader(fr);
    R.GetProgramiv(g_rprog, GL_LINK_STATUS, &ok);
    if (!ok) return 0;
    G.GetIntegerv(GL_CURRENT_PROGRAM, &prog0);
    R.UseProgram(g_rprog);
    R.Uniform1i(R.GetUniformLocation(g_rprog, "t"), 0);
    R.UseProgram((unsigned)prog0);
    R.GenVertexArrays(1, &g_rvao);
    return 1;
}

void display_present_rotated(void)
{
    if (!g_srot || g_rstate < 0) return;
    unsigned tex = glpresent_fb0_texture();
    if (!tex) return;                           // (not the window's context)
    if (!g_rstate) {
        g_rstate = rotate_setup() ? 1 : -1;
        fprintf(stderr, g_rstate > 0 ? "[display] UA_ROTATE=%d: Unity's %dx%d frame turned onto %dx%d of the %dx%d window by the GPU\n"
                                     : "[display] UA_ROTATE=%d: no GLES 3 program for the turn: frames of %dx%d are not shown (%dx%d of %dx%d)\n",
                g_srot, g_srw, g_srh, g_rfw, g_rfh, g_sdw, g_sdh);
        if (g_rstate < 0) return;
    }
    static const unsigned sw[] = { GL_SCISSOR_TEST, GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE, GL_RASTERIZER_DISCARD,
                                   GL_POLYGON_OFFSET_FILL, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE };
    unsigned char on[sizeof sw / sizeof *sw], mask[4];
    int prog = 0, vao = 0, unit = 0, tex0 = 0, smp = 0, dfb = 0, vp[4];
    float c[4];
    for (unsigned i = 0; i < sizeof sw / sizeof *sw; i++) if ((on[i] = G.IsEnabled(sw[i]))) G.Disable(sw[i]);
    G.GetIntegerv(GL_CURRENT_PROGRAM, &prog);
    G.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    G.GetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    if (unit != GL_TEXTURE0) R.ActiveTexture(GL_TEXTURE0);
    G.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    G.GetIntegerv(GL_SAMPLER_BINDING, &smp);
    G.GetIntegerv(GL_VIEWPORT, vp);
    G.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
    G.GetBooleanv(GL_COLOR_WRITEMASK, mask);
    G.GetFloatv(GL_COLOR_CLEAR_VALUE, c);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);  // (the driver's: the window itself)
    G.ColorMask(1, 1, 1, 1);
    if (g_rbars) {                              // a new pass of the window: nothing of it loaded, bars black
        static const unsigned ds[2] = { GL_DEPTH, GL_STENCIL };
        G.InvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 2, ds);
        G.ClearColor(0, 0, 0, 1);
        G.Clear(GL_COLOR_BUFFER_BIT);
    } else {
        static const unsigned all[3] = { GL_COLOR, GL_DEPTH, GL_STENCIL };
        G.InvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 3, all);
    }
    R.Viewport(g_rx, g_ry, g_rfw, g_rfh);
    R.UseProgram(g_rprog);
    R.BindVertexArray(g_rvao);
    G.BindTexture(GL_TEXTURE_2D, tex);
    if (smp) R.BindSampler(0, 0);
    R.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (smp) R.BindSampler(0, (unsigned)smp);
    G.BindTexture(GL_TEXTURE_2D, (unsigned)tex0);
    if (unit != GL_TEXTURE0) R.ActiveTexture((unsigned)unit);
    R.BindVertexArray((unsigned)vao);
    R.UseProgram((unsigned)prog);
    R.Viewport(vp[0], vp[1], vp[2], vp[3]);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned)dfb);
    G.ClearColor(c[0], c[1], c[2], c[3]);
    G.ColorMask(mask[0], mask[1], mask[2], mask[3]);
    for (unsigned i = 0; i < sizeof sw / sizeof *sw; i++) if (on[i]) G.Enable(sw[i]);
}

static void on_current(void *ctx, int window) { glpresent_made_current(window ? ctx : NULL); }

static const char *g_backend = "fbdev";
static egl_host_display g_hd;
static int g_refresh;

static int pbuffer_mode(void)
{
    const char *e = getenv("UA_EGL_PBUFFER");
    return e && *e && *e != '0';
}

// The libEGL egl_glue will use (same search), opened RTLD_LOCAL: egl_glue still opens it
// RTLD_GLOBAL on Unity's first EGL call, as it always has.
static void *open_egl(void)
{
    const char *names[] = { getenv("ANDROID_SHIM_LIBEGL"), "libEGL.so.1", "libEGL.so" };
    for (int i = 0; i < 3; i++) {
        void *h = names[i] && *names[i] ? dlopen(names[i], RTLD_NOW | RTLD_LOCAL) : NULL;
        if (h) return h;
    }
    return NULL;
}

const char *display_auto_choice(char *why, size_t n)
{
    if (pbuffer_mode()) { snprintf(why, n, "UA_EGL_PBUFFER"); return "fbdev"; }
    const char *fb = getenv("ANDROID_FBDEV");
    if (!fb || !*fb) fb = "/dev/fb0";
    if (access(fb, F_OK) != 0) { snprintf(why, n, "no %s", fb); return "sdl"; }
    void *egl = open_egl();
    if (!egl) { snprintf(why, n, "no libEGL"); return "fbdev"; }   // Unity fails later, as before
    const char *(*query)(void *, int32_t) = (const char *(*)(void *, int32_t))dlsym(egl, "eglQueryString");
    int32_t (*get_error)(void) = (int32_t (*)(void))dlsym(egl, "eglGetError");
    // Client extensions (EGL_EXT_client_extensions): Mesa and the window-system flavors of vendor
    // drivers list their platforms; an fbdev-only EGL lists none (or answers NULL).
    const char *ext = query ? query(NULL, EGL_EXTENSIONS) : NULL;
    // A NULL answer leaves EGL_BAD_DISPLAY pending on this thread, which is also Unity's UI thread:
    // Unity reads eglGetError after creating its probe context and would take it for a failure.
    if (get_error) get_error();
    static const char *const platforms[] = { "_platform_gbm", "_platform_wayland", "_platform_x11",
                                             "_platform_xcb", "_platform_device", "_platform_surfaceless" };
    for (size_t i = 0; ext && i < sizeof platforms / sizeof platforms[0]; i++)
        if (strstr(ext, platforms[i])) {
            snprintf(why, n, "libEGL supports window systems (EGL_*%s)", platforms[i]);
            return "sdl";
        }
    snprintf(why, n, "%s and an fbdev-only libEGL", fb);
    return "fbdev";
}

// The fbdev path: Mali's fbdev EGL on the framebuffer; fbdev.c sets up its buffers and flips, and
// with UA_RENDER_SIZE / UA_RENDER_SCALE the window has that size: on Allwinner's display engine the
// framebuffer is cropped to it and the engine scales it up; elsewhere Unity is told it, the EGL
// surface keeps the panel's size and the GPU scaler draws the frame over it.
// PowerVR's whole-screen window: its driver presents (fbdev_driver_presents), and a render size
// goes to the GPU scaler over the whole screen
static void on_whole_screen(void)
{
    static int done;
    if (done++) return;
    int w, h, pw, ph;
    if (fbdev_driver_presents(&w, &h, &pw, &ph)) {
        egl_glue_set_report_size(w, h);
        display_gpu_scale(w, h, pw, ph);
    }
}

static int use_fbdev(void)
{
    if (pbuffer_mode() && !gldriver_mesa()) return 0;
    fbdev_setup();
    egl_glue_set_whole_screen_hook(on_whole_screen);
    int w, h, pw, ph;
    if (fbdev_render_size(&w, &h)) android_window_set_size(w, h);
    else if (fbdev_gpu_render_size(&w, &h, &pw, &ph)) {
        static struct { unsigned short w, h; } panel;      // Mali's fbdev_window: the surface's size
        panel.w = (unsigned short)pw;
        panel.h = (unsigned short)ph;
        android_window_set_size(w, h);
        android_window_set_native(&panel);
        egl_glue_set_report_size(w, h);
        display_gpu_scale(w, h, pw, ph);
        if (fbdev_rotation()) {
            display_gpu_rotate(fbdev_rotation());
            egl_glue_set_current_hook(on_current);
        } else if (fbdev_bars_height()) display_gpu_spanned();
    }
    // UA_GL_DRIVER=mesa (gldriver.c): Unity's window is a pbuffer (the panel's size under the GPU
    // scaler, as Mali's surface would be), copied to the framebuffer at each swap
    if (gldriver_mesa()) {
        int gw, gh, gpw = 0, gph = 0;
        if (!fbdev_render_size(&gw, &gh)) fbdev_gpu_render_size(&gw, &gh, &gpw, &gph);
        gldriver_fbdev_present(gpw, gph);
    }
    ANativeWindow *win = android_window_get();
    android_window_set_max_size(ANativeWindow_getWidth(win), ANativeWindow_getHeight(win));
    return 0;
}

int display_init(const char *title)
{
    const char *want = getenv("UA_DISPLAY");
    if (!want || !*want) want = "auto";
    int is_auto = !strcmp(want, "auto");
    if (!is_auto && strcmp(want, "fbdev") && strcmp(want, "sdl")) {
        fprintf(stderr, "[display] UA_DISPLAY=%s: auto, fbdev or sdl\n", want);
        return -1;
    }
    if (!strcmp(want, "fbdev")) return use_fbdev();
    if (pbuffer_mode() && gldriver_mesa()) {               // Mesa's surfaceless EGL: the fbdev path, the host shows the frames
        if (strcmp(want, "auto")) fprintf(stderr, "[display] UA_GL_DRIVER=mesa: the fbdev path, UA_DISPLAY=%s ignored\n", want);
        return use_fbdev();
    }
    if (pbuffer_mode()) {
        if (!is_auto) fprintf(stderr, "[display] UA_EGL_PBUFFER is set: pbuffer path, UA_DISPLAY=sdl ignored\n");
        return 0;
    }
    if (is_auto) {
        char why[160];
        const char *pick = display_auto_choice(why, sizeof why);
        fprintf(stderr, "[display] auto: %s (%s)\n", pick, why);
        if (strcmp(pick, "sdl")) return use_fbdev();
    }
    int w = 0, h = 0, hz = 0;
    if (display_sdl_open(title, &g_hd, &w, &h, &hz) != 0) {
        if (!is_auto) return -1;
        fprintf(stderr, "[display] no SDL window: falling back to the fbdev path\n");
        return use_fbdev();
    }
    egl_glue_set_host_display(&g_hd);
    android_window_set_size(w, h);              // (with UA_RENDER_SIZE / UA_RENDER_SCALE: the smaller size)
    g_refresh = hz;
    g_backend = "sdl";
    return 0;
}

const char *display_backend(void) { return g_backend; }

const so_shim *display_egl_shims(void) { return strcmp(g_backend, "sdl") ? NULL : egl_host_shims; }

int display_refresh_rate(void) { return g_refresh; }

int display_quit_requested(void) { return !strcmp(g_backend, "sdl") && display_sdl_quit_requested(); }
