// Unity's dynamic geometry in one persistently mapped buffer (UA_GL_ARENA=1; off by default).
//
// The Mali-G31's driver does a draw's work in the draw call: about 30 us for a sprite chunk on the
// RG34XX-SP's A53, part of it for each buffer object the draw reads (drawbench: 36.2 us a draw with
// a vertex and an index buffer of its own, 33.5 with both at offsets of one buffer). Unity's
// DynamicVBO gives every chunk a vertex and an index buffer of its pool. This layer keeps Unity's
// buffers for dynamic geometry (glBufferData with GL_DYNAMIC_DRAW or GL_STREAM_DRAW on the vertex or
// index target, as glpersist.c takes) as ranges of one arena: a buffer with persistent, coherent
// storage (GL_EXT_buffer_storage), mapped once. The game keeps its buffer names; the layer binds
// the arena wherever the game binds one of them and moves what points into them by the range's
// offset:
//   glBindBuffer, glBindBufferBase/Range     the arena (a range of it) bound instead
//   glVertexAttribPointer, glVertexAttribIPointer   the pointer + the offset of the bound buffer
//   glDrawElements and its Range, Instanced and BaseVertex forms   the indices + the offset of the
//                                            vertex array's index buffer
//   glMapBufferRange                         the arena's mapping + offset (flush and unmap: nothing,
//                                            coherent); a map that is not unsynchronized waits; one that
//                                            invalidates a buffer the GPU may still read gets a new range
//                                            (below)
//   glBufferSubData                          a memcpy into the mapping when the GPU is past every draw
//                                            and copy that used the buffer (below), else the offset moved
//   glCopyBufferSubData                      the offsets moved
//   glBufferData on such a buffer again      a new range (the old one freed as below; no room: the
//                                            buffer leaves the arena for the driver's storage)
//   glDeleteBuffers                          the range freed once the GPU is past the frame it was
//                                            deleted in (a fence a frame, from the swap hook)
//   glGetIntegerv (buffer bindings), glGetBufferParameteriv, glIsBuffer   the game's view
// The game writes the same bytes and the GPU reads the same bytes in the same order: the picture is
// the same. What the layer does not move (indirect draws, transform feedback, pixel transfers,
// shader storage or a texture buffer reading one of these buffers) is logged
// ("[glarena] ... not moved") and turns the arena off for buffers made after it. A full arena: the
// buffer is made as before (glpersist.c's, the driver's). Logged: "[glarena] ..." at the start and
// every 10 s (buffers in the arena, its use, the ranges waiting for the GPU).
// UA_GL_ARENA_MB=<n>: the arena's size (default 8).
//
// Unity maps each chunk unsynchronized with GL_MAP_INVALIDATE_RANGE_BIT over the whole buffer, and
// takes a buffer back from its pool for the next chunks without waiting for the GPU to have drawn
// the last ones: the driver gives an invalidated buffer that the GPU still reads new storage (it
// "orphans" it). Here, too: such a map of a buffer last used (by a draw, a dispatch or a copy) in a
// frame the GPU is not past moves the buffer to a new range of the arena (a partial invalidation
// copies the rest of the old range over), the old range freed once the GPU is past this frame; the
// vertex attributes that point into it, in every vertex array, and the uniform buffer bindings to it
// are pointed at the new range (Unity sets them once and binds them again only when they change). A
// buffer sized again (a new range) or leaving a full arena (the driver's storage) is pointed at the
// same way. Logged as "moved by the game's invalidations", "sized again", "uniform bindings moved". More
// vertex arrays, attributes or uniform bindings than the layer follows: logged, and buffers stay where
// they are (an invalidating map waits for the GPU instead).
//
// Draws from an index buffer in the arena go out as glDrawRangeElements (below). The Mali driver reads a
// draw's indices for the vertices it uses and keeps that range per index buffer, refreshed only by
// writes through GL; the game's writes into the arena's mapping never reach it. Without the ranges the
// RG34XX-SP drew Hollow Knight with stale ones (2026-11-06: sprites stretched into black slabs, HUD
// masks missing; not the GPU's timing: waiting for the GPU before every write changed nothing).
//
// glBufferSubData without the driver. Unity uploads each constant buffer a draw changes with
// glBufferSubData (one ~100 - 400 byte update per draw in Hollow Knight); on the arena the Mali
// driver has to order the write after the queued draws that read other ranges of the same buffer
// object, at several us a call. The layer notes, for each of its buffers, the frame of the last
// draw (or dispatch) that could read it (its element buffer, the vertex attributes pointing into it,
// the uniform buffer bindings when the draw is made) and of the last buffer copy that read or wrote
// it, and the last frame a fence shows the GPU is past (the fences made at the swaps); a
// glBufferSubData into a buffer last used before that frame is a memcpy into the coherent mapping:
// nothing queued can read it, and the commands made after it see it. Any other goes to the driver as
// before. Once anything the layer does not follow reads a buffer (the "not moved" cases), every
// glBufferSubData goes to the driver. UA_GL_ARENA_SUBDATA=1 turns this on (off by default).
#include "glarena.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned GLbitfield;
typedef unsigned char GLboolean; typedef intptr_t GLintptr; typedef intptr_t GLsizeiptr; typedef void *GLsync;

#define GL_EXTENSIONS 0x1F03
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_UNIFORM_BUFFER_BINDING 0x8A28
#define GL_COPY_READ_BUFFER 0x8F36
#define GL_COPY_WRITE_BUFFER 0x8F37
#define GL_BUFFER_SIZE 0x8764
#define GL_BUFFER_USAGE 0x8765
#define GL_BUFFER_MAPPED 0x88BC
#define GL_STREAM_DRAW 0x88E0
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_RANGE_BIT 0x0004
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#define GL_MAP_PERSISTENT_BIT_EXT 0x0040
#define GL_MAP_COHERENT_BIT_EXT 0x0080
#define GL_DYNAMIC_STORAGE_BIT_EXT 0x0100
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_ALREADY_SIGNALED 0x911A
#define GL_CONDITION_SATISFIED 0x911C
#define ALIGN 256u                           // (any uniform buffer offset alignment)

