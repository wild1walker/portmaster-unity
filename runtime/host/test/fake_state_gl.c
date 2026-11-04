// A fake libGLESv2 for test_glstate.c: the state glstate.c follows, kept as a driver keeps it
// (state_model.h), and a count of the calls it got.
#include "state_model.h"

static model g_m;
static int g_init;
static model *M(void) { if (!g_init) { m_init(&g_m); g_init = 1; } g_m.calls++; return &g_m; }
model *fake_state(void) { if (!g_init) { m_init(&g_m); g_init = 1; } return &g_m; }

void glActiveTexture(GLenum u) { m_ActiveTexture(M(), u); }
void glBindTexture(GLenum t, GLuint x) { m_BindTexture(M(), t, x); }
void glTexParameteri(GLenum t, GLenum p, GLint v) { m_TexParameteri(M(), t, p, v); }
void glDeleteTextures(GLsizei n, const GLuint *x) { m_DeleteTextures(M(), n, x); }
void glUseProgram(GLuint p) { M()->prog = p; }
void glBindBuffer(GLenum t, GLuint b) { m_BindBuffer(M(), t, b); }
void glBindBufferBase(GLenum t, GLuint i, GLuint b) { m_BindBufferBase(M(), t, i, b); }
void glBindBufferRange(GLenum t, GLuint i, GLuint b, intptr_t o, intptr_t s) { (void)o; (void)s; m_BindBufferBase(M(), t, i, b); }
void glDeleteBuffers(GLsizei n, const GLuint *b) { m_DeleteBuffers(M(), n, b); }
void glBindVertexArray(GLuint a) { m_BindVertexArray(M(), a); }
void glDeleteVertexArrays(GLsizei n, const GLuint *a) { m_DeleteVertexArrays(M(), n, a); }
void glEnable(GLenum c) { m_Enable(M(), c, 1); }
void glDisable(GLenum c) { m_Enable(M(), c, 0); }
void glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { model *m = M(); m->blend[0] = a; m->blend[1] = b; m->blend[2] = c; m->blend[3] = d; }
void glBlendFunc(GLenum a, GLenum b) { model *m = M(); m->blend[0] = a; m->blend[1] = b; m->blend[2] = a; m->blend[3] = b; }
void glBlendEquationSeparate(GLenum a, GLenum b) { model *m = M(); m->blendeq[0] = a; m->blendeq[1] = b; }
void glBlendEquation(GLenum a) { model *m = M(); m->blendeq[0] = a; m->blendeq[1] = a; }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{ model *m = M(); m->colormask[0] = r != 0; m->colormask[1] = g != 0; m->colormask[2] = b != 0; m->colormask[3] = a != 0; }
void glDepthFunc(GLenum f) { M()->depthfunc = f; }
void glDepthMask(GLboolean k) { M()->depthmask = k != 0; }
void glStencilFuncSeparate(GLenum face, GLenum f, GLint r, GLuint k) { m_StencilFuncSeparate(M(), face, f, r, k); }
void glStencilFunc(GLenum f, GLint r, GLuint k) { m_StencilFuncSeparate(M(), 0x0408, f, r, k); }
void glStencilOpSeparate(GLenum face, GLenum a, GLenum b, GLenum c) { m_StencilOpSeparate(M(), face, a, b, c); }
void glStencilOp(GLenum a, GLenum b, GLenum c) { m_StencilOpSeparate(M(), 0x0408, a, b, c); }
void glStencilMask(GLuint k) { m_StencilMaskSeparate(M(), 0x0408, k); }
void glStencilMaskSeparate(GLenum face, GLuint k) { m_StencilMaskSeparate(M(), face, k); }
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { model *m = M(); m->viewport[0] = x; m->viewport[1] = y; m->viewport[2] = w; m->viewport[3] = h; }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { model *m = M(); m->scissor[0] = x; m->scissor[1] = y; m->scissor[2] = w; m->scissor[3] = h; }
void glFrontFace(GLenum f) { M()->frontface = f; }
void glCullFace(GLenum f) { M()->cullface = f; }
void glVertexAttribPointer(GLuint a, GLint s, GLenum t, GLboolean n, GLsizei st, const void *p) { m_VertexAttribPointer(M(), a, s, t, n, st, p); }
void glVertexAttribIPointer(GLuint a, GLint s, GLenum t, GLsizei st, const void *p) { m_VertexAttribPointer(M(), a, s, t, 0, st, p); }
void glEnableVertexAttribArray(GLuint a) { model *m = M(); if (a < M_ATTR) m->attr_on[m->vao][a] = 1; }
void glDisableVertexAttribArray(GLuint a) { model *m = M(); if (a < M_ATTR) m->attr_on[m->vao][a] = 0; }
