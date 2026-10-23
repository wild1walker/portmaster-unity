// A fake libGLESv2 for test_glopt.c: three framebuffers (3: colour texture 9 and the packed
// depth/stencil texture 20, 64x32; 4: colour texture 10 only; 5: depth renderbuffer 30 only, 64x32),
// the fixed-function state glopt.c reads and sets (tests, write masks, stencil operations, scissor,
// clear values), and a record of every glClear with the state it ran with.
#include <string.h>

#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_TEXTURE 0x1702
#define GL_RENDERBUFFER 0x8D41
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_TEXTURE_WIDTH 0x1000
#define GL_TEXTURE_HEIGHT 0x1001
#define GL_RENDERBUFFER_WIDTH 0x8D42
#define GL_RENDERBUFFER_HEIGHT 0x8D43
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_SCISSOR_TEST 0x0C11
#define GL_RASTERIZER_DISCARD 0x8C89
#define GL_SCISSOR_BOX 0x0C10
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
#define GL_FRONT_AND_BACK 0x0408

int fake_draw_fb, fake_read_fb, fake_tex2d, fake_rb;
int fake_depth_test, fake_stencil_test, fake_scissor, fake_discard;
int fake_depth_mask = 1, fake_stencil_mask = 0xff, fake_stencil_back_mask = 0xff;
int fake_box[4] = { 0, 0, 64, 32 };
int fake_ops[6] = { GL_KEEP, GL_KEEP, GL_KEEP, GL_KEEP, GL_KEEP, GL_KEEP };
float fake_clear_depth = 1; int fake_clear_stencil;
int fake_draws;

// every glClear: its mask and the state it ran with
typedef struct { int fb; unsigned mask; float depth; int stencil, scissor, depth_mask, stencil_mask, stencil_back_mask; } fake_clear_t;
fake_clear_t fake_clears[64];
int fake_nclears;

void glBindFramebuffer(unsigned t, unsigned fb)
{
    if (t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER) fake_draw_fb = (int)fb;
    if (t == GL_FRAMEBUFFER || t == GL_READ_FRAMEBUFFER) fake_read_fb = (int)fb;
}
void glClear(unsigned m)
{
    if (fake_nclears < 64)
        fake_clears[fake_nclears++] = (fake_clear_t){ fake_draw_fb, m, fake_clear_depth, fake_clear_stencil, fake_scissor,
                                                      fake_depth_mask, fake_stencil_mask, fake_stencil_back_mask };
}
void glClearDepthf(float d) { fake_clear_depth = d; }
void glClearStencil(int s) { fake_clear_stencil = s; }
static int *cap(unsigned c)
{
    static int other;
    return c == GL_DEPTH_TEST ? &fake_depth_test : c == GL_STENCIL_TEST ? &fake_stencil_test : c == GL_SCISSOR_TEST ? &fake_scissor :
           c == GL_RASTERIZER_DISCARD ? &fake_discard : &other;
}
void glEnable(unsigned c) { *cap(c) = 1; }
void glDisable(unsigned c) { *cap(c) = 0; }
unsigned char glIsEnabled(unsigned c) { return (unsigned char)*cap(c); }
void glDepthMask(unsigned char m) { fake_depth_mask = m; }
void glStencilMask(unsigned m) { fake_stencil_mask = fake_stencil_back_mask = (int)m; }
void glStencilMaskSeparate(unsigned face, unsigned m)
{
    if (face == GL_FRONT || face == GL_FRONT_AND_BACK) fake_stencil_mask = (int)m;
    if (face == GL_BACK || face == GL_FRONT_AND_BACK) fake_stencil_back_mask = (int)m;
}
void glStencilOp(unsigned f, unsigned zf, unsigned zp) { fake_ops[0] = fake_ops[3] = (int)f; fake_ops[1] = fake_ops[4] = (int)zf; fake_ops[2] = fake_ops[5] = (int)zp; }
void glScissor(int x, int y, int w, int h) { fake_box[0] = x; fake_box[1] = y; fake_box[2] = w; fake_box[3] = h; }

