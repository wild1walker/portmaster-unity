// Draws whose target already holds what they would write, left out. UA_GL_MEMO=1 (default) / 0 / check.
//
// Some passes draw the same thing every frame. URP's colour grading LUT (a 1024x32 texture its
// ColorGradingLutPass draws every frame, in every URP version, whenever post-processing is on) is
// built from the volume settings, which in most scenes do not change: one full-screen draw of a
// heavy shader (tonemapping, curves, channel mixer: about 2.2 ms on the Mali-G31) that writes the
// same pixels as the frame before. White Mirror draws it twice a frame (two cameras).
//
// A draw's pixels are a function of what it reads: the program, the fixed-function state that
// affects rasterization and the colour write, the uniform values, the textures and samplers it
// samples, the vertex and index data, the draw call's arguments and the target's format. This
// follows passes that begin by invalidating the colour attachment of a framebuffer that has only
// that one (a texture; Unity's load action DontCare is that invalidate) and keys the pass's first
// draw: all of the above (the uniforms read back with glGetUniform*; the textures, samplers and
// buffers by a count bumped at every write into them, or, for small buffers the game rewrites every
// frame, by their bytes), and keeps the key with the texture's count after the draw. When such a pass
// comes again for that texture, nothing has written the texture since (its count is the one the draw
// left) and the first draw's key is the same byte for byte, the texture already holds exactly what
// the draw would write: neither the draw nor the pass's invalidate is sent (the invalidate is held
// back until the pass's first command that writes pixels, and dropped when that command is a draw
// left out; also when the framebuffer is left with nothing done in it). Pixels a full-screen draw
// does not cover are undefined after the invalidate either way. Only draws that cannot depend on what
// is under them qualify: blending off, no depth or stencil attachment (the tests then pass and write
// nothing), single-sampled, triangles, no transform feedback or occlusion query running, a program
// without storage buffers, images or atomic counters, nothing sampled that is the target itself or
// that this cannot follow (EGL images, texture buffers, textures and buffers the GPU may write
// through image units, storage, atomic-counter or transform-feedback bindings). A framebuffer-fetch
// shader reads the invalidated (undefined) contents in both cases.
//
// Writes this follows (each bumps the count): draws, clears, blits and invalidates into a
// framebuffer (its attached textures), texture uploads, copies, mipmaps, glCopyImageSubData,
// deletions; buffer data, sub-data, mapped writes (copied into the buffer's shadow at the flush or
// unmap, before the driver sees the unmap), buffer copies, pixel-pack reads; sampler parameters;
// program links. A texture's own parameters (filtering, wrapping) count separately, as an input of
// the draws that sample it: they do not change what a draw wrote into it (Unity sets a render
// texture's filtering when it binds it for sampling). A parameter set to the value it has is no
// change (Unity sets the same ones again). A buffer written a second time gets a shadow of its bytes
// (at most 64 KB a buffer, 8 MB in all), one of at most 4 KB or written as a uniform buffer from its
// first write, so a draw whose vertex, index or uniform-block data the game uploads again every
// frame, as Unity does for its blit quads and per-draw constants (2021.2: into a new buffer each
// time), is compared by content. Mapped writes are copied only into small buffers a keyed draw read
// (the mapping is slow to read). Bytes the shadow does not know (written before it existed, mapped
// and not copied or not flushed) never compare equal.
//
// UA_GL_MEMO=check: nothing is left out; each draw that would have been is drawn, its target read
// back before and after it and the two compared (logged: same / other pixels): the evidence, on a
// given device and game, that the texture held what the draw writes. UA_GL_MEMO_DEBUG=1 (or
// <w>x<h>: draws with that viewport only) logs why the first 200 keyed draws were not left out.
// Only the render thread's calls (the one that swaps; glmemo_frame() names it) are followed for
// framebuffers and draws; uploads are followed on every thread.
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glmemo.h"

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned GLbitfield;
typedef unsigned char GLboolean; typedef intptr_t GLintptr; typedef intptr_t GLsizeiptr; typedef int64_t GLint64;

#define GL_NO_ERROR 0
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_COLOR 0x1800
#define GL_DEPTH 0x1801
#define GL_STENCIL 0x1802
#define GL_NONE 0
#define GL_TEXTURE 0x1702
#define GL_RENDERBUFFER 0x8D41
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL 0x8CD2
#define GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE 0x8211
#define GL_UNSIGNED_NORMALIZED 0x8C17
#define GL_DRAW_BUFFER0 0x8825
#define GL_SAMPLE_BUFFERS 0x80A8
#define GL_BLEND 0x0BE2
#define GL_DITHER 0x0BD0
#define GL_CULL_FACE 0x0B44
#define GL_CULL_FACE_MODE 0x0B45
#define GL_FRONT_FACE 0x0B46
#define GL_SCISSOR_TEST 0x0C11
#define GL_SCISSOR_BOX 0x0C10
#define GL_VIEWPORT 0x0BA2
#define GL_DEPTH_RANGE 0x0B70
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#define GL_SAMPLE_COVERAGE 0x80A0
#define GL_RASTERIZER_DISCARD 0x8C89
#define GL_PRIMITIVE_RESTART_FIXED_INDEX 0x8D69
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#define GL_TRANSFORM_FEEDBACK_ACTIVE 0x8E24
#define GL_ANY_SAMPLES_PASSED 0x8C2F
#define GL_ANY_SAMPLES_PASSED_CONSERVATIVE 0x8D6A
#define GL_PRIMITIVES_GENERATED 0x8C87
#define GL_CURRENT_QUERY 0x8865
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE0 0x84C0
#define GL_SAMPLER_BINDING 0x8919
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_3D 0x806F
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z 0x851A
#define GL_TEXTURE_2D_MULTISAMPLE 0x9100
#define GL_TEXTURE_2D_MULTISAMPLE_ARRAY 0x9102
#define GL_TEXTURE_CUBE_MAP_ARRAY 0x9009
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#define GL_TEXTURE_BUFFER 0x8C2A
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_BINDING_3D 0x806A
#define GL_TEXTURE_BINDING_2D_ARRAY 0x8C1D
#define GL_TEXTURE_BINDING_CUBE_MAP 0x8514
#define GL_TEXTURE_BINDING_2D_MULTISAMPLE 0x9104
#define GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY 0x9105
#define GL_TEXTURE_BINDING_CUBE_MAP_ARRAY 0x900A
#define GL_TEXTURE_BINDING_EXTERNAL_OES 0x8D67
#define GL_TEXTURE_BINDING_BUFFER 0x8C2C
#define GL_TEXTURE_BORDER_COLOR 0x1004
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_COPY_READ_BUFFER 0x8F36
#define GL_COPY_WRITE_BUFFER 0x8F37
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_TRANSFORM_FEEDBACK_BUFFER 0x8C8E
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#define GL_ATOMIC_COUNTER_BUFFER 0x92C0
#define GL_DRAW_INDIRECT_BUFFER 0x8F3F
#define GL_DISPATCH_INDIRECT_BUFFER 0x90EE
#define GL_TEXTURE_BUFFER_BINDING 0x8C2A
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_UNIFORM_BUFFER_BINDING 0x8A28
#define GL_UNIFORM_BUFFER_START 0x8A29
#define GL_UNIFORM_BUFFER_SIZE 0x8A2A
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define GL_TRANSFORM_FEEDBACK_BUFFER_BINDING 0x8C8F
#define GL_SHADER_STORAGE_BUFFER_BINDING 0x90D3
#define GL_ATOMIC_COUNTER_BUFFER_BINDING 0x92C1
#define GL_DRAW_INDIRECT_BUFFER_BINDING 0x8F43
#define GL_DISPATCH_INDIRECT_BUFFER_BINDING 0x90EF
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_RANGE_BIT 0x0004
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_MAP_FLUSH_EXPLICIT_BIT 0x0010
#define GL_MAP_PERSISTENT_BIT_EXT 0x0040
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_ACTIVE_ATTRIBUTES 0x8B89
#define GL_ACTIVE_UNIFORM_BLOCKS 0x8A36
#define GL_UNIFORM_BLOCK_INDEX 0x8A3A
#define GL_UNIFORM_BLOCK_BINDING 0x8A3F
#define GL_UNIFORM_BLOCK_DATA_SIZE 0x8A40
#define GL_ACTIVE_ATOMIC_COUNTER_BUFFERS 0x92D9
#define GL_SHADER_STORAGE_BLOCK 0x92E6
#define GL_ACTIVE_RESOURCES 0x92F5
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED 0x8622
#define GL_VERTEX_ATTRIB_ARRAY_SIZE 0x8623
#define GL_VERTEX_ATTRIB_ARRAY_STRIDE 0x8624
#define GL_VERTEX_ATTRIB_ARRAY_TYPE 0x8625
#define GL_VERTEX_ATTRIB_ARRAY_NORMALIZED 0x886A
#define GL_VERTEX_ATTRIB_ARRAY_POINTER 0x8645
#define GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING 0x889F
#define GL_VERTEX_ATTRIB_ARRAY_INTEGER 0x88FD
#define GL_VERTEX_ATTRIB_ARRAY_DIVISOR 0x88FE
#define GL_CURRENT_VERTEX_ATTRIB 0x8626
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_SKIP_ROWS 0x0D03
#define GL_PACK_SKIP_PIXELS 0x0D04
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400

static int g_on, g_check, g_ready, g_srgb_ctl, g_es31, g_es32;
static int g_debug, g_debug_lines;          // UA_GL_MEMO_DEBUG=1: why draws were not left out (the first 200)
static int g_debug_w, g_debug_h;            // UA_GL_MEMO_DEBUG=<w>x<h>: only draws with that viewport
static pthread_t g_thread;
static int g_have_thread;
static long g_frame;
static long g_left_out, g_check_same, g_check_diff, g_check_unread, g_keys;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;   // (the shadows: uploads may come from other threads)

// (the render thread, its context an OpenGL ES 3 one: what draws and framebuffers are followed on)
static int mine(void) { return g_ready && g_have_thread && pthread_equal(pthread_self(), g_thread); }

// ---- the driver (or the next host wrapper) and the queries
static void (*q_GetIntegerv)(GLenum, GLint *);
static void (*q_GetFloatv)(GLenum, float *);
static void (*q_GetBooleanv)(GLenum, GLboolean *);
static GLboolean (*q_IsEnabled)(GLenum);
static GLenum (*q_GetError)(void);
static const unsigned char *(*q_GetString)(GLenum);
static void (*q_GetFbAttachment)(GLenum, GLenum, GLenum, GLint *);
static void (*q_GetProgramiv)(GLuint, GLenum, GLint *);
static void (*q_GetActiveUniform)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *);
static void (*q_GetActiveUniformsiv)(GLuint, GLsizei, const GLuint *, GLenum, GLint *);
static GLint (*q_GetUniformLocation)(GLuint, const char *);
static void (*q_GetUniformfv)(GLuint, GLint, float *);
static void (*q_GetUniformiv)(GLuint, GLint, GLint *);
static void (*q_GetUniformuiv)(GLuint, GLint, GLuint *);
static void (*q_GetActiveAttrib)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *);
static GLint (*q_GetAttribLocation)(GLuint, const char *);
static void (*q_GetActiveUniformBlockiv)(GLuint, GLuint, GLenum, GLint *);
static void (*q_GetIntegeri_v)(GLenum, GLuint, GLint *);
static void (*q_GetInteger64i_v)(GLenum, GLuint, GLint64 *);
static void (*q_GetProgramInterfaceiv)(GLuint, GLenum, GLenum, GLint *);
static void (*q_GetVertexAttribiv)(GLuint, GLenum, GLint *);
static void (*q_GetVertexAttribfv)(GLuint, GLenum, float *);
static void (*q_GetVertexAttribPointerv)(GLuint, GLenum, void **);
static void (*q_GetQueryiv)(GLenum, GLenum, GLint *);
static void (*q_GetTexParameteriv)(GLenum, GLenum, GLint *);
static void (*q_GetTexParameterfv)(GLenum, GLenum, float *);
static void (*q_GetSamplerParameteriv)(GLuint, GLenum, GLint *);
static void (*q_GetSamplerParameterfv)(GLuint, GLenum, float *);
static void (*s_ActiveTexture)(GLenum);
static void (*s_BindFramebufferRaw)(GLenum, GLuint);
static void (*s_BindBufferRaw)(GLenum, GLuint);
static void (*s_PixelStorei)(GLenum, GLint);
static void (*s_ReadPixelsRaw)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);

static void drain(void) { for (int i = 0; q_GetError && i < 16 && q_GetError() != GL_NO_ERROR; i++) ; }