static so_shim g_shims[40];
static int g_on, g_ok = -1, g_closed;       // g_ok: the extension (-1: not checked yet); g_closed: no new buffers
static int g_copy;                          // glBufferSubData by memcpy where the GPU is past the buffer (UA_GL_ARENA_SUBDATA=1)
static long g_done;                         // the GPU is past every command made while g_frames < g_done (fences)
static long g_subcopy, g_subdriver;
static int g_track = 1;                     // every use of these buffers followed (0: one is not; every invalidating map moves)
static int g_fixed;                         // something points into these buffers that the layer cannot point elsewhere: buffers stay put
static long g_orphans, g_orphan_waits, g_resized, g_rebound;
static int g_shadow = 1;                    // glBufferSubData into a buffer the GPU may still read: a new range (UA_GL_ARENA_SHADOW=0: the driver's)
static long g_subshadow;
static int g_ib = 1;                        // UA_GL_ARENA_IB=0: index buffers stay the driver's
static int g_range = 1;                     // draws from an index buffer here as glDrawRangeElements (below; UA_GL_ARENA_RANGE=0: not)
static long g_ranged, g_unranged;
static int g_sync;                          // UA_GL_ARENA_SYNC=1 (a check): a write into a buffer the GPU may still read waits for the GPU, nothing moves
static long g_mapk[2][2][2];                // maps by [unsynchronized][invalidating][the GPU may still read the buffer]
static struct { GLbitfield a; long n; } g_acc[8];   // the access bits the game maps with
static GLuint g_A;                          // the arena
static char *g_ptr;
static unsigned g_size;
static long g_in, g_moves, g_full, g_frames;
static double g_last_log;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

// ---- the game's buffers in the arena: name -> range
typedef struct { GLuint name; unsigned off, size, len; GLenum usage; long used; } vbuf;   // len: as the game sized it; used: g_frames at the last draw or copy that could read or write it
static vbuf *g_tab;
static size_t g_cap, g_n;
static vbuf *find(GLuint name)
{
    if (!name || !g_cap) return NULL;
    size_t i = name & (g_cap - 1);
    while (g_tab[i].name) { if (g_tab[i].name == name) return &g_tab[i]; i = (i + 1) & (g_cap - 1); }
    return NULL;
}
static vbuf *add(GLuint name)
{
    if ((g_n + 1) * 2 > g_cap) {
        size_t cap = g_cap ? g_cap * 2 : 1024;
        vbuf *t = calloc(cap, sizeof *t);
        if (!t) return NULL;
        for (size_t i = 0; i < g_cap; i++)
            if (g_tab[i].name) { size_t j = g_tab[i].name & (cap - 1); while (t[j].name) j = (j + 1) & (cap - 1); t[j] = g_tab[i]; }
        free(g_tab); g_tab = t; g_cap = cap;
    }
    size_t i = name & (g_cap - 1);
    while (g_tab[i].name && g_tab[i].name != name) i = (i + 1) & (g_cap - 1);
    if (!g_tab[i].name) g_n++;
    g_tab[i].name = name;
    return &g_tab[i];
}
static void drop(GLuint name)
{
    vbuf *p = find(name);
    if (!p) return;
    size_t i = (size_t)(p - g_tab);
    p->name = 0; g_n--;
    for (size_t j = (i + 1) & (g_cap - 1); g_tab[j].name; j = (j + 1) & (g_cap - 1)) {
        vbuf e = g_tab[j]; g_tab[j].name = 0; g_n--;
        *add(e.name) = e;
    }
}

// ---- the arena's free ranges (sorted by offset, neighbours merged) and the ranges the GPU may still read
typedef struct { unsigned off, size; } range;
static range *g_free; static size_t g_nfree, g_capfree;
static unsigned g_used;
static int alloc(unsigned size, unsigned *off)
{
    size = (size + ALIGN - 1) & ~(ALIGN - 1);
    for (size_t i = 0; i < g_nfree; i++)
        if (g_free[i].size >= size) {
            *off = g_free[i].off;
            g_free[i].off += size; g_free[i].size -= size;
            if (!g_free[i].size) { memmove(&g_free[i], &g_free[i + 1], (g_nfree - i - 1) * sizeof *g_free); g_nfree--; }
            g_used += size;
            return 1;
        }
    return 0;
}
static void release(unsigned off, unsigned size)
{
    size = (size + ALIGN - 1) & ~(ALIGN - 1);
    g_used -= size;
    size_t i = 0;
    while (i < g_nfree && g_free[i].off < off) i++;
    int left = i > 0 && g_free[i - 1].off + g_free[i - 1].size == off, right = i < g_nfree && off + size == g_free[i].off;
    if (left && right) { g_free[i - 1].size += size + g_free[i].size; memmove(&g_free[i], &g_free[i + 1], (g_nfree - i - 1) * sizeof *g_free); g_nfree--; }
    else if (left) g_free[i - 1].size += size;
    else if (right) { g_free[i].off = off; g_free[i].size += size; }
    else {
        if (g_nfree == g_capfree) {
            size_t c = g_capfree ? g_capfree * 2 : 256;
            range *t = realloc(g_free, c * sizeof *t);
            if (!t) return;                 // (the range is lost to the arena: nothing worse)
            g_free = t; g_capfree = c;
        }
        memmove(&g_free[i + 1], &g_free[i], (g_nfree - i) * sizeof *g_free);
        g_free[i] = (range){ off, size }; g_nfree++;
    }
}
// freed while g_frames was f: reused once a fence made after it (f + 1 or later, at the swaps) has signalled
typedef struct { unsigned off, size; long frame; } pending;
static pending *g_pend; static size_t g_npend, g_cappend;
static void defer(unsigned off, unsigned size)
{
    if (g_npend == g_cappend) {
        size_t c = g_cappend ? g_cappend * 2 : 256;
        pending *t = realloc(g_pend, c * sizeof *t);
        if (!t) return;
        g_pend = t; g_cappend = c;
    }
    g_pend[g_npend++] = (pending){ off, size, g_frames };
}
#define FENCES 8
static GLsync g_fence[FENCES]; static long g_fence_frame[FENCES];

