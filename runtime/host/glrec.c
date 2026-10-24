// GL frame recorder: the render thread's GL calls of a frame are held until the swap and submitted
// to the driver then, so the host knows the whole frame before the driver sees any of it.
//
//   UA_GL_REC=census   count the render thread's GL calls a frame by entry point; nothing is held.
//                      Shows what a recorder has to hold, and which calls would end what it knows
//                      (anything that returns a value or reads client memory submits first)
//   UA_GL_REC=1        record every frame and submit it unchanged at the swap (the cost of holding)
//   UA_GL_REC=2        record, and submit the same frame with less tiler work where the whole
//                      frame shows it changes no pixel (glrec_fg.c: the frame graph)
//   UA_GL_REC_LOG=<s>  statistics every <s> seconds (default 10)
//
// Wrappers (glrec_gen.h, generated from Khronos' GLES 3.2 headers by glrec_gen.py) for every GLES
// 3.2 entry point and each extension a Unity player names. While recording (the thread that swaps,
// from its second frame on), a call that only sets state or queues GPU work is appended to a
// command buffer with its arguments by value and the memory its pointers point at copied (uniform
// values, buffer data, attachment lists, client-side indices); any other call (queries, maps,
// fences, texture definitions, shader and program management, glFinish, eglMakeCurrent) first
// submits what is held, then runs: the driver always sees the calls in the order Unity made
// them. Buffer maps for writing (Unity writes its dynamic geometry through a map, a flush of the
// range and an unmap for each draw) are held too: the game writes into memory of the host's, and
// the flushed ranges (or, without GL_MAP_FLUSH_EXPLICIT_BIT, the whole range of a map that
// invalidates it) are written into the real mapping when the map is submitted, with the same
// flags; a map that reads, a persistent one, or one whose untouched bytes must keep their contents
// is made at once. Texture uploads are held with their pixels copied (the size from the pixel
// store state), or as an offset into the bound unpack buffer. A draw that reads client-side vertex arrays (whose memory is read at the call) or an
// indirect draw without a buffer submits first too: a small shadow of the vertex array state
// (the bound vertex array object, its element buffer, its enabled attributes and which of them
// have no buffer) knows; what it does not know yet it asks the driver, whose state is then that of
// the held calls' start (nothing held has reached it), binding the object for the question and
// back. Submitting calls the next layer (glopt.c, msaafix.c, gltrace.c, the driver) in order.
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#include "glrec.h"
#include "glrec_fg.h"
#define GLREC_PART 1
#include "glrec_gen.h"
#undef GLREC_PART

#ifndef GL_MAP_PERSISTENT_BIT_EXT
#define GL_MAP_PERSISTENT_BIT_EXT 0x0040
#endif
#define MAXOP OP_N
static int g_mode;               // 0 off, 1 census, 2 record, 3 record + frame graph
static int g_on;                 // recording (the render thread is known)
static pthread_t g_thread;
static int g_have_thread;
static inline int mine(void) { return g_have_thread && pthread_equal(pthread_self(), g_thread); }
// ---- the command buffer: [op | nargs << 32] then nargs 64-bit slots; blobs in a byte arena
static uint64_t *g_cmd;
static size_t g_ncmd, g_capcmd;
static unsigned char *g_blob;
static size_t g_nblob, g_capblob;
#define BLOB_TAG (1ull << 63)
#define HOLD_MAX (32u << 20)         // bytes held before an early submission
static unsigned long g_size_submits;
static void submit(int at_swap);
static inline int glrec_recording(void)
{
    if (!g_on || !pthread_equal(pthread_self(), g_thread)) return 0;
    if (g_ncmd * 8 + g_nblob > HOLD_MAX) { g_size_submits++; submit(0); }     // (before this call: in order)
    return 1;
}

// statistics (the render thread's), since the last log line
static unsigned long g_calls[MAXOP], g_submits[MAXOP], g_rec_calls, g_held_words, g_held_blob;
static unsigned long g_frames, g_swap_submits;
static double g_replay_s, g_fg_s;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void *grow(void *p, size_t *cap, size_t need, size_t elem)
{
    size_t c = *cap ? *cap : 4096;
    while (c < need) c *= 2;
    void *n = realloc(p, c * elem);
    if (!n) { fprintf(stderr, "[glrec] out of memory\n"); abort(); }
    *cap = c;
    return n;
}