// ---- generations: every write into a texture, buffer, sampler or program bumps its count
#define MAX_NAME 65536
static uint32_t *g_tex_gen, *g_buf_gen, *g_smp_gen, *g_prog_gen;
static uint32_t *g_tpar_gen;                   // a texture's sampling parameters (how it is read, not what it holds)
static unsigned char *g_tex_vol, *g_buf_vol;    // written where this cannot follow: never an input
static void bump(uint32_t *gen, GLuint n) { if (gen && n < MAX_NAME) __atomic_add_fetch(&gen[n], 1, __ATOMIC_RELAXED); }
static uint32_t gen_of(const uint32_t *gen, GLuint n) { return n < MAX_NAME ? __atomic_load_n(&gen[n], __ATOMIC_RELAXED) : 0; }

// ---- buffer shadows: the bytes of small buffers the game writes more than once
#define SHADOW_MAX (64 * 1024)
#define SHADOW_SMALL 4096                          // small buffers: shadowed from their first write
#define SHADOW_TOTAL (8 * 1024 * 1024)
#define CHUNK 16                                   // validity per 16 bytes (std140's vec4)
typedef struct { unsigned char *data, *valid; uint32_t size; } shadow_t;
static shadow_t **g_shadow;
static uint32_t *g_buf_size;
static unsigned char *g_buf_writes;
static unsigned char *g_buf_wanted;                // read by a keyed draw: its mapped writes are copied too
static size_t g_shadow_bytes;
typedef struct { GLuint buf; GLintptr off; GLsizeiptr len; GLbitfield access; unsigned char *ptr; } mapping_t;
#define MAX_MAP 32
static mapping_t g_map[MAX_MAP];

static void shadow_free(GLuint b)
{
    shadow_t *s = g_shadow[b];
    if (!s) return;
    g_shadow_bytes -= s->size;
    free(s->data); free(s->valid); free(s);
    g_shadow[b] = NULL;
}
static void shadow_invalidate(shadow_t *s, uint64_t off, uint64_t len)
{
    if (!s || off >= s->size) return;
    if (off + len > s->size) len = s->size - off;
    for (uint64_t c = off / CHUNK; c * CHUNK < off + len; c++) s->valid[c] = 0;
}
static void shadow_write(shadow_t *s, uint64_t off, uint64_t len, const void *src)
{
    if (!s) return;
    if (off >= s->size || !src) { shadow_invalidate(s, off, len); return; }
    if (off + len > s->size) len = s->size - off;
    memcpy(s->data + off, src, len);
    for (uint64_t c = off / CHUNK; c * CHUNK < off + len; c++) {
        uint64_t end = (c + 1) * CHUNK < s->size ? (c + 1) * CHUNK : s->size;
        if (c * CHUNK >= off && end <= off + len) s->valid[c] = 1;              // (a chunk half written keeps its state)
    }
}
static int shadow_valid(const shadow_t *s, uint64_t off, uint64_t len)
{
    if (!s || off + len > s->size || len == 0) return 0;
    for (uint64_t c = off / CHUNK; c * CHUNK < off + len; c++) if (!s->valid[c]) return 0;
    return 1;
}
// a write of buffer b (through target t) is about to happen: its generation, and its shadow from the second write on;
// small buffers and uniform data from the first (Unity 2021's GLES backend writes the per-draw constants of a draw into
// a new 656-byte buffer, through the array buffer binding, and binds it as a uniform buffer afterwards)
static shadow_t *buffer_written(GLuint b, GLenum t)
{
    if (!b || b >= MAX_NAME) return NULL;
    bump(g_buf_gen, b);
    if (t == GL_UNIFORM_BUFFER || (g_buf_size[b] && g_buf_size[b] <= SHADOW_SMALL)) g_buf_writes[b] = 2;
    else if (g_buf_writes[b] < 2) g_buf_writes[b]++;
    shadow_t *s = g_shadow[b];
    uint32_t size = g_buf_size[b];
    if (!s && g_buf_writes[b] >= 2 && size && size <= SHADOW_MAX && g_shadow_bytes + size <= SHADOW_TOTAL) {
        s = calloc(1, sizeof *s);
        if (s) { s->data = malloc(size); s->valid = calloc(size / CHUNK + 1, 1); s->size = size; }
        if (!s || !s->data || !s->valid) { if (s) { free(s->data); free(s->valid); free(s); } return NULL; }
        g_shadow[b] = s;
        g_shadow_bytes += size;
    }
    return s;
}
// a mapped write of buffer b: copied into its shadow only when a keyed draw reads b and b is small (the mapping is
// write-combined memory, slow to read: Hollow Knight's sprite batches flushed through mappings every frame cost its
// render thread a tenth of its time when they were copied); otherwise its count is bumped and its shadow forgets it
static int mapped_copy(GLuint b) { return g_buf_wanted[b] && g_buf_size[b] && g_buf_size[b] <= SHADOW_SMALL; }
static shadow_t *buffer_bumped(GLuint b)
{
    if (!b || b >= MAX_NAME) return NULL;
    bump(g_buf_gen, b);
    if (g_buf_writes[b] < 2) g_buf_writes[b]++;
    return g_shadow[b];
}
static mapping_t *mapping(GLuint b) { for (int i = 0; i < MAX_MAP; i++) if (g_map[i].buf == b && b) return &g_map[i]; return NULL; }

// ---- framebuffers: what is attached (looked up while bound, again after a change)
#define MAX_FB 4096
#define MAX_ATT 6
typedef struct {
    unsigned char known, candidate;
    GLuint tex[MAX_ATT]; int ntex;          // the textures attached (their generations bump at a write)
    GLuint colour;                         // a candidate's one texture
    GLint comp;                            // its component type (check mode's read back)
} fbinfo;
static fbinfo g_fb[MAX_FB];
static GLuint g_draw_fb, g_read_fb;