// ---- what the game has bound (0: none of its arena buffers; the arena is bound in the driver then)
static GLuint vb_array, vb_copyr, vb_copyw, vb_uniform;
static GLuint g_vao;
#define VAOS 64
static struct { GLuint vao, elem; } g_velem[VAOS];   // a vertex array's index buffer, when one of these
#define ATTRS 16
static struct { GLuint vao, attr[ATTRS]; int used; } g_vattr[VAOS];   // a vertex array's attributes' buffers, when these
typedef struct { GLint size; GLenum type; GLboolean norm, integer; GLsizei stride; const void *ptr; } attrfmt;
static attrfmt g_vfmt[VAOS][ATTRS];        // ... and how each points into it (the game's pointer: an offset into its buffer)
#define UBOS 96
static GLuint g_ubo[UBOS]; static unsigned g_nubo;  // the uniform buffer bindings to these buffers (0: another)
static GLintptr g_ubo_o[UBOS]; static GLsizeiptr g_ubo_n[UBOS];   // ... their ranges (n -1: the whole buffer)
static void unfollowed(const char *what)
{
    if (!g_fixed) fprintf(stderr, "[glarena] %s: more than the layer follows; an invalidated buffer the GPU may still read waits for it\n", what);
    g_fixed = 1; g_copy = 0; g_track = 0;
}
// (the last vertex array looked up in each table: a game makes many calls with one bound)
static int g_elast, g_alast;
static GLuint *elem_slot(GLuint vao, int make)
{
    if (g_velem[g_elast].vao == vao && (g_velem[g_elast].elem || make)) return &g_velem[g_elast].elem;
    for (int i = 0; i < VAOS; i++) if (g_velem[i].vao == vao && (g_velem[i].elem || make)) { g_elast = i; return &g_velem[i].elem; }
    if (!make) return NULL;
    for (int i = 0; i < VAOS; i++) if (!g_velem[i].elem) { g_velem[i].vao = vao; g_elast = i; return &g_velem[i].elem; }
    return NULL;
}
static GLuint *bound_slot(GLenum t)
{
    switch (t) {
    case GL_ARRAY_BUFFER: return &vb_array;
    case GL_COPY_READ_BUFFER: return &vb_copyr;
    case GL_COPY_WRITE_BUFFER: return &vb_copyw;
    case GL_UNIFORM_BUFFER: return &vb_uniform;
    case GL_ELEMENT_ARRAY_BUFFER: { static GLuint none; GLuint *s = elem_slot(g_vao, 0); none = 0; return s ? s : &none; }
    default: return NULL;
    }
}
static vbuf *bound(GLenum t) { GLuint *s = bound_slot(t); return s && *s ? find(*s) : NULL; }
static int *attr_used(GLuint vao, GLuint **attr, int make)
{
    if (g_vattr[g_alast].used && g_vattr[g_alast].vao == vao) { *attr = g_vattr[g_alast].attr; return &g_vattr[g_alast].used; }
    for (int i = 0; i < VAOS; i++) if (g_vattr[i].used && g_vattr[i].vao == vao) { g_alast = i; *attr = g_vattr[i].attr; return &g_vattr[i].used; }
    if (!make) return NULL;
    for (int i = 0; i < VAOS; i++)
        if (!g_vattr[i].used) {
            g_vattr[i].vao = vao; g_vattr[i].used = 1; memset(g_vattr[i].attr, 0, sizeof g_vattr[i].attr);
            g_alast = i; *attr = g_vattr[i].attr; return &g_vattr[i].used;
        }
    unfollowed("vertex arrays");
    return NULL;
}
static inline void use(GLuint name) { vbuf *p = find(name); if (p) p->used = g_frames; }
// a draw or dispatch: every buffer of these it could read is used in this frame
static void draw_uses(void)
{
    if (!g_track || g_ok <= 0) return;
    GLuint *e = elem_slot(g_vao, 0), *attr;
    if (e && *e) use(*e);
    if (attr_used(g_vao, &attr, 0)) for (int a = 0; a < ATTRS; a++) if (attr[a]) use(attr[a]);
    for (unsigned i = 0; i < g_nubo; i++) if (g_ubo[i]) use(g_ubo[i]);
}

