// A fake libGLESv2 for test_glmemo.c: framebuffers 3 (colour texture 9 only), 4 (colour texture 9 and depth texture 20)
// and 5 (colour texture 10 only); program 7 (uniform vec4 u_a at location 0, sampler2D s_tex at 1, attribute a_pos at
// location 0), program 6 the same with one uniform block, program 8 the same but sampling through s_tex what texture unit 0 holds; texture
// units, sampler bindings, buffers with their storage (mapped writes go to it), uniform-buffer ranges, vertex
// attributes, the fixed-function state glmemo.c reads. Each draw "renders": the colour texture of the draw framebuffer
// gets a value made of the inputs (u_a, the sampled texture's contents, the uniform block's and vertex buffer's bytes,
// and fake_hidden, an input no key has: check mode's "different"). A log of the calls that change pixels: I invalidate,
// C clear, D draw, U texture upload.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned char GLboolean;
typedef intptr_t GLintptr; typedef intptr_t GLsizeiptr; typedef int64_t GLint64;

char fake_log[4096];
static int nlog;
static void logc(char c) { if (nlog < (int)sizeof fake_log - 1) fake_log[nlog++] = c; }
int fake_draws, fake_invalidates, fake_reads;
unsigned fake_hidden;
const char *fake_version = "OpenGL ES 3.2 fake";
int fake_blend, fake_scissor, fake_tf;
uint32_t fake_content[64];                 // per texture: what was rendered into it

static int draw_fb, read_fb, program, active;
static int unit_tex[16], unit_smp[16];
static float u_a[4];
static int u_unit;
static int tex_param[64];

// buffers
static unsigned char storage[32][256];
static int bsize[32];
static int bind_array, bind_element, bind_uniform, bind_copyw;
static int ubo_buf[4]; static GLint64 ubo_off[4], ubo_size[4];
static int attr_en, attr_buf, attr_size = 3, attr_type = 0x1406, attr_stride; static intptr_t attr_ptr;
static int map_buf; static GLintptr map_off;

static int *target_binding(GLenum t)
{
    static int none;
    return t == 0x8892 ? &bind_array : t == 0x8893 ? &bind_element : t == 0x8A11 ? &bind_uniform : t == 0x8F37 ? &bind_copyw : &none;
}

const unsigned char *glGetString(GLenum n) { return (const unsigned char *)(n == 0x1F02 ? fake_version : ""); }
GLenum glGetError(void) { return 0; }
void glBindFramebuffer(GLenum t, GLuint fb)
{
    if (t == 0x8D40 || t == 0x8CA9) draw_fb = (int)fb;
    if (t == 0x8D40 || t == 0x8CA8) read_fb = (int)fb;
}
void glGetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum p, GLint *v)
{
    int fb = t == 0x8CA8 ? read_fb : draw_fb, type = 0, name = 0;
    if (a == 0x8CE0 && (fb == 3 || fb == 4)) { type = 0x1702; name = 9; }
    if (a == 0x8CE0 && fb == 5) { type = 0x1702; name = 10; }
    if ((a == 0x8D00 || a == 0x8D20) && fb == 4) { type = 0x1702; name = 20; }
    *v = p == 0x8CD0 ? type : p == 0x8CD1 ? name : p == 0x8CD2 ? 0 : p == 0x8211 ? (type ? 0x8C17 : 0) : 0;
}
static uint32_t mix(uint32_t h, uint32_t x) { return (h ^ x) * 16777619u; }
static uint32_t bytes_hash(uint32_t h, const unsigned char *p, int n) { for (int i = 0; i < n; i++) h = mix(h, p[i]); return h; }
static void render(void)
{
    int tex = draw_fb == 5 ? 10 : (draw_fb == 3 || draw_fb == 4) ? 9 : 0;
    fake_draws++; logc('D');
    if (!tex) return;
    uint32_t h = 2166136261u;
    h = bytes_hash(h, (const unsigned char *)u_a, sizeof u_a);
    int st = program == 8 ? unit_tex[0] : unit_tex[u_unit & 15];
    h = mix(h, (uint32_t)st); h = mix(h, st > 0 && st < 64 ? fake_content[st] : 0); h = mix(h, (uint32_t)tex_param[st & 63]);
    if (program == 6 && ubo_buf[0]) h = bytes_hash(h, storage[ubo_buf[0]] + ubo_off[0], 16);
    if (attr_en && attr_buf) h = bytes_hash(h, storage[attr_buf] + attr_ptr, 36);
    h = mix(h, fake_hidden);
    fake_content[tex] = fake_blend ? mix(fake_content[tex], h) : h;
}
void glDrawArrays(GLenum m, GLint f, GLsizei c) { (void)m; (void)f; (void)c; render(); }
void glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) { (void)m; (void)c; (void)t; (void)i; render(); }
void glDrawRangeElements(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i) { (void)s; (void)e; glDrawElements(m, c, t, i); }
void glClear(unsigned m) { (void)m; logc('C'); int tex = draw_fb == 5 ? 10 : draw_fb ? 9 : 0; if (tex) fake_content[tex] = 0; }
void glClearBufferfv(GLenum b, GLint d, const float *v) { (void)b; (void)d; (void)v; logc('C'); }
void glClearBufferiv(GLenum b, GLint d, const GLint *v) { (void)b; (void)d; (void)v; logc('C'); }
void glClearBufferuiv(GLenum b, GLint d, const GLuint *v) { (void)b; (void)d; (void)v; logc('C'); }
void glClearBufferfi(GLenum b, GLint d, float v, GLint s) { (void)b; (void)d; (void)v; (void)s; logc('C'); }
void glBlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, unsigned m, GLenum fl)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)m; (void)fl; logc('B'); }
void glInvalidateFramebuffer(GLenum t, GLsizei n, const GLenum *a) { (void)t; (void)n; (void)a; fake_invalidates++; logc('I'); }
void glInvalidateSubFramebuffer(GLenum t, GLsizei n, const GLenum *a, GLint x, GLint y, GLsizei w, GLsizei h)
{ (void)x; (void)y; (void)w; (void)h; glInvalidateFramebuffer(t, n, a); }
void glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l) { (void)t; (void)a; (void)tt; (void)tex; (void)l; }
void glFramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint rb) { (void)t; (void)a; (void)rt; (void)rb; }
void glDeleteFramebuffers(GLsizei n, const GLuint *f) { (void)n; (void)f; }
void glDrawBuffers(GLsizei n, const GLenum *b) { (void)n; (void)b; }
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p)
{
    (void)x; (void)y; (void)f; (void)t;
    fake_reads++;
    int tex = read_fb == 5 ? 10 : read_fb ? 9 : 0;
    uint32_t *o = p;
    for (int i = 0; i < w * h; i++) o[i] = fake_content[tex];
}
void glPixelStorei(GLenum p, GLint v) { (void)p; (void)v; }

