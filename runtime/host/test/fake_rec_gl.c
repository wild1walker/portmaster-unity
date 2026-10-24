// A fake libGLESv2 for test_glrec.c: a log of the commands that reach the driver (fake_log, one
// "name(args)" each, space-separated, a "bind(<fb>)" before the first command on another draw
// framebuffer: binds with nothing after them, such as glrec_fg.c's while it asks about a
// framebuffer, do not show; queries are answered and not logged), and the state glrec.c
// and glrec_fg.c ask for. Framebuffers: 0 the window (with depth and stencil); 1 colour texture 10
// and depth/stencil texture 20; 2 colour texture 11; 3 colour texture 12 and depth/stencil texture
// 21. Program 5 samples _MainTex (location 3, unit from its uniform); program 6 samples nothing.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <GLES3/gl32.h>

char fake_log[16384];
static unsigned draw_fb, logged_fb;
static void logf_(const char *fmt, ...)
{
    size_t n = strlen(fake_log);
    if (draw_fb != logged_fb) { snprintf(fake_log + n, sizeof fake_log - n, "bind(%u) ", draw_fb); logged_fb = draw_fb; n = strlen(fake_log); }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fake_log + n, sizeof fake_log - n, fmt, ap);
    va_end(ap);
    n = strlen(fake_log);
    if (n + 1 < sizeof fake_log) { fake_log[n] = ' '; fake_log[n + 1] = 0; }
}

static GLuint read_fb, prog, vao, array_buf, active, tex2d[32];
static GLuint vao_ebo[16], vao_en[16], vao_buf[16][16];
static int unit5;                     // program 5's _MainTex unit
static GLboolean depth_test, stencil_test, scissor, discard, cmask[4] = { 1, 1, 1, 1 }, dmask = 1;
static GLuint smask[2] = { 0xff, 0xff };
static GLint box[4] = { 0, 0, 64, 32 };
static GLenum dfunc = GL_LESS;
static GLfloat ccol[4];

