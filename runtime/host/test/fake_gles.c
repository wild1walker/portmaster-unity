// A stand-in libGLESv2.so.2 for test_pointer: the GL state pointer.c reads and sets, and a
// 640x480 RGBA "window" (framebuffer 0) that scissored glClear calls fill. Clears into another
// framebuffer, or with rasterizer discard on, land nowhere (as in GL). FAKE_GLES2=1: a GLES 2
// context, where GLES 3 enums are errors (fake_errors).
#include <stdlib.h>
#include <string.h>

#define W 640
#define H 480
unsigned char fake_fb0[H][W][4];          // row 0 = bottom (GL)
int fake_calls, fake_clears, fake_other_fb_clears, fake_bad_scissor, fake_errors;
int fake_scissor_on, fake_discard_on, fake_box[4] = { 0, 0, W, H }, fake_draw_fb;
float fake_cc[4];
unsigned char fake_mask[4] = { 1, 1, 1, 1 };

#define GL_SCISSOR_BOX 0x0C10
#define GL_SCISSOR_TEST 0x0C11
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_RASTERIZER_DISCARD 0x8C89

static int es2(void) { return getenv("FAKE_GLES2") != NULL; }
static int *cap(unsigned c)
{
    if (c == GL_RASTERIZER_DISCARD && es2()) { fake_errors++; return NULL; }
    return c == GL_SCISSOR_TEST ? &fake_scissor_on : c == GL_RASTERIZER_DISCARD ? &fake_discard_on : NULL;
}
const unsigned char *glGetString(unsigned name)
{
    fake_calls++;
    return (const unsigned char *)(name != 0x1F02 ? "fake" : es2() ? "OpenGL ES 2.0 fake" : "OpenGL ES 3.2 fake");
}
void glEnable(unsigned c) { fake_calls++; if (cap(c)) *cap(c) = 1; }
void glDisable(unsigned c) { fake_calls++; if (cap(c)) *cap(c) = 0; }
unsigned char glIsEnabled(unsigned c) { fake_calls++; return cap(c) ? (unsigned char)*cap(c) : 0; }
void glScissor(int x, int y, int w, int h) { fake_calls++; fake_box[0] = x; fake_box[1] = y; fake_box[2] = w; fake_box[3] = h; }
void glClearColor(float r, float g, float b, float a) { fake_calls++; fake_cc[0] = r; fake_cc[1] = g; fake_cc[2] = b; fake_cc[3] = a; }
void glColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { fake_calls++; fake_mask[0] = r; fake_mask[1] = g; fake_mask[2] = b; fake_mask[3] = a; }
void glBindFramebuffer(unsigned target, unsigned fb)
{
    fake_calls++;
    if (target == GL_DRAW_FRAMEBUFFER && es2()) { fake_errors++; return; }
    if (target == GL_DRAW_FRAMEBUFFER || target == 0x8D40) fake_draw_fb = (int)fb;
}
void glGetIntegerv(unsigned p, int *v)
{
    fake_calls++;
    if (p == GL_SCISSOR_BOX) memcpy(v, fake_box, sizeof fake_box);
    else if (p == GL_DRAW_FRAMEBUFFER_BINDING) *v = fake_draw_fb;
}
void glGetFloatv(unsigned p, float *v) { fake_calls++; if (p == GL_COLOR_CLEAR_VALUE) memcpy(v, fake_cc, sizeof fake_cc); }
void glGetBooleanv(unsigned p, unsigned char *v) { fake_calls++; if (p == GL_COLOR_WRITEMASK) memcpy(v, fake_mask, 4); }
void glClear(unsigned bits)
{
    fake_calls++;
    if (!(bits & GL_COLOR_BUFFER_BIT) || fake_discard_on) return;
    if (fake_draw_fb) { fake_other_fb_clears++; return; }
    fake_clears++;
    int x0 = 0, y0 = 0, x1 = W, y1 = H;
    if (fake_scissor_on)
    {
        if (fake_box[0] < 0 || fake_box[1] < 0 || fake_box[2] <= 0 || fake_box[3] <= 0 ||
            fake_box[0] + fake_box[2] > W || fake_box[1] + fake_box[3] > H) fake_bad_scissor++;
        x0 = fake_box[0]; y0 = fake_box[1]; x1 = x0 + fake_box[2]; y1 = y0 + fake_box[3];
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > W) x1 = W;
        if (y1 > H) y1 = H;
    }
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            for (int c = 0; c < 4; c++)
                if (fake_mask[c]) fake_fb0[y][x][c] = (unsigned char)(fake_cc[c] * 255.f + 0.5f);
}