// textures
void glActiveTexture(GLenum u) { active = (int)(u - 0x84C0) & 15; }
void glBindTexture(GLenum t, GLuint tex) { (void)t; unit_tex[active] = (int)tex; }
void glBindSampler(GLuint u, GLuint s) { unit_smp[u & 15] = (int)s; }
static void upload(void) { logc('U'); int t = unit_tex[active]; if (t > 0 && t < 64) fake_content[t] = mix(fake_content[t], 77); }
void glTexImage2D(GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLint b, GLenum fm, GLenum ty, const void *p)
{ (void)t; (void)l; (void)f; (void)w; (void)h; (void)b; (void)fm; (void)ty; (void)p; upload(); }
void glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *p)
{ (void)t; (void)l; (void)x; (void)y; (void)w; (void)h; (void)f; (void)ty; (void)p; upload(); }
#define NOOP(name, args) void name args { }
NOOP(glTexImage3D, (GLenum a, GLint b, GLint c, GLsizei d, GLsizei e, GLsizei f, GLint g, GLenum h, GLenum i, const void *j))
NOOP(glTexSubImage3D, (GLenum a, GLint b, GLint c, GLint d, GLint e, GLsizei f, GLsizei g, GLsizei h, GLenum i, GLenum j, const void *k))
NOOP(glCompressedTexImage2D, (GLenum a, GLint b, GLenum c, GLsizei d, GLsizei e, GLint f, GLsizei g, const void *h))
NOOP(glCompressedTexImage3D, (GLenum a, GLint b, GLenum c, GLsizei d, GLsizei e, GLsizei f, GLint g, GLsizei h, const void *i))
NOOP(glCompressedTexSubImage2D, (GLenum a, GLint b, GLint c, GLint d, GLsizei e, GLsizei f, GLenum g, GLsizei h, const void *i))
NOOP(glCompressedTexSubImage3D, (GLenum a, GLint b, GLint c, GLint d, GLint e, GLsizei f, GLsizei g, GLsizei h, GLenum i, GLsizei j, const void *k))
NOOP(glTexStorage2D, (GLenum a, GLsizei b, GLenum c, GLsizei d, GLsizei e))
NOOP(glTexStorage3D, (GLenum a, GLsizei b, GLenum c, GLsizei d, GLsizei e, GLsizei f))
NOOP(glCopyTexImage2D, (GLenum a, GLint b, GLenum c, GLint d, GLint e, GLsizei f, GLsizei g, GLint h))
NOOP(glCopyTexSubImage2D, (GLenum a, GLint b, GLint c, GLint d, GLint e, GLint f, GLsizei g, GLsizei h))
NOOP(glCopyTexSubImage3D, (GLenum a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLsizei h, GLsizei i))
NOOP(glGenerateMipmap, (GLenum a))
NOOP(glDeleteTextures, (GLsizei a, const GLuint *b))
NOOP(glSamplerParameteri, (GLuint a, GLenum b, GLint c))
NOOP(glSamplerParameteriv, (GLuint a, GLenum b, const GLint *c))
NOOP(glSamplerParameterfv, (GLuint a, GLenum b, const float *c))
NOOP(glSamplerParameterf, (GLuint a, GLenum b, float c))
NOOP(glDeleteSamplers, (GLsizei a, const GLuint *b))
NOOP(glLinkProgram, (GLuint a))
NOOP(glProgramBinary, (GLuint a, GLenum b, const void *c, GLsizei d))
NOOP(glDeleteProgram, (GLuint a))
NOOP(glTexParameteriv, (GLenum a, GLenum b, const GLint *c))
NOOP(glTexParameterfv, (GLenum a, GLenum b, const float *c))
NOOP(glTexParameterf, (GLenum a, GLenum b, float c))
NOOP(glGetSamplerParameteriv, (GLuint a, GLenum b, GLint *c))
NOOP(glGetSamplerParameterfv, (GLuint a, GLenum b, float *c))
NOOP(glGetTexParameterfv, (GLenum a, GLenum b, float *c))
void glTexParameteri(GLenum t, GLenum p, GLint v) { (void)t; (void)p; tex_param[unit_tex[active] & 63] = v; }
void glGetTexParameteriv(GLenum t, GLenum p, GLint *v) { (void)t; (void)p; *v = tex_param[unit_tex[active] & 63]; }

