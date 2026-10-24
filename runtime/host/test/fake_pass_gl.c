// A fake libGLESv2 for test_glpass.c: framebuffers 3 (colour texture 9, 64x32) and 4 (colour texture 9
// too, and texture 10), 5 (colour renderbuffer 30, 64x32), the fixed-function state glpass.c reads and
// sets (scissor, colour mask, clear colour, dither, rasterizer discard), and a record of every glClear
// and draw with the framebuffer and state they ran with.
#include <string.h>

#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
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
#define GL_SCISSOR_TEST 0x0C11
#define GL_RASTERIZER_DISCARD 0x8C89
#define GL_DITHER 0x0BD0
#define GL_SCISSOR_BOX 0x0C10
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_DRAW_BUFFER0 0x8825
#define GL_MAX_COLOR_ATTACHMENTS 0x8CDF
#define GL_EXTENSIONS 0x1F03

int fake_draw_fb, fake_read_fb, fake_tex2d, fake_rb;
int fake_scissor, fake_discard, fake_dither = 1;
int fake_box[4] = { 0, 0, 64, 32 };
int fake_mask[4] = { 1, 1, 1, 1 };
float fake_cc[4];
int fake_draws, fake_invalidates;

typedef struct { int fb; unsigned mask; float c[4]; int scissor, discard, dither, cmask[4]; } fake_clear_t;
fake_clear_t fake_clears[64];
int fake_nclears;
int fake_order[128], fake_norder;    // 'c' clear, 'd' draw, 'i' invalidate, in order

void glBindFramebuffer(unsigned t, unsigned fb)
{
    if (t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER) fake_draw_fb = (int)fb;
    if (t == GL_FRAMEBUFFER || t == GL_READ_FRAMEBUFFER) fake_read_fb = (int)fb;
}
void glClear(unsigned m)
{
    if (fake_nclears < 64) {
        fake_clear_t *c = &fake_clears[fake_nclears++];
        *c = (fake_clear_t){ fake_draw_fb, m, { fake_cc[0], fake_cc[1], fake_cc[2], fake_cc[3] }, fake_scissor, fake_discard, fake_dither,
                             { fake_mask[0], fake_mask[1], fake_mask[2], fake_mask[3] } };
    }
    if (fake_norder < 128) fake_order[fake_norder++] = 'c';
}
void glClearColor(float r, float g, float b, float a) { fake_cc[0] = r; fake_cc[1] = g; fake_cc[2] = b; fake_cc[3] = a; }
void glColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { fake_mask[0] = r; fake_mask[1] = g; fake_mask[2] = b; fake_mask[3] = a; }
static int *cap(unsigned c)
{
    static int other;
    return c == GL_SCISSOR_TEST ? &fake_scissor : c == GL_RASTERIZER_DISCARD ? &fake_discard : c == GL_DITHER ? &fake_dither : &other;
}
void glEnable(unsigned c) { *cap(c) = 1; }
void glDisable(unsigned c) { *cap(c) = 0; }
unsigned char glIsEnabled(unsigned c) { return (unsigned char)*cap(c); }
void glScissor(int x, int y, int w, int h) { fake_box[0] = x; fake_box[1] = y; fake_box[2] = w; fake_box[3] = h; }

void glGetIntegerv(unsigned p, int *v)
{
    switch (p) {
    case GL_DRAW_FRAMEBUFFER_BINDING: *v = fake_draw_fb; break;
    case GL_READ_FRAMEBUFFER_BINDING: *v = fake_read_fb; break;
    case GL_TEXTURE_BINDING_2D: *v = fake_tex2d; break;
    case GL_RENDERBUFFER_BINDING: *v = fake_rb; break;
    case GL_SCISSOR_BOX: memcpy(v, fake_box, sizeof fake_box); break;
    case GL_COLOR_WRITEMASK: memcpy(v, fake_mask, sizeof fake_mask); break;
    case GL_DRAW_BUFFER0: *v = fake_draw_fb ? GL_COLOR_ATTACHMENT0 : 0x0405; break;
    case GL_MAX_COLOR_ATTACHMENTS: *v = 4; break;
    default: *v = 0;
    }
}
void glGetFloatv(unsigned p, float *v) { if (p == GL_COLOR_CLEAR_VALUE) memcpy(v, fake_cc, sizeof fake_cc); }
const unsigned char *glGetString(unsigned n) { return (const unsigned char *)(n == GL_EXTENSIONS ? "GL_EXT_foo" : "fake"); }