void glGetIntegerv(unsigned p, int *v)
{
    switch (p) {
    case GL_DRAW_FRAMEBUFFER_BINDING: *v = fake_draw_fb; break;
    case GL_READ_FRAMEBUFFER_BINDING: *v = fake_read_fb; break;
    case GL_TEXTURE_BINDING_2D: *v = fake_tex2d; break;
    case GL_RENDERBUFFER_BINDING: *v = fake_rb; break;
    case GL_SCISSOR_BOX: memcpy(v, fake_box, sizeof fake_box); break;
    case GL_DEPTH_WRITEMASK: *v = fake_depth_mask; break;
    case GL_STENCIL_WRITEMASK: *v = fake_stencil_mask; break;
    case GL_STENCIL_BACK_WRITEMASK: *v = fake_stencil_back_mask; break;
    case GL_STENCIL_FAIL: *v = fake_ops[0]; break;
    case GL_STENCIL_PASS_DEPTH_FAIL: *v = fake_ops[1]; break;
    case GL_STENCIL_PASS_DEPTH_PASS: *v = fake_ops[2]; break;
    case GL_STENCIL_BACK_FAIL: *v = fake_ops[3]; break;
    case GL_STENCIL_BACK_PASS_DEPTH_FAIL: *v = fake_ops[4]; break;
    case GL_STENCIL_BACK_PASS_DEPTH_PASS: *v = fake_ops[5]; break;
    case GL_STENCIL_CLEAR_VALUE: *v = fake_clear_stencil; break;
    default: *v = 0;
    }
}
void glGetFloatv(unsigned p, float *v) { if (p == GL_DEPTH_CLEAR_VALUE) *v = fake_clear_depth; }

void glGetFramebufferAttachmentParameteriv(unsigned t, unsigned a, unsigned p, int *v)
{
    int fb = t == GL_READ_FRAMEBUFFER ? fake_read_fb : fake_draw_fb, type = 0, name = 0;
    if (a == GL_COLOR_ATTACHMENT0 && (fb == 3 || fb == 4)) { type = GL_TEXTURE; name = fb == 3 ? 9 : 10; }
    if ((a == GL_DEPTH_ATTACHMENT || a == GL_STENCIL_ATTACHMENT) && fb == 3) { type = GL_TEXTURE; name = 20; }
    if (a == GL_DEPTH_ATTACHMENT && fb == 5) { type = GL_RENDERBUFFER; name = 30; }
    *v = p == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE ? type : p == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME ? name : 0;
}
void glBindTexture(unsigned t, unsigned tex) { (void)t; fake_tex2d = (int)tex; }
void glBindRenderbuffer(unsigned t, unsigned rb) { (void)t; fake_rb = (int)rb; }
void glGetTexLevelParameteriv(unsigned t, int l, unsigned p, int *v)
{ (void)t; (void)l; *v = fake_tex2d == 20 ? (p == GL_TEXTURE_WIDTH ? 64 : p == GL_TEXTURE_HEIGHT ? 32 : 0) : 0; }
void glGetRenderbufferParameteriv(unsigned t, unsigned p, int *v)
{ (void)t; *v = fake_rb == 30 ? (p == GL_RENDERBUFFER_WIDTH ? 64 : p == GL_RENDERBUFFER_HEIGHT ? 32 : 0) : 0; }

void glDrawArrays(unsigned m, int f, int c) { (void)m; (void)f; (void)c; fake_draws++; }
void glDrawElements(unsigned m, int c, unsigned t, const void *i) { (void)m; (void)c; (void)t; (void)i; fake_draws++; }
void glBlitFramebuffer(int a, int b, int c, int d, int e, int f, int g, int h, unsigned m, unsigned fl)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)m; (void)fl; }
void glInvalidateFramebuffer(unsigned t, int n, const unsigned *a) { (void)t; (void)n; (void)a; }
void glFramebufferTexture2D(unsigned t, unsigned a, unsigned tt, unsigned tex, int l) { (void)t; (void)a; (void)tt; (void)tex; (void)l; }
void glFramebufferRenderbuffer(unsigned t, unsigned a, unsigned rt, unsigned rb) { (void)t; (void)a; (void)rt; (void)rb; }
void glDeleteFramebuffers(int n, const unsigned *f) { (void)n; (void)f; }
void glDeleteTextures(int n, const unsigned *t) { (void)n; (void)t; }
void glDeleteRenderbuffers(int n, const unsigned *r) { (void)n; (void)r; }
void glTexImage2D(unsigned t, int l, int f, int w, int h, int b, unsigned fm, unsigned ty, const void *p)
{ (void)t; (void)l; (void)f; (void)w; (void)h; (void)b; (void)fm; (void)ty; (void)p; }
void glTexSubImage2D(unsigned t, int l, int x, int y, int w, int h, unsigned f, unsigned ty, const void *p)
{ (void)t; (void)l; (void)x; (void)y; (void)w; (void)h; (void)f; (void)ty; (void)p; }
void glRenderbufferStorage(unsigned t, unsigned f, int w, int h) { (void)t; (void)f; (void)w; (void)h; }
void glClearBufferfi(unsigned b, int d, float v, int s) { (void)b; (void)d; (void)v; (void)s; }
void glClearBufferfv(unsigned b, int d, const float *v) { (void)b; (void)d; (void)v; }
void glClearBufferiv(unsigned b, int d, const int *v) { (void)b; (void)d; (void)v; }
void glClearBufferuiv(unsigned b, int d, const unsigned *v) { (void)b; (void)d; (void)v; }
