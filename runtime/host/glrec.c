// GL frame recorder: the render thread's GL calls of a frame are held until the swap and submitted
// to the driver then, so the host knows the whole frame before the driver sees any of it.
//
//   UA_GL_REC=census   count the render thread's GL calls a frame by entry point; nothing is held.
//                      Shows what a recorder has to hold, and which calls would end what it knows
//                      (anything that returns a value or reads client memory submits first)
//   UA_GL_REC=time     census, and the time each entry point takes (the layers below this one and the
//                      driver: what the render thread spends in GL, call by call), read from the ARM
//                      generic timer's counter (CNTVCT: about 0.1 us a call; clock_gettime where it
//                      cannot be read)
//   UA_GL_REC=1        record every frame and submit it unchanged at the swap (the cost of holding)
//   UA_GL_REC=2        record, and submit the same frame with less tiler work where the whole
//                      frame shows it changes no pixel (glrec_fg.c: the frame graph)
//   UA_GL_REC=thread   record, and replay each frame and make Unity's swap on a thread of its own
//                      (the GL thread) while the render thread records the next (below, "thread")
//   UA_GL_REC=auto     thread where glpresent.c's present thread runs (a driver whose swap waits for
//                      the GPU, whose draw calls hold the render thread: RK3326's Mali), else off
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
#define _GNU_SOURCE               // (pthread_setname_np)
#include <dlfcn.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "glrec.h"
#include "glrec_fg.h"
#include "glopt.h"
#include "glpresent.h"
#include "glspec.h"
#include "glmerge.h"
#include "../android/egl_glue.h"
#define GLREC_PART 1
#include "glrec_gen.h"
#undef GLREC_PART

#ifndef GL_MAP_PERSISTENT_BIT_EXT
#define GL_MAP_PERSISTENT_BIT_EXT 0x0040
#endif
#define MAXOP OP_N
static int g_mode;               // 0 off, 1 census, 2 record, 3 record + frame graph, 4 record + GL thread
static int g_auto;               // UA_GL_REC=auto: 4 where the present thread runs, else 0
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
#define HOLD_MAX_T (8u << 20)        // (UA_GL_REC=thread: a frame that holds more, a level's uploads, is
                                     //  made on the render thread as with UA_GL_REC=1, its context taken back)
static unsigned long g_size_submits;
static int g_tstarted, g_toff;       // UA_GL_REC=thread: the GL thread runs; given up (all on the render thread again)
static void submit(int at_swap);
static void flush_held(void);
static void need_ctx(void);
static inline int glrec_recording(void)
{
    if (!g_on || !pthread_equal(pthread_self(), g_thread)) return 0;
    if (g_tstarted && !g_toff) {
        if (g_ncmd * 8 + g_nblob > HOLD_MAX_T) { g_size_submits++; need_ctx(); submit(0); }
    } else if (g_ncmd * 8 + g_nblob > HOLD_MAX) { g_size_submits++; flush_held(); }  // (before this call: in order)
    return 1;
}

// statistics (the render thread's), since the last log line
static unsigned long g_calls[MAXOP], g_submits[MAXOP], g_rec_calls, g_held_words, g_held_blob;
static unsigned long g_frames, g_swap_submits;
static unsigned long g_direct[MAXOP];   // (recording: calls made at once, not held, by entry point)
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
static const unsigned char *g_rblob;          // the blob arena of the calls being replayed
static const void *glrec_ptr(uint64_t v) { return v & BLOB_TAG ? (const void *)(g_rblob + (v & ~BLOB_TAG)) : (const void *)(uintptr_t)v; }
static inline uint64_t rec_fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float rec_bitsf(uint64_t v) { uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); return f; }

static inline void glrec_count(unsigned op) { if (mine()) g_calls[op]++; }
static void glrec_submit_for(unsigned op);

// ---- UA_GL_REC=time: ticks of the generic timer (or ns of CLOCK_MONOTONIC) in each entry point
static int g_time;
static double g_tick_ns;
static unsigned long long g_ticks[MAXOP];
static int g_counter;               // the counter can be read (probed)
#if defined(__arm__)
static inline uint64_t counter(void)
{
    uint32_t lo, hi;
    __asm__ volatile("isb\n\tmrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi) :: "memory");
    return (uint64_t)hi << 32 | lo;
}
static uint32_t counter_freq(void) { uint32_t f; __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(f)); return f; }
#elif defined(__aarch64__)
static inline uint64_t counter(void) { uint64_t v; __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) :: "memory"); return v; }
static uint32_t counter_freq(void) { uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return (uint32_t)f; }
#else
static inline uint64_t counter(void) { return 0; }
static uint32_t counter_freq(void) { return 0; }
#endif
static inline uint64_t ticks(void)
{
    if (g_counter) return counter();
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static sigjmp_buf g_probe_jb;
static void probe_ill(int sig) { (void)sig; siglongjmp(g_probe_jb, 1); }
static void time_init(void)
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = probe_ill;
    sigaction(SIGILL, &sa, &old);
    volatile uint32_t f = 0; volatile uint64_t a = 0, b = 0;   // (volatile: kept across the longjmp)
    if (!sigsetjmp(g_probe_jb, 1)) { f = counter_freq(); a = counter(); usleep(1000); b = counter(); }
    sigaction(SIGILL, &old, NULL);
    g_counter = f > 0 && b > a;
    g_tick_ns = g_counter ? 1e9 / f : 1.0;
    fprintf(stderr, "[glrec] timing with %s\n", g_counter ? "the generic timer's counter" : "clock_gettime");
}
// (0 when not timing: the census only counts)
static inline uint64_t glrec_t0(void) { return g_time && mine() ? ticks() : 0; }
static inline void glrec_t1(unsigned op, uint64_t t0) { if (!mine()) return; g_calls[op]++; if (t0) g_ticks[op] += ticks() - t0; }
static inline void glrec_tadd(unsigned op, uint64_t t0) { if (t0) g_ticks[op] += ticks() - t0; }