static fbinfo *info(GLuint fb)
{
    if (fb == 0 || fb >= MAX_FB) return NULL;
    fbinfo *f = &g_fb[fb];
    if (f->known) return f;
    *f = (fbinfo){ .known = 1 };
    GLint rfb = 0;
    q_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    GLenum t = GL_DRAW_FRAMEBUFFER;
    GLint dfb = 0;
    q_GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
    if ((GLuint)dfb != fb) { if ((GLuint)rfb == fb) t = GL_READ_FRAMEBUFFER; else { f->known = 0; return NULL; } }
    static const GLenum atts[MAX_ATT] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0 + 1, GL_COLOR_ATTACHMENT0 + 2,
                                          GL_COLOR_ATTACHMENT0 + 3, GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT };
    int others = 0, colour_ok = 0;
    for (int i = 0; i < MAX_ATT; i++) {
        GLint type = 0, name = 0;
        q_GetFbAttachment(t, atts[i], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        if (type == GL_NONE) continue;
        if (i) others = 1;
        if (type != GL_TEXTURE) continue;
        q_GetFbAttachment(t, atts[i], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
        if (name > 0 && f->ntex < MAX_ATT) f->tex[f->ntex++] = (GLuint)name;
        if (i == 0 && name > 0 && name < MAX_NAME) {
            GLint level = -1;
            q_GetFbAttachment(t, atts[i], GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
            q_GetFbAttachment(t, atts[i], GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &f->comp);
            colour_ok = level == 0;
            f->colour = (GLuint)name;
        }
    }
    GLint db = GL_NONE, sb = 0;
    if (t == GL_DRAW_FRAMEBUFFER) {
        q_GetIntegerv(GL_DRAW_BUFFER0, &db);
        for (int i = 1; i < 4 && !others; i++) { GLint d = GL_NONE; q_GetIntegerv(GL_DRAW_BUFFER0 + i, &d); if (d != GL_NONE) others = 1; }
        q_GetIntegerv(GL_SAMPLE_BUFFERS, &sb);
    }
    f->candidate = t == GL_DRAW_FRAMEBUFFER && colour_ok && !others && db == GL_COLOR_ATTACHMENT0 && sb == 0 && !g_tex_vol[f->colour];
    return f;
}
static void forget_fb(GLuint fb) { if (fb && fb < MAX_FB) g_fb[fb].known = 0; }

// ---- per texture: the key of the draw that last wrote it, and its generation after that draw
typedef struct {
    GLuint tex;
    int valid;
    uint32_t gen_after;
    unsigned char *key; size_t len, cap;
    int fails; long retry, used, hits, keyed;
    unsigned char *pixels; size_t npix; GLint box[4]; int logged;   // check mode: what that draw wrote
    int debug;                                                       // (UA_GL_MEMO_DEBUG lines for it)
} memo_t;
#define MAX_MEMO 64
static memo_t g_memo[MAX_MEMO];

static memo_t *memo_find(GLuint t, int add)
{
    memo_t *free_slot = NULL, *old = NULL;
    for (int i = 0; i < MAX_MEMO; i++) {
        if (g_memo[i].tex == t && t) { g_memo[i].used = g_frame; return &g_memo[i]; }
        if (!g_memo[i].tex && !free_slot) free_slot = &g_memo[i];
        if (!old || g_memo[i].used < old->used) old = &g_memo[i];
    }
    if (!add) return NULL;
    memo_t *m = free_slot ? free_slot : old;
    unsigned char *k = m->key, *p = m->pixels; size_t cap = m->cap;
    *m = (memo_t){ .tex = t, .key = k, .cap = cap, .pixels = p, .used = g_frame };
    return m;
}
static void memo_drop(GLuint t)
{
    memo_t *m = memo_find(t, 0);
    if (m) { m->valid = 0; m->fails = 0; m->retry = 0; m->logged = 0; }
}

// ---- the pass's held-back invalidate
static struct { int on; GLenum target; GLsizei n; GLenum att[8]; GLuint tex; int discard; } g_def;
// the pass on the draw framebuffer: commands that wrote pixels since it was bound, and whether the first was an
// invalidate of its colour attachment (only a draw right after that is ever left out, or keyed)
static int g_pass_ops, g_pass_inval;
static void (*r_Invalidate)(GLenum, GLsizei, const GLenum *);
static void (*r_Discard)(GLenum, GLsizei, const GLenum *);
static void tex_written(GLuint t);
static void flush_deferred(void)
{
    if (!g_def.on) return;
    g_def.on = 0;
    if (g_def.discard) r_Discard(g_def.target, g_def.n, g_def.att); else r_Invalidate(g_def.target, g_def.n, g_def.att);
    tex_written(g_def.tex);
}
static void drop_deferred(void) { g_def.on = 0; }

static void tex_written(GLuint t)
{
    if (!t) return;
    if (g_def.on && g_def.tex == t && mine()) flush_deferred();     // (the invalidate came first)
    bump(g_tex_gen, t);
}
static void fb_written(GLuint fb)
{
    fbinfo *f = info(fb);
    if (!f) return;
    for (int i = 0; i < f->ntex; i++) tex_written(f->tex[i]);
}

// ---- programs: what a draw's key reads of them (built once per link)
typedef struct { GLint loc; GLenum type; } uelem;
typedef struct {
    uint32_t gen; int ok;
    uelem *u; int nu;                       // default-block uniforms, one entry per array element
    int nblocks; GLint block_size[16];
    GLint attr[16]; int nattr;
} pinfo;
static pinfo **g_pinfo;

static int comps(GLenum t, int *kind, GLenum *binding)
{
    // kind: 0 float, 1 int, 2 uint, 3 sampler; the binding a sampler's texture is found under
    *binding = 0;
    switch (t) {
    case 0x1406: *kind = 0; return 1;          // float
    case 0x8B50: *kind = 0; return 2;          // vec2
    case 0x8B51: *kind = 0; return 3;
    case 0x8B52: *kind = 0; return 4;
    case 0x8B5A: *kind = 0; return 4;          // mat2
    case 0x8B5B: *kind = 0; return 9;          // mat3
    case 0x8B5C: *kind = 0; return 16;         // mat4
    case 0x8B65: case 0x8B67: *kind = 0; return 6;    // mat2x3, mat3x2
    case 0x8B66: case 0x8B69: *kind = 0; return 8;    // mat2x4, mat4x2
    case 0x8B68: case 0x8B6A: *kind = 0; return 12;   // mat3x4, mat4x3
    case 0x1404: case 0x8B56: *kind = 1; return 1;    // int, bool
    case 0x8B53: case 0x8B57: *kind = 1; return 2;
    case 0x8B54: case 0x8B58: *kind = 1; return 3;
    case 0x8B55: case 0x8B59: *kind = 1; return 4;
    case 0x1405: *kind = 2; return 1;          // uint
    case 0x8DC6: *kind = 2; return 2;
    case 0x8DC7: *kind = 2; return 3;
    case 0x8DC8: *kind = 2; return 4;
    case 0x8B5E: case 0x8B62: case 0x8DCA: case 0x8DD2: *kind = 3; *binding = GL_TEXTURE_BINDING_2D; return 1;
    case 0x8B5F: case 0x8DCB: case 0x8DD3: *kind = 3; *binding = GL_TEXTURE_BINDING_3D; return 1;
    case 0x8B60: case 0x8DC5: case 0x8DCC: case 0x8DD4: *kind = 3; *binding = GL_TEXTURE_BINDING_CUBE_MAP; return 1;
    case 0x8DC1: case 0x8DC4: case 0x8DCF: case 0x8DD7: *kind = 3; *binding = GL_TEXTURE_BINDING_2D_ARRAY; return 1;
    case 0x9108: case 0x9109: case 0x910A: *kind = 3; *binding = GL_TEXTURE_BINDING_2D_MULTISAMPLE; return 1;
    case 0x900C: case 0x900D: case 0x900E: case 0x900F: *kind = 3; *binding = GL_TEXTURE_BINDING_CUBE_MAP_ARRAY; return 1;
    default: return 0;                         // images, external and buffer samplers, anything else: not followed
    }
}

static pinfo *program(GLuint p)
{
    if (!p || p >= MAX_NAME) return NULL;
    pinfo *pi = g_pinfo[p];
    uint32_t gen = gen_of(g_prog_gen, p);
    if (pi && pi->gen == gen) return pi->ok ? pi : NULL;
    if (pi) { free(pi->u); free(pi); }
    pi = calloc(1, sizeof *pi);
    g_pinfo[p] = pi;
    if (!pi) return NULL;
    pi->gen = gen;
    GLint nu = 0, na = 0, nb = 0, n = 0;
    q_GetProgramiv(p, GL_ACTIVE_UNIFORMS, &nu);
    q_GetProgramiv(p, GL_ACTIVE_ATTRIBUTES, &na);
    q_GetProgramiv(p, GL_ACTIVE_UNIFORM_BLOCKS, &nb);
    int ok = nu >= 0 && nu < 512 && na >= 0 && na <= 16 && nb >= 0 && nb <= 16;
    if (ok && g_es31) { q_GetProgramiv(p, GL_ACTIVE_ATOMIC_COUNTER_BUFFERS, &n); if (n) ok = 0; }
    if (ok && g_es31 && q_GetProgramInterfaceiv) { n = 0; q_GetProgramInterfaceiv(p, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES, &n); if (n) ok = 0; }
    int cap = 0;
    for (GLint i = 0; ok && i < nu; i++) {
        char name[256]; GLsizei len = 0; GLint size = 0; GLenum type = 0; GLint block = -1; GLuint idx = (GLuint)i;
        q_GetActiveUniform(p, idx, sizeof name, &len, &size, &type, name);
        q_GetActiveUniformsiv(p, 1, &idx, GL_UNIFORM_BLOCK_INDEX, &block);
        if (block != -1) continue;                      // (a uniform block's member: its buffer is in the key)
        int kind; GLenum b;
        if (!comps(type, &kind, &b) || size < 1 || size > 1024 || len <= 0 || len >= (GLsizei)sizeof name - 8) { ok = 0; break; }
        char *br = strchr(name, '[');
        if (br) *br = 0;
        for (GLint e = 0; e < size; e++) {
            char el[272];
            if (size > 1 || br) snprintf(el, sizeof el, "%s[%d]", name, e); else snprintf(el, sizeof el, "%s", name);
            GLint loc = q_GetUniformLocation(p, el);
            if (loc < 0) { ok = 0; break; }
            if (pi->nu == cap) {
                cap = cap ? cap * 2 : 16;
                uelem *nw = realloc(pi->u, (size_t)cap * sizeof *nw);
                if (!nw) { ok = 0; break; }
                pi->u = nw;
            }
            pi->u[pi->nu++] = (uelem){ loc, type };
        }
    }
    for (GLint i = 0; ok && i < nb; i++) q_GetActiveUniformBlockiv(p, (GLuint)i, GL_UNIFORM_BLOCK_DATA_SIZE, &pi->block_size[i]);
    pi->nblocks = nb;
    for (GLint i = 0; ok && i < na; i++) {
        char name[256]; GLsizei len = 0; GLint size = 0; GLenum type = 0;
        q_GetActiveAttrib(p, (GLuint)i, sizeof name, &len, &size, &type, name);
        GLint loc = q_GetAttribLocation(p, name);
        if (loc >= 0 && pi->nattr < 16) pi->attr[pi->nattr++] = loc;   // (gl_VertexID and the like have none)
        if (size != 1) ok = 0;                          // (attribute arrays: not followed)
    }
    pi->ok = ok;
    return ok ? pi : NULL;
}

// ---- the key: everything a draw's pixels depend on
static unsigned char *g_kb; static size_t g_klen, g_kcap; static int g_kfail;
static size_t g_koff_uni, g_koff_blocks, g_koff_attrs;    // (where the key's parts begin: UA_GL_MEMO_DEBUG)
static void put(const void *p, size_t n)
{
    if (g_kfail) return;
    if (g_klen + n > g_kcap) {
        size_t c = g_kcap ? g_kcap : 4096;
        while (c < g_klen + n) c *= 2;
        if (c > (1u << 20)) { g_kfail = 1; return; }
        unsigned char *k = realloc(g_kb, c);
        if (!k) { g_kfail = 1; return; }
        g_kb = k; g_kcap = c;
    }
    memcpy(g_kb + g_klen, p, n);
    g_klen += n;
}
static void put_i(int64_t v) { put(&v, sizeof v); }

// a buffer range a draw reads: its bytes (shadowed buffers), else the buffer and its generation
static void put_range(GLuint b, uint64_t off, uint64_t len)
{
    if (!b || b >= MAX_NAME || g_buf_vol[b]) { g_kfail = 1; return; }
    pthread_mutex_lock(&g_lock);
    shadow_t *s = g_shadow[b];
    if (!s || !shadow_valid(s, off, len)) g_buf_wanted[b] = 1;
    if (s) {
        if (shadow_valid(s, off, len)) { put_i(-1); put_i((int64_t)len); put(s->data + off, len); }
        else g_kfail = 1;
    } else {
        put_i(b); put_i(gen_of(g_buf_gen, b)); put_i((int64_t)off); put_i((int64_t)len);
    }
    pthread_mutex_unlock(&g_lock);
}
static int shadowed(GLuint b) { return b && b < MAX_NAME && g_shadow[b] != NULL; }

typedef struct { int kind; GLenum mode; GLint first; GLsizei count; GLenum type; uintptr_t indices; GLint basevertex; } drawdesc;
enum { D_ARRAYS = 1, D_ELEMENTS, D_RANGE, D_BASEVERTEX, D_OTHER };

static int type_size(GLenum t) { return t == GL_UNSIGNED_BYTE ? 1 : t == GL_UNSIGNED_SHORT ? 2 : t == GL_UNSIGNED_INT ? 4 : 0; }
static int attr_bytes(GLint size, GLenum type)
{
    int b = type == 0x1400 || type == 0x1401 ? 1 : type == 0x1402 || type == 0x1403 || type == 0x140B || type == 0x8D61 ? 2 :
            type == 0x1404 || type == 0x1405 || type == 0x1406 || type == 0x140C || type == 0x8368 || type == 0x8D9F ? 4 : 0;
    if (type == 0x8368 || type == 0x8D9F) return 4;    // packed 2_10_10_10 formats: one word
    return b * size;
}

static int build_key(const drawdesc *d, const fbinfo *f)
{
    g_klen = 0; g_kfail = 0;
    g_keys++;
    put_i(d->kind); put_i(d->mode); put_i(d->first); put_i(d->count); put_i(d->type); put_i((int64_t)d->indices); put_i(d->basevertex);
    GLint prog = 0;
    q_GetIntegerv(GL_CURRENT_PROGRAM, &prog);
    pinfo *pi = program((GLuint)prog);
    if (!pi) return 0;
    put_i(prog); put_i(gen_of(g_prog_gen, (GLuint)prog));
    // fixed-function state (blending, rasterizer discard, transform feedback and queries are checked before)
    GLint v[4] = { 0, 0, 0, 0 }; float fr[2] = { 0, 1 }; GLboolean bm[4] = { 1, 1, 1, 1 };
    q_GetIntegerv(GL_VIEWPORT, v); put(v, sizeof v);
    int sc = q_IsEnabled(GL_SCISSOR_TEST);
    put_i(sc);
    if (sc) { q_GetIntegerv(GL_SCISSOR_BOX, v); put(v, sizeof v); }
    q_GetBooleanv(GL_COLOR_WRITEMASK, bm); put(bm, sizeof bm);
    put_i(q_IsEnabled(GL_DITHER));
    int cull = q_IsEnabled(GL_CULL_FACE);
    put_i(cull);
    v[0] = v[1] = 0;
    q_GetIntegerv(GL_CULL_FACE_MODE, &v[0]); q_GetIntegerv(GL_FRONT_FACE, &v[1]); put(v, 2 * sizeof *v);
    q_GetFloatv(GL_DEPTH_RANGE, fr); put(fr, sizeof fr);
    put_i(q_IsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE)); put_i(q_IsEnabled(GL_SAMPLE_COVERAGE));
    put_i(q_IsEnabled(GL_PRIMITIVE_RESTART_FIXED_INDEX));
    if (g_srgb_ctl) put_i(q_IsEnabled(GL_FRAMEBUFFER_SRGB_EXT));
    put_i(f->colour); put_i(f->comp);
    // uniforms, and the textures and samplers at the samplers' units
    GLint active = 0;
    q_GetIntegerv(GL_ACTIVE_TEXTURE, &active);
    g_koff_uni = g_klen;
    for (int i = 0; i < pi->nu && !g_kfail; i++) {
        int kind; GLenum binding;
        int n = comps(pi->u[i].type, &kind, &binding);
        union { float f[16]; GLint i[16]; GLuint u[16]; } val;
        memset(&val, 0, sizeof val);
        if (kind == 0) q_GetUniformfv((GLuint)prog, pi->u[i].loc, val.f);
        else if (kind == 2) q_GetUniformuiv((GLuint)prog, pi->u[i].loc, val.u);
        else q_GetUniformiv((GLuint)prog, pi->u[i].loc, val.i);
        put(&val, (size_t)n * 4);
        if (kind != 3) continue;
        GLint unit = val.i[0], tex = 0, smp = 0;
        if (unit < 0 || unit >= 96) { g_kfail = 1; break; }
        s_ActiveTexture(GL_TEXTURE0 + (GLenum)unit);
        q_GetIntegerv(binding, &tex);
        q_GetIntegerv(GL_SAMPLER_BINDING, &smp);
        if (tex <= 0 || tex >= MAX_NAME || g_tex_vol[tex] || (GLuint)tex == f->colour || smp < 0 || smp >= MAX_NAME) { g_kfail = 1; break; }
        put_i(tex); put_i(gen_of(g_tex_gen, (GLuint)tex)); put_i(gen_of(g_tpar_gen, (GLuint)tex));
        put_i(smp); put_i(smp ? gen_of(g_smp_gen, (GLuint)smp) : 0);
    }
    s_ActiveTexture((GLenum)active);
    // uniform blocks: the bytes of their ranges
    g_koff_blocks = g_klen;
    for (int i = 0; i < pi->nblocks && !g_kfail; i++) {
        GLint bind = 0, buf = 0;
        GLint64 start = 0, size = 0;
        q_GetActiveUniformBlockiv((GLuint)prog, (GLuint)i, GL_UNIFORM_BLOCK_BINDING, &bind);
        q_GetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, (GLuint)bind, &buf);
        q_GetInteger64i_v(GL_UNIFORM_BUFFER_START, (GLuint)bind, &start);
        q_GetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, (GLuint)bind, &size);
        if (buf <= 0 || buf >= MAX_NAME || start < 0) { g_kfail = 1; break; }
        uint64_t len = (uint64_t)pi->block_size[i];
        if (size > 0 && (uint64_t)size < len) len = (uint64_t)size;
        put_i(bind);
        put_range((GLuint)buf, (uint64_t)start, len);
    }
    // vertex attributes and indices
    g_koff_attrs = g_klen;
    GLint eb = 0;
    int64_t vmin = d->first, vmax = (int64_t)d->first + d->count - 1;
    if (d->kind != D_ARRAYS) {
        q_GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eb);
        int is = type_size(d->type);
        if (eb <= 0 || !is) { g_kfail = 1; return 0; }
        put_range((GLuint)eb, d->indices, (uint64_t)d->count * (uint64_t)is);
        vmin = -1; vmax = -1;
        pthread_mutex_lock(&g_lock);
        shadow_t *s = g_shadow[eb];
        if (s && !g_kfail && shadow_valid(s, d->indices, (uint64_t)d->count * (uint64_t)is)) {
            const unsigned char *p = s->data + d->indices;
            for (GLsizei k = 0; k < d->count; k++) {
                uint16_t x16 = 0; uint32_t x32 = 0;
                if (is == 2) memcpy(&x16, p + 2 * k, 2);
                if (is == 4) memcpy(&x32, p + 4 * k, 4);
                int64_t x = is == 1 ? p[k] : is == 2 ? x16 : x32;
                if (vmin < 0 || x < vmin) vmin = x;
                if (x > vmax) vmax = x;
            }
            vmin += d->basevertex; vmax += d->basevertex;
        }
        pthread_mutex_unlock(&g_lock);
    }
    for (int i = 0; i < pi->nattr && !g_kfail; i++) {
        GLuint loc = (GLuint)pi->attr[i];
        GLint en = 0;
        q_GetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en);
        put_i(loc); put_i(en);
        if (!en) { float c[4] = { 0, 0, 0, 0 }; q_GetVertexAttribfv(loc, GL_CURRENT_VERTEX_ATTRIB, c); put(c, sizeof c); continue; }
        GLint a[7] = { 0 };
        void *ptr = NULL;
        static const GLenum q[7] = { GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, GL_VERTEX_ATTRIB_ARRAY_SIZE, GL_VERTEX_ATTRIB_ARRAY_TYPE,
                                     GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, GL_VERTEX_ATTRIB_ARRAY_INTEGER, GL_VERTEX_ATTRIB_ARRAY_STRIDE,
                                     GL_VERTEX_ATTRIB_ARRAY_DIVISOR };
        for (int k = 0; k < 7; k++) q_GetVertexAttribiv(loc, q[k], &a[k]);
        q_GetVertexAttribPointerv(loc, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
        put(a + 1, 6 * sizeof *a);
        int eb_bytes = attr_bytes(a[1], (GLenum)a[2]);
        if (a[0] <= 0 || !eb_bytes) { g_kfail = 1; break; }
        uint64_t off = (uintptr_t)ptr, stride = a[5] ? (uint64_t)a[5] : (uint64_t)eb_bytes;
        if (shadowed((GLuint)a[0])) {
            int64_t lo = a[6] ? 0 : vmin, hi = a[6] ? 0 : vmax;    // (an instanced attribute: instance 0)
            if (lo < 0 || hi < lo) { g_kfail = 1; break; }
            put_range((GLuint)a[0], off + (uint64_t)lo * stride, (uint64_t)(hi - lo) * stride + (uint64_t)eb_bytes);
        } else {
            put_i(off); put_range((GLuint)a[0], 0, 0);
        }
    }
    return !g_kfail;
}

