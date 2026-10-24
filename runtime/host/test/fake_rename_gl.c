// A fake libGLESv2 for test_glrename.c that computes pictures: 2D RGBA8 textures with pixels and
// parameters, framebuffers with a colour attachment, texture units, glClear, glBlitFramebuffer,
// glReadPixels (also into a pixel pack buffer), fences that are always done, and programs whose
// draws fill the whole viewport of the draw framebuffer's colour buffer:
//   program 5  "copy": out = texture(tex) * scale (uniform vec4 scale, sampler tex)
//   program 6  "invert": out = 255 - texture(tex)
//   program 7  "paint": out = (x + 16 y + seed) per channel, seed a uniform float
//   program 8  "add": out = texture(tex) + 1 per channel (the blend-mode sprite of a grab pass)
//   programs it links itself from source: texelFetch of unit 0 at the fragment (glrename.c's copy)
// The texture a sampler reads is unit n's 2D binding (n the sampler uniform's value). An
// invalidated colour buffer is filled with 0xCD, so a picture that relied on it shows.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_TEXTURE 0x1702
#define GL_PIXEL_PACK_BUFFER 0x88EB

#define NTEX 64
typedef struct { int alive, w, h, imm; int p[16]; float f[4]; unsigned char *px; } tex_t;
tex_t fake_tex[NTEX];
typedef struct { unsigned col; } fb_t;
fb_t fake_fb[64];
int fake_draw_fb, fake_read_fb, fake_active, fake_unit[8], fake_prog, fake_vao, fake_vp[4] = { 0, 0, 4, 4 };
int fake_draws, fake_blits, fake_reads, fake_invalidates, fake_clears, fake_copies;
float fake_cc[4];
float fake_scale[4] = { 1, 1, 1, 1 }, fake_seed;
int fake_sampler_unit = 0;            // the sampler uniform of programs 5, 6, 8
int fake_ours_unit = 0;              // glrename's program's sampler
static int next_prog = 20;
static int prog_is_fetch[64];
static unsigned char *pbo_data; static int pbo_size; static int pbo_bound, pbo_name = 0;
static int ebuf_gen;

static int param_index(unsigned p)
{
    static const unsigned ps[] = { 0x2801, 0x2800, 0x2802, 0x2803, 0x8072, 0x813C, 0x813D, 0x884C, 0x884D, 0x8E42, 0x8E43, 0x8E44, 0x8E45 };
    for (int i = 0; i < 13; i++) if (ps[i] == p) return i;
    return -1;
}