static inline uint64_t *glrec_cmd(unsigned op, unsigned n)
{
    if (g_ncmd + n + 1 > g_capcmd) g_cmd = grow(g_cmd, &g_capcmd, g_ncmd + n + 1, sizeof *g_cmd);
    uint64_t *c = g_cmd + g_ncmd;
    c[0] = op | (uint64_t)n << 32;
    g_ncmd += n + 1;
    g_calls[op]++; g_rec_calls++;
    return c + 1;
}

static inline uint64_t glrec_blob(const void *p, size_t n)
{
    if (!p) return 0;
    size_t at = (g_nblob + 7) & ~(size_t)7;
    if (at + n > g_capblob) g_blob = grow(g_blob, &g_capblob, at + n, 1);
    memcpy(g_blob + at, p, n);
    g_nblob = at + n;
    return BLOB_TAG | at;
}
static const void *glrec_ptr(uint64_t v) { return v & BLOB_TAG ? (const void *)(g_blob + (v & ~BLOB_TAG)) : (const void *)(uintptr_t)v; }
static inline uint64_t rec_fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float rec_bitsf(uint64_t v) { uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); return f; }

static inline void glrec_count(unsigned op) { if (mine()) g_calls[op]++; }
static void glrec_submit_for(unsigned op);

// ---- the vertex array shadow (what a draw reads from client memory)
static void (*d_GetIntegerv)(GLenum, GLint *);
static void (*d_GetVertexAttribiv)(GLuint, GLenum, GLint *);
static void (*d_BindVertexArray)(GLuint);
#define MAXATTR 16
typedef struct {
    GLuint name; unsigned char used, ebo_known; GLuint ebo;
    uint32_t en_known, enabled, client_known, client;       // per attribute: known, enabled, no buffer
} vao_t;
#define NVAO 2048
static vao_t g_vao[NVAO];
static int g_nvao;
static GLuint s_vao, s_array, s_indirect;
static int s_vao_known, s_array_known, s_indirect_known;

static void bindings_reset(void);
static void pixelstore_reset(void);
static void bp_set(GLenum target, GLuint buf);
static void shadow_reset(void)
{
    memset(g_vao, 0, sizeof g_vao);
    g_nvao = 0;
    s_vao_known = s_array_known = s_indirect_known = 0;
    bindings_reset();
    pixelstore_reset();
}

static vao_t *vao_get(GLuint name)
{
    for (unsigned i = (name * 2654435761u) % NVAO, k = 0; k < NVAO; k++, i = (i + 1) % NVAO) {
        if (g_vao[i].used && g_vao[i].name == name) return &g_vao[i];
        if (!g_vao[i].used) {
            if (g_nvao > NVAO / 2) { shadow_reset(); return vao_get(name); }
            g_nvao++;
            g_vao[i] = (vao_t){ .name = name, .used = 1 };
            return &g_vao[i];
        }
    }
    shadow_reset();
    return vao_get(name);
}

static GLuint driver_vao_bind(GLuint v)
{
    GLint dv = 0;
    d_GetIntegerv(GL_VERTEX_ARRAY_BINDING, &dv);
    if ((GLuint)dv != v) d_BindVertexArray(v);
    return (GLuint)dv;
}
static void driver_vao_restore(GLuint v, GLuint dv) { if (dv != v) d_BindVertexArray(dv); }

static vao_t *cur_vao(void)
{
    if (!s_vao_known) { GLint v = 0; d_GetIntegerv(GL_VERTEX_ARRAY_BINDING, &v); s_vao = (GLuint)v; s_vao_known = 1; }
    return vao_get(s_vao);
}

