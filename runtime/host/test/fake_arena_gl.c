// A fake libGLESv2 for test_glarena.c: buffer objects with storage (mutable, or immutable and
// persistently mapped), the bindings (the index buffer the vertex array's), attribute pointers,
// and a GPU two frames behind: a draw is read (its indices, and through them its two attributes'
// bytes, into a hash) only when the GPU gets to it, at fake_gpu_frame() two frames later or at a
// wait. A range of the arena reused before the GPU is past it shows as a draw that read other bytes.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned GLbitfield;
typedef unsigned char GLboolean; typedef intptr_t GLintptr; typedef intptr_t GLsizeiptr; typedef void *GLsync;

#define NB 65536
static struct { unsigned char *data; long size; int dead; } B[NB];
static GLuint g_next = 1, g_array, g_copyr, g_copyw, g_uniform, g_vao, g_elem[8];
static struct { GLuint buf; uintptr_t off; int stride, bytes; } A[8][4];
unsigned long fake_calls_bufferdata;           // the driver's glBufferData calls (buffers outside the arena)

// the GPU's queue: draws waiting to be read, with what they read from (as bound at the call)
// (the storage as it was at the call: sizing a buffer again gives it new storage, the queued draws keep the old)
typedef struct { const unsigned char *ebuf; uintptr_t eoff; int count, base; const unsigned char *vbuf[2]; uintptr_t voff[2]; int stride[2], bytes[2]; long frame; int id; } qdraw;
static qdraw *g_q; static int g_nq, g_capq;
static long g_frame;                           // frames the CPU has finished
uint32_t fake_result[200000]; int fake_done[200000];
static int g_id;

static uint32_t hash_draw(const qdraw *d)
{
    uint32_t h = 2166136261u;
    const unsigned short *ix = (const unsigned short *)(d->ebuf + d->eoff);
    for (int i = 0; i < d->count; i++) {
        int v = ix[i] + d->base;
        for (int a = 0; a < 2; a++) {
            const unsigned char *p = d->vbuf[a] + d->voff[a] + (uintptr_t)v * (uintptr_t)d->stride[a];
            for (int k = 0; k < d->bytes[a]; k++) h = (h ^ p[k]) * 16777619u;
        }
    }
    return h;
}
static void gpu_until(long frame)              // the GPU reads every draw of frames up to this one
{
    int k = 0;
    for (int i = 0; i < g_nq; i++) {
        if (g_q[i].frame <= frame) { fake_result[g_q[i].id] = hash_draw(&g_q[i]); fake_done[g_q[i].id] = 1; }
        else g_q[k++] = g_q[i];
    }
    g_nq = k;
}
void fake_gpu_frame(void) { g_frame++; gpu_until(g_frame - 3); }   // (two frames behind the one just finished)
void fake_gpu_all(void) { gpu_until(0x7fffffffL); }

