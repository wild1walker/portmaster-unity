// A fake libGLESv2 for test_gltrace.c: framebuffer bindings (framebuffer 3 has texture 9 as its
// colour buffer, RGBA16F; the window, framebuffer 0, RGBA8), the active texture unit and the 2D
// texture bound on each, the pixel pack buffer, one program (5) with a vec4 and a sampler uniform,
// shader sources, and glReadPixels answering a fixed pixel per framebuffer. It records what the
// tracer's reads leave behind: the bindings must be Unity's again afterwards.
// The other draw entry points (Range, BaseVertex, Instanced BaseVertex, MultiDraw, Indirect),
// glCopyTexSubImage2D, and EXT_disjoint_timer_query (in the extension string with FAKE_GL_TIMER=1):
// a GL_TIME_ELAPSED_EXT query adds 1 ms per draw call made while it runs, 0.5 ms per clear, 2 ms
// per blit, 0.25 ms per copy; beginning one while another runs, or ending none, counts an error.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_VIEWPORT 0x0BA2
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_SAMPLE_BUFFERS 0x80A8
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE 0x8211
#define GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE 0x8212
#define GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE 0x8215
#define GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING 0x8210
#define GL_LINEAR 0x2601
#define GL_TEXTURE 0x1702
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHADER_TYPE 0x8B4F
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_SHADER_SOURCE_LENGTH 0x8B88
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_FLOAT_VEC4 0x8B52
#define GL_SAMPLER_2D 0x8B5E
#define GL_INVALID_OPERATION 0x0502

int fake_read_fb, fake_draw_fb, fake_active = GL_TEXTURE0, fake_tex[8], fake_pbo = 4, fake_prog, fake_reads;
int fake_vp[4];
static int fake_error;

// time queries
#define GL_EXTENSIONS 0x1F03
#define GL_TIME_ELAPSED_EXT 0x88BF
#define GL_QUERY_COUNTER_BITS_EXT 0x8864
#define GL_QUERY_RESULT_EXT 0x8866
#define GL_QUERY_RESULT_AVAILABLE_EXT 0x8867
int fake_begins, fake_query_errors, fake_flushes;
static unsigned fake_next_query = 1, fake_running;   // the running query (0: none)
static uint64_t fake_elapsed[1024];                  // ns per query
static void work(uint64_t ns) { if (fake_running && fake_running < 1024) fake_elapsed[fake_running] += ns; }
const unsigned char *glGetString(unsigned name)
{
    if (name != GL_EXTENSIONS) return (const unsigned char *)"fake";
    return (const unsigned char *)(getenv("FAKE_GL_TIMER") ? "GL_OES_foo GL_EXT_disjoint_timer_query GL_EXT_bar" :
                                                              "GL_OES_foo GL_EXT_disjoint_timer_query_webgl2");
}
void glFlush(void) { fake_flushes++; }
int fake_finishes;
void glFinish(void) { fake_finishes++; }
// blending on (src alpha, 1 - src alpha), depth and stencil tests off, all colour channels written
unsigned char glIsEnabled(unsigned cap) { return cap == 0x0BE2; }
void glGenQueriesEXT(int n, unsigned *q) { for (int i = 0; i < n; i++) q[i] = fake_next_query++; }
void glBeginQueryEXT(unsigned t, unsigned q)
{
    if (t != GL_TIME_ELAPSED_EXT) return;
    if (fake_running || q == 0 || q >= 1024) { fake_query_errors++; return; }
    fake_running = q; fake_elapsed[q] = 0; fake_begins++;
}
void glEndQueryEXT(unsigned t) { if (t != GL_TIME_ELAPSED_EXT) return; if (!fake_running) fake_query_errors++; fake_running = 0; }
void glBeginQuery(unsigned t, unsigned q) { glBeginQueryEXT(t, q); }
void glEndQuery(unsigned t) { glEndQueryEXT(t); }
void glGetQueryivEXT(unsigned t, unsigned p, int *v) { *v = t == GL_TIME_ELAPSED_EXT && p == GL_QUERY_COUNTER_BITS_EXT ? 32 : 0; }
void glGetQueryObjectuivEXT(unsigned q, unsigned p, unsigned *v)
{ *v = p == GL_QUERY_RESULT_AVAILABLE_EXT ? q != fake_running : q < 1024 ? (unsigned)fake_elapsed[q] : 0; }
void glGetQueryObjectui64vEXT(unsigned q, unsigned p, uint64_t *v) { (void)p; *v = q < 1024 ? fake_elapsed[q] : 0; }

