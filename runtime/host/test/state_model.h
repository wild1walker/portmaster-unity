// The GL state glstate.c follows, as a driver keeps it: fake_state_gl.c's driver and test_glstate.c's
// reference both apply every call to one of these, and must end up equal.
#include <stdint.h>
#include <string.h>

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned char GLboolean;

#define M_UNITS 4
#define M_TEX 8                      // texture names 1..7
#define M_BUF 8                      // buffer names 1..7
#define M_VAO 4                      // vertex arrays 0..3
#define M_ATTR 4
typedef struct { GLuint buf; GLint size; GLenum type; GLboolean norm; GLsizei stride; uintptr_t ptr; } m_attr;
typedef struct {
    GLenum active;
    GLuint tex[M_UNITS][5];
    GLint param[M_TEX][3];           // MIN_FILTER, MAG_FILTER, WRAP_S of each texture (0: the default)
    GLuint prog, vao;
    GLuint buf[6];                   // ARRAY, (ELEMENT: the vertex array's), UNIFORM, COPY_READ, COPY_WRITE, PIXEL_UNPACK
    GLuint elem[M_VAO];
    m_attr attr[M_VAO][M_ATTR];
    int attr_on[M_VAO][M_ATTR];
    int cap[11];
    GLenum blend[4], blendeq[2];
    int colormask[4];
    GLenum depthfunc; int depthmask;
    GLenum stfunc[2][3], stop[2][3]; GLuint stmask[2];
    GLint viewport[4], scissor[4];
    GLenum frontface, cullface;
    unsigned long calls;             // the calls the driver got
} model;

static int m_tex_index(GLenum t)
{
    switch (t) { case 0x0DE1: return 0; case 0x8513: return 1; case 0x806F: return 2; case 0x8C1A: return 3; case 0x8D65: return 4; }
    return -1;
}
static int m_buf_index(GLenum t)
{
    switch (t) { case 0x8892: return 0; case 0x8893: return 1; case 0x8A11: return 2; case 0x8F36: return 3; case 0x8F37: return 4; case 0x88EC: return 5; }
    return -1;
}
static int m_cap_index(GLenum c)
{
    static const GLenum caps[11] = { 0x0BE2, 0x0B44, 0x0B71, 0x0B90, 0x0C11, 0x8037, 0x0BD0, 0x809E, 0x80A0, 0x8C89, 0x8D69 };
    for (int i = 0; i < 11; i++) if (caps[i] == c) return i;
    return -1;
}
static int m_param_index(GLenum p) { return p == 0x2801 ? 0 : p == 0x2800 ? 1 : p == 0x2802 ? 2 : -1; }
static void m_faces(GLenum face, int *a, int *b) { *a = face == 0x0405 ? 1 : 0; *b = face == 0x0404 ? 0 : 1; }

static void m_init(model *m) { memset(m, 0, sizeof *m); m->active = 0x84C0; }
static void m_ActiveTexture(model *m, GLenum u) { m->active = u; }
static void m_BindTexture(model *m, GLenum t, GLuint x) { int i = m_tex_index(t); if (i >= 0) m->tex[m->active - 0x84C0][i] = x; }
static void m_TexParameteri(model *m, GLenum t, GLenum p, GLint v)
{
    int i = m_tex_index(t), k = m_param_index(p);
    GLuint x = i >= 0 ? m->tex[m->active - 0x84C0][i] : 0;
    if (x && x < M_TEX && k >= 0) m->param[x][k] = v;
}
static void m_DeleteTextures(model *m, GLsizei n, const GLuint *x)
{
    for (GLsizei j = 0; j < n; j++) {
        for (int u = 0; u < M_UNITS; u++) for (int i = 0; i < 5; i++) if (m->tex[u][i] == x[j]) m->tex[u][i] = 0;
        if (x[j] < M_TEX) memset(m->param[x[j]], 0, sizeof m->param[0]);
    }
}
static GLuint *m_bufslot(model *m, int i) { return i == 1 ? &m->elem[m->vao] : &m->buf[i]; }
static void m_BindBuffer(model *m, GLenum t, GLuint b) { int i = m_buf_index(t); if (i >= 0) *m_bufslot(m, i) = b; }
static void m_BindBufferBase(model *m, GLenum t, GLuint idx, GLuint b) { (void)idx; m_BindBuffer(m, t, b); }
static void m_DeleteBuffers(model *m, GLsizei n, const GLuint *b)
{
    for (GLsizei j = 0; j < n; j++) {
        for (int i = 0; i < 6; i++) if (*m_bufslot(m, i) == b[j]) *m_bufslot(m, i) = 0;
        for (int a = 0; a < M_ATTR; a++) if (m->attr[m->vao][a].buf == b[j]) m->attr[m->vao][a].buf = 0;   // (the bound vertex array's)
    }
}
static void m_BindVertexArray(model *m, GLuint a) { m->vao = a; }
static void m_DeleteVertexArrays(model *m, GLsizei n, const GLuint *a)
{
    for (GLsizei j = 0; j < n; j++) if (a[j] && a[j] < M_VAO) {
        if (m->vao == a[j]) m->vao = 0;
        m->elem[a[j]] = 0; memset(m->attr[a[j]], 0, sizeof m->attr[0]); memset(m->attr_on[a[j]], 0, sizeof m->attr_on[0]);
    }
}
static void m_Enable(model *m, GLenum c, int on) { int i = m_cap_index(c); if (i >= 0) m->cap[i] = on; }
static void m_StencilFuncSeparate(model *m, GLenum face, GLenum f, GLint r, GLuint k)
{ int a, b; m_faces(face, &a, &b); for (int s = a; s <= b; s++) { m->stfunc[s][0] = f; m->stfunc[s][1] = (GLenum)r; m->stfunc[s][2] = k; } }
static void m_StencilOpSeparate(model *m, GLenum face, GLenum x, GLenum y, GLenum z)
{ int a, b; m_faces(face, &a, &b); for (int s = a; s <= b; s++) { m->stop[s][0] = x; m->stop[s][1] = y; m->stop[s][2] = z; } }
static void m_StencilMaskSeparate(model *m, GLenum face, GLuint k) { int a, b; m_faces(face, &a, &b); for (int s = a; s <= b; s++) m->stmask[s] = k; }
static void m_VertexAttribPointer(model *m, GLuint a, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p)
{
    if (a < M_ATTR) m->attr[m->vao][a] = (m_attr){ m->buf[0], size, type, norm != 0, stride, (uintptr_t)p };
}
