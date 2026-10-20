// GL test of the SurfaceTexture path with a real GLES 3 context (Mesa's llvmpipe in the test
// image, surfaceless EGL): decoded pictures go through um_surface_update (EGLImage of the packed
// 4:2:0 texture behind an external texture), then are drawn as Unity draws them, with the
// converter's Hidden/VideoDecodeAndroid programs (convert/pmunity/video_decode_android.shader,
// passed as argv[1]) and texture coordinates from getTransformMatrix. Every pixel is compared
// with the desktop players' YCbCr -> RGB conversion of the same Y/U/V (nearest chroma), and
// the upright orientation, the cropping of odd sizes, the alpha split pass and the GL state the
// upload must leave alone are checked.
#define _GNU_SOURCE
#include "../um.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char *read_file(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    if (fread(s, 1, n, f) != (size_t)n) { fclose(f); free(s); return NULL; }
    s[n] = 0;
    fclose(f);
    return s;
}

/* one stage of a pass, as the driver gets it: the text between "#ifdef STAGE" and its "#endif" */
static char *stage(const char *glsl, const char *pass, const char *st)
{
    char key[128];
    snprintf(key, sizeof key, "//// pass %s\n", pass);
    const char *p = strstr(glsl, key);
    if (!p) return NULL;
    snprintf(key, sizeof key, "#ifdef %s\n", st);
    const char *a = strstr(p, key);
    if (!a) return NULL;
    a += strlen(key);
    const char *e = strstr(a, "\n#endif");
    char *s = strndup(a, e - a + 1);
    return s;
}

static GLuint compile(GLenum t, const char *src)
{
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[2048]; glGetShaderInfoLog(s, sizeof b, NULL, b); printf("compile error: %s\n", b); }
    CHECK(ok, "shader compiles");
    return s;
}

static GLuint program(const char *glsl, const char *pass, int bt709)
{
    char *vs = stage(glsl, pass, "VERTEX"), *fs = stage(glsl, pass, "FRAGMENT");
    char *d = fs ? strstr(fs, "const bool UM_BT709 = false;") : NULL;   /* what video.py sets for H.264 clips */
    CHECK(d != NULL, "UM_BT709 in %s", pass);
    if (d && bt709) memcpy(d + strlen("const bool UM_BT709 = "), "true; ", 6);
    CHECK(vs && fs, "pass %s in the shader file", pass);
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "in_POSITION0");
    glBindAttribLocation(p, 1, "in_TEXCOORD0");
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    CHECK(ok, "program links");
    free(vs); free(fs);
    return p;
}

/* the desktop conversions, to 8 bits: Hidden/VideoDecode's YCbCr_TO_RGB1 (VP8 / VP9) and its
   NV12 pass (BT.709, H.264 / HEVC) */
enum { UM_COLOUR_UNITY_SOFTWARE, UM_COLOUR_BT709 };
static int g_colour;
static void ref_rgb(int Y, int U, int V, int out[3])
{
    float y = Y / 255.0f, u = U / 255.0f, v = V / 255.0f;
    float r = y * 1.15625f + v * 1.59375f - 0.872539997f;
    float g = y * 1.15625f - u * 0.390625f - v * 0.8125f + 0.531369984f;
    float b = y * 1.15625f + u * 1.984375f - 1.06861997f;
    if (g_colour == UM_COLOUR_BT709) {
        float ty = y - 0.0625f, tu = u - 0.5f, tv = v - 0.5f;
        r = 1.16439998f * ty + 1.79270005f * tv;
        g = 1.16439998f * ty - 0.213300005f * tu - 0.532899976f * tv;
        b = 1.16439998f * ty + 2.11240005f * tu;
    }
    float c[3] = { r, g, b };
    for (int i = 0; i < 3; i++) {
        float x = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        out[i] = (int)lrintf(x * 255.0f);
    }
}

static uint8_t py(int x, int y) { return (uint8_t)(16 + (x * 7 + y * 13) % 220); }
static uint8_t pu(int x, int y) { return (uint8_t)(30 + (x * 11 + y * 3) % 200); }
static uint8_t pv(int x, int y) { return (uint8_t)(220 - (x * 5 + y * 9) % 200); }