// what the shadow does not know about this vertex array: asked from the driver
static void vao_learn(vao_t *v, uint32_t attribs, int ebo)
{
    uint32_t need_en = attribs & ~v->en_known;
    if (!need_en && !(ebo && !v->ebo_known) && !(v->enabled & ~v->client_known & attribs)) return;
    GLuint dv = driver_vao_bind(v->name);
    for (int i = 0; i < MAXATTR; i++) {
        uint32_t b = 1u << i;
        if (need_en & b) {
            GLint e = 0;
            d_GetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &e);
            v->enabled = e ? v->enabled | b : v->enabled & ~b;
            v->en_known |= b;
        }
        if ((attribs & v->enabled & b) && !(v->client_known & b)) {
            GLint buf = 0;
            d_GetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf);
            v->client = buf ? v->client & ~b : v->client | b;
            v->client_known |= b;
        }
    }
    if (ebo && !v->ebo_known) { GLint e = 0; d_GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &e); v->ebo = (GLuint)e; v->ebo_known = 1; }
    driver_vao_restore(v->name, dv);
}

static int glrec_vao_hook(int r, GLuint array) { (void)r; if (mine()) { s_vao = array; s_vao_known = 1; } return 0; }
static int glrec_buffer_hook(int r, GLenum target, GLuint buffer)
{
    (void)r;
    if (!mine()) return 0;
    bp_set(target, buffer);
    if (target == GL_ARRAY_BUFFER) { s_array = buffer; s_array_known = 1; }
    else if (target == GL_DRAW_INDIRECT_BUFFER) { s_indirect = buffer; s_indirect_known = 1; }
    else if (target == GL_ELEMENT_ARRAY_BUFFER) { vao_t *v = cur_vao(); v->ebo = buffer; v->ebo_known = 1; }
    return 0;
}
static int glrec_vap_hook(int r, GLuint index)
{
    (void)r;
    if (!mine() || index >= MAXATTR) return 0;
    if (!s_array_known) { GLint b = 0; d_GetIntegerv(GL_ARRAY_BUFFER_BINDING, &b); s_array = (GLuint)b; s_array_known = 1; }
    vao_t *v = cur_vao();
    uint32_t b = 1u << index;
    v->client = s_array ? v->client & ~b : v->client | b;
    v->client_known |= b;
    return 0;
}
static int glrec_attrib_hook(int r, GLuint index, int on)
{
    (void)r;
    if (!mine() || index >= MAXATTR) return 0;
    vao_t *v = cur_vao();
    uint32_t b = 1u << index;
    v->enabled = on ? v->enabled | b : v->enabled & ~b;
    v->en_known |= b;
    return 0;
}
// the other buffer binding points (unpack buffer: an upload's pixels; any: which buffer a map is of)
typedef struct { GLenum target, binding; GLuint buf; int known; } bindpoint;
static bindpoint g_bp[] = {
    { GL_PIXEL_UNPACK_BUFFER, GL_PIXEL_UNPACK_BUFFER_BINDING, 0, 0 }, { GL_PIXEL_PACK_BUFFER, GL_PIXEL_PACK_BUFFER_BINDING, 0, 0 },
    { GL_UNIFORM_BUFFER, GL_UNIFORM_BUFFER_BINDING, 0, 0 }, { GL_COPY_READ_BUFFER, GL_COPY_READ_BUFFER_BINDING, 0, 0 },
    { GL_COPY_WRITE_BUFFER, GL_COPY_WRITE_BUFFER_BINDING, 0, 0 }, { GL_TRANSFORM_FEEDBACK_BUFFER, GL_TRANSFORM_FEEDBACK_BUFFER_BINDING, 0, 0 },
    { GL_SHADER_STORAGE_BUFFER, GL_SHADER_STORAGE_BUFFER_BINDING, 0, 0 }, { GL_ATOMIC_COUNTER_BUFFER, GL_ATOMIC_COUNTER_BUFFER_BINDING, 0, 0 },
    { GL_DISPATCH_INDIRECT_BUFFER, GL_DISPATCH_INDIRECT_BUFFER_BINDING, 0, 0 }, { GL_TEXTURE_BUFFER, GL_TEXTURE_BUFFER_BINDING, 0, 0 },
};
#define NBP (sizeof g_bp / sizeof g_bp[0])
static void bindings_reset(void) { for (size_t i = 0; i < NBP; i++) g_bp[i].known = 0; }
static void bp_set(GLenum target, GLuint buf) { for (size_t i = 0; i < NBP; i++) if (g_bp[i].target == target) { g_bp[i].buf = buf; g_bp[i].known = 1; } }
static int buffer_bound(GLenum target, GLuint *buf)
{
    if (target == GL_ARRAY_BUFFER) {
        if (!s_array_known) { GLint b = 0; d_GetIntegerv(GL_ARRAY_BUFFER_BINDING, &b); s_array = (GLuint)b; s_array_known = 1; }
        *buf = s_array; return 1;
    }
    if (target == GL_ELEMENT_ARRAY_BUFFER) { vao_t *v = cur_vao(); vao_learn(v, 0, 1); *buf = v->ebo; return 1; }
    if (target == GL_DRAW_INDIRECT_BUFFER) {
        if (!s_indirect_known) { GLint b = 0; d_GetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING, &b); s_indirect = (GLuint)b; s_indirect_known = 1; }
        *buf = s_indirect; return 1;
    }
    for (size_t i = 0; i < NBP; i++)
        if (g_bp[i].target == target) {
            if (!g_bp[i].known) { GLint b = 0; d_GetIntegerv(g_bp[i].binding, &b); g_bp[i].buf = (GLuint)b; g_bp[i].known = 1; }
            *buf = g_bp[i].buf;
            return 1;
        }
    return 0;
}