void glGenTextures(int n, unsigned *t)
{
    for (int i = 0; i < n; i++) {
        int k = 1;
        while (k < NTEX && fake_tex[k].alive) k++;
        fake_tex[k] = (tex_t){ .alive = 1 };
        fake_tex[k].p[0] = 0x2702; fake_tex[k].p[1] = 0x2601; fake_tex[k].p[6] = 1000;
        fake_tex[k].f[0] = -1000; fake_tex[k].f[1] = 1000;
        t[i] = (unsigned)k;
    }
}
void glDeleteTextures(int n, const unsigned *t)
{
    for (int i = 0; i < n; i++) {
        unsigned k = t[i];
        if (!k || k >= NTEX || !fake_tex[k].alive) continue;
        free(fake_tex[k].px);
        fake_tex[k] = (tex_t){ 0 };
        for (int u = 0; u < 8; u++) if (fake_unit[u] == (int)k) fake_unit[u] = 0;
        if (fake_fb[fake_draw_fb].col == k) fake_fb[fake_draw_fb].col = 0;
        if (fake_fb[fake_read_fb].col == k) fake_fb[fake_read_fb].col = 0;
    }
}
unsigned char glIsTexture(unsigned t) { return t < NTEX && fake_tex[t].alive; }
void glActiveTexture(unsigned u) { fake_active = (int)(u - GL_TEXTURE0); }
void glBindTexture(unsigned t, unsigned tex) { (void)t; if (tex < NTEX && !fake_tex[tex].alive && tex) { fake_tex[tex] = (tex_t){ .alive = 1 }; } fake_unit[fake_active] = (int)tex; }
static tex_t *bound(void) { return &fake_tex[fake_unit[fake_active]]; }
void glTexStorage2D(unsigned t, int l, unsigned f, int w, int h)
{ (void)t; (void)f; tex_t *x = bound(); free(x->px); x->w = w; x->h = h; x->imm = l; x->px = calloc((size_t)w * h, 4); }
void glTexImage2D(unsigned t, int l, int f, int w, int h, int b, unsigned fm, unsigned ty, const void *p)
{
    (void)t; (void)l; (void)f; (void)b; (void)fm; (void)ty;
    tex_t *x = bound(); free(x->px); x->w = w; x->h = h; x->imm = 0; x->px = calloc((size_t)w * h, 4);
    if (p) memcpy(x->px, p, (size_t)w * h * 4);
}
void glTexSubImage2D(unsigned t, int l, int x0, int y0, int w, int h, unsigned f, unsigned ty, const void *p)
{
    (void)t; (void)l; (void)f; (void)ty;
    tex_t *x = bound();
    const unsigned char *s = p;
    for (int y = 0; y < h; y++) memcpy(x->px + ((size_t)(y0 + y) * x->w + x0) * 4, s + (size_t)y * w * 4, (size_t)w * 4);
}
void glTexParameteri(unsigned t, unsigned p, int v) { (void)t; int i = param_index(p); if (i >= 0) bound()->p[i] = v; }
void glTexParameterf(unsigned t, unsigned p, float v) { (void)t; if (p == 0x813A) bound()->f[0] = v; if (p == 0x813B) bound()->f[1] = v; }
void glGetTexParameteriv(unsigned t, unsigned p, int *v)
{
    (void)t;
    tex_t *x = bound();
    int i = param_index(p);
    if (i >= 0) *v = x->p[i];
    else if (p == 0x912F) *v = x->imm > 0;       // IMMUTABLE_FORMAT
    else if (p == 0x82DF) *v = x->imm;           // IMMUTABLE_LEVELS
    else *v = 0;
}
void glGetTexParameterfv(unsigned t, unsigned p, float *v) { (void)t; *v = p == 0x813A ? bound()->f[0] : p == 0x813B ? bound()->f[1] : 0; }
void glGetTexLevelParameteriv(unsigned t, int l, unsigned p, int *v)
{
    (void)t;
    tex_t *x = bound();
    *v = l ? 0 : p == 0x1000 ? x->w : p == 0x1001 ? x->h : p == 0x1003 ? (x->px ? 0x8058 : 0) : 0;
}
void glPixelStorei(unsigned p, int v) { (void)p; (void)v; }

void glGenFramebuffers(int n, unsigned *f) { static unsigned next = 10; for (int i = 0; i < n; i++) f[i] = next++; }
void glBindFramebuffer(unsigned t, unsigned fb)
{
    if (t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER) fake_draw_fb = (int)fb;
    if (t == GL_FRAMEBUFFER || t == GL_READ_FRAMEBUFFER) fake_read_fb = (int)fb;
}
void glFramebufferTexture2D(unsigned t, unsigned a, unsigned tt, unsigned tex, int l)
{
    (void)tt; (void)l;
    if (a != GL_COLOR_ATTACHMENT0) return;
    fake_fb[t == GL_READ_FRAMEBUFFER ? fake_read_fb : fake_draw_fb].col = tex;
}
void glFramebufferTextureLayer(unsigned t, unsigned a, unsigned tex, int l, int y) { (void)t; (void)a; (void)tex; (void)l; (void)y; }
void glFramebufferRenderbuffer(unsigned t, unsigned a, unsigned rt, unsigned rb) { (void)t; (void)a; (void)rt; (void)rb; }
void glDeleteFramebuffers(int n, const unsigned *f) { for (int i = 0; i < n; i++) if (f[i] < 64) fake_fb[f[i]].col = 0; }
void glGetFramebufferAttachmentParameteriv(unsigned t, unsigned a, unsigned p, int *v)
{
    unsigned c = a == GL_COLOR_ATTACHMENT0 ? fake_fb[t == GL_READ_FRAMEBUFFER ? fake_read_fb : fake_draw_fb].col : 0;
    *v = p == 0x8CD0 ? (c ? GL_TEXTURE : 0) : p == 0x8CD1 ? (int)c : 0;
}
static unsigned char *colour(int fb) { unsigned c = fake_fb[fb].col; return c && c < NTEX ? fake_tex[c].px : NULL; }
static int colour_w(int fb) { return fake_tex[fake_fb[fb].col].w; }
static int colour_h(int fb) { return fake_tex[fake_fb[fb].col].h; }