void glBindFramebuffer(unsigned t, unsigned fb)
{
    if (t == GL_FRAMEBUFFER || t == GL_READ_FRAMEBUFFER) fake_read_fb = (int)fb;
    if (t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER) fake_draw_fb = (int)fb;
}
void glViewport(int x, int y, int w, int h) { fake_vp[0] = x; fake_vp[1] = y; fake_vp[2] = w; fake_vp[3] = h; }
void glClear(unsigned m) { (void)m; work(500000); }
void glDrawArrays(unsigned m, int f, int c) { (void)m; (void)f; (void)c; work(1000000); }
void glDrawElements(unsigned m, int c, unsigned t, const void *i) { (void)m; (void)c; (void)t; (void)i; work(1000000); }
void glDrawRangeElements(unsigned m, unsigned s, unsigned e, int c, unsigned t, const void *i)
{ (void)m; (void)s; (void)e; (void)c; (void)t; (void)i; work(1000000); }
void glDrawElementsBaseVertex(unsigned m, int c, unsigned t, const void *i, int b)
{ (void)m; (void)c; (void)t; (void)i; (void)b; work(1000000); }
void glDrawElementsBaseVertexOES(unsigned m, int c, unsigned t, const void *i, int b)
{ (void)m; (void)c; (void)t; (void)i; (void)b; work(1000000); }
void glDrawElementsInstancedBaseVertex(unsigned m, int c, unsigned t, const void *i, int n, int b)
{ (void)m; (void)c; (void)t; (void)i; (void)n; (void)b; work(1000000); }
void glMultiDrawArraysEXT(unsigned m, const int *f, const int *c, int n) { (void)m; (void)f; (void)c; (void)n; work(1000000); }
void glDrawArraysIndirect(unsigned m, const void *p) { (void)m; (void)p; work(1000000); }
void glCopyTexSubImage2D(unsigned t, int l, int xo, int yo, int x, int y, int w, int h)
{ (void)t; (void)l; (void)xo; (void)yo; (void)x; (void)y; (void)w; (void)h; work(250000); }
void glBlitFramebuffer(int a, int b, int c, int d, int e, int f, int g, int h, unsigned m, unsigned fl)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)m; (void)fl; work(2000000); }
void glUseProgram(unsigned p) { fake_prog = (int)p; }
void glActiveTexture(unsigned u) { fake_active = (int)u; }
void glBindTexture(unsigned t, unsigned tex) { (void)t; fake_tex[fake_active - GL_TEXTURE0] = (int)tex; }
void glBindBuffer(unsigned t, unsigned b) { if (t == GL_PIXEL_PACK_BUFFER) fake_pbo = (int)b; }
unsigned glGetError(void) { unsigned e = (unsigned)fake_error; fake_error = 0; return e; }

void glGetIntegerv(unsigned p, int *v)
{
    switch (p) {
    case GL_READ_FRAMEBUFFER_BINDING: *v = fake_read_fb; break;
    case GL_DRAW_FRAMEBUFFER_BINDING: *v = fake_draw_fb; break;
    case GL_VIEWPORT: memcpy(v, fake_vp, sizeof fake_vp); break;
    case GL_ACTIVE_TEXTURE: *v = fake_active; break;
    case GL_TEXTURE_BINDING_2D: *v = fake_tex[fake_active - GL_TEXTURE0]; break;
    case GL_CURRENT_PROGRAM: *v = fake_prog; break;
    case GL_PIXEL_PACK_BUFFER_BINDING: *v = fake_pbo; break;
    case GL_SAMPLE_BUFFERS: *v = 0; break;
    case 0x80C9: *v = 0x0302; break;                             // GL_BLEND_SRC_RGB
    case 0x80C8: *v = 0x0303; break;                             // GL_BLEND_DST_RGB
    case 0x0C23: v[0] = v[1] = v[2] = v[3] = 1; break;           // GL_COLOR_WRITEMASK
    default: *v = 0;
    }
}
void glGetFloatv(unsigned p, float *v)
{
    if (p == GL_COLOR_CLEAR_VALUE) { v[0] = 0; v[1] = 1; v[2] = 0; v[3] = 1; }
}
void glGetFramebufferAttachmentParameteriv(unsigned t, unsigned a, unsigned p, int *v)
{
    (void)a;
    int fb = t == GL_READ_FRAMEBUFFER ? fake_read_fb : fake_draw_fb;
    if (fb != 3) { fake_error = GL_INVALID_OPERATION; return; }    // only framebuffer 3 is described
    switch (p) {
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE: *v = GL_TEXTURE; break;
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME: *v = 9; break;
    case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE: *v = GL_FLOAT; break;
    case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING: *v = GL_LINEAR; break;
    default: *v = p >= GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE && p <= GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE ? 16 : 0;
    }
}
void glReadPixels(int x, int y, int w, int h, unsigned f, unsigned type, void *px)
{
    (void)x; (void)y; (void)w; (void)h; (void)f;
    fake_reads++;
    if (fake_pbo) { fake_error = GL_INVALID_OPERATION; return; }   // into a pack buffer: not ours
    if (fake_read_fb == 3 && type == GL_FLOAT) { float v[4] = { 0.25f, 0.5f, 0.75f, 1 }; memcpy(px, v, sizeof v); }
    else if (fake_read_fb == 0 && type == GL_UNSIGNED_BYTE) { unsigned char v[4] = { 10, 20, 30, 255 }; memcpy(px, v, 4); }
    else fake_error = GL_INVALID_OPERATION;
}