// the pixel store state an upload's size depends on
static const GLenum g_ps_name[6] = { GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_IMAGE_HEIGHT,
                                     GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_IMAGES };
static GLint g_ps[6];
static int g_ps_known;
static void pixelstore_reset(void) { g_ps_known = 0; }
static GLint pixelstore(int i)
{
    if (!(g_ps_known >> i & 1)) { d_GetIntegerv(g_ps_name[i], &g_ps[i]); g_ps_known |= 1 << i; }
    return g_ps[i];
}
static int glrec_pixelstore_hook(int r, GLenum pname, GLint v)
{
    (void)r;
    if (!mine()) return 0;
    for (int i = 0; i < 6; i++) if (g_ps_name[i] == pname) { g_ps[i] = v; g_ps_known |= 1 << i; }
    return 0;
}
static size_t pixel_bytes(GLenum format, GLenum type)
{
    size_t comp;
    switch (format) {
    case GL_RED: case GL_RED_INTEGER: case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX: comp = 1; break;
    case GL_RG: case GL_RG_INTEGER: case GL_LUMINANCE_ALPHA: case GL_DEPTH_STENCIL: comp = 2; break;
    case GL_RGB: case GL_RGB_INTEGER: comp = 3; break;
    case GL_RGBA: case GL_RGBA_INTEGER: case GL_BGRA_EXT: comp = 4; break;
    default: return 0;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: return comp;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: case GL_HALF_FLOAT_OES: return comp * 2;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return comp * 4;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1: return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV:
    case GL_UNSIGNED_INT_24_8: return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: return 8;
    default: return 0;
    }
}
// nonzero: an upload this cannot size (submitted at once)
static int glrec_pixels_hook(int r, GLenum format, GLenum type) { return r && !pixel_bytes(format, type); }
static uint64_t glrec_pixels(const void *p, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type)
{
    GLuint pbo = 0;
    buffer_bound(GL_PIXEL_UNPACK_BUFFER, &pbo);
    if (pbo || !p || w <= 0 || h <= 0 || d <= 0) return (uint64_t)(uintptr_t)p;
    size_t bpp = pixel_bytes(format, type), align = (size_t)pixelstore(0);
    size_t rl = pixelstore(1) > 0 ? (size_t)pixelstore(1) : (size_t)w;
    size_t ih = pixelstore(2) > 0 ? (size_t)pixelstore(2) : (size_t)h;
    size_t row = rl * bpp;
    if (align > 1) row = (row + align - 1) / align * align;
    size_t img = ih * row;
    size_t n = (size_t)pixelstore(5) * img + (size_t)(d - 1) * img + (size_t)pixelstore(4) * row + (size_t)pixelstore(3) * bpp +
               (size_t)(h - 1) * row + (size_t)w * bpp;
    return glrec_blob(p, n);
}
static uint64_t glrec_cpixels(const void *p, GLsizei size)
{
    GLuint pbo = 0;
    buffer_bound(GL_PIXEL_UNPACK_BUFFER, &pbo);
    if (pbo || !p || size <= 0) return (uint64_t)(uintptr_t)p;
    return glrec_blob(p, (size_t)size);
}