// ---- the vertex array shadow (what a draw reads from client memory)
static void (*d_GetIntegerv)(GLenum, GLint *);
static void (*d_GetVertexAttribiv)(GLuint, GLenum, GLint *);
static void (*d_BindVertexArray)(GLuint);
// (what the shadow asks the driver: with UA_GL_REC=thread, Unity's context taken back first)
static void dq_GetIntegerv(GLenum p, GLint *v) { need_ctx(); d_GetIntegerv(p, v); }
static void dq_GetVertexAttribiv(GLuint i, GLenum p, GLint *v) { need_ctx(); d_GetVertexAttribiv(i, p, v); }
static void dq_BindVertexArray(GLuint a) { need_ctx(); d_BindVertexArray(a); }
#define MAXATTR 16
typedef struct {
    GLuint name; unsigned char used, ebo_known; GLuint ebo;
    GLuint abuf[MAXATTR];                                   // each attribute's buffer (where client_known)
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
    dq_GetIntegerv(GL_VERTEX_ARRAY_BINDING, &dv);
    if ((GLuint)dv != v) dq_BindVertexArray(v);
    return (GLuint)dv;
}
static void driver_vao_restore(GLuint v, GLuint dv) { if (dv != v) dq_BindVertexArray(dv); }

static vao_t *cur_vao(void)
{
    if (!s_vao_known) { GLint v = 0; dq_GetIntegerv(GL_VERTEX_ARRAY_BINDING, &v); s_vao = (GLuint)v; s_vao_known = 1; }
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
            dq_GetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &e);
            v->enabled = e ? v->enabled | b : v->enabled & ~b;
            v->en_known |= b;
        }
        if ((attribs & v->enabled & b) && !(v->client_known & b)) {
            GLint buf = 0;
            dq_GetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf);
            v->client = buf ? v->client & ~b : v->client | b;
            v->abuf[i] = (GLuint)buf;
            v->client_known |= b;
        }
    }
    if (ebo && !v->ebo_known) { GLint e = 0; dq_GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &e); v->ebo = (GLuint)e; v->ebo_known = 1; }
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
    if (!s_array_known) { GLint b = 0; dq_GetIntegerv(GL_ARRAY_BUFFER_BINDING, &b); s_array = (GLuint)b; s_array_known = 1; }
    vao_t *v = cur_vao();
    uint32_t b = 1u << index;
    v->client = s_array ? v->client & ~b : v->client | b;
    v->abuf[index] = s_array;
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
        if (!s_array_known) { GLint b = 0; dq_GetIntegerv(GL_ARRAY_BUFFER_BINDING, &b); s_array = (GLuint)b; s_array_known = 1; }
        *buf = s_array; return 1;
    }
    if (target == GL_ELEMENT_ARRAY_BUFFER) { vao_t *v = cur_vao(); vao_learn(v, 0, 1); *buf = v->ebo; return 1; }
    if (target == GL_DRAW_INDIRECT_BUFFER) {
        if (!s_indirect_known) { GLint b = 0; dq_GetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING, &b); s_indirect = (GLuint)b; s_indirect_known = 1; }
        *buf = s_indirect; return 1;
    }
    for (size_t i = 0; i < NBP; i++)
        if (g_bp[i].target == target) {
            if (!g_bp[i].known) { GLint b = 0; dq_GetIntegerv(g_bp[i].binding, &b); g_bp[i].buf = (GLuint)b; g_bp[i].known = 1; }
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
    if (!(g_ps_known >> i & 1)) { dq_GetIntegerv(g_ps_name[i], &g_ps[i]); g_ps_known |= 1 << i; }
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
    uint64_t t_ = glrec_t0();
    void *p = NX(glMapBufferRange)(target, off, len, access);
    glrec_tadd(OP_glMapBufferRange, t_);
    return p;
}
static void glrec_custom_glFlushMappedBufferRange(GLenum target, GLintptr off, GLsizeiptr len)
{
    map_t *m = glrec_recording() ? map_of(target) : NULL;
    if (!m) {
        glrec_submit_for(OP_glFlushMappedBufferRange);
        uint64_t t_ = glrec_t0();
        NX(glFlushMappedBufferRange)(target, off, len);
        glrec_tadd(OP_glFlushMappedBufferRange, t_);
        return;
    }
    int ok = off >= 0 && len >= 0 && off + len <= m->len;
    uint64_t data = ok ? glrec_blob(m->shadow + off, (size_t)len) : 0;
    uint64_t *a = glrec_cmd(OP_glFlushMappedBufferRange, 5);
    a[0] = target; a[1] = (uint64_t)(int64_t)off; a[2] = (uint64_t)(int64_t)len; a[3] = m->id; a[4] = data;
}
static GLboolean glrec_custom_glUnmapBuffer(GLenum target)
{
    map_t *m = glrec_recording() ? map_of(target) : NULL;
    if (!m) {
        glrec_submit_for(OP_glUnmapBuffer);
        uint64_t t_ = glrec_t0();
        GLboolean r = NX(glUnmapBuffer)(target);
        glrec_tadd(OP_glUnmapBuffer, t_);
        return r;
    }
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
        if (!s_indirect_known) { GLint b = 0; dq_GetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING, &b); s_indirect = (GLuint)b; s_indirect_known = 1; }
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
// glShaderSource: UA_GL_MERGE's rewritten source compiled instead of the game's (glmerge.c)
static void glrec_custom_glShaderSource(GLuint sh, GLsizei count, const GLchar *const *str, const GLint *len)
{
    glrec_submit_for(OP_glShaderSource);
    char *r = glmerge_on() ? glmerge_source(sh, count, str, len) : NULL;
    uint64_t t_ = glrec_t0();
    if (r) { const GLchar *one[1] = { r }; NX(glShaderSource)(sh, 1, one, NULL); free(r); }
    else NX(glShaderSource)(sh, count, str, len);
    glrec_tadd(OP_glShaderSource, t_);
    glrec_shader_source(sh, count, str, len);
}
static void glrec_replay_glShaderSource(const uint64_t *a) { (void)a; }      // (never held)
static void glrec_attach_shader(GLuint prog, GLuint sh) { if (g_mode == 3) fg_note_attach(prog, sh); glmerge_attach(prog, sh); }
static void glrec_link_program(GLuint prog) { if (g_mode == 3) fg_note_link(prog, 0); glmerge_linked(prog); }
static void glrec_program_binary(GLuint prog, const void *bin, GLsizei len) { if (g_mode == 3) fg_note_binary(prog, bin, len); glmerge_binary(prog); }
// (Unity passes no length pointer: the buffer it sized from GL_PROGRAM_BINARY_LENGTH is the binary)
static void glrec_get_program_binary(GLuint prog, GLsizei size, GLsizei *len, const void *bin)
{
    long n = len ? *len : size;
    if (g_mode == 3 && bin && n > 0) fg_note_binary_out(prog, bin, n);
}
// the same two for glhooks.c's UA_GL_PROGLOG wrappers, which take these calls before this layer
void glrec_note_program_binary(unsigned prog, const void *bin, long len) { glrec_program_binary(prog, bin, (GLsizei)len); }
void glrec_note_get_program_binary(unsigned prog, long len, const void *bin)
{
    if (g_mode == 3 && bin && len > 0) fg_note_binary_out(prog, bin, len);
}
static void glrec_delete_program(GLuint prog) { if (g_mode == 3) fg_note_delete(prog); glmerge_deleted(prog); }
// (Unity's program binary cache stays on: the frame graph keeps what it learnt from each program's
// sources with the binary Unity reads back for its cache, glrec_fg.c)
// (UA_GL_MERGE: Unity's program binary cache off, every program compiled from the source it rewrote)
static void glrec_get_integerv(GLenum pname, GLint *data) { if (glmerge_on() && pname == GL_NUM_PROGRAM_BINARY_FORMATS && data) *data = 0; }