static GLuint *slot(GLenum t)
{
    switch (t) {
    case 0x8892: return &g_array; case 0x8893: return &g_elem[g_vao]; case 0x8F36: return &g_copyr; case 0x8F37: return &g_copyw;
    case 0x8A11: return &g_uniform; default: return NULL;
    }
}
void glGenBuffers(GLsizei n, GLuint *b) { for (int i = 0; i < n; i++) b[i] = g_next++; }
void glBindBuffer(GLenum t, GLuint b) { GLuint *s = slot(t); if (s) *s = b; }
void glBindBufferBase(GLenum t, GLuint i, GLuint b) { (void)i; glBindBuffer(t, b); }
void glBindBufferRange(GLenum t, GLuint i, GLuint b, GLintptr o, GLsizeiptr n) { (void)i; (void)o; (void)n; glBindBuffer(t, b); }
static void storage(GLenum t, GLsizeiptr size, const void *data)
{
    GLuint b = *slot(t);
    B[b].data = calloc(1, (size_t)size + 64); B[b].size = size;
    if (data) memcpy(B[b].data, data, (size_t)size);
}
void glBufferData(GLenum t, GLsizeiptr size, const void *data, GLenum usage) { (void)usage; fake_calls_bufferdata++; storage(t, size, data); }
void glBufferStorageEXT(GLenum t, GLsizeiptr size, const void *data, GLbitfield f) { (void)f; storage(t, size, data); }
void glBufferSubData(GLenum t, GLintptr o, GLsizeiptr n, const void *data) { memcpy(B[*slot(t)].data + o, data, (size_t)n); }
void *glMapBufferRange(GLenum t, GLintptr o, GLsizeiptr n, GLbitfield a) { (void)n; (void)a; return B[*slot(t)].data + o; }
void glFlushMappedBufferRange(GLenum t, GLintptr o, GLsizeiptr n) { (void)t; (void)o; (void)n; }
GLboolean glUnmapBuffer(GLenum t) { (void)t; return 1; }
void glCopyBufferSubData(GLenum r, GLenum w, GLintptr ro, GLintptr wo, GLsizeiptr n) { memmove(B[*slot(w)].data + wo, B[*slot(r)].data + ro, (size_t)n); }
void glDeleteBuffers(GLsizei n, const GLuint *b)
{
    for (int i = 0; i < n; i++) {
        B[b[i]].dead = 1;                      // (its storage stays: the GPU may still read it, as a driver keeps it)
        GLuint *ss[] = { &g_array, &g_copyr, &g_copyw, &g_uniform, &g_elem[g_vao] };
        for (int k = 0; k < 5; k++) if (*ss[k] == b[i]) *ss[k] = 0;
    }
}
GLboolean glIsBuffer(GLuint b) { return b < NB && B[b].data && !B[b].dead; }
void glGetBufferParameteriv(GLenum t, GLenum p, GLint *v) { *v = p == 0x8764 ? (GLint)B[*slot(t)].size : 0; }
void glGetIntegerv(GLenum p, GLint *v)
{
    switch (p) {
    case 0x8894: *v = (GLint)g_array; break; case 0x8895: *v = (GLint)g_elem[g_vao]; break;
    case 0x8F36: *v = (GLint)g_copyr; break; case 0x8F37: *v = (GLint)g_copyw; break; case 0x8A28: *v = (GLint)g_uniform; break;
    default: *v = 0;
    }
}
const unsigned char *glGetString(GLenum n) { return (const unsigned char *)(n == 0x1F03 ? "GL_OES_x GL_EXT_buffer_storage GL_EXT_y" : "fake"); }
void glVertexAttribPointer(GLuint a, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p)
{
    (void)type; (void)norm;
    A[g_vao][a].buf = g_array; A[g_vao][a].off = (uintptr_t)p; A[g_vao][a].stride = stride; A[g_vao][a].bytes = size;   // (size in bytes: unsigned bytes)
}
void glVertexAttribIPointer(GLuint a, GLint size, GLenum type, GLsizei stride, const void *p) { glVertexAttribPointer(a, size, type, 0, stride, p); }
void glBindVertexArray(GLuint v) { g_vao = v; }
void glDeleteVertexArrays(GLsizei n, const GLuint *v) { for (int i = 0; i < n; i++) if (v[i] == g_vao) g_vao = 0; }
static void queue(int count, const void *indices, int base)
{
    if (g_nq == g_capq) { g_capq = g_capq ? g_capq * 2 : 1024; g_q = realloc(g_q, (size_t)g_capq * sizeof *g_q); }
    qdraw *d = &g_q[g_nq++];
    d->ebuf = B[g_elem[g_vao]].data; d->eoff = (uintptr_t)indices; d->count = count; d->base = base; d->frame = g_frame; d->id = g_id++;
    for (int a = 0; a < 2; a++) { d->vbuf[a] = B[A[g_vao][a].buf].data; d->voff[a] = A[g_vao][a].off; d->stride[a] = A[g_vao][a].stride; d->bytes[a] = A[g_vao][a].bytes; }
}
void glDrawElements(GLenum m, GLsizei n, GLenum t, const void *i) { (void)m; (void)t; queue(n, i, 0); }
void glDrawElementsInstanced(GLenum m, GLsizei n, GLenum t, const void *i, GLsizei k) { (void)k; glDrawElements(m, n, t, i); }
void glDrawRangeElements(GLenum m, GLuint a, GLuint b, GLsizei n, GLenum t, const void *i) { (void)a; (void)b; glDrawElements(m, n, t, i); }
void glDrawElementsBaseVertexOES(GLenum m, GLsizei n, GLenum t, const void *i, GLint v) { (void)m; (void)t; queue(n, i, v); }
void glFinish(void) { fake_gpu_all(); }
GLsync glFenceSync(GLenum c, GLbitfield f) { (void)c; (void)f; return (GLsync)(intptr_t)(g_frame + 1); }   // (after the frame now being made)
GLenum glClientWaitSync(GLsync s, GLbitfield f, uint64_t timeout)
{
    (void)f;
    long fr = (long)(intptr_t)s;                // the GPU is past it once it has read frame fr - 1 ... (fake_gpu_frame's lag)
    if (fr - 1 <= g_frame - 3) return 0x911A;   // GL_ALREADY_SIGNALED
    if (timeout) { gpu_until(fr - 1); return 0x911C; }
    return 0x911B;                              // GL_TIMEOUT_EXPIRED
}
void glDeleteSync(GLsync s) { (void)s; }
