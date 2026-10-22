// A fake libGLESv2 for test_msaafix.c: framebuffer bindings, blits (recorded with the read and draw
// framebuffers they were made from), draws, clears. Framebuffer 1's colour buffer is renderbuffer 7
// with FAKE_RB_SAMPLES samples (default 4); framebuffer 2 has a texture; framebuffer 3 renderbuffer
// 8, single-sampled.
#include <stdlib.h>

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_RENDERBUFFER 0x8D41
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_RENDERBUFFER_SAMPLES 0x8CAB
#define GL_TEXTURE 0x1702
#define GL_SAMPLES 0x80A9
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1

int fake_read_fb, fake_draw_fb, fake_rb;
int fake_blits, fake_blit_read, fake_blit_draw, fake_draws, fake_clears;

void glBindFramebuffer(unsigned target, unsigned fb)
{
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) fake_read_fb = (int)fb;
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) fake_draw_fb = (int)fb;
}
void glBindRenderbuffer(unsigned target, unsigned rb) { (void)target; fake_rb = (int)rb; }
void glGetIntegerv(unsigned p, int *v)
{
    if (p == GL_READ_FRAMEBUFFER_BINDING) *v = fake_read_fb;
    else if (p == GL_DRAW_FRAMEBUFFER_BINDING) *v = fake_draw_fb;
    else if (p == GL_RENDERBUFFER_BINDING) *v = fake_rb;
    else if (p == GL_SAMPLES) *v = 0;
    else *v = 0;
}
void glGetFramebufferAttachmentParameteriv(unsigned target, unsigned att, unsigned p, int *v)
{
    (void)att;
    int fb = target == GL_DRAW_FRAMEBUFFER ? fake_draw_fb : fake_read_fb;
    if (p == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE) *v = fb == 1 || fb == 3 ? GL_RENDERBUFFER : fb == 2 ? GL_TEXTURE : 0;
    else if (p == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) *v = fb == 1 ? 7 : fb == 3 ? 8 : fb == 2 ? 22 : 0;
}
void glGetRenderbufferParameteriv(unsigned target, unsigned p, int *v)
{
    (void)target;
    if (p == GL_RENDERBUFFER_SAMPLES) *v = fake_rb == 7 ? (getenv("FAKE_RB_SAMPLES") ? atoi(getenv("FAKE_RB_SAMPLES")) : 4) : 0;
}
void glBlitFramebuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, unsigned mask, unsigned filter)
{
    (void)sx0; (void)sy0; (void)sx1; (void)sy1; (void)dx0; (void)dy0; (void)dx1; (void)dy1; (void)mask; (void)filter;
    fake_blits++; fake_blit_read = fake_read_fb; fake_blit_draw = fake_draw_fb;
}
void glClear(unsigned bits) { (void)bits; fake_clears++; }
void glDrawArrays(unsigned m, int f, int c) { (void)m; (void)f; (void)c; fake_draws++; }
void glDrawElements(unsigned m, int c, unsigned t, const void *i) { (void)m; (void)c; (void)t; (void)i; fake_draws++; }