// ---- buffer maps for writing: the game writes into host memory, put into the real mapping at submission
#define NMAP 16
typedef struct { int used; GLenum target; GLuint buf; GLbitfield access; GLsizeiptr len; unsigned char *shadow; uint32_t id; } map_t;
static map_t g_map[NMAP];
static uint32_t g_map_id;
typedef struct { uint32_t id; void *p; GLsizeiptr len; } rmap_t;      // submitted maps (the real pointer)
static rmap_t g_rmap[NMAP];
static unsigned long g_maps_held;

static map_t *map_of(GLenum target)
{
    GLuint buf = 0;
    if (!buffer_bound(target, &buf)) return NULL;
    for (int i = 0; i < NMAP; i++) if (g_map[i].used && g_map[i].target == target && g_map[i].buf == buf) return &g_map[i];
    return NULL;
}

static void *glrec_custom_glMapBufferRange(GLenum target, GLintptr off, GLsizeiptr len, GLbitfield access)
{
    GLuint buf = 0;
    if (glrec_recording() && (access & GL_MAP_WRITE_BIT) && !(access & (GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT_EXT)) &&
        (access & (GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT)) &&
        off >= 0 && len > 0 && len <= (GLsizeiptr)(64 << 20) && buffer_bound(target, &buf) && buf && !map_of(target)) {
        map_t *m = NULL;
        for (int i = 0; i < NMAP && !m; i++) if (!g_map[i].used) m = &g_map[i];
        unsigned char *sh = m ? malloc((size_t)len) : NULL;
        if (sh) {
            *m = (map_t){ 1, target, buf, access, len, sh, ++g_map_id };
            uint64_t *a = glrec_cmd(OP_glMapBufferRange, 5);
            a[0] = target; a[1] = (uint64_t)(int64_t)off; a[2] = (uint64_t)(int64_t)len; a[3] = access; a[4] = m->id;
            g_maps_held++;
            return sh;
        }
    }
    glrec_submit_for(OP_glMapBufferRange);
    return NX(glMapBufferRange)(target, off, len, access);
}
static void glrec_custom_glFlushMappedBufferRange(GLenum target, GLintptr off, GLsizeiptr len)
{
    map_t *m = glrec_recording() ? map_of(target) : NULL;
    if (!m) { glrec_submit_for(OP_glFlushMappedBufferRange); NX(glFlushMappedBufferRange)(target, off, len); return; }
    int ok = off >= 0 && len >= 0 && off + len <= m->len;
    uint64_t data = ok ? glrec_blob(m->shadow + off, (size_t)len) : 0;
    uint64_t *a = glrec_cmd(OP_glFlushMappedBufferRange, 5);
    a[0] = target; a[1] = (uint64_t)(int64_t)off; a[2] = (uint64_t)(int64_t)len; a[3] = m->id; a[4] = data;
}
static GLboolean glrec_custom_glUnmapBuffer(GLenum target)
{
    map_t *m = glrec_recording() ? map_of(target) : NULL;
    if (!m) { glrec_submit_for(OP_glUnmapBuffer); return NX(glUnmapBuffer)(target); }
    uint64_t data = m->access & GL_MAP_FLUSH_EXPLICIT_BIT ? 0 : glrec_blob(m->shadow, (size_t)m->len);
    uint64_t *a = glrec_cmd(OP_glUnmapBuffer, 4);
    a[0] = target; a[1] = m->id; a[2] = data; a[3] = (uint64_t)m->len;
    free(m->shadow);
    m->used = 0;
    return GL_TRUE;         // (the real unmap's answer is not known yet: a lost buffer store is not reported)
}
static rmap_t *rmap(uint32_t id, int add)
{
    for (int i = 0; i < NMAP; i++) if (g_rmap[i].id == id && g_rmap[i].p) return &g_rmap[i];
    if (!add) return NULL;
    for (int i = 0; i < NMAP; i++) if (!g_rmap[i].p) { g_rmap[i].id = id; return &g_rmap[i]; }
    return NULL;
}
static void glrec_replay_glMapBufferRange(const uint64_t *a)
{
    void *p = NX(glMapBufferRange)((GLenum)a[0], (GLintptr)(int64_t)a[1], (GLsizeiptr)(int64_t)a[2], (GLbitfield)a[3]);
    rmap_t *r = p ? rmap((uint32_t)a[4], 1) : NULL;
    if (r) { r->p = p; r->len = (GLsizeiptr)(int64_t)a[2]; }
}
static const void *glrec_ptr(uint64_t v);
static void glrec_replay_glFlushMappedBufferRange(const uint64_t *a)
{
    rmap_t *r = rmap((uint32_t)a[3], 0);
    GLintptr off = (GLintptr)(int64_t)a[1];
    GLsizeiptr len = (GLsizeiptr)(int64_t)a[2];
    if (r && a[4]) memcpy((unsigned char *)r->p + off, glrec_ptr(a[4]), (size_t)len);
    NX(glFlushMappedBufferRange)((GLenum)a[0], off, len);
}
static void glrec_replay_glUnmapBuffer(const uint64_t *a)
{
    rmap_t *r = rmap((uint32_t)a[1], 0);
    if (r && a[2]) memcpy(r->p, glrec_ptr(a[2]), (size_t)a[3]);
    NX(glUnmapBuffer)((GLenum)a[0]);
    if (r) r->p = NULL;
}