void glClearColor(float r, float g, float b, float a) { fake_cc[0] = r; fake_cc[1] = g; fake_cc[2] = b; fake_cc[3] = a; }
void glClear(unsigned m)
{
    fake_clears++;
    unsigned char *d = colour(fake_draw_fb);
    if (!(m & 0x4000) || !d) return;
    for (int i = 0; i < colour_w(fake_draw_fb) * colour_h(fake_draw_fb); i++)
        for (int c = 0; c < 4; c++) d[4 * i + c] = (unsigned char)(fake_cc[c] * 255 + 0.5f);
}
void glInvalidateFramebuffer(unsigned t, int n, const unsigned *a)
{
    (void)n; (void)a;
    fake_invalidates++;
    unsigned char *d = colour(t == GL_READ_FRAMEBUFFER ? fake_read_fb : fake_draw_fb);
    if (d) memset(d, 0xCD, (size_t)colour_w(fake_draw_fb) * colour_h(fake_draw_fb) * 4);
}
void glBlitFramebuffer(int a, int b, int c, int d, int e, int f, int g, int h, unsigned m, unsigned fl)
{
    (void)a; (void)b; (void)e; (void)f; (void)g; (void)h; (void)m; (void)fl;
    fake_blits++;
    unsigned char *s = colour(fake_read_fb), *t = colour(fake_draw_fb);
    if (s && t) memcpy(t, s, (size_t)c * d * 4);
}
void glReadPixels(int x, int y, int w, int h, unsigned f, unsigned t, void *p)
{
    (void)x; (void)y; (void)f; (void)t;
    fake_reads++;
    unsigned char *s = colour(fake_read_fb);
    if (!s) return;
    if (pbo_bound) { if (pbo_size >= w * h * 4) memcpy(pbo_data, s, (size_t)w * h * 4); }
    else memcpy(p, s, (size_t)w * h * 4);
}