// ---- UA_GL_REC=thread: the held frames replayed, and Unity's swaps made, on a thread of their own
// Unity's render thread records each frame as with UA_GL_REC=1. At the swap, Unity's context and the
// frame (its calls, then the whole swap: the host's swap hooks, overlays and the real swap, which
// egl_glue.c hands over) go to the GL thread, and the render thread goes on with the next frame with a
// context of its own current, made in Unity's share group (the aux): there it makes the calls that only
// need what the group shares (names of buffers, textures, renderbuffers, samplers, shaders and
// programs; fences, strings). A fence the game makes is held like any call, the game given a stand-in
// until the GL thread has made it (waiting on one held here hands it over first). Anything that needs
// Unity's context itself here (state read back, texture definitions, shader and program work, names
// of vertex arrays or framebuffers, what the shadow does not know) takes the context back: it waits
// for the GL thread to finish what it has and let the context go; the next swap takes it over again.
// One frame in flight: a swap waits until the GL thread has made the previous one. Another
// eglMakeCurrent on the render thread leaves the GL thread out from there on.
static int g_ctx_here = 1;               // the render thread has Unity's context current (else the aux)
static EGLDisplay g_edpy;
static EGLSurface g_edraw, g_eread;
static EGLContext g_ectx, g_eaux;
static EGLSurface g_udraw, g_uread;      // what Unity was told was current (eglGetCurrentSurface/Context, through
static EGLContext g_uctx;                // egl_glue.c): told again while the context is on the GL thread
static void *(*g_resolve)(const char *);
static pthread_t g_glt;
static pthread_mutex_t g_tm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tc = PTHREAD_COND_INITIALIZER;
static int g_tjob, g_tbusy, g_tgive, g_tglt_ctx;    // a job waits; the GL thread works; the context wanted back; the GL thread has it
static glpresent_tstate g_pst;           // glpresent.c's view of the context, with the context
static uint64_t *g_jcmd; static size_t g_jncmd, g_jcapcmd;        // the job: its calls,
static unsigned char *g_jblob; static size_t g_jnblob, g_jcapblob;
static egl_glue_swap_fn g_jswap; static void *g_jsd, *g_jss;   // then the swap (NULL: a flush)
static unsigned long g_hand_seq;         // jobs handed over
static double g_t_wait_s, g_t_glt_s;     // the render thread waiting for the GL thread; the GL thread working
static unsigned long g_t_returns, g_t_jobs;
static EGLBoolean (*e_MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static void replay_buf(const uint64_t *cmd, size_t from, size_t to);
static void glrec_call(unsigned op, const uint64_t *a);
// UA_GL_REC_DEBUG=<n>: the first <n> steps of the hand-overs logged
static int g_tdebug = -1;
#define TDBG(...) do { if (g_tdebug < 0) { const char *e_ = getenv("UA_GL_REC_DEBUG"); g_tdebug = e_ ? atoi(e_) : 0; } \
                       if (g_tdebug > 0) { g_tdebug--; fprintf(stderr, "[glrec] t %.4f ", now()); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// recording here with Unity's context on the GL thread
static inline int held_here(void) { return g_tstarted && !g_toff && !g_ctx_here && g_on && mine(); }

static void need_ctx(void)
{
    if (!g_tstarted || g_ctx_here || !mine()) return;
    double t0 = now();
    TDBG("need_ctx: asking (job %d busy %d glt_ctx %d)", g_tjob, g_tbusy, g_tglt_ctx);
    pthread_mutex_lock(&g_tm);
    g_tgive = 1;
    pthread_cond_broadcast(&g_tc);
    while (g_tjob || g_tbusy || g_tglt_ctx) pthread_cond_wait(&g_tc, &g_tm);
    g_tgive = 0;
    pthread_mutex_unlock(&g_tm);
    e_MakeCurrent(g_edpy, g_edraw, g_eread, g_ectx);
    glpresent_tstate_give(&g_pst);
    glspec_rethread(); glopt_rethread();
    g_ctx_here = 1;
    TDBG("need_ctx: here (%.2f ms)", (now() - t0) * 1000);
    g_t_returns++;
    g_t_wait_s += now() - t0;
}

// the held calls (and a swap) to the GL thread, Unity's context with them
static void hand(egl_glue_swap_fn swap, void *d, void *s)
{
    double t0 = now();
    TDBG("hand: %s, %zu words (job %d busy %d here %d)", swap ? "swap" : "flush", g_ncmd, g_tjob, g_tbusy, g_ctx_here);
    pthread_mutex_lock(&g_tm);
    while (g_tjob || g_tbusy) pthread_cond_wait(&g_tc, &g_tm);
    g_t_wait_s += now() - t0;
    if (g_ctx_here) {
        glpresent_tstate_take(&g_pst);
        e_MakeCurrent(g_edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_eaux);
        g_ctx_here = 0;
    }
    g_held_words += g_ncmd; g_held_blob += g_nblob;
    uint64_t *c = g_jcmd; size_t cc = g_jcapcmd;
    g_jcmd = g_cmd; g_jncmd = g_ncmd; g_jcapcmd = g_capcmd;
    g_cmd = c; g_capcmd = cc; g_ncmd = 0;
    unsigned char *b = g_jblob; size_t bc = g_jcapblob;
    g_jblob = g_blob; g_jnblob = g_nblob; g_jcapblob = g_capblob;
    g_blob = b; g_capblob = bc; g_nblob = 0;
    g_jswap = swap; g_jsd = d; g_jss = s;
    g_hand_seq++; g_t_jobs++;
    g_tjob = 1;
    pthread_cond_broadcast(&g_tc);
    pthread_mutex_unlock(&g_tm);
}

static void flush_held(void) { if (g_tstarted && !g_toff && !g_ctx_here) hand(NULL, NULL, NULL); else submit(0); }


static void *glt_main(void *u)
{
    (void)u;
    pthread_setname_np(pthread_self(), "ua_glthread");
    pthread_mutex_lock(&g_tm);
    for (;;) {
        while (!g_tjob && !(g_tgive && g_tglt_ctx)) pthread_cond_wait(&g_tc, &g_tm);
        if (g_tjob) {
            g_tjob = 0; g_tbusy = 1;
            pthread_mutex_unlock(&g_tm);
            double t0 = now();
            TDBG("glt: job %zu words%s (has ctx %d)", g_jncmd, g_jswap ? " + swap" : "", g_tglt_ctx);
            if (!g_tglt_ctx) {
                e_MakeCurrent(g_edpy, g_edraw, g_eread, g_ectx);
                glpresent_tstate_give(&g_pst);
                glspec_rethread(); glopt_rethread();
                g_tglt_ctx = 1;
            }
            g_rblob = g_jblob;
            if (glmerge_on()) {
                fg_frame mf = { .cmd = g_jcmd, .ncmd = g_jncmd, .at_swap = g_jswap != NULL, .call = glrec_call, .ptr = glrec_ptr, .next = glrec_next };
                glmerge_submit(&mf);
            } else replay_buf(g_jcmd, 0, g_jncmd);
            g_jncmd = 0; g_jnblob = 0;
            TDBG("glt: replayed (%.2f ms)", (now() - t0) * 1000);
            if (g_jswap) g_jswap(g_jsd, g_jss);
            else NX(glFlush)();
            TDBG("glt: done (%.2f ms)", (now() - t0) * 1000);
            // (a long frame's buffers not kept: a frame of play needs a few hundred KB)
            if (g_jcapcmd * sizeof *g_jcmd > HOLD_MAX_T) { free(g_jcmd); g_jcmd = NULL; g_jcapcmd = 0; }
            if (g_jcapblob > HOLD_MAX_T) { free(g_jblob); g_jblob = NULL; g_jcapblob = 0; }
            g_t_glt_s += now() - t0;
            pthread_mutex_lock(&g_tm);
            g_tbusy = 0;
            pthread_cond_broadcast(&g_tc);
            continue;
        }
        // the render thread wants the context: let it go
        TDBG("glt: giving the context back");
        glpresent_tstate_take(&g_pst);
        e_MakeCurrent(g_edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        g_tglt_ctx = 0;
        pthread_cond_broadcast(&g_tc);
    }
    return NULL;
}

// the render thread's current context, display and surfaces taken over; the aux made; the GL thread started
static int tstart(void)
{
    void *e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!e) e = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    EGLDisplay (*cur_dpy)(void) = e ? (EGLDisplay (*)(void))dlsym(e, "eglGetCurrentDisplay") : NULL;
    EGLSurface (*cur_surf)(EGLint) = e ? (EGLSurface (*)(EGLint))dlsym(e, "eglGetCurrentSurface") : NULL;
    EGLContext (*cur_ctx)(void) = e ? (EGLContext (*)(void))dlsym(e, "eglGetCurrentContext") : NULL;
    EGLBoolean (*query_ctx)(EGLDisplay, EGLContext, EGLint, EGLint *) = e ? (EGLBoolean (*)(EGLDisplay, EGLContext, EGLint, EGLint *))dlsym(e, "eglQueryContext") : NULL;
    EGLBoolean (*choose)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = e ? (EGLBoolean (*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))dlsym(e, "eglChooseConfig") : NULL;
    EGLContext (*create)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = e ? (EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *))dlsym(e, "eglCreateContext") : NULL;
    const char *(*qstr)(EGLDisplay, EGLint) = e ? (const char *(*)(EGLDisplay, EGLint))dlsym(e, "eglQueryString") : NULL;
    *(void **)&e_MakeCurrent = e ? dlsym(e, "eglMakeCurrent") : NULL;
    if (!cur_dpy || !cur_surf || !cur_ctx || !query_ctx || !choose || !create || !qstr || !e_MakeCurrent) {
        fprintf(stderr, "[glrec] thread: no EGL library: the frames stay on the render thread\n");
        return -1;
    }
    g_edpy = cur_dpy(); g_ectx = cur_ctx(); g_edraw = cur_surf(EGL_DRAW); g_eread = cur_surf(EGL_READ);
    // (Unity's view: through the layers under this one, egl_glue.c's host surface)
    EGLSurface (*u_surf)(EGLint) = g_resolve ? (EGLSurface (*)(EGLint))g_resolve("eglGetCurrentSurface") : NULL;
    EGLContext (*u_ctx)(void) = g_resolve ? (EGLContext (*)(void))g_resolve("eglGetCurrentContext") : NULL;
    g_udraw = u_surf ? u_surf(EGL_DRAW) : g_edraw; g_uread = u_surf ? u_surf(EGL_READ) : g_eread;
    g_uctx = u_ctx ? u_ctx() : g_ectx;
    const char *x = g_edpy ? qstr(g_edpy, EGL_EXTENSIONS) : NULL;
    if (!g_ectx || !x || !strstr(x, "EGL_KHR_surfaceless_context")) {
        fprintf(stderr, "[glrec] thread: %s: the frames stay on the render thread\n",
                !g_ectx ? "no context current on the render thread" : "no EGL_KHR_surfaceless_context");
        return -1;
    }
    EGLint id = 0, ver = 0, n = 0;
    EGLConfig cfg = NULL;
    query_ctx(g_edpy, g_ectx, EGL_CONFIG_ID, &id);
    query_ctx(g_edpy, g_ectx, EGL_CONTEXT_CLIENT_VERSION, &ver);
    if (id) { EGLint a[] = { EGL_CONFIG_ID, id, EGL_NONE }; choose(g_edpy, a, &cfg, 1, &n); }
    if (n < 1) { EGLint a[] = { EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */, EGL_NONE }; choose(g_edpy, a, &cfg, 1, &n); }
    EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, ver > 0 ? ver : 3, EGL_NONE };
    g_eaux = n > 0 ? create(g_edpy, cfg, g_ectx, ca) : EGL_NO_CONTEXT;
    if (g_eaux == EGL_NO_CONTEXT) { fprintf(stderr, "[glrec] thread: no context could be made in Unity's share group: the frames stay on the render thread\n"); return -1; }
    if (pthread_create(&g_glt, NULL, glt_main, NULL) != 0) { fprintf(stderr, "[glrec] thread: no thread: the frames stay on the render thread\n"); return -1; }
    g_tstarted = 1;
    fprintf(stderr, "[glrec] thread: the GL thread replays the frames and makes the swaps from here on (display %p, surfaces %p %p, context %p; aux %p)\n",
            g_edpy, g_edraw, g_eread, g_ectx, g_eaux);
    return 0;
}

