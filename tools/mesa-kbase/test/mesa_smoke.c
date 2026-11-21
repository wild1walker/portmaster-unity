// mesa_smoke.c: Mesa's EGL/GLES on the kbase backend, off the device (with fake_kbase.c): a surfaceless
// display, a GLES 3 context on a 720x480 pbuffer, a shader program, frames of clear + draw + glReadPixels
// + eglSwapBuffers, a fence sync waited for, glFinish, everything destroyed. Checks that each call
// returns (no crash, no hang, no GL error); the pixels mean nothing (the fake runs no GPU jobs).
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s (egl 0x%x gl 0x%x)\n", __FILE__, __LINE__, #c, eglGetError(), glGetError()); return 1; } } while (0)

int main(int argc, char **argv)
{
   int frames = argc > 1 ? atoi(argv[1]) : 30;
   EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint maj, min;
   CHECK(d != EGL_NO_DISPLAY && eglInitialize(d, &maj, &min));
   const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                         EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
   EGLConfig c; EGLint n;
   CHECK(eglChooseConfig(d, ca, &c, 1, &n) && n == 1);
   const EGLint sa[] = { EGL_WIDTH, 720, EGL_HEIGHT, 480, EGL_NONE };
   EGLSurface s = eglCreatePbufferSurface(d, c, sa);
   eglBindAPI(EGL_OPENGL_ES_API);
   const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
   EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
   CHECK(s && x && eglMakeCurrent(d, s, s, x));
   printf("%s | %s | %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
   const char *vs = "#version 300 es\nin vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }\n";
   const char *fs = "#version 300 es\nprecision mediump float; uniform vec4 col; out vec4 o; void main() { o = col; }\n";
   GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), pr = glCreateProgram();
   glShaderSource(v, 1, &vs, NULL); glCompileShader(v);
   glShaderSource(f, 1, &fs, NULL); glCompileShader(f);
   glAttachShader(pr, v); glAttachShader(pr, f); glBindAttribLocation(pr, 0, "p"); glLinkProgram(pr);
   GLint ok = 0; glGetProgramiv(pr, GL_LINK_STATUS, &ok);
   CHECK(ok);
   const float tri[] = { -1, -1, 3, -1, -1, 3 };
   GLuint b; glGenBuffers(1, &b); glBindBuffer(GL_ARRAY_BUFFER, b); glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
   glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   glUseProgram(pr);
   GLint loc = glGetUniformLocation(pr, "col");
   unsigned char *px = malloc(720 * 480 * 4);
   for (int i = 0; i < frames; i++) {
      glViewport(0, 0, 720, 480);
      glClearColor(0.1f * (i % 10), 0.2f, 0.3f, 1);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
      glUniform4f(loc, 1, 0.5f, 0.25f, 1);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glReadPixels(0, 0, 720, 480, GL_RGBA, GL_UNSIGNED_BYTE, px);
      CHECK(eglSwapBuffers(d, s));
      CHECK(glGetError() == GL_NO_ERROR);
   }
   GLsync sy = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
   CHECK(glClientWaitSync(sy, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull) != GL_WAIT_FAILED);
   glDeleteSync(sy);
   glFinish();
   EGLSyncKHR es = eglCreateSync(d, EGL_SYNC_FENCE, NULL);
   CHECK(es != EGL_NO_SYNC);
   CHECK(eglClientWaitSync(d, es, 0, 1000000000ull) == EGL_CONDITION_SATISFIED);
   eglDestroySync(d, es);
   glDeleteBuffers(1, &b); glDeleteProgram(pr); glDeleteShader(v); glDeleteShader(f);
   eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(d, x); eglDestroySurface(d, s); eglTerminate(d);
   free(px);
   printf("ok mesa_smoke: %d frames\n", frames);
   return 0;
}