/* draw the latched picture with pass `pr` into a w x h target as Unity's blit does, read back */
static uint8_t *draw(GLuint pr, GLuint ext, um_surface *s, int w, int h)
{
    float m[16];
    um_surface_matrix(s, m);
    float sx = m[0], sy = m[5], ox = m[12], oy = m[13];
    /* the quad's texcoords: uv * scale + offset (Unity's ImageFilters::Blit with the matrix's) */
    float q[] = { -1, -1, 0 * sx + ox, 0 * sy + oy,   1, -1, 1 * sx + ox, 0 * sy + oy,
                  -1,  1, 0 * sx + ox, 1 * sy + oy,   1,  1, 1 * sx + ox, 1 * sy + oy };
    GLuint rt, fb;
    glGenTextures(1, &rt);
    glBindTexture(GL_TEXTURE_2D, rt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    glViewport(0, 0, w, h);
    glUseProgram(pr);
    static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    glUniform4fv(glGetUniformLocation(pr, "hlslcc_mtx4x4unity_ObjectToWorld"), 4, ident);
    glUniform4fv(glGetUniformLocation(pr, "hlslcc_mtx4x4unity_MatrixVP"), 4, ident);
    glUniform4f(glGetUniformLocation(pr, "_MainTex_ST"), 1, 1, 0, 0);
    GLint ap = glGetUniformLocation(pr, "_AlphaParam");
    if (ap >= 0) glUniform1f(ap, 1.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ext);
    glUniform1i(glGetUniformLocation(pr, "_MainTex"), 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, q);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, q + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK(glGetError() == GL_NO_ERROR, "draw");
    uint8_t *px = malloc((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &rt);
    return px;
}

static void test_size(const char *glsl, int w, int h, int split, int colour)
{
    printf("-- %dx%d%s%s\n", w, h, split ? " (alpha split)" : "", colour == UM_COLOUR_BT709 ? " BT.709" : "");
    g_colour = colour;
    GLuint ext;
    glGenTextures(1, &ext);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ext);       /* as Unity's ApiGLES::GenTexture does */
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    um_surface *s = um_surface_new(ext);
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    uint8_t *Y = malloc((size_t)w * h), *U = malloc((size_t)cw * ch), *V = malloc((size_t)cw * ch);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) Y[y * w + x] = py(x, y);
    for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) { U[y * cw + x] = pu(x, y); V[y * cw + x] = pv(x, y); }
    um_frame *f = um_frame_get(w, h);
    um_frame_fill(f, Y, w, U, cw, V, cw);
    f->pts_us = 123456;
    um_surface_queue(s, f);
    /* state the upload must leave as it was */
    GLuint other;
    glGenTextures(1, &other);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, other);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    CHECK(um_surface_update(s) == 1, "update latches the frame");
    GLint b2d, act, al, bext;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &b2d);
    glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &bext);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &al);
    CHECK(act == GL_TEXTURE3 && b2d == (GLint)other && bext == 0 && al == 8, "GL state kept (unit %x, 2D %d, ext %d, align %d)",
          act, b2d, bext, al);
    CHECK(um_surface_timestamp(s) == 123456000, "timestamp");
    CHECK(um_surface_update(s) == 0, "nothing new to latch");
    int ow = split ? w / 2 : w;
    GLuint pr = program(glsl, split ? "RGBASplitExternal_To_RGBA" : "RGBAExternal_To_RGBA", colour == UM_COLOUR_BT709);
    uint8_t *px = draw(pr, ext, s, ow, h);
    int maxd = 0, bad = 0, amax = 0;
    for (int j = 0; j < h; j++)
        for (int x = 0; x < ow; x++) {
            int y = h - 1 - j;                      /* read back bottom-up: the picture is upright */
            int ref[3];
            ref_rgb(py(x, y), pu(x / 2, y / 2), pv(x / 2, y / 2), ref);
            const uint8_t *p = px + ((size_t)j * ow + x) * 4;
            for (int c = 0; c < 3; c++) { int d = abs(p[c] - ref[c]); if (d > maxd) maxd = d; if (d > 1) bad++; }
            if (split) {
                float a = (py(x + ow, y) / 255.0f - 0.0627449974f) * 1.15625f;
                int ar = (int)lrintf((a < 0 ? 0 : a > 1 ? 1 : a) * 255);
                int d = abs(p[3] - ar);
                if (d > amax) amax = d;
            } else if (p[3] != 255) bad++;
        }
    CHECK(bad == 0, "%d pixels off by more than 1 (max %d)", bad, maxd);
    if (split) CHECK(amax <= 1, "alpha from the right half (max diff %d)", amax);
    printf("   max difference to the desktop conversion: %d/255\n", maxd);
    free(px);
    /* a second, different frame replaces the first through the same EGLImage */
    um_frame *g = um_frame_get(w, h);
    memset(Y, 128, (size_t)w * h); memset(U, 128, (size_t)cw * ch); memset(V, 128, (size_t)cw * ch);
    um_frame_fill(g, Y, w, U, cw, V, cw);
    um_surface_queue(s, g);
    CHECK(um_surface_update(s) == 1, "second update");
    px = draw(pr, ext, s, ow, h);
    int ref[3];
    ref_rgb(128, 128, 128, ref);
    CHECK(abs(px[0] - ref[0]) <= 1 && abs(px[(size_t)(ow * h - 1) * 4 + 1] - ref[1]) <= 1, "second frame shows (%d vs %d)", px[0], ref[0]);
    free(px);
    um_surface_unref(s);
    glDeleteTextures(1, &ext);
    glDeleteTextures(1, &other);
    glDeleteProgram(pr);
    free(Y); free(U); free(V);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char *glsl = read_file(argc > 1 ? argv[1] : "../../convert/pmunity/video_decode_android.shader");
    if (!glsl) { printf("FAILED: no shader file\n"); return 1; }
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(d, NULL, NULL)) { printf("SKIPPED: no EGL display\n"); return 0; }
    EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLConfig c;
    EGLint n;
    eglChooseConfig(d, ca, &c, 1, &n);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
    EGLint pa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(d, c, pa);
    if (!eglMakeCurrent(d, s, s, ctx)) { printf("SKIPPED: no GLES 3 context\n"); return 0; }
    printf("GL: %s, %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    test_size(glsl, 64, 36, 0, UM_COLOUR_UNITY_SOFTWARE);
    test_size(glsl, 71, 41, 0, UM_COLOUR_UNITY_SOFTWARE);
    test_size(glsl, 128, 40, 1, UM_COLOUR_UNITY_SOFTWARE);
    test_size(glsl, 64, 36, 0, UM_COLOUR_BT709);
    test_size(glsl, 71, 41, 0, UM_COLOUR_BT709);
    printf("%s: %d failure(s) in %d checks\n", fails ? "FAILED" : "PASSED", fails, checks);
    return fails != 0;
}