static void log_stats(void);
static double g_log_every = 10, g_log_next, g_t0;
// egl_glue.c's swap offload: Unity's swap, after the frame's calls, made on the GL thread
static int glrec_swap_offload(egl_glue_swap_fn swap, void *d, void *s)
{
    if (g_mode != 4 || g_toff || !g_on || !mine()) return 0;
    if (!g_tstarted && tstart() != 0) { g_toff = 1; return 0; }
    hand(swap, d, s);
    g_frames++; g_swap_submits++;
    double t = now();
    if (g_log_every > 0 && t >= g_log_next) { g_log_next = t + g_log_every; log_stats(); }
    return 1;
}

// the calls made at once: names that are not shared need Unity's context here
static inline void glrec_imm(unsigned op)
{
    if (!g_tstarted || g_ctx_here || !mine()) return;
    switch (op) {
    case OP_glGenFramebuffers: case OP_glGenVertexArrays: case OP_glGenVertexArraysOES: case OP_glGenQueries:
    case OP_glGenQueriesEXT: case OP_glGenTransformFeedbacks: case OP_glGenProgramPipelines:
        need_ctx(); break;
    default: break;
    }
}

// fences: a stand-in while held (state 1), the driver's once the GL thread made it (2); 0 free
#define NPS 1024
typedef struct { GLsync real; int state; unsigned long seq; } psync;
static psync g_psync[NPS];
static inline int is_proxy(GLsync s) { return (const char *)s >= (const char *)g_psync && (const char *)s < (const char *)(g_psync + NPS); }
static GLsync real_sync(GLsync s)
{
    if (!is_proxy(s)) return s;
    psync *p = (psync *)s;
    if (__atomic_load_n(&p->state, __ATOMIC_ACQUIRE) == 1 && mine() && p->seq > g_hand_seq) flush_held();   // still held here
    if (__atomic_load_n(&p->state, __ATOMIC_ACQUIRE) == 1) TDBG("fence %d: waiting for the GL thread to make it (seq %lu, handed %lu, %s)", (int)(p - g_psync), p->seq, g_hand_seq, mine() ? "render thread" : "another thread");
    pthread_mutex_lock(&g_tm);
    while (__atomic_load_n(&p->state, __ATOMIC_ACQUIRE) == 1) pthread_cond_wait(&g_tc, &g_tm);
    GLsync r = p->state == 2 ? p->real : NULL;
    pthread_mutex_unlock(&g_tm);
    return r;
}
static GLsync glrec_custom_glFenceSync(GLenum cond, GLbitfield flags)
{
    if (held_here()) {
        psync *p = NULL;
        pthread_mutex_lock(&g_tm);
        for (int i = 0; i < NPS && !p; i++) if (!g_psync[i].state) p = &g_psync[i];
        if (p) { p->real = NULL; p->seq = g_hand_seq + 1; __atomic_store_n(&p->state, 1, __ATOMIC_RELEASE); }
        pthread_mutex_unlock(&g_tm);
        if (p) {
            uint64_t *a = glrec_cmd(OP_glFenceSync, 3);
            a[0] = cond; a[1] = flags; a[2] = (uint64_t)(p - g_psync);
            return (GLsync)p;
        }
    }
    glrec_submit_for(OP_glFenceSync);
    uint64_t t_ = glrec_t0();
    GLsync r = NX(glFenceSync)(cond, flags);
    glrec_tadd(OP_glFenceSync, t_);
    return r;
}
static void glrec_replay_glFenceSync(const uint64_t *a)
{
    psync *p = &g_psync[a[2]];
    GLsync r = NX(glFenceSync)((GLenum)a[0], (GLbitfield)a[1]);
    pthread_mutex_lock(&g_tm);
    p->real = r;
    __atomic_store_n(&p->state, 2, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_tc);
    pthread_mutex_unlock(&g_tm);
}
static GLenum glrec_custom_glClientWaitSync(GLsync s, GLbitfield flags, GLuint64 timeout)
{
    GLsync r = real_sync(s);
    uint64_t t_ = glrec_t0();
    GLenum v = r ? NX(glClientWaitSync)(r, flags, timeout) : GL_WAIT_FAILED;
    glrec_t1(OP_glClientWaitSync, t_);
    return v;
}
static void glrec_replay_glClientWaitSync(const uint64_t *a) { (void)a; }
static void free_proxy(psync *p)
{
    pthread_mutex_lock(&g_tm);
    p->real = NULL;
    __atomic_store_n(&p->state, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_tm);
}
static void glrec_custom_glDeleteSync(GLsync s)
{
    if (held_here() && s) {
        uint64_t *a = glrec_cmd(OP_glDeleteSync, 1);
        a[0] = (uint64_t)(uintptr_t)s;
        return;
    }
    GLsync r = real_sync(s);
    uint64_t t_ = glrec_t0();
    if (r || !is_proxy(s)) NX(glDeleteSync)(r);
    glrec_t1(OP_glDeleteSync, t_);
    if (is_proxy(s)) free_proxy((psync *)s);
}
static void glrec_replay_glDeleteSync(const uint64_t *a)
{
    GLsync s = (GLsync)(uintptr_t)a[0];
    if (!is_proxy(s)) { NX(glDeleteSync)(s); return; }
    psync *p = (psync *)s;
    if (p->real) NX(glDeleteSync)(p->real);
    free_proxy(p);
}
static void glrec_custom_glGetSynciv(GLsync s, GLenum pname, GLsizei count, GLsizei *len, GLint *values)
{
    GLsync r = real_sync(s);
    uint64_t t_ = glrec_t0();
    NX(glGetSynciv)(r, pname, count, len, values);
    glrec_t1(OP_glGetSynciv, t_);
}
static void glrec_replay_glGetSynciv(const uint64_t *a) { (void)a; }
static GLboolean glrec_custom_glIsSync(GLsync s)
{
    if (is_proxy(s)) return __atomic_load_n(&((psync *)s)->state, __ATOMIC_ACQUIRE) ? GL_TRUE : GL_FALSE;
    uint64_t t_ = glrec_t0();
    GLboolean r = NX(glIsSync)(s);
    glrec_t1(OP_glIsSync, t_);
    return r;
}
static void glrec_replay_glIsSync(const uint64_t *a) { (void)a; }
static void glrec_custom_glWaitSync(GLsync s, GLbitfield flags, GLuint64 timeout)
{
    if (held_here()) {
        uint64_t *a = glrec_cmd(OP_glWaitSync, 3);
        a[0] = (uint64_t)(uintptr_t)s; a[1] = flags; a[2] = timeout;
        return;
    }
    glrec_submit_for(OP_glWaitSync);
    GLsync r = real_sync(s);
    uint64_t t_ = glrec_t0();
    NX(glWaitSync)(r, flags, timeout);
    glrec_tadd(OP_glWaitSync, t_);
}
static void glrec_replay_glWaitSync(const uint64_t *a)
{
    GLsync s = (GLsync)(uintptr_t)a[0];
    NX(glWaitSync)(is_proxy(s) ? ((psync *)s)->real : s, (GLbitfield)a[1], (GLuint64)a[2]);
}