// ---- check mode: the target's pixels in the draw's viewport
static unsigned char *read_target(const fbinfo *f, GLint box[4], size_t *n)
{
    *n = 0;
    GLenum type = f->comp == GL_FLOAT ? GL_FLOAT : f->comp == GL_UNSIGNED_NORMALIZED || f->comp == 0 ? GL_UNSIGNED_BYTE : 0;
    if (!type || !s_ReadPixelsRaw || !s_PixelStorei || !s_BindFramebufferRaw) return NULL;
    q_GetIntegerv(GL_VIEWPORT, box);
    if (box[2] <= 0 || box[3] <= 0 || box[2] > 8192 || box[3] > 8192) return NULL;
    size_t bytes = (size_t)box[2] * (size_t)box[3] * (type == GL_FLOAT ? 16 : 4);
    unsigned char *p = malloc(bytes);
    if (!p) return NULL;
    GLint rfb = 0, pbo = 0, al = 4, rl = 0, sr = 0, sp = 0;
    q_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    q_GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    q_GetIntegerv(GL_PACK_ALIGNMENT, &al); q_GetIntegerv(GL_PACK_ROW_LENGTH, &rl);
    q_GetIntegerv(GL_PACK_SKIP_ROWS, &sr); q_GetIntegerv(GL_PACK_SKIP_PIXELS, &sp);
    s_BindFramebufferRaw(GL_READ_FRAMEBUFFER, g_draw_fb);
    if (pbo) s_BindBufferRaw(GL_PIXEL_PACK_BUFFER, 0);
    s_PixelStorei(GL_PACK_ALIGNMENT, 4); s_PixelStorei(GL_PACK_ROW_LENGTH, 0);
    s_PixelStorei(GL_PACK_SKIP_ROWS, 0); s_PixelStorei(GL_PACK_SKIP_PIXELS, 0);
    drain();
    s_ReadPixelsRaw(box[0], box[1], box[2], box[3], GL_RGBA, type, p);
    int err = q_GetError() != GL_NO_ERROR;
    s_PixelStorei(GL_PACK_ALIGNMENT, al); s_PixelStorei(GL_PACK_ROW_LENGTH, rl);
    s_PixelStorei(GL_PACK_SKIP_ROWS, sr); s_PixelStorei(GL_PACK_SKIP_PIXELS, sp);
    if (pbo) s_BindBufferRaw(GL_PIXEL_PACK_BUFFER, (GLuint)pbo);
    s_BindFramebufferRaw(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    drain();
    if (err) { free(p); return NULL; }
    *n = bytes;
    return p;
}

// ---- draws
static memo_t *g_cur_memo;                   // the memo of the draw being made
static int g_cur_key, g_cur_match, g_cur_stale;

static int qualifies(const drawdesc *d)
{
    if (d->kind == D_OTHER || d->count <= 0) return 0;
    if (d->mode != GL_TRIANGLES && d->mode != GL_TRIANGLE_STRIP && d->mode != GL_TRIANGLE_FAN) return 0;
    if (q_IsEnabled(GL_BLEND) || q_IsEnabled(GL_RASTERIZER_DISCARD)) return 0;
    GLboolean tf = 0;
    q_GetBooleanv(GL_TRANSFORM_FEEDBACK_ACTIVE, &tf);
    if (tf) return 0;
    GLint qy = 0;
    q_GetQueryiv(GL_ANY_SAMPLES_PASSED, GL_CURRENT_QUERY, &qy); if (qy) return 0;
    q_GetQueryiv(GL_ANY_SAMPLES_PASSED_CONSERVATIVE, GL_CURRENT_QUERY, &qy); if (qy) return 0;
    if (g_es32) { q_GetQueryiv(GL_PRIMITIVES_GENERATED, GL_CURRENT_QUERY, &qy); if (qy) return 0; }
    return 1;
}

// before a draw: 1 = leave it out
static int draw_begin(const drawdesc *d)
{
    g_cur_memo = NULL; g_cur_key = 0; g_cur_match = 0;
    if (!mine()) return 0;
    int ops = g_pass_ops++, first = ops == 0 && g_pass_inval;
    fbinfo *f = first ? info(g_draw_fb) : NULL;
    int ok = f && f->candidate && !g_tex_vol[f->colour] && qualifies(d);
    if (g_debug && !ok && g_debug_lines < 200) {             // (UA_GL_MEMO_DEBUG: a draw into a texture with a key, not keyed)
        fbinfo *df = info(g_draw_fb);
        memo_t *dm = df && df->colour ? memo_find(df->colour, 0) : NULL;
        if (dm && dm->debug) {
            g_debug_lines++;
            fprintf(stderr, "[glmemo] debug: fb%u texture %u frame %ld: a draw not keyed (%s)\n", g_draw_fb, df->colour, g_frame,
                    ops ? "not the pass's first command" : !g_pass_inval ? "the pass did not begin with an invalidate" :
                    !df->candidate ? "not a framebuffer with one texture" : "its state");
        }
    }
    if (ok) {
        memo_t *m = memo_find(f->colour, 1);
        if (m->retry <= g_frame) {
            uint32_t gen = gen_of(g_tex_gen, f->colour);
            g_cur_memo = m;
            g_cur_key = build_key(d, f);
            int same = g_cur_key && m->valid && m->gen_after == gen && m->len == g_klen && !memcmp(m->key, g_kb, g_klen);
            g_cur_stale = m->valid && m->gen_after != gen;
            // (left out only with the pass's invalidate held back: the draw then has the pass to itself, as the one it matches)
            g_cur_match = same && g_def.on && g_def.tex == f->colour;
            if (g_cur_match && !g_check) {
                drop_deferred();
                m->fails = 0; m->hits++; g_left_out++;
                if (!m->logged) {
                    m->logged = 1;
                    GLint prog = 0, vp[4] = { 0, 0, 0, 0 };
                    q_GetIntegerv(GL_CURRENT_PROGRAM, &prog); q_GetIntegerv(GL_VIEWPORT, vp);
                    fprintf(stderr, "[glmemo] fb%u (texture %u, %dx%d): prog%d's draw left out from frame %ld on while it would write what "
                            "the texture holds\n", g_draw_fb, f->colour, vp[2], vp[3], prog, g_frame);
                }
                return 1;
            }
            // (check mode: what the texture holds now, before the draw that would be left out: the last one's output)
            if (g_cur_match) { free(m->pixels); m->pixels = read_target(f, m->box, &m->npix); }
        }
    }
    flush_deferred();
    return 0;
}

static void draw_end(void)
{
    if (!mine()) return;
    fb_written(g_draw_fb);
    memo_t *m = g_cur_memo;
    if (!m) return;
    fbinfo *f = info(g_draw_fb);
    if (g_cur_match) {                          // (check mode: drawn; did it write what the texture held before it?)
        size_t n = 0;
        GLint box[4];
        unsigned char *p = read_target(f, box, &n);
        if (p && m->pixels && n == m->npix && !memcmp(box, m->box, sizeof box)) {
            int same = !memcmp(p, m->pixels, n);
            if (!m->logged) {
                m->logged = 1;
                fprintf(stderr, "[glmemo] check: fb%u texture %u (%dx%d): the first draw that would be left out wrote %s pixels as the "
                        "texture held\n", g_draw_fb, f->colour, box[2], box[3], same ? "the same" : "other");
            }
            if (same) g_check_same++;
            else {
                if (g_check_diff++ < 8) {
                    size_t k = 0;
                    while (k < n && p[k] == m->pixels[k]) k++;
                    fprintf(stderr, "[glmemo] check: fb%u texture %u: the draw wrote other pixels than the texture held "
                            "(first at byte %zu of %zu)\n", g_draw_fb, f->colour, k, n);
                }
            }
        } else if (g_check_unread++ < 8)
            fprintf(stderr, "[glmemo] check: fb%u texture %u: the pixels could not be compared (read back after %s, before %s)\n",
                    g_draw_fb, f->colour, p ? "made" : "failed", m->pixels ? "made" : "failed");
        free(p);
        m->gen_after = gen_of(g_tex_gen, f->colour);
        m->fails = 0; m->hits++;
        return;
    }
    // another key than the one the texture's last draw left, or none, or the texture written since by something else while
    // its key is a frame old (a target other passes write every frame; one written during a wait for a retry is not)
    int failed = (m->valid && (!g_cur_stale || m->keyed >= g_frame - 1)) || !g_cur_key;
    if (failed) m->fails++;
    GLint dvp[4] = { 0, 0, 0, 0 };
    if (g_debug && g_debug_w) q_GetIntegerv(GL_VIEWPORT, dvp);
    m->debug = g_debug && (!g_debug_w || (dvp[2] == g_debug_w && dvp[3] == g_debug_h));
    if (m->debug && g_debug_lines < 200) {
        g_debug_lines++;
        size_t k = 0;
        while (m->valid && k < m->len && k < g_klen && m->key[k] == g_kb[k]) k++;
        int same = m->valid && g_cur_key && m->len == g_klen && k == g_klen;
        fprintf(stderr, "[glmemo] debug: fb%u texture %u frame %ld: %s (key %zu bytes, the last one's %zu, the same up to %zu)\n", g_draw_fb,
                f->colour, g_frame, !g_cur_key ? "no key" : !m->valid ? "first key" : g_cur_stale ? "written since its last key" :
                !same ? "another key" : "the same key, its invalidate not held", g_klen, m->valid ? m->len : 0, k);
        if (m->valid && g_cur_key && !same && g_debug_w) {    // (the keys from 16 bytes before they differ: which input it is)
            char a[200], b[200];
            size_t o = k >= 16 ? k - 16 : 0;
            for (size_t i = 0; i < 48; i++) {
                snprintf(a + 2 * i, 3, "%02x", o + i < m->len ? m->key[o + i] : 0);
                snprintf(b + 2 * i, 3, "%02x", o + i < g_klen ? g_kb[o + i] : 0);
            }
            fprintf(stderr, "[glmemo] debug:   at %zu: last %s\n[glmemo] debug:   at %zu: this %s (uniforms from %zu, blocks from %zu, "
                    "vertices from %zu)\n", o, a, o, b, g_koff_uni, g_koff_blocks, g_koff_attrs);
        }
    }
    m->valid = 0;
    if (g_cur_key) {
        if (m->cap < g_klen) {
            unsigned char *k = realloc(m->key, g_klen);
            if (!k) return;
            m->key = k; m->cap = g_klen;
        }
        memcpy(m->key, g_kb, g_klen);
        m->len = g_klen;
        m->gen_after = gen_of(g_tex_gen, f->colour);
        m->valid = 1;
        m->keyed = g_frame;
    }
    if (failed && m->fails >= 3) {              // keys that never repeat: try again later, and later
        int s = m->fails - 1 < 9 ? m->fails - 1 : 9;
        m->retry = g_frame + (1L << s);
    }
}

#define DRAW(f, args, desc, call) static void (*r_##f) args; \
    static void w_##f args { drawdesc d = desc; if (draw_begin(&d)) return; r_##f call; draw_end(); }
#define OTHER { .kind = D_OTHER }
DRAW(DrawArrays, (GLenum m, GLint a, GLsizei c), ((drawdesc){ D_ARRAYS, m, a, c, 0, 0, 0 }), (m, a, c))
DRAW(DrawElements, (GLenum m, GLsizei c, GLenum t, const void *i), ((drawdesc){ D_ELEMENTS, m, 0, c, t, (uintptr_t)i, 0 }), (m, c, t, i))
DRAW(DrawRangeElements, (GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i),
     ((drawdesc){ D_RANGE, m, 0, c, t, (uintptr_t)i, 0 }), (m, s, e, c, t, i))
DRAW(DrawElementsBaseVertex, (GLenum m, GLsizei c, GLenum t, const void *i, GLint b),
     ((drawdesc){ D_BASEVERTEX, m, 0, c, t, (uintptr_t)i, b }), (m, c, t, i, b))
DRAW(DrawElementsBaseVertexOES, (GLenum m, GLsizei c, GLenum t, const void *i, GLint b),
     ((drawdesc){ D_BASEVERTEX, m, 0, c, t, (uintptr_t)i, b }), (m, c, t, i, b))
DRAW(DrawElementsBaseVertexEXT, (GLenum m, GLsizei c, GLenum t, const void *i, GLint b),
     ((drawdesc){ D_BASEVERTEX, m, 0, c, t, (uintptr_t)i, b }), (m, c, t, i, b))
DRAW(DrawRangeElementsBaseVertex, (GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i, GLint b),
     ((drawdesc){ D_BASEVERTEX, m, 0, c, t, (uintptr_t)i, b }), (m, s, e, c, t, i, b))
DRAW(DrawRangeElementsBaseVertexOES, (GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i, GLint b),
     ((drawdesc){ D_BASEVERTEX, m, 0, c, t, (uintptr_t)i, b }), (m, s, e, c, t, i, b))
DRAW(DrawRangeElementsBaseVertexEXT, (GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i, GLint b),
     ((drawdesc){ D_BASEVERTEX, m, 0, c, t, (uintptr_t)i, b }), (m, s, e, c, t, i, b))
DRAW(DrawArraysInstanced, (GLenum m, GLint a, GLsizei c, GLsizei n), OTHER, (m, a, c, n))
DRAW(DrawElementsInstanced, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n), OTHER, (m, c, t, i, n))
DRAW(DrawArraysInstancedEXT, (GLenum m, GLint a, GLsizei c, GLsizei n), OTHER, (m, a, c, n))
DRAW(DrawElementsInstancedEXT, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n), OTHER, (m, c, t, i, n))
DRAW(DrawElementsInstancedBaseVertex, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint b), OTHER, (m, c, t, i, n, b))
DRAW(DrawElementsInstancedBaseVertexOES, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint b), OTHER, (m, c, t, i, n, b))
DRAW(DrawElementsInstancedBaseVertexEXT, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint b), OTHER, (m, c, t, i, n, b))
DRAW(DrawArraysInstancedBaseInstanceEXT, (GLenum m, GLint a, GLsizei c, GLsizei n, GLuint b), OTHER, (m, a, c, n, b))
DRAW(DrawElementsInstancedBaseInstanceEXT, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLuint b), OTHER, (m, c, t, i, n, b))
DRAW(DrawElementsInstancedBaseVertexBaseInstanceEXT, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint v, GLuint b),
     OTHER, (m, c, t, i, n, v, b))