// ---- the next layer's functions, and the driver's for the layer's own use
static void (*r_BindBuffer)(GLenum, GLuint);
static void (*r_BindBufferBase)(GLenum, GLuint, GLuint);
static void (*r_BindBufferRange)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
static void (*r_BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void (*r_BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
static void *(*r_MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
static void (*r_FlushMappedBufferRange)(GLenum, GLintptr, GLsizeiptr);
static GLboolean (*r_UnmapBuffer)(GLenum);
static void (*r_CopyBufferSubData)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr);
static void (*r_DeleteBuffers)(GLsizei, const GLuint *);
static GLboolean (*r_IsBuffer)(GLuint);
static void (*r_GetBufferParameteriv)(GLenum, GLenum, GLint *);
static void (*r_GetIntegerv)(GLenum, GLint *);
static void (*r_VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
static void (*r_VertexAttribIPointer)(GLuint, GLint, GLenum, GLsizei, const void *);
static void (*r_BindVertexArray)(GLuint);
static void (*r_DeleteVertexArrays)(GLsizei, const GLuint *);
static void (*r_DrawElements)(GLenum, GLsizei, GLenum, const void *);
static void (*r_DrawElementsInstanced)(GLenum, GLsizei, GLenum, const void *, GLsizei);
static void (*r_DrawRangeElements)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
static void (*r_DrawElementsBaseVertex)(GLenum, GLsizei, GLenum, const void *, GLint);
static void (*r_DrawElementsBaseVertexOES)(GLenum, GLsizei, GLenum, const void *, GLint);
static void (*r_DrawElementsBaseVertexEXT)(GLenum, GLsizei, GLenum, const void *, GLint);
static void (*r_DrawRangeElementsBaseVertex)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *, GLint);
static void (*r_DrawRangeElementsBaseVertexOES)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *, GLint);
static void (*r_DrawElementsInstancedBaseVertex)(GLenum, GLsizei, GLenum, const void *, GLsizei, GLint);
static void (*r_DrawElementsInstancedBaseVertexOES)(GLenum, GLsizei, GLenum, const void *, GLsizei, GLint);
static void (*q_BufferStorageEXT)(GLenum, GLsizeiptr, const void *, GLbitfield);
static void (*q_GenBuffers)(GLsizei, GLuint *);
static void (*q_BindBuffer)(GLenum, GLuint);
static void *(*q_MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
static const unsigned char *(*q_GetString)(GLenum);
static void (*q_GetIntegerv)(GLenum, GLint *);
static void (*q_Finish)(void);
static GLsync (*q_FenceSync)(GLenum, GLbitfield);
static GLenum (*q_ClientWaitSync)(GLsync, GLbitfield, uint64_t);
static void (*q_DeleteSync)(GLsync);

static void not_moved(const char *what)
{
    static int n;
    if (n++ < 10) fprintf(stderr, "[glarena] %s reads a buffer in the arena: not moved; no more buffers go into the arena\n", what);
    g_closed = 1;
    g_copy = 0; g_track = 0;                // (a use the layer does not follow: every glBufferSubData to the driver)
}

// the arena, made at the first buffer the game sizes (a context is current then)
static int open_arena(GLenum target)
{
    if (g_ok >= 0) return g_ok;
    const char *e = q_GetString ? (const char *)q_GetString(GL_EXTENSIONS) : NULL;
    g_ok = 0;
    if (!e || !strstr(e, "GL_EXT_buffer_storage") || !q_BufferStorageEXT) {
        fprintf(stderr, "[glarena] the driver has no GL_EXT_buffer_storage: off\n");
        return 0;
    }
    GLint prev = 0;
    q_GetIntegerv(target == GL_ELEMENT_ARRAY_BUFFER ? GL_ELEMENT_ARRAY_BUFFER_BINDING : GL_ARRAY_BUFFER_BINDING, &prev);
    q_GenBuffers(1, &g_A);
    q_BindBuffer(target, g_A);
    q_BufferStorageEXT(target, g_size, NULL, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT | GL_DYNAMIC_STORAGE_BIT_EXT);
    g_ptr = q_MapBufferRange(target, 0, g_size, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT);
    q_BindBuffer(target, (GLuint)prev);
    if (!g_ptr) { fprintf(stderr, "[glarena] the arena could not be mapped: off\n"); return 0; }
    g_free = malloc(256 * sizeof *g_free); g_capfree = 256;
    if (!g_free) return 0;
    g_free[0] = (range){ 0, g_size }; g_nfree = 1;
    fprintf(stderr, "[glarena] Unity's dynamic geometry in one %u MB persistently mapped buffer (UA_GL_ARENA)\n", g_size >> 20);
    g_last_log = now();
    return g_ok = 1;
}

// ---- the wrappers
static void w_BindBuffer(GLenum t, GLuint name)
{
    GLuint *s = t == GL_ELEMENT_ARRAY_BUFFER ? elem_slot(g_vao, 1) : bound_slot(t);
    vbuf *p = g_ok > 0 ? find(name) : NULL;
    if (s) *s = p ? name : 0;
    if (p && !s) not_moved("a binding of this target");
    r_BindBuffer(t, p ? g_A : name);
}
static void ubo_bind(GLuint i, GLuint name, int arena, GLintptr o, GLsizeiptr n)
{
    if (i < UBOS) { g_ubo[i] = arena ? name : 0; g_ubo_o[i] = o; g_ubo_n[i] = n; if (arena && i >= g_nubo) g_nubo = i + 1; }
    else if (arena) unfollowed("uniform buffer bindings");
}
static void w_BindBufferBase(GLenum t, GLuint i, GLuint name)
{
    vbuf *p = g_ok > 0 ? find(name) : NULL;
    if (t == GL_UNIFORM_BUFFER) { vb_uniform = p ? name : 0; ubo_bind(i, name, p != NULL, 0, -1); }
    if (!p) { r_BindBufferBase(t, i, name); return; }
    if (t != GL_UNIFORM_BUFFER) { not_moved("an indexed binding"); r_BindBufferBase(t, i, name); return; }
    r_BindBufferRange(t, i, g_A, p->off, p->len);
}
static void w_BindBufferRange(GLenum t, GLuint i, GLuint name, GLintptr o, GLsizeiptr n)
{
    vbuf *p = g_ok > 0 ? find(name) : NULL;
    if (t == GL_UNIFORM_BUFFER) { vb_uniform = p ? name : 0; ubo_bind(i, name, p != NULL, o, n); }
    if (!p) { r_BindBufferRange(t, i, name, o, n); return; }
    if (t != GL_UNIFORM_BUFFER) { not_moved("an indexed binding"); r_BindBufferRange(t, i, name, o, n); return; }
    r_BindBufferRange(t, i, g_A, (GLintptr)p->off + o, n);
}

static void repoint(GLuint name, GLuint buf, unsigned off, unsigned len);
static void w_BufferData(GLenum t, GLsizeiptr size, const void *data, GLenum usage)
{
    GLuint *s = bound_slot(t);
    vbuf *p = s && *s ? find(*s) : NULL;
    if (p) {                                // one of these sized again: a new range, the old one freed after the GPU
        unsigned off;
        g_resized++;
        if (g_fixed && size > 0 && (unsigned)size <= p->size) {   // (it cannot move: in place, once the GPU is past it)
            q_Finish();
            p->len = (unsigned)size; p->usage = usage; p->used = 0;
            if (data) memcpy(g_ptr + p->off, data, (size_t)size);
            return;
        }
        if (size > 0 && alloc((unsigned)size, &off)) {
            defer(p->off, p->size);
            p->off = off; p->size = (unsigned)size; p->len = (unsigned)size; p->usage = usage; p->used = 0;
            if (data) memcpy(g_ptr + off, data, (size_t)size);
            repoint(*s, g_A, off, p->len);
            return;
        }
        // no room: the buffer leaves the arena and gets the driver's storage under its own name
        GLuint name = *s;
        defer(p->off, p->size);
        drop(name);
        *s = 0;
        g_full++;
        r_BindBuffer(t, name);
        r_BufferData(t, size, data, usage);
        repoint(name, name, 0, (unsigned)size);
        return;
    }
    if (g_on && !g_closed && (t == GL_ARRAY_BUFFER || (t == GL_ELEMENT_ARRAY_BUFFER && g_ib)) && size > 0 &&
        (usage == GL_DYNAMIC_DRAW || usage == GL_STREAM_DRAW) && open_arena(t)) {
        GLint name = 0;
        q_GetIntegerv(t == GL_ARRAY_BUFFER ? GL_ARRAY_BUFFER_BINDING : GL_ELEMENT_ARRAY_BUFFER_BINDING, &name);
        unsigned off;
        if (name && (GLuint)name != g_A) {
            if (alloc((unsigned)size, &off)) {
                vbuf *v = add((GLuint)name);
                if (v) {
                    v->off = off; v->size = (unsigned)size; v->len = (unsigned)size; v->usage = usage; v->used = 0;
                    if (data) memcpy(g_ptr + off, data, (size_t)size);
                    GLuint *slot = t == GL_ELEMENT_ARRAY_BUFFER ? elem_slot(g_vao, 1) : s;
                    if (slot) *slot = (GLuint)name;
                    r_BindBuffer(t, g_A);
                    g_in++;
                    return;
                }
                release(off, (unsigned)size);
            }
            g_full++;
        }
    }
    r_BufferData(t, size, data, usage);
}
static void w_BufferSubData(GLenum t, GLintptr o, GLsizeiptr n, const void *data)
{
    vbuf *p = g_ok > 0 ? bound(t) : NULL;
    int fits = p && data && o >= 0 && n >= 0 && (unsigned long)o + (unsigned long)n <= p->len;
    if (fits && g_sync) {
        if (!(g_track && p->used < g_done) || p->used >= g_frames) q_Finish();
        memcpy(g_ptr + p->off + o, data, (size_t)n);
        g_subcopy++;
        return;
    }
    if (fits && g_copy && p->used < g_done) {
        memcpy(g_ptr + p->off + o, data, (size_t)n);  // (nothing queued reads it; coherent: later commands see it)
        g_subcopy++;
        return;
    }
    unsigned off;
    if (fits && g_shadow && !(g_track && p->used < g_done) && !g_fixed && alloc(p->size, &off)) {
        // the GPU may still read it: a new range with the old bytes and these, the old one kept for the queued
        // draws (as the driver shadows a buffer in use; on the arena's persistent storage it would write in place)
        memcpy(g_ptr + off, g_ptr + p->off, p->len);
        memcpy(g_ptr + off + o, data, (size_t)n);
        defer(p->off, p->size);
        p->off = off;
        p->used = 0;
        GLuint *s = bound_slot(t);
        if (s && *s) repoint(*s, g_A, off, p->len);
        g_subshadow++;
        return;
    }
    if (p) g_subdriver++;
    r_BufferSubData(t, p ? (GLintptr)p->off + o : o, n, data);
}
static void *w_MapBufferRange(GLenum t, GLintptr o, GLsizeiptr n, GLbitfield access)
{
    vbuf *p = g_ok > 0 ? bound(t) : NULL;
    if (!p) return r_MapBufferRange(t, o, n, access);
    if (o < 0 || n < 0 || (unsigned)(o + n) > p->len) return NULL;
    int inval = (access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT)) != 0, busy = !(g_track && p->used < g_done);
    g_mapk[(access & GL_MAP_UNSYNCHRONIZED_BIT) != 0][inval][busy]++;
    for (int k = 0; k < 8; k++) if (g_acc[k].a == access || !g_acc[k].n) { g_acc[k].a = access; g_acc[k].n++; break; }
    if (g_sync) {
        if (busy || p->used >= g_frames) { q_Finish(); g_orphan_waits++; }
        g_moves++;
        return g_ptr + p->off + o;
    }
    if (!(access & GL_MAP_UNSYNCHRONIZED_BIT)) q_Finish();   // the game asked the driver to wait for the GPU
    else if (inval && busy) {
        // invalidated while the GPU may still read it: new storage, as the driver gives
        unsigned off;
        if (!g_fixed && alloc(p->size, &off)) {
            int whole = (access & GL_MAP_INVALIDATE_BUFFER_BIT) || (o == 0 && (unsigned)n >= p->len);
            if (!whole) {                   // (what is not invalidated keeps its bytes)
                if (o > 0) memcpy(g_ptr + off, g_ptr + p->off, (size_t)o);
                if ((unsigned)(o + n) < p->len) memcpy(g_ptr + off + o + n, g_ptr + p->off + o + n, p->len - (unsigned)(o + n));
            }
            defer(p->off, p->size);
            p->off = off;
            p->used = 0;
            GLuint *s = bound_slot(t);
            if (s && *s) repoint(*s, g_A, off, p->len);
            g_orphans++;
        } else { q_Finish(); g_orphan_waits++; }   // (no room, or it cannot move: wait for the GPU instead)
    }
    g_moves++;
    return g_ptr + p->off + o;
}
static void w_FlushMappedBufferRange(GLenum t, GLintptr o, GLsizeiptr n)
{
    if (g_ok > 0 && bound(t)) return;       // coherent
    r_FlushMappedBufferRange(t, o, n);
}
static GLboolean w_UnmapBuffer(GLenum t)
{
    if (g_ok > 0 && bound(t)) return 1;
    return r_UnmapBuffer(t);
}
static void w_CopyBufferSubData(GLenum rt, GLenum wt, GLintptr ro, GLintptr wo, GLsizeiptr n)
{
    vbuf *r = g_ok > 0 ? bound(rt) : NULL, *w = g_ok > 0 ? bound(wt) : NULL;
    if (r) r->used = g_frames;              // (the GPU reads one and writes the other later)
    if (w) w->used = g_frames;
    r_CopyBufferSubData(rt, wt, r ? (GLintptr)r->off + ro : ro, w ? (GLintptr)w->off + wo : wo, n);
}
static void w_DeleteBuffers(GLsizei n, const GLuint *names)
{
    if (g_ok > 0)
        for (GLsizei i = 0; i < n; i++) {
            vbuf *p = find(names[i]);
            if (!p) continue;
            // (a deleted buffer is unbound from where it is bound: the arena there too)
            static const GLenum ts[] = { GL_ARRAY_BUFFER, GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, GL_UNIFORM_BUFFER, GL_ELEMENT_ARRAY_BUFFER };
            for (int k = 0; k < 5; k++) { GLuint *s = bound_slot(ts[k]); if (s && *s == names[i]) { *s = 0; r_BindBuffer(ts[k], 0); } }
            for (int k = 0; k < VAOS; k++) if (g_velem[k].elem == names[i]) g_velem[k].elem = 0;   // (another vertex array's: the game binds again)
            for (int k = 0; k < VAOS; k++) for (int a = 0; a < ATTRS; a++) if (g_vattr[k].attr[a] == names[i]) g_vattr[k].attr[a] = 0;
            for (unsigned k = 0; k < g_nubo; k++) if (g_ubo[k] == names[i]) g_ubo[k] = 0;
            defer(p->off, p->size);
            drop(names[i]);
        }
    r_DeleteBuffers(n, names);
}
static GLboolean w_IsBuffer(GLuint name) { return g_ok > 0 && find(name) ? 1 : r_IsBuffer(name); }
static void w_GetBufferParameteriv(GLenum t, GLenum pname, GLint *v)
{
    vbuf *p = g_ok > 0 ? bound(t) : NULL;
    if (p && pname == GL_BUFFER_SIZE) { *v = (GLint)p->len; return; }
    if (p && pname == GL_BUFFER_USAGE) { *v = (GLint)p->usage; return; }
    if (p && pname == GL_BUFFER_MAPPED) { *v = 0; return; }
    r_GetBufferParameteriv(t, pname, v);
}
static void w_GetIntegerv(GLenum pname, GLint *v)
{
    r_GetIntegerv(pname, v);
    if (g_ok <= 0 || !v || (GLuint)*v != g_A) return;
    GLenum t = pname == GL_ARRAY_BUFFER_BINDING ? GL_ARRAY_BUFFER : pname == GL_ELEMENT_ARRAY_BUFFER_BINDING ? GL_ELEMENT_ARRAY_BUFFER :
               pname == 0x8F36 ? GL_COPY_READ_BUFFER : pname == 0x8F37 ? GL_COPY_WRITE_BUFFER :     // (the binding enums equal the targets)
               pname == GL_UNIFORM_BUFFER_BINDING ? GL_UNIFORM_BUFFER : 0;
    GLuint *s = t ? bound_slot(t) : NULL;
    if (s) *v = (GLint)*s;
}
static void attr_set(GLuint a, GLuint name, attrfmt f)
{
    GLuint *attr;
    if (a >= ATTRS) { if (name) unfollowed("vertex attributes"); return; }
    if (attr_used(g_vao, &attr, name != 0)) {
        attr[a] = name;
        g_vfmt[g_alast][a] = f;              // (attr_used left g_alast at its entry)
    }
}
static void w_VertexAttribPointer(GLuint a, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr)
{
    vbuf *p = g_ok > 0 && vb_array ? find(vb_array) : NULL;
    if (g_ok > 0) attr_set(a, p ? vb_array : 0, (attrfmt){ size, type, norm, 0, stride, ptr });
    r_VertexAttribPointer(a, size, type, norm, stride, p ? (const char *)ptr + p->off : ptr);
}
static void w_VertexAttribIPointer(GLuint a, GLint size, GLenum type, GLsizei stride, const void *ptr)
{
    vbuf *p = g_ok > 0 && vb_array ? find(vb_array) : NULL;
    if (g_ok > 0) attr_set(a, p ? vb_array : 0, (attrfmt){ size, type, 0, 1, stride, ptr });
    r_VertexAttribIPointer(a, size, type, stride, p ? (const char *)ptr + p->off : ptr);
}
// a buffer moved to another range (buf g_A, at off) or out of the arena (buf its own name, at 0): the
// vertex attributes pointing into it, in every vertex array, and the uniform buffer bindings to it
// pointed at the new storage; the game's vertex array, vertex buffer and uniform buffer put back. (The
// game sets these once and binds them again by name: what pointed at the old range would read the
// next buffer put there.)
static void repoint(GLuint name, GLuint buf, unsigned off, unsigned len)
{
    GLuint cur = g_vao;
    GLint prev = -1;
    for (int i = 0; i < VAOS; i++) {
        if (!g_vattr[i].used) continue;
        for (int a = 0; a < ATTRS; a++) {
            if (g_vattr[i].attr[a] != name) continue;
            if (g_vattr[i].vao != cur) { cur = g_vattr[i].vao; r_BindVertexArray(cur); }
            if (prev < 0) { r_GetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev); r_BindBuffer(GL_ARRAY_BUFFER, buf); }
            attrfmt *f = &g_vfmt[i][a];
            if (f->integer) r_VertexAttribIPointer((GLuint)a, f->size, f->type, f->stride, (const char *)f->ptr + off);
            else r_VertexAttribPointer((GLuint)a, f->size, f->type, f->norm, f->stride, (const char *)f->ptr + off);
            if (buf != g_A) g_vattr[i].attr[a] = 0;   // (not one of these any more)
        }
    }
    if (cur != g_vao) r_BindVertexArray(g_vao);
    if (prev >= 0) r_BindBuffer(GL_ARRAY_BUFFER, (GLuint)prev);
    prev = -1;
    for (unsigned k = 0; k < g_nubo; k++) {
        if (g_ubo[k] != name) continue;
        if (prev < 0) r_GetIntegerv(GL_UNIFORM_BUFFER_BINDING, &prev);
        if (g_ubo_n[k] < 0 && buf != g_A) r_BindBufferBase(GL_UNIFORM_BUFFER, k, buf);
        else r_BindBufferRange(GL_UNIFORM_BUFFER, k, buf, (GLintptr)off + g_ubo_o[k], g_ubo_n[k] < 0 ? (GLsizeiptr)len : g_ubo_n[k]);
        if (buf != g_A) g_ubo[k] = 0;
        g_rebound++;
    }
    if (prev >= 0) r_BindBuffer(GL_UNIFORM_BUFFER, (GLuint)prev);
}
static void w_BindVertexArray(GLuint v) { g_vao = v; r_BindVertexArray(v); }
// the attributes each vertex array has enabled (vertex arrays named past VEN: not followed, their draws not ranged)
#define VEN 4096
static unsigned g_ven[VEN];
static void (*r_EnableVertexAttribArray)(GLuint), (*r_DisableVertexAttribArray)(GLuint);
static void w_EnableVertexAttribArray(GLuint a) { if (g_vao < VEN && a < 32) g_ven[g_vao] |= 1u << a; r_EnableVertexAttribArray(a); }
static void w_DisableVertexAttribArray(GLuint a) { if (g_vao < VEN && a < 32) g_ven[g_vao] &= ~(1u << a); r_DisableVertexAttribArray(a); }
static void w_DeleteVertexArrays(GLsizei n, const GLuint *v)
{
    for (GLsizei i = 0; i < n; i++) {
        GLuint *s = elem_slot(v[i], 0), *attr;
        if (s) *s = 0;
        int *u = attr_used(v[i], &attr, 0);
        if (u) *u = 0;
        if (v[i] == g_vao) g_vao = 0;
        if (v[i] < VEN) g_ven[v[i]] = 0;
    }
    r_DeleteVertexArrays(n, v);
}