// buffers
void glBindBuffer(GLenum t, GLuint b) { *target_binding(t) = (int)b; }
void glBufferData(GLenum t, GLsizeiptr s, const void *d, GLenum u)
{ (void)u; int b = *target_binding(t); bsize[b] = (int)s; if (d) memcpy(storage[b], d, (size_t)s); }
void glBufferSubData(GLenum t, GLintptr o, GLsizeiptr s, const void *d) { memcpy(storage[*target_binding(t)] + o, d, (size_t)s); }
void *glMapBufferRange(GLenum t, GLintptr o, GLsizeiptr l, unsigned a) { (void)l; (void)a; map_buf = *target_binding(t); map_off = o; return storage[map_buf] + o; }
void glFlushMappedBufferRange(GLenum t, GLintptr o, GLsizeiptr l) { (void)t; (void)o; (void)l; }
GLboolean glUnmapBuffer(GLenum t) { (void)t; map_buf = 0; return 1; }
NOOP(glCopyBufferSubData, (GLenum a, GLenum b, GLintptr c, GLintptr d, GLsizeiptr e))
NOOP(glDeleteBuffers, (GLsizei a, const GLuint *b))
void glBindBufferBase(GLenum t, GLuint i, GLuint b) { if (t == 0x8A11 && i < 4) { ubo_buf[i] = (int)b; ubo_off[i] = 0; ubo_size[i] = 0; } }
void glBindBufferRange(GLenum t, GLuint i, GLuint b, GLintptr o, GLsizeiptr s) { if (t == 0x8A11 && i < 4) { ubo_buf[i] = (int)b; ubo_off[i] = o; ubo_size[i] = s; } }
void glEnableVertexAttribArray(GLuint i) { if (i == 0) attr_en = 1; }
void glVertexAttribPointer(GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st, const void *p)
{ (void)n; if (i == 0) { attr_buf = bind_array; attr_size = s; attr_type = t; attr_stride = st; attr_ptr = (intptr_t)p; } }