DRAW(MultiDrawArraysEXT, (GLenum m, const GLint *a, const GLsizei *c, GLsizei n), OTHER, (m, a, c, n))
DRAW(MultiDrawElementsEXT, (GLenum m, const GLsizei *c, GLenum t, const void *const *i, GLsizei n), OTHER, (m, c, t, i, n))
DRAW(MultiDrawElementsBaseVertexEXT, (GLenum m, const GLsizei *c, GLenum t, const void *const *i, GLsizei n, const GLint *b),
     OTHER, (m, c, t, i, n, b))
DRAW(MultiDrawElementsBaseVertexOES, (GLenum m, const GLsizei *c, GLenum t, const void *const *i, GLsizei n, const GLint *b),
     OTHER, (m, c, t, i, n, b))
DRAW(DrawArraysIndirect, (GLenum m, const void *p), OTHER, (m, p))
DRAW(DrawElementsIndirect, (GLenum m, GLenum t, const void *p), OTHER, (m, t, p))

// ---- framebuffers: binding, attachments, clears, blits, invalidates
static void (*r_BindFramebuffer)(GLenum, GLuint);
static void w_BindFramebuffer(GLenum t, GLuint fb)
{
    if (mine() && (t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER) && fb != g_draw_fb) drop_deferred();   // (an empty pass)
    r_BindFramebuffer(t, fb);
    if (!mine()) return;
    if ((t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER) && fb != g_draw_fb) { g_draw_fb = fb; g_pass_ops = 0; g_pass_inval = 0; }
    if (t == GL_FRAMEBUFFER || t == GL_READ_FRAMEBUFFER) g_read_fb = fb;
}
static void attachments_changed(GLenum t)
{
    if (!mine()) return;
    GLuint fb = t == GL_READ_FRAMEBUFFER ? g_read_fb : g_draw_fb;
    if (fb == g_draw_fb) drop_deferred();
    forget_fb(fb);
}
static void (*r_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static void w_FramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l) { attachments_changed(t); r_FramebufferTexture2D(t, a, tt, tex, l); }
static void (*r_FramebufferTextureLayer)(GLenum, GLenum, GLuint, GLint, GLint);
static void w_FramebufferTextureLayer(GLenum t, GLenum a, GLuint tex, GLint l, GLint y) { attachments_changed(t); r_FramebufferTextureLayer(t, a, tex, l, y); }
static void (*r_FramebufferTexture)(GLenum, GLenum, GLuint, GLint);
static void w_FramebufferTexture(GLenum t, GLenum a, GLuint tex, GLint l) { attachments_changed(t); r_FramebufferTexture(t, a, tex, l); }
static void (*r_FramebufferTexture2DMS)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
static void w_FramebufferTexture2DMS(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l, GLsizei s)
{ attachments_changed(t); r_FramebufferTexture2DMS(t, a, tt, tex, l, s); }
static void (*r_FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static void w_FramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint rb) { attachments_changed(t); r_FramebufferRenderbuffer(t, a, rt, rb); }
static void (*r_DrawBuffers)(GLsizei, const GLenum *);
static void w_DrawBuffers(GLsizei n, const GLenum *b) { attachments_changed(GL_DRAW_FRAMEBUFFER); r_DrawBuffers(n, b); }
static void (*r_DeleteFramebuffers)(GLsizei, const GLuint *);
static void w_DeleteFramebuffers(GLsizei n, const GLuint *fbs)
{
    if (mine()) for (GLsizei i = 0; fbs && i < n; i++) { if (fbs[i] == g_draw_fb) drop_deferred(); forget_fb(fbs[i]); }
    r_DeleteFramebuffers(n, fbs);
}
static void (*r_Clear)(GLbitfield);
static void pixels_begin(void) { if (mine()) { flush_deferred(); g_pass_ops++; } }
static void w_Clear(GLbitfield m) { pixels_begin(); r_Clear(m); if (mine()) fb_written(g_draw_fb); }
static void (*r_ClearBufferfv)(GLenum, GLint, const float *);
static void w_ClearBufferfv(GLenum b, GLint d, const float *v) { pixels_begin(); r_ClearBufferfv(b, d, v); if (mine()) fb_written(g_draw_fb); }
static void (*r_ClearBufferiv)(GLenum, GLint, const GLint *);
static void w_ClearBufferiv(GLenum b, GLint d, const GLint *v) { pixels_begin(); r_ClearBufferiv(b, d, v); if (mine()) fb_written(g_draw_fb); }
static void (*r_ClearBufferuiv)(GLenum, GLint, const GLuint *);
static void w_ClearBufferuiv(GLenum b, GLint d, const GLuint *v) { pixels_begin(); r_ClearBufferuiv(b, d, v); if (mine()) fb_written(g_draw_fb); }
// (a float argument: on armhf the player calls with softfp, SO_ABI; the next wrapper of the name is called the same way, the
// driver with the host's own convention)
static void (*r_ClearBufferfi)(GLenum, GLint, float, GLint);
static void (SO_ABI *n_ClearBufferfi)(GLenum, GLint, float, GLint);
static SO_ABI void w_ClearBufferfi(GLenum b, GLint d, float v, GLint s)
{
    pixels_begin();
    if (n_ClearBufferfi) n_ClearBufferfi(b, d, v, s); else r_ClearBufferfi(b, d, v, s);
    if (mine()) fb_written(g_draw_fb);
}
static void (*r_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
static void w_BlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield m, GLenum fl)
{
    pixels_begin();
    r_BlitFramebuffer(a, b, c, d, e, f, g, h, m, fl);
    if (mine()) fb_written(g_draw_fb);
}
static int colour_only(GLsizei n, const GLenum *a, GLuint fb)
{
    if (n < 1 || n > 8 || !a) return 0;
    for (GLsizei i = 0; i < n; i++)
        if (a[i] != (fb ? GL_COLOR_ATTACHMENT0 : GL_COLOR) && a[i] != GL_DEPTH_ATTACHMENT && a[i] != GL_STENCIL_ATTACHMENT &&
            a[i] != GL_DEPTH_STENCIL_ATTACHMENT) return 0;
    return 1;
}
// the pass's first invalidate of a texture whose memo may let its draw be left out: held back
static int covers_colour(GLsizei n, const GLenum *a, GLuint fb)
{
    for (GLsizei i = 0; a && i < n; i++) if (a[i] == (fb ? GL_COLOR_ATTACHMENT0 : GL_COLOR)) return 1;
    return 0;
}
static int hold(GLenum t, GLsizei n, const GLenum *a, int discard)
{
    if (!mine() || (t != GL_FRAMEBUFFER && t != GL_DRAW_FRAMEBUFFER)) return 0;
    if (g_pass_ops == 0 && covers_colour(n, a, g_draw_fb)) g_pass_inval = 1;
    if (g_def.on || g_pass_ops) return 0;
    fbinfo *f = info(g_draw_fb);
    if (!f || !f->candidate || !colour_only(n, a, g_draw_fb)) return 0;
    memo_t *m = memo_find(f->colour, 0);
    if (!m || !m->valid || m->retry > g_frame || m->gen_after != gen_of(g_tex_gen, f->colour)) {
        if (m && m->debug && g_debug_lines < 200) {
            g_debug_lines++;
            fprintf(stderr, "[glmemo] debug: fb%u texture %u frame %ld: its invalidate sent (%s)\n", g_draw_fb, f->colour, g_frame,
                    !m->valid ? "no key" : m->retry > g_frame ? "keys tried again later" : "written since its last key");
        }
        return 0;
    }
    g_def.on = 1; g_def.target = t; g_def.n = n; g_def.tex = f->colour; g_def.discard = discard;
    memcpy(g_def.att, a, (size_t)n * sizeof *a);
    return 1;
}
static void invalidated(GLenum t)
{
    if (!mine()) return;
    fb_written(t == GL_READ_FRAMEBUFFER ? g_read_fb : g_draw_fb);
}
static void w_Invalidate(GLenum t, GLsizei n, const GLenum *a)
{
    if (hold(t, n, a, 0)) return;
    if (mine()) flush_deferred();         // (a second invalidate: the first one goes before it)
    r_Invalidate(t, n, a);
    invalidated(t);
}
static void (*r_InvalidateSub)(GLenum, GLsizei, const GLenum *, GLint, GLint, GLsizei, GLsizei);
static void w_InvalidateSub(GLenum t, GLsizei n, const GLenum *a, GLint x, GLint y, GLsizei w, GLsizei h)
{ if (mine()) flush_deferred(); r_InvalidateSub(t, n, a, x, y, w, h); invalidated(t); }
static void w_Discard(GLenum t, GLsizei n, const GLenum *a)
{
    if (hold(t, n, a, 1)) return;
    if (mine()) flush_deferred();
    r_Discard(t, n, a);
    invalidated(t);
}