// the draws: the indices moved by the vertex array's index buffer's offset
static const void *idx(const void *indices)
{
    if (g_ok <= 0) return indices;
    draw_uses();
    GLuint *s = elem_slot(g_vao, 0);
    vbuf *p = s && *s ? find(*s) : NULL;
    return p ? (const char *)indices + p->off : indices;
}
// A draw whose indices are in the arena as glDrawRangeElements, the range from 0 to the last vertex every
// enabled attribute's buffer holds (UA_GL_ARENA_RANGE=0: as the game made it). The Mali driver reads a draw's indices for the vertices it
// uses and keeps that per index buffer, refreshed when the buffer is changed through GL; the game's writes into
// the arena's mapping do not reach it (the GPU drew with stale ranges: sprites stretched, HUD masks missing,
// RG34XX-SP 2026-11-06), so the arena gives it the range instead. 0: an attribute not in the arena or not
// followed (a plain draw then).
static unsigned asize(GLenum type)
{
    switch (type) {
    case 0x1400: case 0x1401: return 1;                    // (GL_BYTE, GL_UNSIGNED_BYTE)
    case 0x1402: case 0x1403: case 0x140B: case 0x8D61: return 2;   // (SHORT, UNSIGNED_SHORT, HALF_FLOAT, HALF_FLOAT_OES)
    default: return 4;
    }
}
static int last_vertex(GLuint *last)
{
    if (g_vao >= VEN || !g_ven[g_vao]) return 0;
    GLuint *attr0;
    if (!attr_used(g_vao, &attr0, 0)) return 0;
    int k = g_alast;
    unsigned en = g_ven[g_vao], best = 0xffffffffu;
    for (int a = 0; a < 32; a++) {
        if (!(en & (1u << a))) continue;
        if (a >= ATTRS || !g_vattr[k].attr[a]) return 0;
        vbuf *p = find(g_vattr[k].attr[a]);
        if (!p) return 0;
        attrfmt *f = &g_vfmt[k][a];
        unsigned elem = (unsigned)f->size * asize(f->type), stride = f->stride ? (unsigned)f->stride : elem, off = (unsigned)(uintptr_t)f->ptr;
        if (f->type == 0x8368 || f->type == 0x8D9F) elem = 4;      // (packed 2_10_10_10)
        if (!stride || off + elem > p->len) return 0;
        unsigned m = (p->len - off - elem) / stride;
        if (m < best) best = m;
    }
    if (best == 0xffffffffu) return 0;
    *last = best;
    return 1;
}
// the range of a draw from an index buffer in the arena: 0 to the last vertex the attributes hold, else (an
// attribute not followed) the indices' own, read from the arena (j: the offset into it); 0: not one of these
static int ranged(GLint base, GLsizei n, GLenum t, const void *j, GLuint *first, GLuint *last)
{
    GLuint *s = elem_slot(g_vao, 0);
    if (!g_range || !s || !*s || !find(*s)) return 0;
    *first = 0;
    if (last_vertex(last) && base >= 0 && (GLuint)base <= *last) { *last -= (GLuint)base; g_ranged++; return 1; }
    uintptr_t o = (uintptr_t)j;
    unsigned sz = t == 0x1401 ? 1 : t == 0x1403 ? 2 : 4;
    if (n <= 0 || o + (uintptr_t)n * sz > g_size) return 0;
    GLuint lo = 0xffffffffu, hi = 0;
    for (GLsizei k = 0; k < n; k++) {
        const unsigned char *q = (const unsigned char *)g_ptr + o + (uintptr_t)k * sz;
        GLuint v = sz == 1 ? q[0] : sz == 2 ? *(const unsigned short *)q : *(const GLuint *)q;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    *first = lo; *last = hi;
    g_unranged++;
    return 1;
}
static void w_DrawElements(GLenum m, GLsizei n, GLenum t, const void *i)
{
    const void *j = idx(i);
    GLuint first, last;
    if (ranged(0, n, t, j, &first, &last)) r_DrawRangeElements(m, first, last, n, t, j);
    else r_DrawElements(m, n, t, j);
}
static void w_DrawElementsInstanced(GLenum m, GLsizei n, GLenum t, const void *i, GLsizei k) { r_DrawElementsInstanced(m, n, t, idx(i), k); }
static void w_DrawRangeElements(GLenum m, GLuint a, GLuint b, GLsizei n, GLenum t, const void *i) { r_DrawRangeElements(m, a, b, n, t, idx(i)); }
static void w_DrawElementsBaseVertex(GLenum m, GLsizei n, GLenum t, const void *i, GLint v) { r_DrawElementsBaseVertex(m, n, t, idx(i), v); }
static void w_DrawElementsBaseVertexOES(GLenum m, GLsizei n, GLenum t, const void *i, GLint v)
{
    const void *j = idx(i);
    GLuint first, last;
    if (r_DrawRangeElementsBaseVertexOES && ranged(v, n, t, j, &first, &last)) r_DrawRangeElementsBaseVertexOES(m, first, last, n, t, j, v);
    else r_DrawElementsBaseVertexOES(m, n, t, j, v);
}
static void w_DrawElementsBaseVertexEXT(GLenum m, GLsizei n, GLenum t, const void *i, GLint v) { r_DrawElementsBaseVertexEXT(m, n, t, idx(i), v); }
static void w_DrawRangeElementsBaseVertex(GLenum m, GLuint a, GLuint b, GLsizei n, GLenum t, const void *i, GLint v)
{ r_DrawRangeElementsBaseVertex(m, a, b, n, t, idx(i), v); }
static void w_DrawRangeElementsBaseVertexOES(GLenum m, GLuint a, GLuint b, GLsizei n, GLenum t, const void *i, GLint v)
{ r_DrawRangeElementsBaseVertexOES(m, a, b, n, t, idx(i), v); }
static void w_DrawElementsInstancedBaseVertex(GLenum m, GLsizei n, GLenum t, const void *i, GLsizei k, GLint v)
{ r_DrawElementsInstancedBaseVertex(m, n, t, idx(i), k, v); }
static void w_DrawElementsInstancedBaseVertexOES(GLenum m, GLsizei n, GLenum t, const void *i, GLsizei k, GLint v)
{ r_DrawElementsInstancedBaseVertexOES(m, n, t, idx(i), k, v); }

// the draws without indices and the dispatches: only what they use
static void (*r_DrawArrays)(GLenum, GLint, GLsizei);
static void (*r_DrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei);
static void (*r_DispatchCompute)(GLuint, GLuint, GLuint);
static void (*r_DispatchComputeIndirect)(GLintptr);
static void w_DrawArrays(GLenum m, GLint f, GLsizei n) { draw_uses(); r_DrawArrays(m, f, n); }
static void w_DrawArraysInstanced(GLenum m, GLint f, GLsizei n, GLsizei k) { draw_uses(); r_DrawArraysInstanced(m, f, n, k); }
static void w_DispatchCompute(GLuint x, GLuint y, GLuint z) { draw_uses(); r_DispatchCompute(x, y, z); }
static void w_DispatchComputeIndirect(GLintptr o) { draw_uses(); r_DispatchComputeIndirect(o); }
// what the layer cannot move, when it reads one of these buffers
static void (*r_DrawArraysIndirect)(GLenum, const void *);
static void w_DrawArraysIndirect(GLenum m, const void *p) { GLint b = 0; if (g_ok > 0) { draw_uses(); r_GetIntegerv(0x8F43, &b); if ((GLuint)b == g_A) not_moved("an indirect draw"); } r_DrawArraysIndirect(m, p); }
static void (*r_DrawElementsIndirect)(GLenum, GLenum, const void *);
static void w_DrawElementsIndirect(GLenum m, GLenum t, const void *p)
{
    GLint b = 0;
    if (g_ok > 0) {
        r_GetIntegerv(0x8F43, &b);
        if ((GLuint)b == g_A || idx(NULL)) not_moved("an indirect draw");   // (its first index is in the command)
    }
    r_DrawElementsIndirect(m, t, p);
}

void glarena_subdata_counts(long *copied, long *driver) { *copied = g_subcopy; *driver = g_subdriver; }

void glarena_frame(void)
{
    if (g_ok <= 0) return;
    g_frames++;
    // this frame's fence; the ranges freed in frames the GPU is past go back to the arena
    int slot = (int)(g_frames % FENCES);
    if (g_fence[slot]) {                    // (8 frames behind: wait for it)
        q_ClientWaitSync(g_fence[slot], 0, 1000000000ull);
        q_DeleteSync(g_fence[slot]);
        long f = g_fence_frame[slot];
        if (f > g_done) g_done = f;
        for (size_t i = 0; i < g_npend;) if (g_pend[i].frame < f) { release(g_pend[i].off, g_pend[i].size); g_pend[i] = g_pend[--g_npend]; } else i++;
        g_fence[slot] = NULL;
    }
    for (int k = 1; k < FENCES; k++) {      // the older ones that have signalled
        int s = (int)((g_frames - FENCES + k) % FENCES);
        if (s < 0) s += FENCES;
        if (!g_fence[s]) continue;
        GLenum r = q_ClientWaitSync(g_fence[s], 0, 0);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) break;
        q_DeleteSync(g_fence[s]);
        long f = g_fence_frame[s];
        if (f > g_done) g_done = f;
        for (size_t i = 0; i < g_npend;) if (g_pend[i].frame < f) { release(g_pend[i].off, g_pend[i].size); g_pend[i] = g_pend[--g_npend]; } else i++;
        g_fence[s] = NULL;
    }
    g_fence[slot] = q_FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    g_fence_frame[slot] = g_frames;
    if (g_frames % 64) return;
    double t = now();
    if (t - g_last_log < 10) return;
    g_last_log = t;
    unsigned pend = 0;
    for (size_t i = 0; i < g_npend; i++) pend += (g_pend[i].size + ALIGN - 1) & ~(ALIGN - 1);
    fprintf(stderr, "[glarena] %zu buffers in the arena (%.1f of %u MB, %.1f MB waiting for the GPU), %ld made, %ld maps (%ld moved by "
            "the game's invalidations, %ld waited), %ld sized again, %ld uniform bindings moved, %ld did not fit; glBufferSubData: %ld copied, "
            "%ld to a new range, %ld to the driver\n", g_n, g_used / 1048576.0, g_size >> 20, pend / 1048576.0, g_in, g_moves, g_orphans, g_orphan_waits,
            g_resized, g_rebound, g_full, g_subcopy, g_subshadow, g_subdriver);
    fprintf(stderr, "[glarena] maps by kind (busy/idle): unsynchronized invalidating %ld/%ld, unsynchronized %ld/%ld, invalidating %ld/%ld, "
            "other %ld/%ld; access", g_mapk[1][1][1], g_mapk[1][1][0], g_mapk[1][0][1], g_mapk[1][0][0], g_mapk[0][1][1], g_mapk[0][1][0],
            g_mapk[0][0][1], g_mapk[0][0][0]);
    for (int k = 0; k < 8 && g_acc[k].n; k++) fprintf(stderr, " 0x%x:%ld", g_acc[k].a, g_acc[k].n);
    if (g_range) fprintf(stderr, "; draws ranged %ld, by their indices %ld", g_ranged, g_unranged);
    fprintf(stderr, "\n");
}