// deleting buffers: held with UA_GL_REC=thread, the shadow unbinding them as the driver will
static void shadow_unbind(GLsizei n, const GLuint *b)
{
    vao_t *v = s_vao_known ? vao_get(s_vao) : NULL;
    for (GLsizei k = 0; k < n; k++) {
        GLuint name = b[k];
        if (!name) continue;
        if (s_array_known && s_array == name) s_array = 0;
        if (s_indirect_known && s_indirect == name) s_indirect = 0;
        for (size_t i = 0; i < NBP; i++) if (g_bp[i].known && g_bp[i].buf == name) g_bp[i].buf = 0;
        if (!v) continue;
        if (v->ebo_known && v->ebo == name) v->ebo = 0;
        for (int i = 0; i < MAXATTR; i++) {
            uint32_t bit = 1u << i;
            if ((v->client_known & bit) && !(v->client & bit) && v->abuf[i] == name) { v->client |= bit; v->abuf[i] = 0; }
        }
    }
}
static void glrec_custom_glDeleteBuffers(GLsizei n, const GLuint *b)
{
    if (held_here() && n > 0 && b) {
        shadow_unbind(n, b);
        uint64_t *a = glrec_cmd(OP_glDeleteBuffers, 2);
        a[0] = (uint64_t)(int64_t)n; a[1] = glrec_blob(b, (size_t)n * sizeof *b);
        return;
    }
    glrec_submit_for(OP_glDeleteBuffers);
    uint64_t t_ = glrec_t0();
    NX(glDeleteBuffers)(n, b);
    glrec_tadd(OP_glDeleteBuffers, t_);
}
static void glrec_replay_glDeleteBuffers(const uint64_t *a) { NX(glDeleteBuffers)((GLsizei)(int64_t)a[0], (const GLuint *)glrec_ptr(a[1])); }