void glGetFramebufferAttachmentParameteriv(unsigned t, unsigned a, unsigned p, int *v)
{
    int fb = t == GL_READ_FRAMEBUFFER ? fake_read_fb : fake_draw_fb, type = 0, name = 0;
    if (a == GL_COLOR_ATTACHMENT0 && (fb == 3 || fb == 4)) { type = GL_TEXTURE; name = 9; }
    if (a == GL_COLOR_ATTACHMENT0 + 1 && fb == 4) { type = GL_TEXTURE; name = 10; }
    if (a == GL_COLOR_ATTACHMENT0 && fb == 5) { type = GL_RENDERBUFFER; name = 30; }
    *v = p == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE ? type : p == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME ? name : 0;
}
void glBindTexture(unsigned t, unsigned tex) { (void)t; fake_tex2d = (int)tex; }
void glBindRenderbuffer(unsigned t, unsigned rb) { (void)t; fake_rb = (int)rb; }
void glGetTexLevelParameteriv(unsigned t, int l, unsigned p, int *v)
{ (void)t; (void)l; *v = fake_tex2d == 9 || fake_tex2d == 10 ? (p == GL_TEXTURE_WIDTH ? 64 : p == GL_TEXTURE_HEIGHT ? 32 : 0) : 0; }
void glGetRenderbufferParameteriv(unsigned t, unsigned p, int *v)
{ (void)t; *v = fake_rb == 30 ? (p == GL_RENDERBUFFER_WIDTH ? 64 : p == GL_RENDERBUFFER_HEIGHT ? 32 : 0) : 0; }

