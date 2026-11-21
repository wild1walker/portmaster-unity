// A fake EGL + GLES driver for glreplay's test (pointer_test.sh: the trace test_glrec's "file" scenario
// wrote, replayed): each call that reaches it printed, with the names this driver gave (programs 105,
// shaders 140, uniform locations 17), so the test sees the game's names mapped to these.
#include <stdio.h>
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
EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s) { (void)d; (void)s; printf("fake: swap\n"); return EGL_TRUE; }
void (*eglGetProcAddress(const char *n))(void) { (void)n; return NULL; }

const GLubyte *glGetString(GLenum n) { (void)n; return (const GLubyte *)"fake"; }
GLenum glGetError(void) { return 0; }
void glGetIntegerv(GLenum p, GLint *v) { (void)p; *v = 0; }
void glGetProgramiv(GLuint p, GLenum n, GLint *v) { (void)p; *v = n == GL_LINK_STATUS; }
void glGetProgramInfoLog(GLuint p, GLsizei n, GLsizei *l, GLchar *s) { (void)p; (void)n; (void)l; s[0] = 0; }
GLuint glCreateShader(GLenum t) { (void)t; return 140; }
void glShaderSource(GLuint s, GLsizei n, const GLchar *const *str, const GLint *len) { (void)len; printf("fake: src %u %d %s\n", s, n, str[0]); }
GLuint glCreateProgram(void) { return 105; }
void glAttachShader(GLuint p, GLuint s) { printf("fake: attach %u %u\n", p, s); }
void glLinkProgram(GLuint p) { printf("fake: link %u\n", p); }
void glUseProgram(GLuint p) { printf("fake: use %u\n", p); }
GLint glGetUniformLocation(GLuint p, const GLchar *n) { printf("fake: loc %u %s\n", p, n); return 17; }
void glUniform4fv(GLint l, GLsizei n, const GLfloat *v) { printf("fake: u4fv %d %d %.2f %.2f\n", l, n, v[0], v[1]); }
void glBindBuffer(GLenum t, GLuint b) { (void)t; printf("fake: buffer %u\n", b); }
static unsigned char store[256];
void *glMapBufferRange(GLenum t, GLintptr o, GLsizeiptr l, GLbitfield a) { (void)t; (void)a; printf("fake: map %ld %ld\n", (long)o, (long)l); return store + o; }
void glFlushMappedBufferRange(GLenum t, GLintptr o, GLsizeiptr l) { (void)t; printf("fake: flush %ld %ld %u %u\n", (long)o, (long)l, store[16 + o], store[20 + o]); }
GLboolean glUnmapBuffer(GLenum t) { (void)t; printf("fake: unmap\n"); return GL_TRUE; }
void glPixelStorei(GLenum p, GLint v) { (void)p; (void)v; }
void glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *p)
{ (void)t; (void)l; (void)x; (void)y; (void)f; (void)ty; printf("fake: texsub %dx%d %s\n", w, h, p ? "pixels" : "null"); }
void glBindTexture(GLenum t, GLuint x) { (void)t; printf("fake: tex %u\n", x); }
void glBindVertexArray(GLuint v) { (void)v; }
void glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) { (void)m; (void)t; printf("fake: drawel %d %u\n", c, ((const unsigned short *)i)[0]); }
void glBindFramebuffer(GLenum t, GLuint f) { (void)t; printf("fake: fb %u\n", f); }
void glClear(GLbitfield m) { printf("fake: clear %x\n", m); }
void glDrawArrays(GLenum m, GLint f, GLsizei c) { (void)m; (void)f; printf("fake: draw %d\n", c); }
void glFlush(void) {}
void glFinish(void) {}