// the draws
static unsigned char at(int unit, int i, int c)
{
    tex_t *x = &fake_tex[fake_unit[unit]];
    return x->px && i < x->w * x->h ? x->px[4 * i + c] : 0;
}
static void run(void)
{
    fake_draws++;
    if (fake_prog == 5) fake_copies++;
    unsigned char *d = colour(fake_draw_fb);
    if (!d) return;
    int w = colour_w(fake_draw_fb), h = colour_h(fake_draw_fb);
    for (int y = 0; y < h && y < fake_vp[3]; y++)
        for (int x = 0; x < w && x < fake_vp[2]; x++) {
            int i = y * w + x;
            for (int c = 0; c < 4; c++) {
                int v;
                if (fake_prog == 5) v = (int)(at(fake_sampler_unit, i, c) * fake_scale[c] + 0.5f);
                else if (fake_prog == 6) v = 255 - at(fake_sampler_unit, i, c);
                else if (fake_prog == 7) v = (x + 16 * y + (int)fake_seed) & 255;
                else if (fake_prog == 8) v = at(fake_sampler_unit, i, c) + 1;
                else if (fake_prog < 64 && prog_is_fetch[fake_prog]) v = at(fake_ours_unit, i, c);
                else v = d[4 * i + c];
                d[4 * i + c] = (unsigned char)(v > 255 ? 255 : v);
            }
        }
}
void glDrawElements(unsigned m, int c, unsigned t, const void *i) { (void)m; (void)c; (void)t; (void)i; run(); }
void glDrawArrays(unsigned m, int f, int c) { (void)m; (void)f; (void)c; run(); }
void glDrawRangeElements(unsigned m, unsigned s, unsigned e, int c, unsigned t, const void *i) { (void)m; (void)s; (void)e; (void)c; (void)t; (void)i; run(); }
void glDrawArraysInstanced(unsigned m, int f, int c, int n) { (void)m; (void)f; (void)c; (void)n; run(); }
void glDrawElementsInstanced(unsigned m, int c, unsigned t, const void *i, int n) { (void)m; (void)c; (void)t; (void)i; (void)n; run(); }
void glViewport(int x, int y, int w, int h) { fake_vp[0] = x; fake_vp[1] = y; fake_vp[2] = w; fake_vp[3] = h; }
void glUseProgram(unsigned p) { fake_prog = (int)p; }
void glBindVertexArray(unsigned v) { fake_vao = (int)v; }
void glGenVertexArrays(int n, unsigned *v) { for (int i = 0; i < n; i++) v[i] = 30 + (unsigned)i; }
void glBindSampler(unsigned u, unsigned s) { (void)u; (void)s; }

// programs
unsigned glCreateShader(unsigned t) { static unsigned n = 40; (void)t; return n++; }
static int last_fetch;
void glShaderSource(unsigned s, int n, const char *const *src, const int *len) { (void)s; (void)n; (void)len; if (strstr(src[0], "texelFetch")) last_fetch = 1; }
void glCompileShader(unsigned s) { (void)s; }
unsigned glCreateProgram(void) { return (unsigned)next_prog++; }
void glAttachShader(unsigned p, unsigned s) { (void)s; if (p < 64 && last_fetch) prog_is_fetch[p] = 1; }
void glLinkProgram(unsigned p) { (void)p; last_fetch = 0; }
void glProgramBinary(unsigned p, unsigned f, const void *b, int l) { (void)p; (void)f; (void)b; (void)l; }
void glDeleteProgram(unsigned p) { (void)p; }
void glGetProgramiv(unsigned p, unsigned q, int *v)
{
    int ours = p < 64 && prog_is_fetch[p];
    switch (q) {
    case 0x8B82: *v = 1; break;                                   // LINK_STATUS
    case 0x8B86: *v = ours ? 1 : p == 7 ? 1 : 2; break;           // ACTIVE_UNIFORMS: sampler + scale/seed
    case 0x8B89: *v = ours ? 0 : 1; break;                        // ACTIVE_ATTRIBUTES
    default: *v = 0;
    }
}
void glGetActiveUniform(unsigned p, unsigned i, int bs, int *len, int *size, unsigned *type, char *name)
{
    (void)bs;
    const char *n = (p == 7 || i == 1) ? (p == 7 ? "seed" : "scale") : "tex";
    *type = p == 7 ? 0x1406 : i == 1 ? 0x8B52 : 0x8B5E;
    *size = 1;
    strcpy(name, n); *len = (int)strlen(n);
}
int glGetUniformLocation(unsigned p, const char *n) { (void)p; return !strcmp(n, "tex") || !strcmp(n, "ua_src") ? 0 : !strcmp(n, "scale") ? 1 : !strcmp(n, "seed") ? 2 : -1; }
void glGetUniformfv(unsigned p, int l, float *v) { (void)p; if (l == 1) memcpy(v, fake_scale, sizeof fake_scale); if (l == 2) v[0] = fake_seed; }
void glGetUniformiv(unsigned p, int l, int *v) { (void)p; if (l == 0) *v = fake_sampler_unit; }
void glUniform1i(int l, int v) { if (l == 0 && fake_prog < 64 && prog_is_fetch[fake_prog]) fake_ours_unit = v; }
void glGetActiveAttrib(unsigned p, unsigned i, int bs, int *len, int *size, unsigned *type, char *name)
{ (void)p; (void)i; (void)bs; strcpy(name, "pos"); *len = 3; *size = 1; *type = 0x8B52; }
int glGetAttribLocation(unsigned p, const char *n) { (void)p; (void)n; return 0; }
void glGetVertexAttribiv(unsigned i, unsigned p, int *v)
{
    (void)i;
    switch (p) {
    case 0x8622: *v = 1; break; case 0x8623: *v = 4; break; case 0x8624: *v = 16; break; case 0x8625: *v = 0x1406; break;
    case 0x889F: *v = 7; break; default: *v = 0;
    }
}
void glGetVertexAttribfv(unsigned i, unsigned p, float *v) { (void)i; (void)p; v[0] = v[1] = v[2] = v[3] = 0; }
void glGetVertexAttribPointerv(unsigned i, unsigned p, void **v) { (void)i; (void)p; *v = NULL; }
void glGetQueryiv(unsigned t, unsigned p, int *v) { (void)t; (void)p; *v = 0; }

