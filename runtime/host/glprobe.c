// glprobe.c: questions about the GPU answered from inside a game run (UA_GL_PROBE=unorm8), on Unity's render
// thread with its own context, once, right before a swap: never as a program of its own (a standalone GL program
// beside EmulationStation oopsed dArkOS's Mali driver; docs/porting.md).
//
// unorm8: does the GPU turn each byte v of an RGBA8 texture into the float v/255 exactly (the float nearest to it)?
// A shader that compares a sampled colour with a material's constant (Blasphemous's StencilMaskSword discards the
// texels equal to its _TargetColor, 168/255, 192/255, 175/255) gives the same answer for a texture stored another way
// only where the GPU's conversion is that exact. A 256x1 RGBA8 texture holding every byte is drawn into a 256x1
// RGBA8 target by a program that samples it as the game's sprites are sampled (texture(), nearest, at the texel's
// centre) and compares the sample with a uniform array of float(v / 255.0) (as the CPU rounds it) and with
// float(v) / 255.0f (float division); the result is read back and logged:
//   [glprobe] unorm8: 256 of 256 bytes sample to float(v/255) exactly (float division: 256); 168: equal, ...
// Every GL state it touches is read first and set back after; its objects are deleted. UA_GL_PROBE_FRAME=<n> the
// frame it runs at (default 120).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glprobe.h"

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_FRAMEBUFFER 0x8D40
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_VIEWPORT 0x0BA2
#define GL_SCISSOR_TEST 0x0C11
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_CULL_FACE 0x0B44
#define GL_DITHER 0x0BD0
#define GL_RASTERIZER_DISCARD 0x8C89
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_SAMPLER_BINDING 0x8919
#define GL_TRIANGLES 0x0004
#define GL_DRAW_BUFFER0 0x8825

static struct {
    void (*GetIntegerv)(unsigned, int *);
    void (*GetBooleanv)(unsigned, unsigned char *);
    unsigned char (*IsEnabled)(unsigned);
    void (*Enable)(unsigned), (*Disable)(unsigned);
    void (*ActiveTexture)(unsigned);
    void (*GenTextures)(int, unsigned *);
    void (*DeleteTextures)(int, const unsigned *);
    void (*BindTexture)(unsigned, unsigned);
    void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (*TexParameteri)(unsigned, unsigned, int);
    void (*PixelStorei)(unsigned, int);
    void (*BindBuffer)(unsigned, unsigned);
    void (*GenFramebuffers)(int, unsigned *);
    void (*DeleteFramebuffers)(int, const unsigned *);
    void (*BindFramebuffer)(unsigned, unsigned);
    void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    unsigned (*CheckFramebufferStatus)(unsigned);
    unsigned (*CreateShader)(unsigned);
    void (*ShaderSource)(unsigned, int, const char *const *, const int *);
    void (*CompileShader)(unsigned);
    void (*GetShaderiv)(unsigned, unsigned, int *);
    void (*GetShaderInfoLog)(unsigned, int, int *, char *);
    unsigned (*CreateProgram)(void);
    void (*AttachShader)(unsigned, unsigned);
    void (*LinkProgram)(unsigned);
    void (*GetProgramiv)(unsigned, unsigned, int *);
    void (*GetProgramInfoLog)(unsigned, int, int *, char *);
    void (*DeleteShader)(unsigned), (*DeleteProgram)(unsigned);
    void (*UseProgram)(unsigned);
    int (*GetUniformLocation)(unsigned, const char *);
    void (*Uniform1i)(int, int);
    void (*Uniform1fv)(int, int, const float *);
    void (*Viewport)(int, int, int, int);
    void (*ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
    void (*GenVertexArrays)(int, unsigned *);
    void (*DeleteVertexArrays)(int, const unsigned *);
    void (*BindVertexArray)(unsigned);
    void (*BindSampler)(unsigned, unsigned);
    void (*DrawArrays)(unsigned, int, int);
    void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    void (*DrawBuffers)(int, const unsigned *);
    void (*GetFramebufferAttachmentParameteriv)(unsigned, unsigned, unsigned, int *);
} gl;

static int g_frame = -1, g_done;

static int resolve(void)
{
    void *h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) return 0;
#define G(n) (*(void **)&gl.n = dlsym(h, "gl" #n), gl.n != NULL)
    return G(GetIntegerv) & G(GetBooleanv) & G(IsEnabled) & G(Enable) & G(Disable) & G(ActiveTexture) & G(GenTextures) &
           G(DeleteTextures) & G(BindTexture) & G(TexImage2D) & G(TexParameteri) & G(PixelStorei) & G(BindBuffer) &
           G(GenFramebuffers) & G(DeleteFramebuffers) & G(BindFramebuffer) & G(FramebufferTexture2D) &
           G(CheckFramebufferStatus) & G(CreateShader) & G(ShaderSource) & G(CompileShader) & G(GetShaderiv) &
           G(GetShaderInfoLog) & G(CreateProgram) & G(AttachShader) & G(LinkProgram) & G(GetProgramiv) &
           G(GetProgramInfoLog) & G(DeleteShader) & G(DeleteProgram) & G(UseProgram) & G(GetUniformLocation) &
           G(Uniform1i) & G(Uniform1fv) & G(Viewport) & G(ColorMask) & G(GenVertexArrays) & G(DeleteVertexArrays) &
           G(BindVertexArray) & G(BindSampler) & G(DrawArrays) & G(ReadPixels) & G(DrawBuffers) &
           G(GetFramebufferAttachmentParameteriv);
#undef G
}