void glBindFramebuffer(GLenum t, GLuint fb)
{
    if (t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER) draw_fb = fb;
    if (t == GL_FRAMEBUFFER || t == GL_READ_FRAMEBUFFER) read_fb = fb;
}
void glClear(GLbitfield m)
{
    logf_("clear(%s%s%s fb%u %.2g%s)", m & GL_COLOR_BUFFER_BIT ? "C" : "", m & GL_DEPTH_BUFFER_BIT ? "D" : "", m & GL_STENCIL_BUFFER_BIT ? "S" : "",
          draw_fb, ccol[0], scissor ? " scissor" : "");
}
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { ccol[0] = r; ccol[1] = g; ccol[2] = b; ccol[3] = a; }
void glClearDepthf(GLfloat d) { (void)d; }
void glClearStencil(GLint s) { (void)s; }
void glDrawArrays(GLenum m, GLint f, GLsizei c) { (void)m; (void)f; logf_("draw(fb%u %d)", draw_fb, c); }
void glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i)
{
    (void)m; (void)t;
    if (vao_ebo[vao & 15]) logf_("drawel(fb%u %d @%lu)", draw_fb, c, (unsigned long)(size_t)i);
    else logf_("drawel(fb%u %d idx %u)", draw_fb, c, i ? ((const unsigned short *)i)[0] : 0);
}
void glUseProgram(GLuint p) { prog = p; logf_("use(%u)", p); }
void glActiveTexture(GLenum t) { active = t - GL_TEXTURE0; }
void glBindTexture(GLenum t, GLuint x) { (void)t; if (active < 32) tex2d[active] = x; logf_("tex(%u:%u)", active, x); }
void glUniform1i(GLint l, GLint v) { if (prog == 5 && l == 3) unit5 = v; logf_("u1i(%d %d)", l, v); }
void glUniform4fv(GLint l, GLsizei n, const GLfloat *v) { (void)n; logf_("u4fv(%d %.2g)", l, v[0]); }
void glInvalidateFramebuffer(GLenum t, GLsizei n, const GLenum *a)
{
    (void)t;
    char b[64] = "";
    for (int i = 0; i < n; i++)
        strcat(b, a[i] == GL_COLOR_ATTACHMENT0 || a[i] == GL_COLOR ? "C" : a[i] == GL_DEPTH_STENCIL_ATTACHMENT ? "DS" :
                  a[i] == GL_DEPTH ? "D" : a[i] == GL_STENCIL ? "S" : "?");
    logf_("inval(fb%u %s)", draw_fb, b);
}
static GLboolean *cap(GLenum c)
{
    static GLboolean other;
    return c == GL_DEPTH_TEST ? &depth_test : c == GL_STENCIL_TEST ? &stencil_test : c == GL_SCISSOR_TEST ? &scissor :
           c == GL_RASTERIZER_DISCARD ? &discard : &other;
}
void glEnable(GLenum c) { *cap(c) = 1; }
void glDisable(GLenum c) { *cap(c) = 0; }
GLboolean glIsEnabled(GLenum c) { return *cap(c); }
void glDepthMask(GLboolean m) { dmask = m; }
void glDepthFunc(GLenum f) { dfunc = f; }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { cmask[0] = r; cmask[1] = g; cmask[2] = b; cmask[3] = a; }
void glStencilMaskSeparate(GLenum f, GLuint m) { if (f != GL_BACK) smask[0] = m; if (f != GL_FRONT) smask[1] = m; }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { box[0] = x; box[1] = y; box[2] = w; box[3] = h; }
void glBindVertexArray(GLuint v) { vao = v; }
void glBindBuffer(GLenum t, GLuint b) { if (t == GL_ARRAY_BUFFER) array_buf = b; if (t == GL_ELEMENT_ARRAY_BUFFER) vao_ebo[vao & 15] = b; }
void glVertexAttribPointer(GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st, const void *p)
{ (void)s; (void)t; (void)n; (void)st; (void)p; if (i < 16) vao_buf[vao & 15][i] = array_buf; }
void glEnableVertexAttribArray(GLuint i) { if (i < 16) vao_en[vao & 15] |= 1u << i; }
void glDisableVertexAttribArray(GLuint i) { if (i < 16) vao_en[vao & 15] &= ~(1u << i); }
// one buffer's store (any buffer: the maps write into it), and texture uploads
unsigned char fake_store[256];
static GLbitfield map_access;
void *glMapBufferRange(GLenum t, GLintptr o, GLsizeiptr l, GLbitfield a)
{
    (void)t; (void)l;
    map_access = a;
    logf_("map(%ld %ld%s)", (long)o, (long)l, a & GL_MAP_READ_BIT ? " read" : "");
    return fake_store + o;
}
void glFlushMappedBufferRange(GLenum t, GLintptr o, GLsizeiptr l) { (void)t; logf_("flushmap(%ld %ld)", (long)o, (long)l); }
GLboolean glUnmapBuffer(GLenum t) { (void)t; logf_("unmap"); return GL_TRUE; }
static GLint unpack_align = 4, unpack_row;
void glPixelStorei(GLenum p, GLint v) { if (p == GL_UNPACK_ALIGNMENT) unpack_align = v; if (p == GL_UNPACK_ROW_LENGTH) unpack_row = v; }
void glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *p)
{
    (void)t; (void)l; (void)x; (void)y; (void)f; (void)ty;
    size_t row = (size_t)(unpack_row ? unpack_row : w) * 4;
    row = (row + (size_t)unpack_align - 1) / (size_t)unpack_align * (size_t)unpack_align;
    const unsigned char *b = p;
    logf_("texsub(%dx%d %u..%u)", w, h, b[0], b[(size_t)(h - 1) * row + (size_t)w * 4 - 1]);
}
void glBufferSubData(GLenum t, GLintptr o, GLsizeiptr s, const void *d) { (void)t; (void)o; logf_("bufsub(%ld %u)", (long)s, ((const unsigned char *)d)[0]); }
// (attachments do not change here: test_glrec checks that glrec_fg.c follows the held calls)
void glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint x, GLint l) { (void)t; (void)a; (void)tt; (void)x; (void)l; logf_("attach(%u)", x); }
void glBindBufferBase(GLenum t, GLuint i, GLuint b) { (void)t; (void)i; (void)b; logf_("bindbase"); }
void glFinish(void) { logf_("finish"); }
void glFlush(void) { logf_("flush"); }
GLenum glGetError(void) { logf_("geterror"); return 0; }

