// glarena.c against fake_arena_gl.c (as libGLESv2.so.2): a game that draws like Unity's DynamicVBO
// (a pool of vertex and index buffers, each written through an unsynchronized map or
// glBufferSubData and used again only three frames later, pruned when unused, sized again, copied
// between), its draws checked against what its own copy of its buffers says they read, by a fake GPU
// two frames behind. Run by pointer_test.sh: test_glarena <scenario>
//   arena   UA_GL_ARENA=1: every draw read what the game wrote, through the arena (buffers deleted or sized
//           again also right after a draw that reads them: their ranges must wait for the GPU)
//   small   UA_GL_ARENA_MB=1: the arena fills, the buffers that do not fit made by the driver: the same
//   nosubdata  UA_GL_ARENA_SUBDATA=0: every glBufferSubData to the driver: the same
//   off     unset: no wrappers
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../glarena.h"

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned GLbitfield;
typedef unsigned char GLboolean; typedef intptr_t GLintptr; typedef intptr_t GLsizeiptr;
void fake_gpu_frame(void);
void fake_gpu_all(void);
extern uint32_t fake_result[]; extern int fake_done[];
extern unsigned long fake_calls_bufferdata;

static const so_shim *g_t;
static void *fn(const char *name)
{
    for (const so_shim *s = g_t; s && s->name; s++) if (!strcmp(s->name, name)) return s->fn;
    fprintf(stderr, "FAIL: no wrapper of %s\n", name);
    exit(1);
}
static unsigned g_seed = 777;
static unsigned rnd(unsigned n) { g_seed = g_seed * 1103515245u + 12345u; return (g_seed >> 16) % n; }

#define MAXG 4096
typedef struct { GLuint name; int size; unsigned char *data; long used; int alive; } gbuf;
static gbuf G[MAXG]; static int g_ng;
static uint32_t expect[200000]; static int g_draws;