#define GLREC_PART 2
#include "glrec_gen.h"

// ---- submitting
static void replay_buf(const uint64_t *cmd, size_t from, size_t to)
{
    for (size_t i = from; i < to;) {
        uint64_t h = cmd[i];
        unsigned op = (unsigned)(h & 0xffffffffu), n = (unsigned)(h >> 32);
        glrec_call(op, cmd + i + 1);
        i += n + 1;
    }
}
static void replay(size_t from, size_t to) { replay_buf(g_cmd, from, to); }

static void submit(int at_swap)
{
    if (!g_ncmd) return;
    double t0 = now();
    g_held_words += g_ncmd; g_held_blob += g_nblob;
    g_rblob = g_blob;
    if (g_mode == 3) {
        fg_frame f = { .cmd = g_cmd, .ncmd = g_ncmd, .at_swap = at_swap, .call = glrec_call, .ptr = glrec_ptr, .next = glrec_next };
        fg_submit(&f);
        g_fg_s += f.analysis_s;
    } else if (glmerge_on()) {
        fg_frame f = { .cmd = g_cmd, .ncmd = g_ncmd, .at_swap = at_swap, .call = glrec_call, .ptr = glrec_ptr, .next = glrec_next };
        glmerge_submit(&f);
    } else replay(0, g_ncmd);
    g_ncmd = 0; g_nblob = 0;
    if (at_swap) g_swap_submits++;
    g_replay_s += now() - t0;
}