// nonzero: the draw reads client memory at the call (vertex arrays, an indirect draw's command)
static int glrec_draw_hook(int r, int indexed, int indirect)
{
    if (!r) return 0;
    vao_t *v = cur_vao();
    vao_learn(v, (1u << MAXATTR) - 1, indexed);
    if (v->enabled & v->client) return 1;
    if (indirect) {
        if (!s_indirect_known) { GLint b = 0; d_GetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING, &b); s_indirect = (GLuint)b; s_indirect_known = 1; }
        if (!s_indirect) return 1;
    }
    return 0;
}
// a draw's indices: an offset into the element buffer, or client memory (copied)
static uint64_t glrec_indices(const void *p, GLsizei count, GLenum type)
{
    vao_t *v = cur_vao();
    if (v->ebo) return (uint64_t)(uintptr_t)p;
    size_t sz = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4;
    return glrec_blob(p, (size_t)(count > 0 ? count : 0) * sz);
}

// ---- after these calls (any thread): what the frame graph needs about programs
static void glrec_shader_source(GLuint sh, GLsizei count, const GLchar *const *str, const GLint *len)
{
    if (g_mode != 3 || !str) return;
    int fetch = 0;
    for (GLsizei i = 0; i < count && !fetch; i++) {
        if (!str[i]) continue;
        size_t n = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
        char *c = malloc(n + 1);
        if (!c) { fetch = 1; break; }
        memcpy(c, str[i], n); c[n] = 0;
        if (strstr(c, "LastFragDepth") || strstr(c, "LastFragStencil") || strstr(c, "framebuffer_fetch_depth_stencil")) fetch = 1;
        free(c);
    }
    // (a name split between two strings: every piece searched again as one)
    if (!fetch && count > 1) {
        size_t tot = 0;
        for (GLsizei i = 0; i < count; i++) if (str[i]) tot += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
        char *c = malloc(tot + 1), *o = c;
        if (!c) fetch = 1;
        for (GLsizei i = 0; c && i < count; i++) {
            if (!str[i]) continue;
            size_t n = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
            memcpy(o, str[i], n); o += n;
        }
        if (c) { *o = 0; fetch = strstr(c, "LastFragDepth") || strstr(c, "LastFragStencil") || strstr(c, "framebuffer_fetch_depth_stencil"); free(c); }
    }
    fg_note_shader(sh, fetch);
}
static void glrec_attach_shader(GLuint prog, GLuint sh) { if (g_mode == 3) fg_note_attach(prog, sh); }
static void glrec_link_program(GLuint prog) { if (g_mode == 3) fg_note_link(prog, 0); }
static void glrec_program_binary(GLuint prog) { if (g_mode == 3) fg_note_link(prog, 1); }
static void glrec_delete_program(GLuint prog) { if (g_mode == 3) fg_note_delete(prog); }
// (Unity's program binary cache: off where the frame graph needs every program's source)
static void glrec_get_integerv(GLenum pname, GLint *data)
{
    if (g_mode == 3 && data && pname == GL_NUM_PROGRAM_BINARY_FORMATS && fg_sources_wanted()) *data = 0;
}