// ---- textures
static GLenum binding_of(GLenum target)
{
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) return GL_TEXTURE_BINDING_CUBE_MAP;
    switch (target) {
    case GL_TEXTURE_2D: return GL_TEXTURE_BINDING_2D;
    case GL_TEXTURE_3D: return GL_TEXTURE_BINDING_3D;
    case GL_TEXTURE_2D_ARRAY: return GL_TEXTURE_BINDING_2D_ARRAY;
    case GL_TEXTURE_CUBE_MAP: return GL_TEXTURE_BINDING_CUBE_MAP;
    case GL_TEXTURE_2D_MULTISAMPLE: return GL_TEXTURE_BINDING_2D_MULTISAMPLE;
    case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: return GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY;
    case GL_TEXTURE_CUBE_MAP_ARRAY: return GL_TEXTURE_BINDING_CUBE_MAP_ARRAY;
    case GL_TEXTURE_EXTERNAL_OES: return GL_TEXTURE_BINDING_EXTERNAL_OES;
    case GL_TEXTURE_BUFFER: return GL_TEXTURE_BINDING_BUFFER;
    default: return 0;
    }
}
static GLuint bound_texture(GLenum target)
{
    GLenum b = binding_of(target);
    GLint t = 0;
    if (b) q_GetIntegerv(b, &t);
    return t > 0 ? (GLuint)t : 0;
}
static void bound_written(GLenum target) { if (g_on) tex_written(bound_texture(target)); }
static void bound_param(GLenum target) { if (g_on) bump(g_tpar_gen, bound_texture(target)); }
static void bound_volatile(GLenum target) { GLuint t = bound_texture(target); if (t && t < MAX_NAME) g_tex_vol[t] = 1; tex_written(t); }

#define TEXW(f, args, call, target) static void (*r_##f) args; static void w_##f args { bound_written(target); r_##f call; }
// (new storage: a new texture under the old name, e.g. a render texture pool's; its memo starts over)
static void respecified(GLenum target) { if (g_on && mine()) memo_drop(bound_texture(target)); }
#define TEXS(f, args, call, target) static void (*r_##f) args; \
    static void w_##f args { bound_written(target); respecified(target); r_##f call; }
TEXS(TexImage2D, (GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLint b, GLenum fm, GLenum ty, const void *p), (t, l, f, w, h, b, fm, ty, p), t)
TEXS(TexImage3D, (GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLsizei d, GLint b, GLenum fm, GLenum ty, const void *p),
     (t, l, f, w, h, d, b, fm, ty, p), t)
TEXW(TexSubImage2D, (GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *p), (t, l, x, y, w, h, f, ty, p), t)
TEXW(TexSubImage3D, (GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum f, GLenum ty, const void *p),
     (t, l, x, y, z, w, h, d, f, ty, p), t)
TEXS(CompressedTexImage2D, (GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei s, const void *p), (t, l, f, w, h, b, s, p), t)
TEXS(CompressedTexImage3D, (GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLsizei d, GLint b, GLsizei s, const void *p),
     (t, l, f, w, h, d, b, s, p), t)
TEXW(CompressedTexSubImage2D, (GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLsizei s, const void *p),
     (t, l, x, y, w, h, f, s, p), t)
TEXW(CompressedTexSubImage3D, (GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum f, GLsizei s, const void *p),
     (t, l, x, y, z, w, h, d, f, s, p), t)
TEXS(TexStorage2D, (GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h), (t, l, f, w, h), t)
TEXS(TexStorage3D, (GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h, GLsizei d), (t, l, f, w, h, d), t)
TEXS(TexStorage2DMultisample, (GLenum t, GLsizei s, GLenum f, GLsizei w, GLsizei h, GLboolean fx), (t, s, f, w, h, fx), t)
TEXW(CopyTexImage2D, (GLenum t, GLint l, GLenum f, GLint x, GLint y, GLsizei w, GLsizei h, GLint b), (t, l, f, x, y, w, h, b), t)
TEXW(CopyTexSubImage2D, (GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h), (t, l, xo, yo, x, y, w, h), t)
TEXW(CopyTexSubImage3D, (GLenum t, GLint l, GLint xo, GLint yo, GLint zo, GLint x, GLint y, GLsizei w, GLsizei h),
     (t, l, xo, yo, zo, x, y, w, h), t)
