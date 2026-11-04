// glstate.c against fake_state_gl.c (as libGLESv2.so.2): random state calls (few names and values, so
// many repeat what is set) through the layer's wrappers, the same calls applied to a reference
// state; after every call the driver's state must equal the reference (nothing that matters left
// out). Run by pointer_test.sh: test_glstate <scenario>
//   random   UA_GL_STATE=1, 200,000 calls in frames of 1 - 80 calls: equal throughout, calls left out
//   count    UA_GL_STATE=count: every call reaches the driver
//   off      unset: no wrappers
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "state_model.h"
#include "../glstate.h"

model *fake_state(void);
static const so_shim *g_t;
static void *fn(const char *name)
{
    for (const so_shim *s = g_t; s && s->name; s++) if (!strcmp(s->name, name)) return s->fn;
    fprintf(stderr, "FAIL: no wrapper of %s\n", name);
    exit(1);
}

static unsigned g_seed = 12345;
static unsigned rnd(unsigned n) { g_seed = g_seed * 1103515245u + 12345u; return (g_seed >> 16) % n; }

static int equal(const model *a, const model *b)
{
    model x = *a, y = *b;
    x.calls = y.calls = 0;
    return !memcmp(&x, &y, sizeof x);
}

int main(int argc, char **argv)
{
    const char *sc = argc > 1 ? argv[1] : "random";
    if (!strcmp(sc, "random")) setenv("UA_GL_STATE", "1", 1);
    else if (!strcmp(sc, "count")) setenv("UA_GL_STATE", "count", 1);
    g_t = glstate_table(NULL);
    if (!strcmp(sc, "off")) {
        if (g_t[0].name) { fprintf(stderr, "FAIL off: wrappers made\n"); return 1; }
        printf("ok off\n");
        return 0;
    }
    void (*ActiveTexture)(GLenum) = fn("glActiveTexture");
    void (*BindTexture)(GLenum, GLuint) = fn("glBindTexture");
    void (*TexParameteri)(GLenum, GLenum, GLint) = fn("glTexParameteri");
    void (*DeleteTextures)(GLsizei, const GLuint *) = fn("glDeleteTextures");
    void (*UseProgram)(GLuint) = fn("glUseProgram");
    void (*BindBuffer)(GLenum, GLuint) = fn("glBindBuffer");
    void (*BindBufferBase)(GLenum, GLuint, GLuint) = fn("glBindBufferBase");
    void (*DeleteBuffers)(GLsizei, const GLuint *) = fn("glDeleteBuffers");
    void (*BindVertexArray)(GLuint) = fn("glBindVertexArray");
    void (*DeleteVertexArrays)(GLsizei, const GLuint *) = fn("glDeleteVertexArrays");
    void (*Enable)(GLenum) = fn("glEnable");
    void (*Disable)(GLenum) = fn("glDisable");
    void (*BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum) = fn("glBlendFuncSeparate");
    void (*BlendFunc)(GLenum, GLenum) = fn("glBlendFunc");
    void (*ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean) = fn("glColorMask");
    void (*DepthMask)(GLboolean) = fn("glDepthMask");
    void (*StencilFuncSeparate)(GLenum, GLenum, GLint, GLuint) = fn("glStencilFuncSeparate");
    void (*StencilFunc)(GLenum, GLint, GLuint) = fn("glStencilFunc");
    void (*StencilOpSeparate)(GLenum, GLenum, GLenum, GLenum) = fn("glStencilOpSeparate");
    void (*StencilMask)(GLuint) = fn("glStencilMask");
    void (*StencilMaskSeparate)(GLenum, GLuint) = fn("glStencilMaskSeparate");
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei) = fn("glViewport");
    void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *) = fn("glVertexAttribPointer");
    void (*VertexAttribIPointer)(GLuint, GLint, GLenum, GLsizei, const void *) = fn("glVertexAttribIPointer");
    void (*EnableVertexAttribArray)(GLuint) = fn("glEnableVertexAttribArray");
    void (*DisableVertexAttribArray)(GLuint) = fn("glDisableVertexAttribArray");

    static const GLenum texT[] = { 0x0DE1, 0x8513 }, bufT[] = { 0x8892, 0x8893, 0x8A11, 0x88EB }, caps[] = { 0x0BE2, 0x0B71, 0x0B90, 0x0C11 };
    static const GLenum faces[] = { 0x0404, 0x0405, 0x0408 }, params[] = { 0x2801, 0x2800, 0x2802 };
    model ref; m_init(&ref);
    glstate_frame();                                    // (the first swap: this thread renders)
    unsigned long n = 200000, sent = 0, frame_left = 1 + rnd(80);
    for (unsigned long i = 0; i < n; i++) {
        GLuint a = rnd(3), b = rnd(3);
        switch (rnd(24)) {
        case 0: { GLenum u = 0x84C0 + rnd(M_UNITS); ActiveTexture(u); m_ActiveTexture(&ref, u); break; }
        case 1: case 2: { GLenum t = texT[rnd(2)]; GLuint x = rnd(M_TEX); BindTexture(t, x); m_BindTexture(&ref, t, x); break; }
        case 3: { GLenum t = texT[rnd(2)], p = params[rnd(3)]; GLint v = 0x2600 + (GLint)rnd(2); TexParameteri(t, p, v); m_TexParameteri(&ref, t, p, v); break; }
        case 4: { GLuint x = 1 + rnd(M_TEX - 1); if (rnd(8)) { BindTexture(0x0DE1, x); m_BindTexture(&ref, 0x0DE1, x); break; }
                  DeleteTextures(1, &x); m_DeleteTextures(&ref, 1, &x); break; }
        case 5: { GLuint p = rnd(3); UseProgram(p); ref.prog = p; break; }
        case 6: case 7: { GLenum t = bufT[rnd(4)]; GLuint x = rnd(M_BUF); BindBuffer(t, x); m_BindBuffer(&ref, t, x); break; }
        case 8: { GLuint x = rnd(M_BUF); BindBufferBase(0x8A11, rnd(4), x); m_BindBufferBase(&ref, 0x8A11, 0, x); break; }
        case 9: { GLuint x = 1 + rnd(M_BUF - 1); if (rnd(6)) { BindBuffer(0x8892, x); m_BindBuffer(&ref, 0x8892, x); break; }
                  DeleteBuffers(1, &x); m_DeleteBuffers(&ref, 1, &x); break; }
        case 10: { GLuint v = rnd(M_VAO); if (rnd(10)) { BindVertexArray(v); m_BindVertexArray(&ref, v); break; }
                   v = 1 + rnd(M_VAO - 1); DeleteVertexArrays(1, &v); m_DeleteVertexArrays(&ref, 1, &v); break; }
        case 11: { GLenum c = caps[rnd(4)]; if (rnd(2)) { Enable(c); m_Enable(&ref, c, 1); } else { Disable(c); m_Enable(&ref, c, 0); } break; }
        case 12: if (rnd(2)) { BlendFuncSeparate(a, b, b, a); ref.blend[0] = a; ref.blend[1] = b; ref.blend[2] = b; ref.blend[3] = a; }
                 else { BlendFunc(a, b); ref.blend[0] = a; ref.blend[1] = b; ref.blend[2] = a; ref.blend[3] = b; } break;
        case 13: ColorMask(a & 1, b & 1, 1, 1); ref.colormask[0] = a & 1; ref.colormask[1] = b & 1; ref.colormask[2] = 1; ref.colormask[3] = 1; break;
        case 14: DepthMask(a & 1); ref.depthmask = a & 1; break;
        case 15: { GLenum f = faces[rnd(3)]; StencilFuncSeparate(f, 0x200 + a, (GLint)b, 0xff); m_StencilFuncSeparate(&ref, f, 0x200 + a, (GLint)b, 0xff); break; }
        case 16: StencilFunc(0x200 + a, (GLint)b, 0xff); m_StencilFuncSeparate(&ref, 0x0408, 0x200 + a, (GLint)b, 0xff); break;
        case 17: { GLenum f = faces[rnd(3)]; StencilOpSeparate(f, a, b, a); m_StencilOpSeparate(&ref, f, a, b, a); break; }
        case 18: if (rnd(2)) { StencilMask(a); m_StencilMaskSeparate(&ref, 0x0408, a); }
                 else { GLenum f = faces[rnd(3)]; StencilMaskSeparate(f, a); m_StencilMaskSeparate(&ref, f, a); } break;
        case 19: Viewport(0, 0, 100 + (GLsizei)a, 50); ref.viewport[0] = 0; ref.viewport[1] = 0; ref.viewport[2] = 100 + (GLint)a; ref.viewport[3] = 50; break;
        case 20: case 21: { GLuint at = rnd(M_ATTR); const void *p = (const void *)(uintptr_t)(rnd(3) * 16);
                   if (rnd(10)) { VertexAttribPointer(at, 4, 0x1406, a & 1, 24, p); m_VertexAttribPointer(&ref, at, 4, 0x1406, a & 1, 24, p); }
                   else { VertexAttribIPointer(at, 4, 0x1406, 24, p); m_VertexAttribPointer(&ref, at, 4, 0x1406, 0, 24, p); } break; }
        case 22: { GLuint at = rnd(M_ATTR); EnableVertexAttribArray(at); if (at < M_ATTR) ref.attr_on[ref.vao][at] = 1; break; }
        case 23: { GLuint at = rnd(M_ATTR); DisableVertexAttribArray(at); if (at < M_ATTR) ref.attr_on[ref.vao][at] = 0; break; }
        }
        sent++;
        if (!equal(fake_state(), &ref)) {
            fprintf(stderr, "FAIL %s: the driver's state differs from the reference after call %lu\n", sc, i);
            return 1;
        }
        if (--frame_left == 0) { glstate_frame(); frame_left = 1 + rnd(80); }
    }
    unsigned long got = fake_state()->calls;
    if (!strcmp(sc, "count") && got != sent) { fprintf(stderr, "FAIL count: %lu of %lu calls reached the driver\n", got, sent); return 1; }
    if (!strcmp(sc, "random") && got >= sent) { fprintf(stderr, "FAIL random: nothing left out\n"); return 1; }
    printf("ok %s: %lu calls, %lu reached the driver, the same state throughout\n", sc, sent, got);
    return 0;
}