#define GLREC_PART 2
#include "glrec_gen.h"

// ---- submitting
static void replay(size_t from, size_t to)
{
    for (size_t i = from; i < to;) {
        uint64_t h = g_cmd[i];
        unsigned op = (unsigned)(h & 0xffffffffu), n = (unsigned)(h >> 32);
        glrec_call(op, g_cmd + i + 1);
        i += n + 1;
    }
}

static void submit(int at_swap)
{
    if (!g_ncmd) return;
    double t0 = now();
    g_held_words += g_ncmd; g_held_blob += g_nblob;
    if (g_mode == 3) {
        fg_frame f = { .cmd = g_cmd, .ncmd = g_ncmd, .at_swap = at_swap, .call = glrec_call, .ptr = glrec_ptr, .next = glrec_next };
        fg_submit(&f);
        g_fg_s += f.analysis_s;
    } else replay(0, g_ncmd);
    g_ncmd = 0; g_nblob = 0;
    if (at_swap) g_swap_submits++;
    g_replay_s += now() - t0;
}

static void glrec_submit_for(unsigned op)
{
    if (!mine()) return;
    g_calls[op]++;
    if (g_on && g_ncmd) { g_submits[op]++; submit(0); }
    else if (g_mode == 1) g_submits[op]++;          // (census: a call that would end what is held)
    // the vertex array shadow: these change what it knows; it asks the driver again
    if (op == OP_glDeleteBuffers || op == OP_glDeleteVertexArrays || op == OP_glDeleteVertexArraysOES ||
        op == OP_glVertexAttribBinding || op == OP_glBindVertexBuffer || op == OP_glVertexAttribFormat ||
        op == OP_glVertexAttribIFormat || op == OP_glVertexBindingDivisor)
        shadow_reset();
    if (g_mode == 3) fg_flush_call(glrec_name[op]);
}

// eglMakeCurrent: what is held goes to the context it was made for
static void *(*g_resolve)(const char *);
static unsigned (*r_MakeCurrent)(void *, void *, void *, void *);
static unsigned w_eglMakeCurrent(void *d, void *dr, void *rd, void *c)
{
    if (mine()) {
        if (g_on && g_ncmd) submit(0);
        shadow_reset();
        if (g_mode == 3) fg_flush_call("eglMakeCurrent");
    }
    if (!r_MakeCurrent) {
        *(void **)&r_MakeCurrent = g_resolve ? g_resolve("eglMakeCurrent") : NULL;
        if (!r_MakeCurrent) { void *e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL); if (e) *(void **)&r_MakeCurrent = dlsym(e, "eglMakeCurrent"); }
        if (!r_MakeCurrent) return 0;
    }
    return r_MakeCurrent(d, dr, rd, c);
}