static void *gl(void *(*next)(const char *), const char *name)
{
    void *p = next ? next(name) : NULL;
    if (p) return p;
    static void *h;
    if (!h) h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    p = h ? dlsym(h, name) : NULL;
    if (!p) {
        void *e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        void *(*gpa)(const char *) = e ? (void *(*)(const char *))dlsym(e, "eglGetProcAddress") : NULL;
        p = gpa ? gpa(name) : NULL;
    }
    return p;
}

const so_shim *glarena_table(void *(*next)(const char *name))
{
    memset(g_shims, 0, sizeof g_shims);
    const char *e = getenv("UA_GL_ARENA");
    if (!e || strcmp(e, "1")) return g_shims;
    const char *mb = getenv("UA_GL_ARENA_MB");
    g_size = (unsigned)(mb && atoi(mb) > 0 ? atoi(mb) : 8) << 20;
    const char *sd = getenv("UA_GL_ARENA_SUBDATA");
    g_copy = sd && !strcmp(sd, "1");
    const char *rg = getenv("UA_GL_ARENA_RANGE");
    g_range = !(rg && !strcmp(rg, "0"));
    const char *ib = getenv("UA_GL_ARENA_IB");
    g_ib = !(ib && !strcmp(ib, "0"));
    const char *sy = getenv("UA_GL_ARENA_SYNC");
    g_sync = sy && !strcmp(sy, "1");
    const char *sh = getenv("UA_GL_ARENA_SHADOW");
    g_shadow = !(sh && !strcmp(sh, "0"));
#define Q(f) *(void **)&q_##f = gl(NULL, "gl" #f)
    Q(BufferStorageEXT); Q(GenBuffers); Q(BindBuffer); Q(MapBufferRange); Q(GetString); Q(GetIntegerv); Q(Finish);
    Q(FenceSync); Q(ClientWaitSync); Q(DeleteSync);
#undef Q
    int k = 0, missing = 0;
#define R(f) do { *(void **)&r_##f = gl(next, "gl" #f); if (r_##f) g_shims[k++] = (so_shim){ "gl" #f, (void *)w_##f }; else missing++; } while (0)
#define O(f) do { *(void **)&r_##f = gl(next, "gl" #f); if (r_##f) g_shims[k++] = (so_shim){ "gl" #f, (void *)w_##f }; } while (0)
    R(BindBuffer); R(BindBufferBase); R(BindBufferRange); R(BufferData); R(BufferSubData); R(MapBufferRange);
    R(FlushMappedBufferRange); R(UnmapBuffer); R(CopyBufferSubData); R(DeleteBuffers); R(IsBuffer); R(GetBufferParameteriv);
    R(GetIntegerv); R(VertexAttribPointer); R(VertexAttribIPointer); R(BindVertexArray); R(DeleteVertexArrays); R(DrawElements);
    R(DrawElementsInstanced); R(DrawRangeElements); R(EnableVertexAttribArray); R(DisableVertexAttribArray);
    O(DrawElementsBaseVertex); O(DrawElementsBaseVertexOES); O(DrawElementsBaseVertexEXT); O(DrawRangeElementsBaseVertex);
    O(DrawRangeElementsBaseVertexOES); O(DrawElementsInstancedBaseVertex); O(DrawElementsInstancedBaseVertexOES);
    O(DrawArraysIndirect); O(DrawElementsIndirect);
    R(DrawArrays); R(DrawArraysInstanced); O(DispatchCompute); O(DispatchComputeIndirect);
#undef R
#undef O
    if (missing || !q_BufferStorageEXT || !q_GenBuffers || !q_BindBuffer || !q_MapBufferRange || !q_GetString || !q_GetIntegerv ||
        !q_Finish || !q_FenceSync || !q_ClientWaitSync || !q_DeleteSync) {
        fprintf(stderr, "[glarena] GL functions missing: off\n");
        memset(g_shims, 0, sizeof g_shims);
        return g_shims;
    }
    g_on = 1;
    return g_shims;
}