// buffers: a pixel pack buffer that holds data; the others only exist
static int bound_pack;
void glGenBuffers(int n, unsigned *b) { for (int i = 0; i < n; i++) b[i] = 50 + (unsigned)i; }
void glBindBuffer(unsigned t, unsigned b) { if (t == GL_PIXEL_PACK_BUFFER) { bound_pack = (int)b; pbo_bound = b != 0; } }
void glBufferData(unsigned t, intptr_t s, const void *d, unsigned u)
{
    (void)d; (void)u;
    if (t == GL_PIXEL_PACK_BUFFER && bound_pack) { free(pbo_data); pbo_data = calloc((size_t)s, 1); pbo_size = (int)s; pbo_name = bound_pack; }
    else ebuf_gen++;
}
void glBufferSubData(unsigned t, intptr_t o, intptr_t s, const void *d) { (void)t; (void)o; (void)s; (void)d; ebuf_gen++; }
void *glMapBufferRange(unsigned t, intptr_t o, intptr_t l, unsigned a) { (void)o; (void)l; (void)a; return t == GL_PIXEL_PACK_BUFFER ? pbo_data : NULL; }
void glFlushMappedBufferRange(unsigned t, intptr_t o, intptr_t l) { (void)t; (void)o; (void)l; }
unsigned char glUnmapBuffer(unsigned t) { (void)t; return 1; }
void glCopyBufferSubData(unsigned a, unsigned b, intptr_t c, intptr_t d, intptr_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void glDeleteBuffers(int n, const unsigned *b) { (void)n; (void)b; }
void glBindBufferBase(unsigned t, unsigned i, unsigned b) { (void)t; (void)i; (void)b; }
void glBindBufferRange(unsigned t, unsigned i, unsigned b, intptr_t o, intptr_t s) { (void)t; (void)i; (void)b; (void)o; (void)s; }
void *glFenceSync(unsigned c, unsigned f) { (void)c; (void)f; return (void *)1; }
unsigned glClientWaitSync(void *s, unsigned f, uint64_t t) { (void)s; (void)f; (void)t; return 0x911A; }
void glDeleteSync(void *s) { (void)s; }
void glFinish(void) { }

// state
static int caps[16];
static int *cap(unsigned c)
{
    static const unsigned cs[] = { 0x0C11, 0x0BE2, 0x0B71, 0x0B90, 0x0B44, 0x0BD0, 0x8C89, 0x8037, 0x809E, 0x80A0, 0x8D69 };
    for (int i = 0; i < 11; i++) if (cs[i] == c) return &caps[i];
    return &caps[15];
}
void glEnable(unsigned c) { *cap(c) = 1; }
void glDisable(unsigned c) { *cap(c) = 0; }
unsigned char glIsEnabled(unsigned c) { return (unsigned char)*cap(c); }
static int cmask[4] = { 1, 1, 1, 1 };
void glColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { cmask[0] = r; cmask[1] = g; cmask[2] = b; cmask[3] = a; }
void glGetIntegerv(unsigned p, int *v)
{
    switch (p) {
    case 0x8CA6: *v = fake_draw_fb; break;                  // DRAW_FRAMEBUFFER_BINDING
    case 0x8CAA: *v = fake_read_fb; break;
    case 0x84E0: *v = (int)GL_TEXTURE0 + fake_active; break;
    case 0x8069: *v = fake_unit[fake_active]; break;         // TEXTURE_BINDING_2D
    case 0x8B8D: *v = fake_prog; break;
    case 0x85B5: *v = fake_vao; break;
    case 0x8895: *v = 8; break;                              // ELEMENT_ARRAY_BUFFER_BINDING: a static index buffer
    case 0x88ED: *v = bound_pack; break;
    case 0x0BA2: memcpy(v, fake_vp, sizeof fake_vp); break;
    case 0x0C23: memcpy(v, cmask, sizeof cmask); break;
    case 0x8825: *v = fake_draw_fb ? (int)GL_COLOR_ATTACHMENT0 : 0x0405; break;
    case 0x0B45: *v = 0x0405; break;
    case 0x0B46: *v = 0x0901; break;
    default: *v = 0;
    }
}
void glGetIntegeri_v(unsigned p, unsigned i, int *v) { (void)p; (void)i; *v = 0; }
void glGetFloatv(unsigned p, float *v) { (void)p; *v = 0; }
void glClearBufferfi(unsigned b, int d, float v, int s) { (void)b; (void)d; (void)v; (void)s; }
void glClearBufferfv(unsigned b, int d, const float *v) { (void)b; (void)d; (void)v; }
void glClearBufferiv(unsigned b, int d, const int *v) { (void)b; (void)d; (void)v; }
void glClearBufferuiv(unsigned b, int d, const unsigned *v) { (void)b; (void)d; (void)v; }
void glCompressedTexImage2D(unsigned t, int l, unsigned f, int w, int h, int b, int s, const void *d)
{ (void)t; (void)l; (void)f; (void)w; (void)h; (void)b; (void)s; (void)d; }
void glCompressedTexSubImage2D(unsigned t, int l, int x, int y, int w, int h, unsigned f, int s, const void *d)
{ (void)t; (void)l; (void)x; (void)y; (void)w; (void)h; (void)f; (void)s; (void)d; }
void glCopyTexImage2D(unsigned t, int l, unsigned f, int x, int y, int w, int h, int b) { (void)t; (void)l; (void)f; (void)x; (void)y; (void)w; (void)h; (void)b; }
void glCopyTexSubImage2D(unsigned t, int l, int xo, int yo, int x, int y, int w, int h) { (void)t; (void)l; (void)xo; (void)yo; (void)x; (void)y; (void)w; (void)h; }
void glCopyTexSubImage3D(unsigned t, int l, int xo, int yo, int zo, int x, int y, int w, int h)
{ (void)t; (void)l; (void)xo; (void)yo; (void)zo; (void)x; (void)y; (void)w; (void)h; }
void glGenerateMipmap(unsigned t) { (void)t; }

// the current EGL context: the thread that calls first has context 1, every other thread 2
#include <pthread.h>
static pthread_t ctx_thread; static int ctx_have;
void *eglGetCurrentContext(void)
{
    if (!ctx_have) { ctx_thread = pthread_self(); ctx_have = 1; }
    return pthread_equal(pthread_self(), ctx_thread) ? (void *)1 : (void *)2;
}