// ---- statistics
static double g_log_every = 10, g_log_next, g_t0;
typedef struct { unsigned op; unsigned long n; } opcount;
static int by_count(const void *a, const void *b)
{
    unsigned long x = ((const opcount *)a)->n, y = ((const opcount *)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}
static void top(const unsigned long *counts, const char *what, int max)
{
    opcount c[MAXOP];
    int k = 0;
    for (unsigned i = 0; i < OP_N; i++) if (counts[i]) c[k++] = (opcount){ i, counts[i] };
    if (!k) return;
    qsort(c, (size_t)k, sizeof c[0], by_count);
    char line[1600];
    size_t o = 0;
    for (int i = 0; i < k && i < max && o + 64 < sizeof line; i++)
        o += (size_t)snprintf(line + o, sizeof line - o, "%s%s %.1f", i ? ", " : "", glrec_name[c[i].op], (double)c[i].n / (double)g_frames);
    fprintf(stderr, "[glrec]   %s: %s\n", what, line);
}
static void log_stats(void)
{
    if (!g_frames) return;
    unsigned long calls = 0, early = 0;
    for (unsigned i = 0; i < OP_N; i++) { calls += g_calls[i]; early += g_submits[i]; }
    double f = (double)g_frames;
    if (g_mode == 1)
        fprintf(stderr, "[glrec] census, %lu frames: %.0f calls a frame; %.1f a frame would submit what is held first\n",
                g_frames, calls / f, early / f);
    else
        fprintf(stderr, "[glrec] %lu frames: %.0f calls a frame, %.0f held (%.0f KB of commands, %.0f KB of data, "
                "%.1f buffer maps); submitted %.2f times a frame (%.2f at the swap); submitting %.2f ms a frame%s\n",
                g_frames, calls / f, g_rec_calls / f, g_held_words * 8 / 1024.0 / f, g_held_blob / 1024.0 / f, g_maps_held / f,
                (early + g_swap_submits + g_size_submits) / f, g_swap_submits / f, g_replay_s * 1000 / f,
                g_mode == 3 ? "" : " (unchanged)");
    g_maps_held = 0;
    top(g_submits, g_mode == 1 ? "would submit first" : "submitted first for", 16);
    if (g_mode == 1) top(g_calls, "calls a frame", 40);
    if (g_mode == 3) fg_log(g_frames, g_fg_s);
    memset(g_calls, 0, sizeof g_calls); memset(g_submits, 0, sizeof g_submits);
    g_rec_calls = g_held_words = g_held_blob = g_frames = g_swap_submits = g_size_submits = 0;
    g_replay_s = g_fg_s = 0;
}

void glrec_swap(void)
{
    if (!g_mode) return;
    if (!g_have_thread) {
        g_thread = pthread_self(); g_have_thread = 1;
        shadow_reset();
        g_on = g_mode >= 2;
        if (g_mode == 3) fg_context();
        g_t0 = now(); g_log_next = g_t0 + g_log_every;
        fprintf(stderr, "[glrec] %s from the next frame on (the render thread)\n",
                g_mode == 1 ? "counting calls" : g_mode == 2 ? "recording, frames submitted unchanged" : "recording, frame graph on");
        return;
    }
    if (!mine()) return;
    if (g_ncmd) submit(1);
    if (g_mode == 3) fg_swap();
    g_frames++;
    double t = now();
    if (g_log_every > 0 && t >= g_log_next) { g_log_next = t + g_log_every; log_stats(); }
}

static so_shim g_table[OP_N + 2];

const so_shim *glrec_table(void *(*next)(const char *name))
{
    memset(g_table, 0, sizeof g_table);
    const char *e = getenv("UA_GL_REC");
    g_mode = !e || !*e || !strcmp(e, "0") ? 0 : !strcmp(e, "census") ? 1 : !strcmp(e, "1") ? 2 : !strcmp(e, "2") ? 3 : 0;
    if (!g_mode) return g_table;
    const char *l = getenv("UA_GL_REC_LOG");
    if (l && *l) g_log_every = atof(l);
    void *h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    void *eh = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    void *(*gpa)(const char *) = eh ? (void *(*)(const char *))dlsym(eh, "eglGetProcAddress") : NULL;
    *(void **)&d_GetIntegerv = h ? dlsym(h, "glGetIntegerv") : NULL;
    *(void **)&d_GetVertexAttribiv = h ? dlsym(h, "glGetVertexAttribiv") : NULL;
    *(void **)&d_BindVertexArray = h ? dlsym(h, "glBindVertexArray") : NULL;
    if (!d_GetIntegerv || !d_GetVertexAttribiv || !d_BindVertexArray) {
        fprintf(stderr, "[glrec] no GLES 3 driver: off\n");
        g_mode = 0;
        return g_table;
    }
    g_resolve = next;
    int k = 0;
    for (unsigned i = 0; i < OP_N; i++) {
        void *p = next ? next(glrec_name[i]) : NULL;
        if (!p && h) p = dlsym(h, glrec_name[i]);
        if (!p && gpa) p = gpa(glrec_name[i]);
        glrec_next[i] = p;
        if (p) g_table[k++] = glrec_shims[i];          // (a function the driver lacks stays unknown to Unity)
    }
    g_table[k++] = (so_shim){ "eglMakeCurrent", (void *)w_eglMakeCurrent };
    if (g_mode == 3 && fg_init() != 0) g_mode = 2;
    fprintf(stderr, "[glrec] UA_GL_REC=%s: %d entry points wrapped\n", e, k);
    return g_table;
}