// shaders 1 (vertex) and 2 (fragment); program 5 has a vec4 _Color and a sampler2D _MainTex (unit 2)
static const char *src(unsigned sh) { return sh == 1 ? "void main() { gl_Position = vec4(0.0); }" : "void main() {}"; }
void glAttachShader(unsigned prog, unsigned sh) { (void)prog; (void)sh; }
void glGetShaderiv(unsigned sh, unsigned p, int *v)
{
    if (p == GL_SHADER_TYPE) *v = sh == 1 ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
    if (p == GL_SHADER_SOURCE_LENGTH) *v = (int)strlen(src(sh)) + 1;
}
void glGetShaderSource(unsigned sh, int n, int *len, char *out)
{
    int l = (int)strlen(src(sh));
    if (l > n - 1) l = n - 1;
    memcpy(out, src(sh), (size_t)l); out[l] = 0;
    if (len) *len = l;
}
void glGetProgramiv(unsigned prog, unsigned p, int *v)
{
    if (p == GL_ACTIVE_UNIFORMS) *v = prog == 5 ? 2 : 0;
    if (p == 0x8B89) *v = prog == 5 ? 1 : 0;                     // GL_ACTIVE_ATTRIBUTES
    if (p == 0x8A36) *v = 0;                                     // GL_ACTIVE_UNIFORM_BLOCKS
}
// its attribute in_POSITION0 (location 0) reads buffer 4 from byte 16
void glGetActiveAttrib(unsigned prog, unsigned i, int n, int *len, int *size, unsigned *type, char *name)
{ (void)prog; (void)i; (void)n; strcpy(name, "in_POSITION0"); *len = 12; *size = 1; *type = 0x8B52; }
int glGetAttribLocation(unsigned prog, const char *name) { (void)prog; return !strcmp(name, "in_POSITION0") ? 0 : -1; }
void glGetVertexAttribiv(unsigned i, unsigned p, int *v) { *v = i != 0 ? 0 : p == 0x8622 ? 1 : p == 0x889F ? 4 : 0; }
void glGetVertexAttribPointerv(unsigned i, unsigned p, void **v) { (void)i; (void)p; *v = (void *)(intptr_t)16; }
void glGetActiveUniformBlockiv(unsigned prog, unsigned i, unsigned p, int *v) { (void)prog; (void)i; (void)p; *v = 0; }
void glGetIntegeri_v(unsigned p, unsigned i, int *v) { (void)p; (void)i; *v = 0; }
void glGetInteger64i_v(unsigned p, unsigned i, int64_t *v) { (void)p; (void)i; *v = 0; }
void glGetActiveUniform(unsigned prog, unsigned i, int n, int *len, int *size, unsigned *type, char *name)
{
    (void)prog; (void)n;
    const char *nm = i == 0 ? "_Color" : "_MainTex";
    strcpy(name, nm); *len = (int)strlen(nm); *size = 1; *type = i == 0 ? GL_FLOAT_VEC4 : GL_SAMPLER_2D;
}
int glGetUniformLocation(unsigned prog, const char *name) { (void)prog; return !strcmp(name, "_MainTex") ? 7 : 3; }
void glGetUniformiv(unsigned prog, int loc, int *v) { (void)prog; *v = loc == 7 ? 2 : 0; }