TEXW(GenerateMipmap, (GLenum t), (t), t)
static void (*r_EGLImageTargetTexture2DOES)(GLenum, void *);
static void w_EGLImageTargetTexture2DOES(GLenum t, void *img) { bound_volatile(t); r_EGLImageTargetTexture2DOES(t, img); }
static void (*r_TexBufferEXT)(GLenum, GLenum, GLuint);
static void w_TexBufferEXT(GLenum t, GLenum f, GLuint b) { bound_volatile(t); r_TexBufferEXT(t, f, b); }
static void (*r_TexBufferOES)(GLenum, GLenum, GLuint);
static void w_TexBufferOES(GLenum t, GLenum f, GLuint b) { bound_volatile(t); r_TexBufferOES(t, f, b); }
static void (*r_TexBuffer)(GLenum, GLenum, GLuint);
static void w_TexBuffer(GLenum t, GLenum f, GLuint b) { bound_volatile(t); r_TexBuffer(t, f, b); }
// parameters: a change of value is a write (Unity sets the same values again)
static int pcount(GLenum p) { return p == GL_TEXTURE_BORDER_COLOR ? 4 : 1; }
static void (*r_TexParameteri)(GLenum, GLenum, GLint);
static void w_TexParameteri(GLenum t, GLenum p, GLint v)
{
    GLint cur[4] = { 0 };
    if (g_on) { q_GetTexParameteriv(t, p, cur); if (cur[0] != v) bound_param(t); }
    r_TexParameteri(t, p, v);
}
static void (*r_TexParameteriv)(GLenum, GLenum, const GLint *);
static void w_TexParameteriv(GLenum t, GLenum p, const GLint *v)
{
    GLint cur[4] = { 0 };
    if (g_on) { q_GetTexParameteriv(t, p, cur); if (!v || memcmp(cur, v, (size_t)pcount(p) * sizeof *v)) bound_param(t); }
    r_TexParameteriv(t, p, v);
}
static void (*r_TexParameterfv)(GLenum, GLenum, const float *);
static void w_TexParameterfv(GLenum t, GLenum p, const float *v)
{
    float cur[4] = { 0 };
    if (g_on) { q_GetTexParameterfv(t, p, cur); if (!v || memcmp(cur, v, (size_t)pcount(p) * sizeof *v)) bound_param(t); }
    r_TexParameterfv(t, p, v);
}
static void (*r_TexParameterf)(GLenum, GLenum, float);
static void (SO_ABI *n_TexParameterf)(GLenum, GLenum, float);
static SO_ABI void w_TexParameterf(GLenum t, GLenum p, float v)
{
    float cur[4] = { 0 };
    if (g_on) { q_GetTexParameterfv(t, p, cur); if (cur[0] != v) bound_param(t); }
    if (n_TexParameterf) n_TexParameterf(t, p, v); else r_TexParameterf(t, p, v);
}
static void (*r_CopyImageSubData)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei);
static void w_CopyImageSubData(GLuint s, GLenum st, GLint sl, GLint sx, GLint sy, GLint sz, GLuint d, GLenum dt, GLint dl,
                               GLint dx, GLint dy, GLint dz, GLsizei w, GLsizei h, GLsizei dp)
{
    if (dt != GL_RENDERBUFFER && g_on) tex_written(d);
    r_CopyImageSubData(s, st, sl, sx, sy, sz, d, dt, dl, dx, dy, dz, w, h, dp);
}
static void (*r_DeleteTextures)(GLsizei, const GLuint *);
static void w_DeleteTextures(GLsizei n, const GLuint *t)
{
    for (GLsizei i = 0; g_on && t && i < n; i++) {
        if (mine() && g_def.on && g_def.tex == t[i]) drop_deferred();
        bump(g_tex_gen, t[i]); bump(g_tpar_gen, t[i]);
        if (mine()) {
            memo_drop(t[i]);
            for (int k = 1; k < MAX_FB; k++)              // (its framebuffers: looked up again)
                if (g_fb[k].known) for (int j = 0; j < g_fb[k].ntex; j++) if (g_fb[k].tex[j] == t[i]) g_fb[k].known = 0;
        }
    }
    r_DeleteTextures(n, t);
}
static void (*r_BindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
static void w_BindImageTexture(GLuint u, GLuint t, GLint l, GLboolean ly, GLint y, GLenum a, GLenum f)
{ if (g_on && t && t < MAX_NAME) { g_tex_vol[t] = 1; tex_written(t); } r_BindImageTexture(u, t, l, ly, y, a, f); }

// ---- samplers and programs
static void (*r_SamplerParameteri)(GLuint, GLenum, GLint);
static void w_SamplerParameteri(GLuint s, GLenum p, GLint v)
{
    GLint cur[4] = { 0 };
    if (g_on) { q_GetSamplerParameteriv(s, p, cur); if (cur[0] != v) bump(g_smp_gen, s); }
    r_SamplerParameteri(s, p, v);
}
static void (*r_SamplerParameteriv)(GLuint, GLenum, const GLint *);
static void w_SamplerParameteriv(GLuint s, GLenum p, const GLint *v)
{
    GLint cur[4] = { 0 };
    if (g_on) { q_GetSamplerParameteriv(s, p, cur); if (!v || memcmp(cur, v, (size_t)pcount(p) * sizeof *v)) bump(g_smp_gen, s); }
    r_SamplerParameteriv(s, p, v);
}
static void (*r_SamplerParameterfv)(GLuint, GLenum, const float *);
static void w_SamplerParameterfv(GLuint s, GLenum p, const float *v)
{
    float cur[4] = { 0 };
    if (g_on) { q_GetSamplerParameterfv(s, p, cur); if (!v || memcmp(cur, v, (size_t)pcount(p) * sizeof *v)) bump(g_smp_gen, s); }
    r_SamplerParameterfv(s, p, v);
}
static void (*r_SamplerParameterf)(GLuint, GLenum, float);
static void (SO_ABI *n_SamplerParameterf)(GLuint, GLenum, float);
static SO_ABI void w_SamplerParameterf(GLuint s, GLenum p, float v)
{
    float cur[4] = { 0 };
    if (g_on) { q_GetSamplerParameterfv(s, p, cur); if (cur[0] != v) bump(g_smp_gen, s); }
    if (n_SamplerParameterf) n_SamplerParameterf(s, p, v); else r_SamplerParameterf(s, p, v);
}
static void (*r_DeleteSamplers)(GLsizei, const GLuint *);
static void w_DeleteSamplers(GLsizei n, const GLuint *s) { for (GLsizei i = 0; s && i < n; i++) bump(g_smp_gen, s[i]); r_DeleteSamplers(n, s); }
static void (*r_LinkProgram)(GLuint);
static void w_LinkProgram(GLuint p) { bump(g_prog_gen, p); r_LinkProgram(p); }
static void (*r_ProgramBinary)(GLuint, GLenum, const void *, GLsizei);
static void w_ProgramBinary(GLuint p, GLenum f, const void *b, GLsizei l) { bump(g_prog_gen, p); r_ProgramBinary(p, f, b, l); }
static void (*r_DeleteProgram)(GLuint);
static void w_DeleteProgram(GLuint p) { bump(g_prog_gen, p); r_DeleteProgram(p); }

// ---- buffers
static GLuint bound_buffer(GLenum t)
{
    GLenum q = t == GL_ARRAY_BUFFER ? GL_ARRAY_BUFFER_BINDING : t == GL_ELEMENT_ARRAY_BUFFER ? GL_ELEMENT_ARRAY_BUFFER_BINDING :
               t == GL_UNIFORM_BUFFER ? GL_UNIFORM_BUFFER_BINDING : t == GL_COPY_READ_BUFFER ? GL_COPY_READ_BUFFER :
               t == GL_COPY_WRITE_BUFFER ? GL_COPY_WRITE_BUFFER : t == GL_PIXEL_PACK_BUFFER ? GL_PIXEL_PACK_BUFFER_BINDING :
               t == GL_PIXEL_UNPACK_BUFFER ? GL_PIXEL_UNPACK_BUFFER_BINDING : t == GL_TRANSFORM_FEEDBACK_BUFFER ? GL_TRANSFORM_FEEDBACK_BUFFER_BINDING :
               t == GL_SHADER_STORAGE_BUFFER ? GL_SHADER_STORAGE_BUFFER_BINDING : t == GL_ATOMIC_COUNTER_BUFFER ? GL_ATOMIC_COUNTER_BUFFER_BINDING :
               t == GL_DRAW_INDIRECT_BUFFER ? GL_DRAW_INDIRECT_BUFFER_BINDING : t == GL_DISPATCH_INDIRECT_BUFFER ? GL_DISPATCH_INDIRECT_BUFFER_BINDING :
               t == GL_TEXTURE_BUFFER ? GL_TEXTURE_BUFFER_BINDING : 0;
    GLint b = 0;
    if (q) q_GetIntegerv(q, &b);
    return b > 0 && b < MAX_NAME ? (GLuint)b : 0;
}
static void (*r_BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void w_BufferData(GLenum t, GLsizeiptr s, const void *d, GLenum u)
{
    GLuint b = g_on ? bound_buffer(t) : 0;
    r_BufferData(t, s, d, u);
    if (!b) return;
    pthread_mutex_lock(&g_lock);
    if (g_shadow[b] && (s < 0 || (uint64_t)s != g_shadow[b]->size)) shadow_free(b);
    g_buf_size[b] = s > 0 && (uint64_t)s <= 0xffffffffu ? (uint32_t)s : 0;
    shadow_t *sh = buffer_written(b, t);
    if (sh) { shadow_invalidate(sh, 0, sh->size); if (d) shadow_write(sh, 0, (uint64_t)s, d); }
    pthread_mutex_unlock(&g_lock);
}
static void (*r_BufferStorageEXT)(GLenum, GLsizeiptr, const void *, GLbitfield);
static void w_BufferStorageEXT(GLenum t, GLsizeiptr s, const void *d, GLbitfield f)
{
    GLuint b = g_on ? bound_buffer(t) : 0;
    r_BufferStorageEXT(t, s, d, f);
    if (!b) return;
    pthread_mutex_lock(&g_lock);
    shadow_free(b);
    g_buf_size[b] = s > 0 && (uint64_t)s <= 0xffffffffu ? (uint32_t)s : 0;
    if (f & GL_MAP_PERSISTENT_BIT_EXT) g_buf_vol[b] = 1;       // (written through a pointer at any time)
    shadow_t *sh = buffer_written(b, t);
    if (sh) { shadow_invalidate(sh, 0, sh->size); if (d) shadow_write(sh, 0, (uint64_t)s, d); }
    pthread_mutex_unlock(&g_lock);
}
static void (*r_BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
static void w_BufferSubData(GLenum t, GLintptr o, GLsizeiptr s, const void *d)
{
    GLuint b = g_on ? bound_buffer(t) : 0;
    r_BufferSubData(t, o, s, d);
    if (!b || o < 0 || s < 0) return;
    pthread_mutex_lock(&g_lock);
    shadow_t *sh = buffer_written(b, t);
    if (sh) shadow_write(sh, (uint64_t)o, (uint64_t)s, d);
    pthread_mutex_unlock(&g_lock);
}
static void *(*r_MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
static void *w_MapBufferRange(GLenum t, GLintptr o, GLsizeiptr l, GLbitfield a)
{
    void *p = r_MapBufferRange(t, o, l, a);
    if (!g_on || !(a & GL_MAP_WRITE_BIT)) return p;
    GLuint b = bound_buffer(t);
    if (!b) return p;
    pthread_mutex_lock(&g_lock);
    if (a & GL_MAP_PERSISTENT_BIT_EXT) g_buf_vol[b] = 1;
    shadow_t *sh = mapped_copy(b) ? buffer_written(b, t) : buffer_bumped(b);
    if (sh) { if (a & GL_MAP_INVALIDATE_BUFFER_BIT) shadow_invalidate(sh, 0, sh->size); else shadow_invalidate(sh, (uint64_t)o, (uint64_t)l); }
    mapping_t *m = mapping(b);
    for (int i = 0; !m && i < MAX_MAP; i++) if (!g_map[i].buf) m = &g_map[i];
    if (m) *m = (mapping_t){ p ? b : 0, o, l, a, p };
    else g_buf_vol[b] = 1;                                     // (more mappings than followed)
    pthread_mutex_unlock(&g_lock);
    return p;
}
static void (*r_FlushMappedBufferRange)(GLenum, GLintptr, GLsizeiptr);
static void w_FlushMappedBufferRange(GLenum t, GLintptr o, GLsizeiptr l)
{
    GLuint b = g_on ? bound_buffer(t) : 0;
    if (b) {
        pthread_mutex_lock(&g_lock);
        mapping_t *m = mapping(b);
        if (m && mapped_copy(b)) {
            shadow_t *sh = buffer_written(b, t);
            if (sh && o >= 0 && l >= 0 && o + l <= m->len) shadow_write(sh, (uint64_t)(m->off + o), (uint64_t)l, m->ptr + o);
        } else {
            shadow_t *sh = buffer_bumped(b);
            if (sh) shadow_invalidate(sh, (uint64_t)((m ? m->off : 0) + o), (uint64_t)l);
        }
        pthread_mutex_unlock(&g_lock);
    }
    r_FlushMappedBufferRange(t, o, l);
}
static GLboolean (*r_UnmapBuffer)(GLenum);
static GLboolean w_UnmapBuffer(GLenum t)
{
    GLuint b = g_on ? bound_buffer(t) : 0;
    if (b) {
        pthread_mutex_lock(&g_lock);
        mapping_t *m = mapping(b);
        if (m && (m->access & GL_MAP_WRITE_BIT)) {
            int copy = mapped_copy(b);
            shadow_t *sh = copy ? buffer_written(b, t) : buffer_bumped(b);
            if (sh && !(m->access & GL_MAP_FLUSH_EXPLICIT_BIT)) {
                if (copy) shadow_write(sh, (uint64_t)m->off, (uint64_t)m->len, m->ptr);
                else shadow_invalidate(sh, (uint64_t)m->off, (uint64_t)m->len);
            }
        }
        if (m) m->buf = 0;
        pthread_mutex_unlock(&g_lock);
    }
    return r_UnmapBuffer(t);
}
static void (*r_CopyBufferSubData)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr);
static void w_CopyBufferSubData(GLenum rt, GLenum wt, GLintptr ro, GLintptr wo, GLsizeiptr s)
{
    GLuint rb = g_on ? bound_buffer(rt) : 0, wb = g_on ? bound_buffer(wt) : 0;
    r_CopyBufferSubData(rt, wt, ro, wo, s);
    if (!wb) return;
    pthread_mutex_lock(&g_lock);
    shadow_t *d = buffer_written(wb, wt), *src = rb ? g_shadow[rb] : NULL;
    if (d) {
        if (src && ro >= 0 && wo >= 0 && s >= 0 && shadow_valid(src, (uint64_t)ro, (uint64_t)s) && (uint64_t)(wo + s) <= d->size) {
            memmove(d->data + wo, src->data + ro, (size_t)s);
            shadow_write(d, (uint64_t)wo, (uint64_t)s, d->data + wo);
        } else shadow_invalidate(d, (uint64_t)wo, (uint64_t)s);
    }
    pthread_mutex_unlock(&g_lock);
}
static void (*r_DeleteBuffers)(GLsizei, const GLuint *);
static void w_DeleteBuffers(GLsizei n, const GLuint *bs)
{
    r_DeleteBuffers(n, bs);
    if (!g_on) return;
    pthread_mutex_lock(&g_lock);
    for (GLsizei i = 0; bs && i < n; i++) {
        GLuint b = bs[i];
        if (!b || b >= MAX_NAME) continue;
        bump(g_buf_gen, b);
        shadow_free(b);
        g_buf_size[b] = 0; g_buf_writes[b] = 0; g_buf_wanted[b] = 0;
        mapping_t *m = mapping(b);
        if (m) m->buf = 0;
    }
    pthread_mutex_unlock(&g_lock);
}
// bindings the GPU writes through: never an input
static int gpu_written(GLenum t) { return t == GL_SHADER_STORAGE_BUFFER || t == GL_ATOMIC_COUNTER_BUFFER || t == GL_TRANSFORM_FEEDBACK_BUFFER; }
static void (*r_BindBufferBase)(GLenum, GLuint, GLuint);
// (a buffer bound for uniform blocks: shadowed from its next write on)
static void bound_as(GLenum t, GLuint b)
{
    if (!g_on || !b || b >= MAX_NAME) return;
    if (gpu_written(t)) g_buf_vol[b] = 1;
    if (t == GL_UNIFORM_BUFFER) g_buf_writes[b] = 2;
}
static void w_BindBufferBase(GLenum t, GLuint i, GLuint b) { bound_as(t, b); r_BindBufferBase(t, i, b); }
static void (*r_BindBufferRange)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
static void w_BindBufferRange(GLenum t, GLuint i, GLuint b, GLintptr o, GLsizeiptr s)
{ bound_as(t, b); r_BindBufferRange(t, i, b, o, s); }
static void (*r_ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void w_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p)
{
    GLuint b = g_on ? bound_buffer(GL_PIXEL_PACK_BUFFER) : 0;
    r_ReadPixels(x, y, w, h, f, t, p);
    if (!b) return;
    pthread_mutex_lock(&g_lock);
    shadow_t *sh = buffer_written(b, GL_PIXEL_PACK_BUFFER);
    if (sh) shadow_invalidate(sh, 0, sh->size);
    pthread_mutex_unlock(&g_lock);
}

void glmemo_frame(void)
{
    if (!g_on) return;
    if (!g_have_thread) {                       // the swapping thread, its framebuffers, and whether its context can be followed
        g_thread = pthread_self(); g_have_thread = 1;
        GLint fb = 0;
        q_GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb); g_draw_fb = (GLuint)fb;
        q_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &fb); g_read_fb = (GLuint)fb;
        const char *v = q_GetString ? (const char *)q_GetString(GL_VERSION) : NULL;
        const char *x = q_GetString ? (const char *)q_GetString(GL_EXTENSIONS) : NULL;
        g_ready = v && !strncmp(v, "OpenGL ES 3.", 12);
        g_es31 = g_ready && v[12] >= '1';
        g_es32 = g_ready && v[12] >= '2';
        g_srgb_ctl = x && strstr(x, "GL_EXT_sRGB_write_control") != NULL;
        if (!g_ready) {
            fprintf(stderr, "[glmemo] not an OpenGL ES 3 context (%s): off\n", v ? v : "?");
            g_on = 0;
            return;
        }
    }
    drop_deferred();
    g_pass_ops = 0; g_pass_inval = 0;           // (the swap ends the pass)
    g_frame++;
    if (g_check && g_frame % 300 == 0 && (g_check_same || g_check_diff))
        fprintf(stderr, "[glmemo] check, %ld frames: %ld draws would have been left out; drawn, %ld wrote the same pixels as the "
                "texture held, %ld other ones\n", g_frame, g_check_same + g_check_diff, g_check_same, g_check_diff);
    if (!g_check && g_frame % 3600 == 0)
        fprintf(stderr, "[glmemo] %ld frames: %ld draws left out (%.2f a frame), %ld keys made\n", g_frame, g_left_out,
                (double)g_left_out / (double)g_frame, g_keys);
}

static so_shim g_shims[128];

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
        if (gpa) p = gpa(name);
    }
    return p;
}