static void drew(void) { fake_draws++; if (fake_norder < 128) fake_order[fake_norder++] = 'd'; }
void glDrawArrays(unsigned m, int f, int c) { (void)m; (void)f; (void)c; drew(); }
void glDrawElements(unsigned m, int c, unsigned t, const void *i) { (void)m; (void)c; (void)t; (void)i; drew(); }
void glDrawArraysInstanced(unsigned m, int f, int c, int n) { (void)m; (void)f; (void)c; (void)n; drew(); }
void glDrawElementsInstanced(unsigned m, int c, unsigned t, const void *i, int n) { (void)m; (void)c; (void)t; (void)i; (void)n; drew(); }
void glDrawRangeElements(unsigned m, unsigned s, unsigned e, int c, unsigned t, const void *i)
{ (void)m; (void)s; (void)e; (void)c; (void)t; (void)i; drew(); }
void glBlitFramebuffer(int a, int b, int c, int d, int e, int f, int g, int h, unsigned m, unsigned fl)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)m; (void)fl; drew(); }
void glInvalidateFramebuffer(unsigned t, int n, const unsigned *a)
{ (void)t; (void)n; (void)a; fake_invalidates++; if (fake_norder < 128) fake_order[fake_norder++] = 'i'; }
void glFramebufferTexture2D(unsigned t, unsigned a, unsigned tt, unsigned tex, int l) { (void)t; (void)a; (void)tt; (void)tex; (void)l; }
void glFramebufferTextureLayer(unsigned t, unsigned a, unsigned tex, int l, int y) { (void)t; (void)a; (void)tex; (void)l; (void)y; }
void glFramebufferRenderbuffer(unsigned t, unsigned a, unsigned rt, unsigned rb) { (void)t; (void)a; (void)rt; (void)rb; }
void glDrawBuffers(int n, const unsigned *b) { (void)n; (void)b; }
void glDeleteFramebuffers(int n, const unsigned *f) { (void)n; (void)f; }
void glDeleteTextures(int n, const unsigned *t) { (void)n; (void)t; }
void glDeleteRenderbuffers(int n, const unsigned *r) { (void)n; (void)r; }
void glRenderbufferStorage(unsigned t, unsigned f, int w, int h) { (void)t; (void)f; (void)w; (void)h; }
void glRenderbufferStorageMultisample(unsigned t, int s, unsigned f, int w, int h) { (void)t; (void)s; (void)f; (void)w; (void)h; }
void glTexImage2D(unsigned t, int l, int f, int w, int h, int b, unsigned fm, unsigned ty, const void *p)
{ (void)t; (void)l; (void)f; (void)w; (void)h; (void)b; (void)fm; (void)ty; (void)p; }
void glTexSubImage2D(unsigned t, int l, int x, int y, int w, int h, unsigned f, unsigned ty, const void *p)
{ (void)t; (void)l; (void)x; (void)y; (void)w; (void)h; (void)f; (void)ty; (void)p; }
void glTexStorage2D(unsigned t, int l, unsigned f, int w, int h) { (void)t; (void)l; (void)f; (void)w; (void)h; }
void glCompressedTexImage2D(unsigned t, int l, unsigned f, int w, int h, int b, int s, const void *d)
{ (void)t; (void)l; (void)f; (void)w; (void)h; (void)b; (void)s; (void)d; }
void glCompressedTexSubImage2D(unsigned t, int l, int x, int y, int w, int h, unsigned f, int s, const void *d)
{ (void)t; (void)l; (void)x; (void)y; (void)w; (void)h; (void)f; (void)s; (void)d; }
void glCopyTexImage2D(unsigned t, int l, unsigned f, int x, int y, int w, int h, int b) { (void)t; (void)l; (void)f; (void)x; (void)y; (void)w; (void)h; (void)b; }
void glCopyTexSubImage2D(unsigned t, int l, int xo, int yo, int x, int y, int w, int h)
{ (void)t; (void)l; (void)xo; (void)yo; (void)x; (void)y; (void)w; (void)h; }
void glTexImage3D(unsigned t, int l, int f, int w, int h, int d, int b, unsigned fm, unsigned ty, const void *p)
{ (void)t; (void)l; (void)f; (void)w; (void)h; (void)d; (void)b; (void)fm; (void)ty; (void)p; }
void glTexSubImage3D(unsigned t, int l, int x, int y, int z, int w, int h, int d, unsigned f, unsigned ty, const void *p)
{ (void)t; (void)l; (void)x; (void)y; (void)z; (void)w; (void)h; (void)d; (void)f; (void)ty; (void)p; }
void glTexStorage3D(unsigned t, int l, unsigned f, int w, int h, int d) { (void)t; (void)l; (void)f; (void)w; (void)h; (void)d; }
void glCopyTexSubImage3D(unsigned t, int l, int xo, int yo, int zo, int x, int y, int w, int h)
{ (void)t; (void)l; (void)xo; (void)yo; (void)zo; (void)x; (void)y; (void)w; (void)h; }
void glCompressedTexImage3D(unsigned t, int l, unsigned f, int w, int h, int d, int b, int s, const void *p)
{ (void)t; (void)l; (void)f; (void)w; (void)h; (void)d; (void)b; (void)s; (void)p; }
void glCompressedTexSubImage3D(unsigned t, int l, int x, int y, int z, int w, int h, int d, unsigned f, int s, const void *p)
{ (void)t; (void)l; (void)x; (void)y; (void)z; (void)w; (void)h; (void)d; (void)f; (void)s; (void)p; }
void glGenerateMipmap(unsigned t) { (void)t; }
void glClearBufferfi(unsigned b, int d, float v, int s) { (void)b; (void)d; (void)v; (void)s; }
void glClearBufferfv(unsigned b, int d, const float *v) { (void)b; (void)d; (void)v; }
void glClearBufferiv(unsigned b, int d, const int *v) { (void)b; (void)d; (void)v; }
void glClearBufferuiv(unsigned b, int d, const unsigned *v) { (void)b; (void)d; (void)v; }
