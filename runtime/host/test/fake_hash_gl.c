// A fake EGL + GLES driver with pixels for glreplay --hash's test (pointer_test.sh): one 4x4 RGBA picture that
// every clear and draw changes (by its arguments and the bound framebuffer), read back by glReadPixels as that
// picture repeated over the size asked for. FAKE_HASH_DIFF=<n> makes the n-th draw or clear (from 1) change it
// differently: what a driver that draws one call otherwise looks like to glpixdiff.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES3/gl32.h>

static int dpy, surf, ctx;
EGLDisplay eglGetDisplay(EGLNativeDisplayType d) { (void)d; return &dpy; }
EGLBoolean eglInitialize(EGLDisplay d, EGLint *maj, EGLint *min) { (void)d; *maj = 1; *min = 4; return EGL_TRUE; }
EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *a, EGLConfig *c, EGLint n, EGLint *num) { (void)d; (void)a; (void)n; *c = &surf; *num = 1; return EGL_TRUE; }
EGLBoolean eglBindAPI(EGLenum api) { (void)api; return EGL_TRUE; }
EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *a) { (void)d; (void)c; (void)a; return &surf; }
EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext s, const EGLint *a) { (void)d; (void)c; (void)s; (void)a; return &ctx; }
EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface r, EGLSurface w, EGLContext c) { (void)d; (void)r; (void)w; (void)c; return EGL_TRUE; }
EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s) { (void)d; (void)s; return EGL_TRUE; }
void (*eglGetProcAddress(const char *n))(void) { (void)n; return NULL; }

static uint8_t px[4 * 4 * 4];
static GLuint draw_fb, read_fb;
static int ncalls, diff_at = -1;
static void change(unsigned a, unsigned b)
{
    if (diff_at < 0) { const char *e = getenv("FAKE_HASH_DIFF"); diff_at = e ? atoi(e) : 0; }
    ncalls++;
    for (int i = 0; i < (int)sizeof px; i++) px[i] = (uint8_t)(px[i] * 31 + a + b * 7 + draw_fb * 13 + i + (ncalls == diff_at));
}
const GLubyte *glGetString(GLenum n) { (void)n; return (const GLubyte *)"fake"; }
GLenum glGetError(void) { return 0; }
void glGetIntegerv(GLenum p, GLint *v)
{
    *v = p == GL_DRAW_FRAMEBUFFER_BINDING ? (GLint)draw_fb : p == GL_READ_FRAMEBUFFER_BINDING ? (GLint)read_fb : p == GL_PACK_ALIGNMENT ? 4 :
         p == GL_READ_BUFFER ? GL_BACK : 0;
}
void glGetProgramiv(GLuint p, GLenum n, GLint *v) { (void)p; *v = n == GL_LINK_STATUS; }
void glGetProgramInfoLog(GLuint p, GLsizei n, GLsizei *l, GLchar *s) { (void)p; (void)n; (void)l; s[0] = 0; }
void glBindFramebuffer(GLenum t, GLuint f) { if (t != GL_READ_FRAMEBUFFER) draw_fb = f; if (t != GL_DRAW_FRAMEBUFFER) read_fb = f; }
void glClear(GLbitfield m) { change(m, 1); }
void glDrawArrays(GLenum m, GLint f, GLsizei c) { change(m + (unsigned)f, (unsigned)c); }
void glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) { (void)t; (void)i; change(m, (unsigned)c + 100); }
void glReadBuffer(GLenum b) { (void)b; }
void glPixelStorei(GLenum p, GLint v) { (void)p; (void)v; }
void glBindBuffer(GLenum t, GLuint b) { (void)t; (void)b; }
void glBindTexture(GLenum t, GLuint x) { (void)t; (void)x; }
void glBindRenderbuffer(GLenum t, GLuint x) { (void)t; (void)x; }
void glGetRenderbufferParameteriv(GLenum t, GLenum p, GLint *v) { (void)t; *v = p == GL_RENDERBUFFER_WIDTH || p == GL_RENDERBUFFER_HEIGHT ? 8 : 0; }
void glGetTexLevelParameteriv(GLenum t, GLint l, GLenum p, GLint *v) { (void)t; (void)l; *v = p == GL_TEXTURE_WIDTH || p == GL_TEXTURE_HEIGHT ? 8 : 0; }
// a framebuffer object's attachment 0: a renderbuffer, normalized; the others none
void glGetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum p, GLint *v)
{
    (void)t;
    int has = a == GL_COLOR_ATTACHMENT0;
    *v = p == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE ? (has ? GL_RENDERBUFFER : GL_NONE) : p == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME ? 1 :
         p == GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE ? GL_UNSIGNED_NORMALIZED : 0;
}
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p)
{
    (void)x; (void)y; (void)f; (void)t;
    uint8_t *o = p;
    for (GLsizei r = 0; r < h; r++)
        for (GLsizei c = 0; c < w; c++) memcpy(o + ((size_t)r * w + c) * 4, px + ((r % 4) * 4 + c % 4) * 4, 4);
}
void glFlush(void) {}
void glFinish(void) {}