const so_shim *glmemo_table(void *(*next)(const char *name))
{
    memset(g_shims, 0, sizeof g_shims);
    g_on = 0; g_check = 0; g_ready = 0; g_have_thread = 0; g_frame = 0;
    const char *e = getenv("UA_GL_MEMO");
    if (e && !strcmp(e, "0")) return g_shims;
    // (the frame recorder holds the calls until the swap, texture name virtualisation answers for other names than the
    // driver's: the state this reads back would not be the state of the calls it follows)
    const char *rec = getenv("UA_GL_REC"), *ren = getenv("UA_GL_RENAME");
    if ((rec && *rec && strcmp(rec, "0")) || (ren && *ren && strcmp(ren, "0"))) {
        fprintf(stderr, "[glmemo] off: UA_GL_REC or UA_GL_RENAME is on\n");
        return g_shims;
    }
    g_check = e && !strcmp(e, "check");
    const char *dbg = getenv("UA_GL_MEMO_DEBUG");
    g_debug = dbg && atoi(dbg) > 0;
    g_debug_w = g_debug_h = 0;
    if (dbg && strchr(dbg, 'x')) { g_debug_w = atoi(dbg); g_debug_h = atoi(strchr(dbg, 'x') + 1); }
#define Q(var, name) *(void **)&var = gl(NULL, name)
    Q(q_GetIntegerv, "glGetIntegerv"); Q(q_GetFloatv, "glGetFloatv"); Q(q_GetBooleanv, "glGetBooleanv"); Q(q_IsEnabled, "glIsEnabled");
    Q(q_GetError, "glGetError"); Q(q_GetString, "glGetString"); Q(q_GetFbAttachment, "glGetFramebufferAttachmentParameteriv");
    Q(q_GetProgramiv, "glGetProgramiv"); Q(q_GetActiveUniform, "glGetActiveUniform"); Q(q_GetActiveUniformsiv, "glGetActiveUniformsiv");
    Q(q_GetUniformLocation, "glGetUniformLocation"); Q(q_GetUniformfv, "glGetUniformfv"); Q(q_GetUniformiv, "glGetUniformiv");
    Q(q_GetUniformuiv, "glGetUniformuiv"); Q(q_GetActiveAttrib, "glGetActiveAttrib"); Q(q_GetAttribLocation, "glGetAttribLocation");
    Q(q_GetActiveUniformBlockiv, "glGetActiveUniformBlockiv"); Q(q_GetIntegeri_v, "glGetIntegeri_v"); Q(q_GetInteger64i_v, "glGetInteger64i_v");
    Q(q_GetProgramInterfaceiv, "glGetProgramInterfaceiv"); Q(q_GetVertexAttribiv, "glGetVertexAttribiv");
    Q(q_GetVertexAttribfv, "glGetVertexAttribfv"); Q(q_GetVertexAttribPointerv, "glGetVertexAttribPointerv"); Q(q_GetQueryiv, "glGetQueryiv");
    Q(q_GetTexParameteriv, "glGetTexParameteriv"); Q(q_GetTexParameterfv, "glGetTexParameterfv");
    Q(q_GetSamplerParameteriv, "glGetSamplerParameteriv"); Q(q_GetSamplerParameterfv, "glGetSamplerParameterfv");
    Q(s_ActiveTexture, "glActiveTexture"); Q(s_BindFramebufferRaw, "glBindFramebuffer"); Q(s_BindBufferRaw, "glBindBuffer");
    Q(s_PixelStorei, "glPixelStorei"); Q(s_ReadPixelsRaw, "glReadPixels");
#undef Q
    if (!(q_GetIntegerv && q_GetFloatv && q_GetBooleanv && q_IsEnabled && q_GetError && q_GetString && q_GetFbAttachment && q_GetProgramiv &&
          q_GetActiveUniform && q_GetActiveUniformsiv && q_GetUniformLocation && q_GetUniformfv && q_GetUniformiv && q_GetUniformuiv &&
          q_GetActiveAttrib && q_GetAttribLocation && q_GetActiveUniformBlockiv && q_GetIntegeri_v && q_GetInteger64i_v &&
          q_GetVertexAttribiv && q_GetVertexAttribfv && q_GetVertexAttribPointerv && q_GetQueryiv && q_GetTexParameteriv &&
          q_GetTexParameterfv && q_GetSamplerParameteriv && q_GetSamplerParameterfv && s_ActiveTexture))
        return g_shims;                         // (a GLES 2 driver: nothing wrapped)
    if (!g_tex_gen) {
        g_tex_gen = calloc(MAX_NAME, sizeof *g_tex_gen); g_buf_gen = calloc(MAX_NAME, sizeof *g_buf_gen);
        g_tpar_gen = calloc(MAX_NAME, sizeof *g_tpar_gen);
        g_smp_gen = calloc(MAX_NAME, sizeof *g_smp_gen); g_prog_gen = calloc(MAX_NAME, sizeof *g_prog_gen);
        g_tex_vol = calloc(MAX_NAME, 1); g_buf_vol = calloc(MAX_NAME, 1); g_buf_writes = calloc(MAX_NAME, 1);
        g_buf_size = calloc(MAX_NAME, sizeof *g_buf_size); g_shadow = calloc(MAX_NAME, sizeof *g_shadow);
        g_buf_wanted = calloc(MAX_NAME, 1);
        g_pinfo = calloc(MAX_NAME, sizeof *g_pinfo);
    }
    if (!(g_tex_gen && g_tpar_gen && g_buf_gen && g_smp_gen && g_prog_gen && g_tex_vol && g_buf_vol && g_buf_writes && g_buf_wanted && g_buf_size && g_shadow && g_pinfo))
        return g_shims;
    memset(g_fb, 0, sizeof g_fb); memset(g_memo, 0, sizeof g_memo); memset(g_map, 0, sizeof g_map);
    g_def.on = 0;
    int k = 0, ok = 1;
#define REQ(f, n) do { *(void **)&r_##f = gl(next, n); if (r_##f) g_shims[k++] = (so_shim){ n, (void *)w_##f }; else ok = 0; } while (0)
#define OPT(f, n) do { *(void **)&r_##f = gl(next, n); if (r_##f) g_shims[k++] = (so_shim){ n, (void *)w_##f }; } while (0)
    // (float arguments: the next host wrapper if there is one, called as the player calls; else the driver)
#define FLT(f, n) do { *(void **)&n_##f = next ? next(n) : NULL; *(void **)&r_##f = n_##f ? NULL : gl(NULL, n); \
                       if (n_##f || r_##f) g_shims[k++] = (so_shim){ n, (void *)w_##f }; } while (0)
    REQ(BindFramebuffer, "glBindFramebuffer"); REQ(FramebufferTexture2D, "glFramebufferTexture2D");
    REQ(FramebufferRenderbuffer, "glFramebufferRenderbuffer"); REQ(DeleteFramebuffers, "glDeleteFramebuffers");
    REQ(DrawBuffers, "glDrawBuffers"); REQ(Clear, "glClear"); REQ(ClearBufferfv, "glClearBufferfv"); REQ(ClearBufferiv, "glClearBufferiv");
    REQ(ClearBufferuiv, "glClearBufferuiv"); FLT(ClearBufferfi, "glClearBufferfi"); REQ(BlitFramebuffer, "glBlitFramebuffer");
    REQ(Invalidate, "glInvalidateFramebuffer"); REQ(InvalidateSub, "glInvalidateSubFramebuffer");
    OPT(Discard, "glDiscardFramebufferEXT");
    OPT(FramebufferTextureLayer, "glFramebufferTextureLayer"); OPT(FramebufferTexture, "glFramebufferTexture");
    OPT(FramebufferTexture2DMS, "glFramebufferTexture2DMultisampleEXT");
    REQ(TexImage2D, "glTexImage2D"); REQ(TexImage3D, "glTexImage3D"); REQ(TexSubImage2D, "glTexSubImage2D");
    REQ(TexSubImage3D, "glTexSubImage3D"); REQ(CompressedTexImage2D, "glCompressedTexImage2D");
    REQ(CompressedTexImage3D, "glCompressedTexImage3D"); REQ(CompressedTexSubImage2D, "glCompressedTexSubImage2D");
    REQ(CompressedTexSubImage3D, "glCompressedTexSubImage3D"); REQ(TexStorage2D, "glTexStorage2D"); REQ(TexStorage3D, "glTexStorage3D");
    OPT(TexStorage2DMultisample, "glTexStorage2DMultisample"); REQ(CopyTexImage2D, "glCopyTexImage2D");
    REQ(CopyTexSubImage2D, "glCopyTexSubImage2D"); REQ(CopyTexSubImage3D, "glCopyTexSubImage3D"); REQ(GenerateMipmap, "glGenerateMipmap");
    OPT(EGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES"); OPT(TexBufferEXT, "glTexBufferEXT");
    OPT(TexBufferOES, "glTexBufferOES"); OPT(TexBuffer, "glTexBuffer");
    REQ(TexParameteri, "glTexParameteri"); REQ(TexParameteriv, "glTexParameteriv"); REQ(TexParameterfv, "glTexParameterfv");
    FLT(TexParameterf, "glTexParameterf"); OPT(CopyImageSubData, "glCopyImageSubData"); REQ(DeleteTextures, "glDeleteTextures");
    OPT(BindImageTexture, "glBindImageTexture");
    REQ(SamplerParameteri, "glSamplerParameteri"); REQ(SamplerParameteriv, "glSamplerParameteriv");
    REQ(SamplerParameterfv, "glSamplerParameterfv"); FLT(SamplerParameterf, "glSamplerParameterf");
    REQ(DeleteSamplers, "glDeleteSamplers"); REQ(LinkProgram, "glLinkProgram"); REQ(ProgramBinary, "glProgramBinary");
    REQ(DeleteProgram, "glDeleteProgram");
    REQ(BufferData, "glBufferData"); OPT(BufferStorageEXT, "glBufferStorageEXT"); REQ(BufferSubData, "glBufferSubData");
    REQ(MapBufferRange, "glMapBufferRange"); REQ(FlushMappedBufferRange, "glFlushMappedBufferRange"); REQ(UnmapBuffer, "glUnmapBuffer");
    REQ(CopyBufferSubData, "glCopyBufferSubData"); REQ(DeleteBuffers, "glDeleteBuffers"); REQ(BindBufferBase, "glBindBufferBase");
    REQ(BindBufferRange, "glBindBufferRange"); REQ(ReadPixels, "glReadPixels");
    REQ(DrawArrays, "glDrawArrays"); REQ(DrawElements, "glDrawElements"); REQ(DrawRangeElements, "glDrawRangeElements");
    OPT(DrawElementsBaseVertex, "glDrawElementsBaseVertex"); OPT(DrawElementsBaseVertexOES, "glDrawElementsBaseVertexOES");
    OPT(DrawElementsBaseVertexEXT, "glDrawElementsBaseVertexEXT"); OPT(DrawRangeElementsBaseVertex, "glDrawRangeElementsBaseVertex");
    OPT(DrawRangeElementsBaseVertexOES, "glDrawRangeElementsBaseVertexOES");
    OPT(DrawRangeElementsBaseVertexEXT, "glDrawRangeElementsBaseVertexEXT");
    OPT(DrawArraysInstanced, "glDrawArraysInstanced"); OPT(DrawElementsInstanced, "glDrawElementsInstanced");
    OPT(DrawArraysInstancedEXT, "glDrawArraysInstancedEXT"); OPT(DrawElementsInstancedEXT, "glDrawElementsInstancedEXT");
    OPT(DrawElementsInstancedBaseVertex, "glDrawElementsInstancedBaseVertex");
    OPT(DrawElementsInstancedBaseVertexOES, "glDrawElementsInstancedBaseVertexOES");
    OPT(DrawElementsInstancedBaseVertexEXT, "glDrawElementsInstancedBaseVertexEXT");
    OPT(DrawArraysInstancedBaseInstanceEXT, "glDrawArraysInstancedBaseInstanceEXT");
    OPT(DrawElementsInstancedBaseInstanceEXT, "glDrawElementsInstancedBaseInstanceEXT");
    OPT(DrawElementsInstancedBaseVertexBaseInstanceEXT, "glDrawElementsInstancedBaseVertexBaseInstanceEXT");
    OPT(MultiDrawArraysEXT, "glMultiDrawArraysEXT"); OPT(MultiDrawElementsEXT, "glMultiDrawElementsEXT");
    OPT(MultiDrawElementsBaseVertexEXT, "glMultiDrawElementsBaseVertexEXT");
    OPT(MultiDrawElementsBaseVertexOES, "glMultiDrawElementsBaseVertexOES");
    OPT(DrawArraysIndirect, "glDrawArraysIndirect"); OPT(DrawElementsIndirect, "glDrawElementsIndirect");
#undef REQ
#undef OPT
#undef FLT
    if (!r_Discard) *(void **)&r_Discard = (void *)r_Invalidate;   // (held back only when the game has it)
    if (!ok || k >= (int)(sizeof g_shims / sizeof *g_shims)) {
        memset(g_shims, 0, sizeof g_shims);     // (a GLES 2 driver: nothing wrapped)
        return g_shims;
    }
    g_on = 1;
    fprintf(stderr, "[glmemo] %s (UA_GL_MEMO%s)\n", g_check ? "check: draws that would be left out are drawn and their pixels compared"
            : "draws whose target already holds what they would write are left out", g_check ? "=check" : "");
    return g_shims;
}