// programs
void glUseProgram(GLuint p) { program = (int)p; }
void glUniform4f(GLint l, float a, float b, float c, float d) { if (l == 0) { u_a[0] = a; u_a[1] = b; u_a[2] = c; u_a[3] = d; } }
void glUniform1i(GLint l, GLint v) { if (l == 1) u_unit = v; }
void glGetProgramiv(GLuint p, GLenum q, GLint *v)
{ *v = q == 0x8B86 ? 2 : q == 0x8B89 ? 1 : q == 0x8A36 ? (p == 6) : 0; }   // uniforms, attributes, blocks (program 6); no atomics
void glGetProgramInterfaceiv(GLuint p, GLenum i, GLenum q, GLint *v) { (void)p; (void)i; (void)q; *v = 0; }
void glGetActiveUniform(GLuint p, GLuint i, GLsizei n, GLsizei *len, GLint *size, GLenum *type, char *name)
{
    (void)p;
    const char *s = i == 0 ? "u_a" : "s_tex";
    strncpy(name, s, (size_t)n);
    *len = (GLsizei)strlen(s); *size = 1; *type = i == 0 ? 0x8B52 : 0x8B5E;
}
void glGetActiveUniformsiv(GLuint p, GLsizei n, const GLuint *i, GLenum q, GLint *v) { (void)p; (void)n; (void)i; (void)q; *v = -1; }
GLint glGetUniformLocation(GLuint p, const char *n) { (void)p; return !strcmp(n, "u_a") ? 0 : !strcmp(n, "s_tex") ? 1 : -1; }
void glGetUniformfv(GLuint p, GLint l, float *v) { (void)p; if (l == 0) memcpy(v, u_a, sizeof u_a); }
void glGetUniformiv(GLuint p, GLint l, GLint *v) { (void)p; if (l == 1) *v = u_unit; }
void glGetUniformuiv(GLuint p, GLint l, GLuint *v) { (void)p; (void)l; *v = 0; }
void glGetActiveAttrib(GLuint p, GLuint i, GLsizei n, GLsizei *len, GLint *size, GLenum *type, char *name)
{ (void)p; (void)i; strncpy(name, "a_pos", (size_t)n); *len = 5; *size = 1; *type = 0x8B51; }
GLint glGetAttribLocation(GLuint p, const char *n) { (void)p; return !strcmp(n, "a_pos") ? 0 : -1; }
void glGetActiveUniformBlockiv(GLuint p, GLuint i, GLenum q, GLint *v) { (void)p; (void)i; *v = q == 0x8A40 ? 16 : 0; }   // data size 16, binding 0
void glGetIntegeri_v(GLenum q, GLuint i, GLint *v) { *v = q == 0x8A28 && i < 4 ? ubo_buf[i] : 0; }
void glGetInteger64i_v(GLenum q, GLuint i, GLint64 *v) { *v = i >= 4 ? 0 : q == 0x8A29 ? ubo_off[i] : q == 0x8A2A ? ubo_size[i] : 0; }
void glGetVertexAttribiv(GLuint i, GLenum q, GLint *v)
{
    *v = 0;
    if (i != 0) return;
    switch (q) {
    case 0x8622: *v = attr_en; break;
    case 0x889F: *v = attr_buf; break;
    case 0x8623: *v = attr_size; break;
    case 0x8625: *v = attr_type; break;
    case 0x8624: *v = attr_stride; break;
    }
}
void glGetVertexAttribfv(GLuint i, GLenum q, float *v) { (void)i; (void)q; v[0] = v[1] = v[2] = 0; v[3] = 1; }
void glGetVertexAttribPointerv(GLuint i, GLenum q, void **p) { (void)q; *p = i == 0 ? (void *)attr_ptr : NULL; }
void glGetQueryiv(GLenum t, GLenum q, GLint *v) { (void)t; (void)q; *v = 0; }

// state
unsigned char glIsEnabled(GLenum c) { return c == 0x0BE2 ? (unsigned char)fake_blend : c == 0x0C11 ? (unsigned char)fake_scissor : c == 0x0BD0; }
void glGetBooleanv(GLenum q, GLboolean *v)
{
    if (q == 0x0C23) { v[0] = v[1] = v[2] = v[3] = 1; return; }
    *v = q == 0x8E24 ? (GLboolean)fake_tf : 0;
}
void glGetFloatv(GLenum q, float *v) { if (q == 0x0B70) { v[0] = 0; v[1] = 1; } else *v = 0; }
void glGetIntegerv(GLenum q, GLint *v)
{
    switch (q) {
    case 0x8CA6: *v = draw_fb; break;
    case 0x8CAA: *v = read_fb; break;
    case 0x8B8D: *v = program; break;
    case 0x84E0: *v = 0x84C0 + active; break;
    case 0x8069: *v = unit_tex[active]; break;     // GL_TEXTURE_BINDING_2D
    case 0x8919: *v = unit_smp[active]; break;     // GL_SAMPLER_BINDING
    case 0x8825: *v = draw_fb ? 0x8CE0 : 0x0405; break;   // GL_DRAW_BUFFER0
    case 0x8894: *v = bind_array; break;
    case 0x8895: *v = bind_element; break;
    case 0x8A28: *v = bind_uniform; break;
    case 0x8F37: *v = bind_copyw; break;
    case 0x0BA2: v[0] = 0; v[1] = 0; v[2] = 32; v[3] = 1; break;   // the viewport (the target is 32x1)
    case 0x0D05: *v = 4; break;
    default: *v = 0;
    }
}