static const char *VS =
    "#version 300 es\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";
// texel i sampled at its centre as a sprite is (texture(), nearest); red: equal to float(i/255) (as the CPU rounds
// it), green: equal to float(i)/255.0f, blue: above (255), below (0) or equal (128) to the first
static const char *FS =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2D;\n"
    "uniform sampler2D t;\n"
    "uniform float a[256];\n"
    "uniform float b[256];\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "    int i = int(gl_FragCoord.x);\n"
    "    vec4 s = texture(t, vec2((float(i) + 0.5) / 256.0, 0.5));\n"
    "    vec3 ta = vec3(a[i]), tb = vec3(b[i]);\n"
    "    bvec3 ea = equal(s.xyz, ta), eb = equal(s.xyz, tb);\n"
    "    float r = (ea.x && ea.y && ea.z) ? 1.0 : 0.0;\n"
    "    float g = (eb.x && eb.y && eb.z) ? 1.0 : 0.0;\n"
    "    float bl = s.x > a[i] ? 1.0 : s.x < a[i] ? 0.0 : 128.0 / 255.0;\n"
    "    o = vec4(r, g, bl, 1.0);\n"
    "}\n";

static unsigned shader(unsigned type, const char *src)
{
    unsigned s = gl.CreateShader(type);
    gl.ShaderSource(s, 1, &src, NULL);
    gl.CompileShader(s);
    int ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        gl.GetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[glprobe] shader: %s\n", log);
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

static void unorm8(void)
{
    // the state it touches, as it was
    int draw_fb = 0, read_fb = 0, prog = 0, active = 0, tex0 = 0, vao = 0, vp[4], pack = 4, unpack = 4, pbo = 0,
        ubo = 0, sampler0 = 0, unpack_row = 0, pack_row = 0;
    unsigned char mask[4];
    unsigned caps[] = { GL_SCISSOR_TEST, GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE, GL_DITHER,
                        GL_RASTERIZER_DISCARD };
    unsigned char on[sizeof caps / sizeof *caps];
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &prog);
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &active);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    gl.GetIntegerv(GL_SAMPLER_BINDING, &sampler0);
    gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    gl.GetIntegerv(GL_VIEWPORT, vp);
    gl.GetIntegerv(GL_PACK_ALIGNMENT, &pack);
    gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
    gl.GetIntegerv(GL_PACK_ROW_LENGTH, &pack_row);
    gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row);
    gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &ubo);
    gl.GetBooleanv(GL_COLOR_WRITEMASK, mask);
    for (unsigned i = 0; i < sizeof caps / sizeof *caps; i++) on[i] = gl.IsEnabled(caps[i]);

    unsigned char texel[256 * 4], out[256 * 4];
    for (int i = 0; i < 256; i++) { texel[4 * i] = texel[4 * i + 1] = texel[4 * i + 2] = (unsigned char)i; texel[4 * i + 3] = 255; }
    float a[256], b[256];
    for (int i = 0; i < 256; i++) { a[i] = (float)(i / 255.0); b[i] = (float)i / 255.0f; }
    unsigned src = 0, dst = 0, fb = 0, va = 0, vs = 0, fs = 0, p = 0;
    int ok = 0;
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl.BindSampler(0, 0);
    gl.GenTextures(1, &src);
    gl.BindTexture(GL_TEXTURE_2D, src);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.GenTextures(1, &dst);
    gl.BindTexture(GL_TEXTURE_2D, dst);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.GenFramebuffers(1, &fb);
    gl.BindFramebuffer(GL_FRAMEBUFFER, fb);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
    unsigned db = GL_COLOR_ATTACHMENT0;
    gl.DrawBuffers(1, &db);
    if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && (vs = shader(GL_VERTEX_SHADER, VS)) &&
        (fs = shader(GL_FRAGMENT_SHADER, FS))) {
        p = gl.CreateProgram();
        gl.AttachShader(p, vs);
        gl.AttachShader(p, fs);
        gl.LinkProgram(p);
        int linked = 0;
        gl.GetProgramiv(p, GL_LINK_STATUS, &linked);
        if (linked) {
            gl.UseProgram(p);
            gl.Uniform1i(gl.GetUniformLocation(p, "t"), 0);
            gl.Uniform1fv(gl.GetUniformLocation(p, "a"), 256, a);
            gl.Uniform1fv(gl.GetUniformLocation(p, "b"), 256, b);
            gl.BindTexture(GL_TEXTURE_2D, src);
            gl.GenVertexArrays(1, &va);
            gl.BindVertexArray(va);
            for (unsigned i = 0; i < sizeof caps / sizeof *caps; i++) gl.Disable(caps[i]);
            gl.ColorMask(1, 1, 1, 1);
            gl.Viewport(0, 0, 256, 1);
            gl.DrawArrays(GL_TRIANGLES, 0, 3);
            gl.BindFramebuffer(GL_READ_FRAMEBUFFER, fb);
            gl.ReadPixels(0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
            ok = 1;
        } else {
            char log[512] = "";
            gl.GetProgramInfoLog(p, sizeof log, NULL, log);
            fprintf(stderr, "[glprobe] link: %s\n", log);
        }
    }

    // set back
    gl.BindVertexArray((unsigned)vao);
    gl.UseProgram((unsigned)prog);
    gl.BindTexture(GL_TEXTURE_2D, (unsigned)tex0);
    gl.BindSampler(0, (unsigned)sampler0);
    gl.ActiveTexture((unsigned)active);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned)draw_fb);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)read_fb);
    gl.Viewport(vp[0], vp[1], vp[2], vp[3]);
    gl.ColorMask(mask[0], mask[1], mask[2], mask[3]);
    for (unsigned i = 0; i < sizeof caps / sizeof *caps; i++) if (on[i]) gl.Enable(caps[i]);
    gl.PixelStorei(GL_PACK_ALIGNMENT, pack);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, unpack);
    gl.PixelStorei(GL_PACK_ROW_LENGTH, pack_row);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, (unsigned)pbo);
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, (unsigned)ubo);
    if (va) gl.DeleteVertexArrays(1, &va);
    if (p) gl.DeleteProgram(p);
    if (vs) gl.DeleteShader(vs);
    if (fs) gl.DeleteShader(fs);
    if (fb) gl.DeleteFramebuffers(1, &fb);
    gl.DeleteTextures(1, &src);
    gl.DeleteTextures(1, &dst);
    if (!ok) { fprintf(stderr, "[glprobe] unorm8: the probe did not run\n"); return; }

    int na = 0, nb = 0, above = 0, below = 0;
    for (int i = 0; i < 256; i++) {
        na += out[4 * i] > 127; nb += out[4 * i + 1] > 127;
        if (out[4 * i] <= 127) { if (out[4 * i + 2] > 192) above++; else if (out[4 * i + 2] < 64) below++; }
    }
    fprintf(stderr, "[glprobe] unorm8: %d of 256 bytes sample to float(v/255) exactly (%d above it, %d below), "
            "%d to float(v)/255.0f; 168: %s, 192: %s, 175: %s\n", na, above, below, nb,
            out[4 * 168] > 127 ? "equal" : "differs", out[4 * 192] > 127 ? "equal" : "differs",
            out[4 * 175] > 127 ? "equal" : "differs");
    char line[1100]; int o = 0;
    for (int i = 0; i < 256; i++) if (out[4 * i] <= 127) o += snprintf(line + o, sizeof line - o, " %d", i);
    if (o) fprintf(stderr, "[glprobe] unorm8: the bytes that differ:%.1000s\n", line);
}

void glprobe_frame(void)
{
    if (g_done) return;
    if (g_frame < 0) {
        const char *e = getenv("UA_GL_PROBE");
        if (!e || strcmp(e, "unorm8")) { g_done = 1; return; }
        const char *f = getenv("UA_GL_PROBE_FRAME");
        g_frame = f && atoi(f) > 0 ? atoi(f) : 120;
    }
    if (--g_frame > 0) return;
    g_done = 1;
    if (!resolve()) { fprintf(stderr, "[glprobe] no GLES 3 functions\n"); return; }
    unorm8();
}
