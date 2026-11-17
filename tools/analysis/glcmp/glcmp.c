// glcmp: renders one texture through one GLES 3.0 fragment program with Mesa (surfaceless EGL,
// llvmpipe) at one fragment per texel, and writes the RGBA8 result.
//   glcmp <frag.glsl> <tex.raw> <w> <h> <fmt: rgba|r8> <palette.raw (256x1 RGB)> <out.raw> [r g b a: vertex colour 0..255]
// Uniforms the Maikel programs use are set to fixed test values (glcmp sets every float/vec4 uniform it
// finds by name from a small table; unknown ones keep 0).
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *p, long *n) {
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(s + 1); if (fread(b, 1, s, f) != (size_t)s) exit(2); b[s] = 0; fclose(f);
    if (n) *n = s; return b;
}

static GLuint shader(GLenum t, const char *src) {
    GLuint s = glCreateShader(t); glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "compile: %s\n", log); exit(3); }
    return s;
}

static const char *VS =
    "#version 300 es\nin vec2 pos;\nuniform vec2 size;\nuniform vec4 vcolor;\nout vec4 vs_COLOR0;\nout vec2 vs_TEXCOORD0;\n"
    "void main(){ vs_TEXCOORD0 = pos * 0.5 + 0.5; vs_COLOR0 = vcolor; gl_Position = vec4(pos, 0.0, 1.0); }\n";

int main(int argc, char **argv) {
    if (argc < 8) { fprintf(stderr, "usage\n"); return 2; }
    int w = atoi(argv[3]), h = atoi(argv[4]); int r8 = !strcmp(argv[5], "r8");
    float vc[4] = {1, 1, 1, 1};
    if (argc >= 12) for (int i = 0; i < 4; i++) vc[i] = atoi(argv[8 + i]) / 255.0f;
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (!eglInitialize(d, NULL, NULL)) { fprintf(stderr, "egl\n"); return 4; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE}; EGLConfig cfg; EGLint n;
    eglChooseConfig(d, ca, &cfg, 1, &n);
    EGLint xa[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext c = eglCreateContext(d, n > 0 ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, xa);
    if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) { fprintf(stderr, "current\n"); return 4; }
    if (getenv("GLCMP_INFO")) fprintf(stderr, "GL %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    // the fragment program (the FRAGMENT part of a Unity GLES3 entry)
    char *fs = slurp(argv[1], NULL);
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, VS)); glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "pos"); glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096]; glGetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "link: %s\n", log); return 3; }
    glUseProgram(p);
    // textures: _MainTex (unit 0), _PaletteTex (unit 1), nearest
    long tn; unsigned char *tex = (unsigned char *)slurp(argv[2], &tn);
    GLuint t[2]; glGenTextures(2, t);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, t[0]); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (r8) glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, tex);
    else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    long pn; unsigned char *pal = (unsigned char *)slurp(argv[6], &pn);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, t[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, pal);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLint u;
    if ((u = glGetUniformLocation(p, "_MainTex")) >= 0) glUniform1i(u, 0);
    if ((u = glGetUniformLocation(p, "_PaletteTex")) >= 0) glUniform1i(u, 1);
    if ((u = glGetUniformLocation(p, "_AlphaTex")) >= 0) glUniform1i(u, 0);
    glUniform4fv(glGetUniformLocation(p, "vcolor"), 1, vc);
    // the effects' uniforms at mid values (their variants then do something)
    struct { const char *n; float v[4]; } U[] = {
        {"_Color", {1, 1, 1, 1}}, {"_LerpColor", {0.8f, 0.2f, 0.4f, 1}}, {"_LerpAmount", {0.37f}},
        {"_ColorizeColor", {0.3f, 0.6f, 0.9f, 1}}, {"_ColorizeMultColor", {0.9f, 0.7f, 0.5f, 1}}, {"_ColorizeStrength", {0.45f}},
        {"_DamageEffectColor", {1, 0.1f, 0.1f, 1}}, {"_DamageEffectHighlight", {0.6f, 0.6f, 0.6f, 1}}, {"_DamageEffectTimescale", {1}},
        {"_Time", {1.5f, 3.0f, 6.0f, 9.0f}}, {"_ID", {0}}, {"_AlphaSplitEnabled", {0}},
    };
    for (unsigned i = 0; i < sizeof U / sizeof U[0]; i++)
        if ((u = glGetUniformLocation(p, U[i].n)) >= 0) {
            GLint sz; GLenum ty; char nm[64]; GLint cnt; GLint idx = -1;
            glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &cnt);
            for (int k = 0; k < cnt; k++) { glGetActiveUniform(p, k, sizeof nm, NULL, &sz, &ty, nm); if (!strcmp(nm, U[i].n)) { idx = k; break; } }
            if (idx >= 0 && ty == GL_FLOAT) glUniform1f(u, U[i].v[0]); else glUniform4fv(u, 1, U[i].v);
        }
    // target
    GLuint fb, rb; glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glViewport(0, 0, w, h); glClearColor(0.25f, 0.5f, 0.75f, 1); glClear(GL_COLOR_BUFFER_BIT);
    float q[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLuint vb; glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb); glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    unsigned char *out = malloc((size_t)w * h * 4); glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    FILE *o = fopen(argv[7], "wb"); fwrite(out, 1, (size_t)w * h * 4, o); fclose(o);
    return glGetError() == GL_NO_ERROR ? 0 : 5;
}