static void (*BindBuffer)(GLenum, GLuint); static void (*BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void (*BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *); static void *(*MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
static void (*FlushMappedBufferRange)(GLenum, GLintptr, GLsizeiptr); static GLboolean (*UnmapBuffer)(GLenum);
static void (*CopyBufferSubData)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr); static void (*DeleteBuffers)(GLsizei, const GLuint *);
static void (*GetIntegerv)(GLenum, GLint *); static void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
static void (*BindVertexArray)(GLuint); static void (*DrawElements)(GLenum, GLsizei, GLenum, const void *);
static void (*DrawElementsBaseVertexOES)(GLenum, GLsizei, GLenum, const void *, GLint);
static void (*GenBuffers)(GLsizei, GLuint *);
static void (*BindBufferBase)(GLenum, GLuint, GLuint); static void (*BindBufferRange)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
extern unsigned long fake_calls_subdata;
void glarena_subdata_counts(long *copied, long *driver);

// a buffer of at least <need> bytes the GPU is done with (three frames), else a new one
static gbuf *take(GLenum target, int need, long frame)
{
    for (int tries = 0; tries < 8 && g_ng; tries++) {
        gbuf *b = &G[rnd((unsigned)g_ng)];
        if (b->alive && b->used <= frame - 3 && b->size >= need) return b;
    }
    if (g_ng == MAXG) { for (int i = 0; i < g_ng; i++) if (G[i].alive && G[i].used <= frame - 3 && G[i].size >= need) return &G[i]; return NULL; }
    gbuf *b = &G[g_ng++];
    GenBuffers(1, &b->name);
    b->size = need + (int)rnd(4096); b->data = calloc(1, (size_t)b->size); b->alive = 1; b->used = -100;
    BindBuffer(target, b->name);
    BufferData(target, b->size, NULL, rnd(2) ? 0x88E8 : 0x88E0);
    return b;
}
static void write(GLenum target, gbuf *b, int off, const unsigned char *src, int n)
{
    BindBuffer(target, b->name);
    if (rnd(4)) {
        unsigned char *m = MapBufferRange(target, 0, b->size, 0x36);
        memcpy(m + off, src, (size_t)n);
        FlushMappedBufferRange(target, off, n);
        UnmapBuffer(target);
    } else BufferSubData(target, off, n, src);
    memcpy(b->data + off, src, (size_t)n);
}

int main(int argc, char **argv)
{
    const char *sc = argc > 1 ? argv[1] : "arena";
    if (strcmp(sc, "off")) setenv("UA_GL_ARENA", "1", 1);
    if (!strcmp(sc, "small")) setenv("UA_GL_ARENA_MB", "1", 1);
    if (!strcmp(sc, "nosubdata")) setenv("UA_GL_ARENA_SUBDATA", "0", 1);
    g_t = glarena_table(NULL);
    if (!strcmp(sc, "off")) {
        if (g_t[0].name) { fprintf(stderr, "FAIL off: wrappers made\n"); return 1; }
        printf("ok off\n"); return 0;
    }
    BindBuffer = fn("glBindBuffer"); BufferData = fn("glBufferData"); BufferSubData = fn("glBufferSubData");
    MapBufferRange = fn("glMapBufferRange"); FlushMappedBufferRange = fn("glFlushMappedBufferRange"); UnmapBuffer = fn("glUnmapBuffer");
    CopyBufferSubData = fn("glCopyBufferSubData"); DeleteBuffers = fn("glDeleteBuffers"); GetIntegerv = fn("glGetIntegerv");
    VertexAttribPointer = fn("glVertexAttribPointer"); BindVertexArray = fn("glBindVertexArray"); DrawElements = fn("glDrawElements");
    DrawElementsBaseVertexOES = fn("glDrawElementsBaseVertexOES");
    BindBufferBase = fn("glBindBufferBase"); BindBufferRange = fn("glBindBufferRange");
    extern void glGenBuffers(GLsizei, GLuint *);
    GenBuffers = glGenBuffers;              // (not wrapped: names are the driver's)
    BindVertexArray(1);
    unsigned char vbytes[64 * 8], ibytes[96 * 2], ubytes[16384];   // (buffers sized again reach 8.5 KB)
    long deleted = 0, respec = 0, copies = 0, busy = 0;
    gbuf *last_ub = NULL; int last_ul = 0;
    for (long f = 0; f < 1500; f++) {
        int nd = 1 + (int)rnd(30);
        for (int d = 0; d < nd; d++) {
            if (!rnd(40)) BindVertexArray(1 + rnd(2));
            int nv = 4 + (int)rnd(60), ni = 3 * (1 + (int)rnd(32)), base = rnd(3) ? 0 : (int)rnd(4);
            int voff = (int)rnd(64) * 8, ioff = (int)rnd(64) * 2;
            gbuf *vb = take(0x8892, voff + (nv + base) * 8, f), *ib = take(0x8893, ioff + ni * 2, f);
            if (!vb || !ib) continue;
            for (int i = 0; i < (nv + base) * 8; i++) vbytes[i] = (unsigned char)rnd(256);
            for (int i = 0; i < ni; i++) { unsigned short v = (unsigned short)rnd((unsigned)nv); memcpy(ibytes + i * 2, &v, 2); }
            write(0x8892, vb, voff, vbytes, (nv + base) * 8);
            write(0x8893, ib, ioff, ibytes, ni * 2);
            BindBuffer(0x8892, vb->name);
            GLint chk = 0; GetIntegerv(0x8894, &chk);
            if ((GLuint)chk != vb->name) { fprintf(stderr, "FAIL %s: the array buffer binding reads %d, not %u\n", sc, chk, vb->name); return 1; }
            VertexAttribPointer(0, 4, 0x1401, 0, 8, (const void *)(intptr_t)voff);
            VertexAttribPointer(1, 4, 0x1401, 0, 8, (const void *)(intptr_t)(voff + 4));
            BindBuffer(0x8893, ib->name);
            // a constant buffer, as Unity's: a free one from the pool, or (a third of them) the one the last draw
            // read, written again (GL orders it after that draw: the driver's write), uploaded with glBufferSubData
            // through the array target and bound as uniform buffer 0, whole or from an offset
            int ul = 16 * (1 + (int)rnd(24)), uo = rnd(2) ? 0 : 256;
            vb->used = ib->used = f;                        // (not the constant buffer: Unity's are other buffers)
            gbuf *ub = last_ub && last_ub->alive && last_ub != vb && last_ub != ib && rnd(3) == 0 && last_ub->size >= uo + ul
                       ? last_ub : take(0x8892, uo + ul, f);
            if (!ub) continue;
            if (ub == last_ub) busy++;
            int whole = !uo && rnd(2);                     // (bound whole: written whole, GL leaves the rest undefined)
            if (whole) ul = ub->size;
            for (int i = 0; i < ul; i++) ubytes[i] = (unsigned char)rnd(256);
            BindBuffer(0x8892, ub->name);
            BufferSubData(0x8892, uo, ul, ubytes);
            memcpy(ub->data + uo, ubytes, (size_t)ul);
            if (whole) BindBufferBase(0x8A11, 0, ub->name); else BindBufferRange(0x8A11, 0, ub->name, uo, ul);
            BindBuffer(0x8892, vb->name);                  // (the attribute pointers were made with it bound)
            if (base) DrawElementsBaseVertexOES(4, ni, 0x1403, (const void *)(intptr_t)ioff, base);
            else DrawElements(4, ni, 0x1403, (const void *)(intptr_t)ioff);
            // what the draw reads, by the game's own copy
            uint32_t h = 2166136261u;
            for (int i = 0; i < ni; i++) {
                unsigned short v; memcpy(&v, ib->data + ioff + i * 2, 2);
                for (int a = 0; a < 2; a++) for (int k = 0; k < 4; k++) h = (h ^ vb->data[voff + (v + base) * 8 + a * 4 + k]) * 16777619u;
            }
            for (int k = 0; k < ul; k++) h = (h ^ ub->data[uo + k]) * 16777619u;
            expect[g_draws++] = h;
            vb->used = ib->used = ub->used = f;
            last_ub = ub; last_ul = ul;
        }
        // a copy between two buffers the GPU is done with
        if (!rnd(5) && g_ng > 2) {
            gbuf *a = &G[rnd((unsigned)g_ng)], *b = &G[rnd((unsigned)g_ng)];
            if (a != b && a->alive && b->alive && a->used <= f - 3 && b->used <= f - 3) {
                int n = (a->size < b->size ? a->size : b->size) / 2;
                BindBuffer(0x8F36, a->name); BindBuffer(0x8F37, b->name);
                CopyBufferSubData(0x8F36, 0x8F37, 0, n / 2, n / 2);
                memmove(b->data + n / 2, a->data, (size_t)(n / 2));
                b->used = f; copies++;
            }
        }
        // sized again, pruned
        for (int k = 0; k < 3; k++) {
            if (!g_ng) break;
            gbuf *b = &G[rnd((unsigned)g_ng)];
            if (!b->alive || b->used > f - (long)rnd(8)) continue;   // (also one drawn this frame: GL keeps its storage for the GPU)
            if (rnd(3)) { DeleteBuffers(1, &b->name); b->alive = 0; deleted++; }
            else {
                int n = 512 + (int)rnd(8192);
                BindBuffer(0x8892, b->name); BufferData(0x8892, n, NULL, 0x88E8);
                free(b->data); b->data = calloc(1, (size_t)n); b->size = n; respec++;
            }
        }
        glarena_frame();
        fake_gpu_frame();
    }
    fake_gpu_all();
    for (int i = 0; i < g_draws; i++)
        if (!fake_done[i] || fake_result[i] != expect[i]) {
            fprintf(stderr, "FAIL %s: draw %d read other bytes than the game wrote (of %d)\n", sc, i, g_draws);
            return 1;
        }
    if (!strcmp(sc, "small") && !fake_calls_bufferdata) { fprintf(stderr, "FAIL small: the arena never filled\n"); return 1; }
    long sub_copied, sub_driver;
    glarena_subdata_counts(&sub_copied, &sub_driver);
    if (!strcmp(sc, "arena") && (sub_copied == 0 || sub_driver == 0)) {
        fprintf(stderr, "FAIL arena: glBufferSubData %ld copied, %ld to the driver: both paths must be taken\n", sub_copied, sub_driver);
        return 1;
    }
    if (!strcmp(sc, "nosubdata") && sub_copied) { fprintf(stderr, "FAIL nosubdata: %ld copied\n", sub_copied); return 1; }
    printf("ok %s: %d draws read what the game wrote (%d buffers, %ld deleted, %ld sized again, %ld copies, %lu outside the arena; "
           "glBufferSubData %ld copied, %ld to the driver, %ld into a buffer the last draw read)\n",
           sc, g_draws, g_ng, deleted, respec, copies, fake_calls_bufferdata, sub_copied, sub_driver, busy);
    (void)last_ul;
    return 0;
}
