// A stand-in libGLESv2.so.2 for test_pointer and test_osk: the GL state pointer.c / osk.c read and set, and a
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
extern int fake_read_fb;
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
    if (target == 0x8CA8 && es2()) { fake_errors++; return; }
    if (target == GL_DRAW_FRAMEBUFFER || target == 0x8D40) fake_draw_fb = (int)fb;
    if (target == 0x8CA8 || target == 0x8D40) fake_read_fb = (int)fb;     // GL_READ_FRAMEBUFFER
}
void fake_more_state(unsigned p, int *v);
void glGetIntegerv(unsigned p, int *v)
{
    fake_calls++;
    if (p == GL_SCISSOR_BOX) memcpy(v, fake_box, sizeof fake_box);
    else if (p == GL_DRAW_FRAMEBUFFER_BINDING) *v = fake_draw_fb;
    else fake_more_state(p, v);
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

// ---- textures, framebuffers and glBlitFramebuffer (osk.c's copy path) -------------------------
// One texture can be attached to framebuffer objects; a blit from such a read framebuffer to
// framebuffer 0 copies its pixels (no scaling, no scissor: a blit with the scissor test on is
// counted in fake_bad_blit). FAKE_SAMPLES=1: a multisampled window; FAKE_SRGB=1: an sRGB one.
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_SAMPLE_BUFFERS 0x80A8
#define GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING 0x8210
int fake_tex_binding, fake_read_fb, fake_pbo, fake_unpack[4] = { 4, 0, 0, 0 };   // alignment, row length, skip rows, skip pixels
int fake_blits, fake_bad_blit, fake_uploads, fake_tex_format;
static unsigned char *tex_px;               // texture 1's pixels (the only one handed out)
static int tex_w, tex_h, fbo_tex;           // texture attached to framebuffer 1

static int unpack_slot(unsigned p)
{
    return p == GL_UNPACK_ALIGNMENT ? 0 : p == GL_UNPACK_ROW_LENGTH ? 1 : p == GL_UNPACK_SKIP_ROWS ? 2 : p == GL_UNPACK_SKIP_PIXELS ? 3 : -1;
}
void fake_more_state(unsigned p, int *v)
{
    if (p == GL_TEXTURE_BINDING_2D) *v = fake_tex_binding;
    else if (p == GL_READ_FRAMEBUFFER_BINDING) *v = fake_read_fb;
    else if (p == GL_PIXEL_UNPACK_BUFFER_BINDING) *v = fake_pbo;
    else if (unpack_slot(p) >= 0) *v = fake_unpack[unpack_slot(p)];
    else if (p == GL_SAMPLE_BUFFERS) *v = getenv("FAKE_SAMPLES") ? 1 : 0;
}
void glGenTextures(int n, unsigned *t) { fake_calls++; for (int i = 0; i < n; i++) t[i] = 1; }
void glBindTexture(unsigned target, unsigned t) { fake_calls++; if (target == GL_TEXTURE_2D) fake_tex_binding = (int)t; }
void glTexImage2D(unsigned target, int level, int internal, int w, int h, int border, unsigned fmt, unsigned type, const void *data)
{
    fake_calls++;
    if (fake_tex_binding != 1) { fake_errors++; return; }
    free(tex_px);
    tex_px = calloc((size_t)w * h, 4);
    tex_w = w; tex_h = h; fake_tex_format = internal;
}
void glTexSubImage2D(unsigned target, int level, int x, int y, int w, int h, unsigned fmt, unsigned type, const void *data)
{
    fake_calls++;
    if (fake_tex_binding != 1 || !tex_px || fake_pbo || fake_unpack[0] != 4 || fake_unpack[1] || fake_unpack[2] || fake_unpack[3] ||
        x + w > tex_w || y + h > tex_h) { fake_errors++; return; }
    for (int j = 0; j < h; j++) memcpy(tex_px + ((size_t)(y + j) * tex_w + x) * 4, (const unsigned char *)data + (size_t)j * w * 4, (size_t)w * 4);
    fake_uploads++;
}
void glTexParameteri(unsigned target, unsigned p, int v) { fake_calls++; }
void glPixelStorei(unsigned p, int v) { fake_calls++; if (unpack_slot(p) >= 0) fake_unpack[unpack_slot(p)] = v; }
void glBindBuffer(unsigned target, unsigned b) { fake_calls++; if (target == GL_PIXEL_UNPACK_BUFFER) fake_pbo = (int)b; }
void glGenFramebuffers(int n, unsigned *f) { fake_calls++; for (int i = 0; i < n; i++) f[i] = 1; }
void glFramebufferTexture2D(unsigned target, unsigned att, unsigned tt, unsigned t, int level)
{
    fake_calls++;
    if (target == GL_READ_FRAMEBUFFER && fake_read_fb == 1) fbo_tex = (int)t; else fake_errors++;
}
unsigned glCheckFramebufferStatus(unsigned target) { fake_calls++; return target == GL_READ_FRAMEBUFFER && fake_read_fb == 1 && fbo_tex ? 0x8CD5 : 0; }
void glBlitFramebuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, unsigned mask, unsigned filter)
{
    fake_calls++;
    if (fake_scissor_on || fake_discard_on || es2() || getenv("FAKE_SAMPLES")) { fake_bad_blit++; return; }
    if (fake_read_fb != 1 || !fbo_tex || fake_draw_fb != 0 || sx1 - sx0 != dx1 - dx0 || sy1 - sy0 != dy1 - dy0 ||
        sx1 > tex_w || sy1 > tex_h || dx1 > W || dy1 > H) { fake_bad_blit++; return; }
    for (int y = 0; y < sy1 - sy0; y++)
        for (int x = 0; x < sx1 - sx0; x++)
            memcpy(fake_fb0[dy0 + y][dx0 + x], tex_px + ((size_t)(sy0 + y) * tex_w + sx0 + x) * 4, 4);
    fake_blits++;
}
void glGetFramebufferAttachmentParameteriv(unsigned target, unsigned att, unsigned p, int *v)
{
    fake_calls++;
    if (p == GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING) *v = getenv("FAKE_SRGB") ? 0x8C40 : 0x2601;
}