static void glrec_submit_for(unsigned op)
{
    if (!mine()) return;
    need_ctx();                                     // (UA_GL_REC=thread: Unity's context here first)
    g_calls[op]++;
    if (g_on) g_direct[op]++;
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
static unsigned (*r_MakeCurrent)(void *, void *, void *, void *);
static unsigned w_eglMakeCurrent(void *d, void *dr, void *rd, void *c)
{
    if (mine()) {
        if (g_tstarted && !g_toff) {
            need_ctx();
            g_toff = 1;
            fprintf(stderr, "[glrec] thread: eglMakeCurrent on the render thread: the frames stay there from here on\n");
        }
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

// eglGetCurrentContext/Surface on the render thread while Unity's context is on the GL thread: what
// Unity was told before (else it takes its window for gone, and draws frames it never shows)
static EGLSurface (*r_GetCurrentSurface)(EGLint);
static EGLSurface w_eglGetCurrentSurface(EGLint which)
{
    if (g_tstarted && !g_toff && !g_ctx_here && mine()) return which == EGL_READ ? g_uread : g_udraw;
    if (!r_GetCurrentSurface) {
        *(void **)&r_GetCurrentSurface = g_resolve ? g_resolve("eglGetCurrentSurface") : NULL;
        if (!r_GetCurrentSurface) { void *e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL); if (e) *(void **)&r_GetCurrentSurface = dlsym(e, "eglGetCurrentSurface"); }
        if (!r_GetCurrentSurface) return EGL_NO_SURFACE;
    }
    return r_GetCurrentSurface(which);
}
static EGLContext (*r_GetCurrentContext)(void);
static EGLContext w_eglGetCurrentContext(void)
{
    if (g_tstarted && !g_toff && !g_ctx_here && mine()) return g_uctx;
    if (!r_GetCurrentContext) {
        *(void **)&r_GetCurrentContext = g_resolve ? g_resolve("eglGetCurrentContext") : NULL;
        if (!r_GetCurrentContext) { void *e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL); if (e) *(void **)&r_GetCurrentContext = dlsym(e, "eglGetCurrentContext"); }
        if (!r_GetCurrentContext) return EGL_NO_CONTEXT;
    }
    return r_GetCurrentContext();
}

// ---- statistics
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
// UA_GL_REC=time: the entry points by time a frame, with their calls and the time of one
static void top_time(double frames)
{
    opcount c[MAXOP];
    int k = 0;
    double all = 0;
    for (unsigned i = 0; i < OP_N; i++) if (g_ticks[i]) { c[k++] = (opcount){ i, (unsigned long)(g_ticks[i] >> 4) }; all += g_ticks[i]; }
    if (!k) return;
    qsort(c, (size_t)k, sizeof c[0], by_count);
    char line[2400];
    size_t o = 0;
    for (int i = 0; i < k && i < 30 && o + 96 < sizeof line; i++) {
        unsigned op = c[i].op;
        o += (size_t)snprintf(line + o, sizeof line - o, "%s%s %.3f ms (%.1f x %.1f us)", i ? ", " : "", glrec_name[op],
                              g_ticks[op] * g_tick_ns / 1e6 / frames, g_calls[op] / frames,
                              g_calls[op] ? g_ticks[op] * g_tick_ns / 1e3 / g_calls[op] : 0.0);
    }
    fprintf(stderr, "[glrec]   time in GL: %.2f ms a frame: %s\n", all * g_tick_ns / 1e6 / frames, line);
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
    else top(g_direct, "made at once (not held)", 16);
    if (g_time) top_time(f);
    if (g_mode == 3) fg_log(g_frames, g_fg_s);
    glmerge_log();
    if (g_tstarted)
        fprintf(stderr, "[glrec] thread: the GL thread replaying and swapping %.2f ms a frame; the render thread waiting for it "
                "%.2f ms a frame; %.2f jobs and %.2f context returns a frame%s\n", g_t_glt_s * 1000 / f, g_t_wait_s * 1000 / f,
                g_t_jobs / f, g_t_returns / f, g_toff ? " (left out now)" : "");
    g_t_glt_s = g_t_wait_s = 0; g_t_jobs = g_t_returns = 0;
    memset(g_calls, 0, sizeof g_calls); memset(g_submits, 0, sizeof g_submits); memset(g_direct, 0, sizeof g_direct); memset(g_ticks, 0, sizeof g_ticks);
    g_rec_calls = g_held_words = g_held_blob = g_frames = g_swap_submits = g_size_submits = 0;
    g_replay_s = g_fg_s = 0;
}

void glrec_swap(void)
{
    if (!g_mode) return;
    if (!g_have_thread) {
        g_thread = pthread_self(); g_have_thread = 1;
        if (g_auto && !glpresent_active()) {
            g_mode = 0;
            fprintf(stderr, "[glrec] UA_GL_REC=auto: no present thread here: off\n");
            return;
        }
        shadow_reset();
        g_on = g_mode >= 2;
        if (g_mode == 3) fg_context();
        g_t0 = now(); g_log_next = g_t0 + g_log_every;
        fprintf(stderr, "[glrec] %s from the next frame on (the render thread)\n",
                g_mode == 1 ? "counting calls" : g_mode == 2 ? "recording, frames submitted unchanged" :
                g_mode == 4 ? "recording, frames replayed on a GL thread of their own" : "recording, frame graph on");
        return;
    }
    if (!mine()) return;
    if (g_ncmd) submit(1);
    if (g_mode == 3) fg_swap();
    g_frames++;
    double t = now();
    if (g_log_every > 0 && t >= g_log_next) { g_log_next = t + g_log_every; log_stats(); }
}

static so_shim g_table[OP_N + 4];

const so_shim *glrec_table(void *(*next)(const char *name))
{
    memset(g_table, 0, sizeof g_table);
    const char *e = getenv("UA_GL_REC");
    g_time = e && !strcmp(e, "time");
    g_mode = !e || !*e || !strcmp(e, "0") ? 0 : !strcmp(e, "census") || g_time ? 1 : !strcmp(e, "1") ? 2 : !strcmp(e, "2") ? 3 : !strcmp(e, "thread") || !strcmp(e, "auto") ? 4 : 0;
    g_auto = g_mode == 4 && !strcmp(e, "auto");
    if (!g_mode) return g_table;
    if (g_time) time_init();
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
    if (g_mode == 4) {
        g_table[k++] = (so_shim){ "eglGetCurrentContext", (void *)w_eglGetCurrentContext };
        g_table[k++] = (so_shim){ "eglGetCurrentSurface", (void *)w_eglGetCurrentSurface };
        egl_glue_set_swap_offload(glrec_swap_offload);
    }
    if (g_mode == 3 && fg_init() != 0) g_mode = 2;
    if (glmerge_init()) {
        if (g_mode == 2 || g_mode == 4) glmerge_set_next(glrec_next);
        else fprintf(stderr, "[glmerge] UA_GL_MERGE needs UA_GL_REC=1, thread or auto: off\n");
    }
    fprintf(stderr, "[glrec] UA_GL_REC=%s: %d entry points wrapped\n", e, k);
    return g_table;
}