// ---- queries (not logged)
void glGetIntegerv(GLenum p, GLint *v)
{
    switch (p) {
    case GL_DRAW_FRAMEBUFFER_BINDING: *v = (GLint)draw_fb; break;
    case GL_READ_FRAMEBUFFER_BINDING: *v = (GLint)read_fb; break;
    case GL_CURRENT_PROGRAM: *v = (GLint)prog; break;
    case GL_ACTIVE_TEXTURE: *v = (GLint)(GL_TEXTURE0 + active); break;
    case GL_TEXTURE_BINDING_2D: *v = active < 32 ? (GLint)tex2d[active] : 0; break;
    case GL_VERTEX_ARRAY_BINDING: *v = (GLint)vao; break;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: *v = (GLint)vao_ebo[vao & 15]; break;
    case GL_ARRAY_BUFFER_BINDING: *v = (GLint)array_buf; break;
    case GL_UNPACK_ALIGNMENT: *v = unpack_align; break;
    case GL_UNPACK_ROW_LENGTH: *v = unpack_row; break;
    case GL_DEPTH_FUNC: *v = (GLint)dfunc; break;
    case GL_STENCIL_FUNC: case GL_STENCIL_BACK_FUNC: *v = GL_ALWAYS; break;
    case GL_STENCIL_FAIL: case GL_STENCIL_PASS_DEPTH_FAIL: case GL_STENCIL_PASS_DEPTH_PASS:
    case GL_STENCIL_BACK_FAIL: case GL_STENCIL_BACK_PASS_DEPTH_FAIL: case GL_STENCIL_BACK_PASS_DEPTH_PASS: *v = GL_KEEP; break;
    case GL_STENCIL_WRITEMASK: *v = (GLint)smask[0]; break;
    case GL_STENCIL_BACK_WRITEMASK: *v = (GLint)smask[1]; break;
    case GL_SCISSOR_BOX: memcpy(v, box, sizeof box); break;
    default: *v = 0;
    }
}
void glGetBooleanv(GLenum p, GLboolean *v)
{
    if (p == GL_COLOR_WRITEMASK) memcpy(v, cmask, 4);
    else if (p == GL_DEPTH_WRITEMASK) *v = dmask;
    else *v = 0;
}
void glGetFloatv(GLenum p, GLfloat *v) { if (p == GL_COLOR_CLEAR_VALUE) memcpy(v, ccol, sizeof ccol); else *v = p == GL_DEPTH_CLEAR_VALUE ? 1 : 0; }
void glGetVertexAttribiv(GLuint i, GLenum p, GLint *v)
{
    if (p == GL_VERTEX_ATTRIB_ARRAY_ENABLED) *v = i < 16 && (vao_en[vao & 15] >> i & 1);
    else if (p == GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING) *v = i < 16 ? (GLint)vao_buf[vao & 15][i] : 0;
    else *v = 0;
}
void glGetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum p, GLint *v)
{
    GLuint fb = t == GL_READ_FRAMEBUFFER ? read_fb : draw_fb;
    GLint type = GL_NONE, name = 0;
    if (fb == 0) { if (a == GL_DEPTH || a == GL_STENCIL || a == GL_BACK) type = GL_FRAMEBUFFER_DEFAULT; }
    else if (a == GL_COLOR_ATTACHMENT0) { type = GL_TEXTURE; name = fb == 1 ? 10 : fb == 2 ? 11 : 12; }
    else if ((a == GL_DEPTH_ATTACHMENT || a == GL_STENCIL_ATTACHMENT) && (fb == 1 || fb == 3)) { type = GL_TEXTURE; name = fb == 1 ? 20 : 21; }
    *v = p == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE ? type : p == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME ? name : 0;
}
void glGetProgramiv(GLuint p, GLenum n, GLint *v) { *v = n == GL_ACTIVE_UNIFORMS ? (p == 5 ? 2 : 1) : 0; }
void glGetActiveUniform(GLuint p, GLuint i, GLsizei bs, GLsizei *len, GLint *size, GLenum *type, GLchar *name)
{
    const char *n = p == 5 && i == 0 ? "_MainTex" : "_Color";
    snprintf(name, (size_t)bs, "%s", n);
    *len = (GLsizei)strlen(name); *size = 1;
    *type = p == 5 && i == 0 ? GL_SAMPLER_2D : GL_FLOAT_VEC4;
}
GLint glGetUniformLocation(GLuint p, const GLchar *n) { return p == 5 && !strcmp(n, "_MainTex") ? 3 : 7; }
void glGetUniformiv(GLuint p, GLint l, GLint *v) { *v = p == 5 && l == 3 ? unit5 : 0; }
#include <stdlib.h>
const GLubyte *glGetString(GLenum n)
{
    if (n != GL_EXTENSIONS) return (const GLubyte *)"fake";
    return (const GLubyte *)(getenv("FAKE_DS_FETCH") ? "GL_EXT_fake GL_ARM_shader_framebuffer_fetch_depth_stencil" : "GL_EXT_fake");
}
GLuint glCreateShader(GLenum t) { (void)t; return 40; }
void glShaderSource(GLuint s, GLsizei n, const GLchar *const *str, const GLint *len) { (void)s; (void)n; (void)str; (void)len; }
void glCompileShader(GLuint s) { (void)s; }
void glAttachShader(GLuint p, GLuint s) { (void)p; (void)s; }
void glLinkProgram(GLuint p) { (void)p; }
void glDeleteProgram(GLuint p) { (void)p; }
