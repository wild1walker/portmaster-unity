// Copies a GLES frame can do without: a full-screen copy of one texture into another, left out by
// giving the copy's name the source's storage (texture name virtualisation). UA_GL_RENAME=1 / 0 / check.
//
// Unity copies textures with draws: a grab pass (a blend-mode sprite samples "the picture so far")
// is a full-screen draw of the camera's colour texture A into the grab texture C, after which the
// camera goes on drawing into A; an image effect's last blit copies its result into the camera's
// target. On a tiler each copy is a pass of its own (a load, a full-screen draw, a store), and the
// camera that comes back to A after it starts a pass that loads A again. Here, such a draw is not
// made. Instead C's name is given A's storage, which holds exactly what the copy would have
// written (the draw is first proved to be an exact copy, below), and A's name is given C's old
// storage, which then "owes" A's picture: the two names hold the same picture, one storage has it.
// The picture is given to the owing storage only when it is needed, and in the cheapest way:
//
//   - a pass that draws into the owing storage (the camera comes back to A): at its start, the
//     colour buffer is invalidated and one full-screen triangle copies the other storage into it,
//     texel by texel (texelFetch, highp): the tile is filled the way the tiler's own load would
//     fill it, in the pass the camera starts anyway, so the copy pass is gone;
//   - a write that replaces the whole picture (a whole clear, the game's invalidation, a texture
//     redefinition, its deletion): nothing is copied; the writer gets the owing storage (the two
//     names swap storages, which is free) and the other name keeps the picture;
//   - a read (a texture unit holds the name at a draw, a read framebuffer has it): the reader
//     gets the storage with the picture (a swap); if both names are read at once, or a texture
//     upload changes part of one, the picture is copied (a blit, a pass of its own: as before).
//
// A swap exchanges the two storages' texture parameters too (each name keeps its filtering and
// wrapping), and re-attaches every framebuffer and re-binds every texture unit that has either
// name. The names the game sees never change: every GL entry point that takes or returns a
// texture name maps it (binding, attachment, deletion, copies, image units, labels, queries of
// bindings and attachments). The storages of all live names are always a permutation of those
// names (a deleted name's storage is given back first, with a copy if its picture lives
// elsewhere), so names the driver generates never collide with names the game holds.
//
// Which draws are copies. A draw is a candidate when it is a plain, non-instanced draw into a
// framebuffer with one colour buffer (a 2D RGBA8 texture, one level), with blending off, all
// channels written, no scissor (or one that covers it), depth and stencil left alone, and a
// program with one sampler, which holds a 2D RGBA8 texture of the same size (one level) as the
// target, the viewport covering both. Whether the program, with exactly these inputs, copies its
// texture texel for texel cannot be seen from GL; it is measured, once per set of inputs: the
// draw is made as usual, then once more with a texture of random bytes (with the source's
// sampling parameters) in place of the source and a scratch texture in place of the target; the
// scratch texture is read back (asynchronously, a frame or two later) and compared with the noise
// bit for bit. A weight of 1/256 on a neighbouring texel, a colour scale or an offset shows up on
// the noise; only an exact copy reproduces it. The inputs a result is kept for: the program (and
// its link), every uniform's value, the source's parameters and sampler object, the sizes, the
// viewport and fixed-function state, the draw's arguments, the vertex attributes' formats and the
// contents of the vertex and index data they read (a buffer's write count, or the bytes of its
// last small write: Unity writes each blit's quad anew). Same inputs, same result: from then on
// such a draw is left out as above. A program found not to be an exact copy four times is not
// measured again.
//
// UA_GL_RENAME=check: nothing is left out; at each draw that would be, the copy is made and then
// compared with its source (both read back: slow): any difference is logged.
//
// Only the render thread's draws are followed (glrename_frame names it, from the swap hook); a
// texture or framebuffer call of another thread on a name this has swapped gives every picture
// back its own storage and turns this off.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glrename.h"

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned GLbitfield;
typedef unsigned char GLboolean; typedef intptr_t GLintptr; typedef intptr_t GLsizeiptr; typedef float GLfloat;
typedef struct __GLsync *GLsync; typedef uint64_t GLuint64;

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
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_TEXTURE 0x1702
#define GL_RENDERBUFFER 0x8D41
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_BINDING_3D 0x806A
#define GL_TEXTURE_BINDING_2D_ARRAY 0x8C1D
#define GL_TEXTURE_BINDING_CUBE_MAP 0x8514
#define GL_TEXTURE_BINDING_EXTERNAL_OES 0x8D67
#define GL_TEXTURE_BINDING_2D_MULTISAMPLE 0x9104
#define GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY 0x9105
#define GL_TEXTURE_BINDING_CUBE_MAP_ARRAY 0x900A
#define GL_TEXTURE_BINDING_BUFFER 0x8C2C
#define GL_IMAGE_BINDING_NAME 0x8F3A
#define GL_TEXTURE_WIDTH 0x1000
#define GL_TEXTURE_HEIGHT 0x1001
#define GL_TEXTURE_INTERNAL_FORMAT 0x1003
#define GL_TEXTURE_IMMUTABLE_FORMAT 0x912F
#define GL_TEXTURE_IMMUTABLE_LEVELS 0x82DF
#define GL_RGBA8 0x8058
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_NEAREST 0x2600
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_WRAP_R 0x8072
#define GL_TEXTURE_MIN_LOD 0x813A
#define GL_TEXTURE_MAX_LOD 0x813B
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#define GL_TEXTURE_SWIZZLE_G 0x8E43
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#define GL_TEXTURE_SWIZZLE_A 0x8E45
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_SAMPLER_BINDING 0x8919
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define GL_PROGRAM_PIPELINE_BINDING 0x825A
#define GL_VIEWPORT 0x0BA2
#define GL_SCISSOR_TEST 0x0C11
#define GL_SCISSOR_BOX 0x0C10
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_CULL_FACE 0x0B44
#define GL_CULL_FACE_MODE 0x0B45
#define GL_FRONT_FACE 0x0B46
#define GL_DITHER 0x0BD0
#define GL_RASTERIZER_DISCARD 0x8C89
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#define GL_SAMPLE_COVERAGE 0x80A0
#define GL_PRIMITIVE_RESTART_FIXED_INDEX 0x8D69
#define GL_DEPTH_FUNC 0x0B74
#define GL_DEPTH_WRITEMASK 0x0B72
#define GL_ALWAYS 0x0207
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_DRAW_BUFFER0 0x8825
#define GL_TRANSFORM_FEEDBACK_ACTIVE 0x8E24
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_ACTIVE_ATTRIBUTES 0x8B89
#define GL_ACTIVE_UNIFORM_BLOCKS 0x8A36
#define GL_LINK_STATUS 0x8B82
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED 0x8622
#define GL_VERTEX_ATTRIB_ARRAY_SIZE 0x8623
#define GL_VERTEX_ATTRIB_ARRAY_STRIDE 0x8624
#define GL_VERTEX_ATTRIB_ARRAY_TYPE 0x8625
#define GL_VERTEX_ATTRIB_ARRAY_NORMALIZED 0x886A
#define GL_VERTEX_ATTRIB_ARRAY_INTEGER 0x88FD
#define GL_VERTEX_ATTRIB_ARRAY_DIVISOR 0x88FE
#define GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING 0x889F
#define GL_VERTEX_ATTRIB_ARRAY_POINTER 0x8645
#define GL_CURRENT_VERTEX_ATTRIB 0x8626
#define GL_ANY_SAMPLES_PASSED 0x8C2F
#define GL_ANY_SAMPLES_PASSED_CONSERVATIVE 0x8D6A
#define GL_CURRENT_QUERY 0x8865
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_ALREADY_SIGNALED 0x911A
#define GL_CONDITION_SATISFIED 0x911C
#define GL_MAP_READ_BIT 0x0001
#define GL_STREAM_READ 0x88E1
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_INT 0x1404
#define GL_UNSIGNED_INT_VEC4 0x8DC8
#define GL_BOOL 0x8B56
#define GL_SAMPLER_2D 0x8B5E
#define GL_TRIANGLES 0x0004
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_READ_ONLY 0x88B8

static int g_on, g_check;
static pthread_t g_thread;
static int g_have_thread;
// The render context: the first EGL context that makes a texture or framebuffer call (framebuffer
// names and texture units belong to a context; Unity may set its context up on one thread and
// render with it on another). Its calls are followed from the start (render textures are made
// before the first frame); if the context that swaps is another one, nothing is renamed. Another
// context's framebuffers are not followed: a texture attached to one is never renamed.
static void *(*q_GetCurrentContext)(void);
static void *volatile g_ctx;
static volatile int g_multi;
static int mine(void)
{
    void *c = q_GetCurrentContext();
    if (!c) return 0;
    if (!g_ctx) (void)__sync_val_compare_and_swap(&g_ctx, NULL, c);
    if (c == g_ctx) return 1;
    g_multi = 1;
    return 0;
}
static long g_renamed, g_copied_tile, g_copied_pass, g_swaps, g_probes, g_probe_yes, g_probe_no, g_check_bad, g_checked;

// ---- the name map: the game's texture name -> the GL texture that is its storage
// (identity but for swapped names; the storages of the live names are a permutation of them)
#define MAX_MAP 64
typedef struct { GLuint app, real; } map_t;
static map_t g_map[MAX_MAP];
static volatile int g_nmap;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_foreign;       // another thread touched a swapped name: give everything back
static GLuint to_real(GLuint a)
{
    if (!g_nmap || !a) return a;
    GLuint r = a;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nmap; i++) if (g_map[i].app == a) { r = g_map[i].real; break; }
    pthread_mutex_unlock(&g_lock);
    if (r != a && !mine()) g_foreign = 1;
    return r;
}
static GLuint to_app(GLuint r)
{
    if (!g_nmap || !r) return r;
    GLuint a = r;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nmap; i++) if (g_map[i].real == r) { a = g_map[i].app; break; }
    pthread_mutex_unlock(&g_lock);
    return a;
}
static int map_set(GLuint app, GLuint real)
{
    pthread_mutex_lock(&g_lock);
    int i = 0, ok = 1;
    while (i < g_nmap && g_map[i].app != app) i++;
    if (app == real) { if (i < g_nmap) g_map[i] = g_map[--g_nmap]; }
    else if (i < g_nmap) g_map[i].real = real;
    else if (g_nmap < MAX_MAP) { g_map[g_nmap].app = app; g_map[g_nmap].real = real; g_nmap++; }
    else ok = 0;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

// ---- GL entry points
static void (*r_BindTexture)(GLenum, GLuint);
static void (*r_ActiveTexture)(GLenum);
static void (*r_DeleteTextures)(GLsizei, const GLuint *);
static GLboolean (*r_IsTexture)(GLuint);
static void (*r_BindFramebuffer)(GLenum, GLuint);
static void (*r_DeleteFramebuffers)(GLsizei, const GLuint *);
static void (*r_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static void (*r_FramebufferTexture3D)(GLenum, GLenum, GLenum, GLuint, GLint, GLint);
static void (*r_FramebufferTexture3DOES)(GLenum, GLenum, GLenum, GLuint, GLint, GLint);
static void (*r_FramebufferTextureLayer)(GLenum, GLenum, GLuint, GLint, GLint);
static void (*r_FramebufferTexture)(GLenum, GLenum, GLuint, GLint);
static void (*r_FramebufferTextureEXT)(GLenum, GLenum, GLuint, GLint);
static void (*r_FramebufferTextureOES)(GLenum, GLenum, GLuint, GLint);
static void (*r_FramebufferTexture2DMS)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
static void (*r_FramebufferTexture2DMSIMG)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
static void (*r_FramebufferTextureMultiviewOVR)(GLenum, GLenum, GLuint, GLint, GLint, GLsizei);
static void (*r_FramebufferTextureMSMultiviewOVR)(GLenum, GLenum, GLuint, GLint, GLsizei, GLint, GLsizei);
static void (*r_FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static void (*r_GetFbAttachment)(GLenum, GLenum, GLenum, GLint *);
static void (*r_GetIntegerv)(GLenum, GLint *);
static void (*r_GetIntegeri_v)(GLenum, GLuint, GLint *);
static void (*r_CopyImageSubData)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei);
static void (*r_CopyImageSubDataEXT)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei);
static void (*r_CopyImageSubDataOES)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei);
static void (*r_BindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
static void (*r_ObjectLabel)(GLenum, GLuint, GLsizei, const char *);
static void (*r_ObjectLabelKHR)(GLenum, GLuint, GLsizei, const char *);
static void (*r_GetObjectLabel)(GLenum, GLuint, GLsizei, GLsizei *, char *);
static void (*r_GetObjectLabelKHR)(GLenum, GLuint, GLsizei, GLsizei *, char *);
static void (*r_GetObjectLabelEXT)(GLenum, GLuint, GLsizei, GLsizei *, char *);
static void (*r_LabelObjectEXT)(GLenum, GLuint, GLsizei, const char *);
static void (*r_GetTextureParameteriv)(GLuint, GLenum, GLint *);
static void (*r_TextureView)(GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint);
static void (*r_TextureViewEXT)(GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint);
static void (*r_TextureViewOES)(GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint);
static void (*r_Clear)(GLbitfield);
static void (*r_ClearBufferfv)(GLenum, GLint, const float *);
static void (*r_ClearBufferiv)(GLenum, GLint, const GLint *);
static void (*r_ClearBufferuiv)(GLenum, GLint, const GLuint *);
static void (*r_Invalidate)(GLenum, GLsizei, const GLenum *);
static void (*r_InvalidateSub)(GLenum, GLsizei, const GLenum *, GLint, GLint, GLsizei, GLsizei);
static void (*r_Discard)(GLenum, GLsizei, const GLenum *);
static void (*r_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
static void (*r_ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*r_DispatchCompute)(GLuint, GLuint, GLuint);
static void (*r_DispatchComputeIndirect)(GLintptr);
static void (*r_DrawArrays)(GLenum, GLint, GLsizei);
static void (*r_DrawElements)(GLenum, GLsizei, GLenum, const void *);
static void (*r_DrawRangeElements)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
static void (*r_DrawElementsBaseVertex)(GLenum, GLsizei, GLenum, const void *, GLint);
static void (*r_DrawElementsBaseVertexOES)(GLenum, GLsizei, GLenum, const void *, GLint);
static void (*r_DrawElementsBaseVertexEXT)(GLenum, GLsizei, GLenum, const void *, GLint);
static void (*r_BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void (*r_BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
static void *(*r_MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
static void (*r_FlushMappedBufferRange)(GLenum, GLintptr, GLsizeiptr);
static GLboolean (*r_UnmapBuffer)(GLenum);
static void (*r_CopyBufferSubData)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr);
static void (*r_DeleteBuffers)(GLsizei, const GLuint *);
static void (*r_BindBufferBase)(GLenum, GLuint, GLuint);
static void (*r_BindBufferRange)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
static void (*r_LinkProgram)(GLuint);
static void (*r_ProgramBinary)(GLuint, GLenum, const void *, GLsizei);
static void (*r_DeleteProgram)(GLuint);
// queries and state, straight to the driver
static void (*q_GetIntegerv)(GLenum, GLint *);
static void (*q_GetFloatv)(GLenum, float *);
static GLboolean (*q_IsEnabled)(GLenum);
static void (*q_GetFbAttachment)(GLenum, GLenum, GLenum, GLint *);
static void (*q_GetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *);
static void (*q_GetTexParameteriv)(GLenum, GLenum, GLint *);
static void (*q_GetTexParameterfv)(GLenum, GLenum, float *);
static void (*q_TexParameteri)(GLenum, GLenum, GLint);
static void (*q_TexParameterf)(GLenum, GLenum, float);
static void (*q_BindTexture)(GLenum, GLuint);
static void (*q_ActiveTexture)(GLenum);
static void (*q_GenTextures)(GLsizei, GLuint *);
static void (*q_DeleteTextures)(GLsizei, const GLuint *);
static void (*q_TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void (*q_TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
static void (*q_PixelStorei)(GLenum, GLint);
static void (*q_GenFramebuffers)(GLsizei, GLuint *);
static void (*q_GetProgramiv)(GLuint, GLenum, GLint *);
static void (*q_GetActiveUniform)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *);
static void (*q_GetActiveAttrib)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *);
static GLint (*q_GetUniformLocation)(GLuint, const char *);
static GLint (*q_GetAttribLocation)(GLuint, const char *);
static void (*q_GetUniformfv)(GLuint, GLint, float *);
static void (*q_GetUniformiv)(GLuint, GLint, GLint *);
static void (*q_GetVertexAttribiv)(GLuint, GLenum, GLint *);
static void (*q_GetVertexAttribfv)(GLuint, GLenum, float *);
static void (*q_GetVertexAttribPointerv)(GLuint, GLenum, void **);
static void (*q_GetQueryiv)(GLenum, GLenum, GLint *);
static void (*q_Enable)(GLenum), (*q_Disable)(GLenum);
static void (*q_ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
static void (*q_Viewport)(GLint, GLint, GLsizei, GLsizei);
static void (*q_UseProgram)(GLuint);
static void (*q_BindVertexArray)(GLuint);
static void (*q_GenVertexArrays)(GLsizei, GLuint *);
static void (*q_BindSampler)(GLuint, GLuint);
static GLuint (*q_CreateShader)(GLenum);
static void (*q_ShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
static void (*q_CompileShader)(GLuint);
static GLuint (*q_CreateProgram)(void);
static void (*q_AttachShader)(GLuint, GLuint);
static void (*q_LinkProgram)(GLuint);
static void (*q_Uniform1i)(GLint, GLint);
static void (*q_GenBuffers)(GLsizei, GLuint *);
static void (*q_BindBuffer)(GLenum, GLuint);
static void (*q_BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void *(*q_MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
static GLboolean (*q_UnmapBuffer)(GLenum);
static GLsync (*q_FenceSync)(GLenum, GLbitfield);
static GLenum (*q_ClientWaitSync)(GLsync, GLbitfield, GLuint64);
static void (*q_DeleteSync)(GLsync);
static void (*q_ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*q_Finish)(void);

// ---- the render thread's texture units (2D bindings: only 2D textures are ever swapped)
#define MAX_UNIT 32
static GLuint g_unit[MAX_UNIT];      // the game's name on each unit's GL_TEXTURE_2D
static int g_active;                 // GL_ACTIVE_TEXTURE - GL_TEXTURE0

// ---- the render thread's framebuffers: what is attached where, by the game's names
#define MAX_FB 4096
#define MAX_SLOT 11                  // COLOR0..7, DEPTH, STENCIL, DEPTH_STENCIL
enum { K_NONE, K_TEX2D, K_LAYER, K_TEX, K_TEX2DMS, K_TEX3D, K_OTHER };
typedef struct { unsigned char kind; GLenum textarget; GLuint name; GLint level, layer; GLsizei samples; } slot_t;
typedef struct { unsigned char used; slot_t s[MAX_SLOT]; } fbo_t;
static fbo_t *g_fbo;                 // [MAX_FB]
static GLuint g_draw_fb, g_read_fb;
static int g_pass_colour;            // a colour write (draw, colour clear, blit) since the draw framebuffer was bound (or re-attached)
static int slot_of(GLenum a)
{
    if (a >= GL_COLOR_ATTACHMENT0 && a < GL_COLOR_ATTACHMENT0 + 8) return (int)(a - GL_COLOR_ATTACHMENT0);
    return a == GL_DEPTH_ATTACHMENT ? 8 : a == GL_STENCIL_ATTACHMENT ? 9 : a == GL_DEPTH_STENCIL_ATTACHMENT ? 10 : -1;
}
static GLenum point_of(int s) { return s < 8 ? GL_COLOR_ATTACHMENT0 + (GLenum)s : s == 8 ? GL_DEPTH_ATTACHMENT : s == 9 ? GL_STENCIL_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT; }
static GLuint bound_fb(GLenum t) { return t == GL_READ_FRAMEBUFFER ? g_read_fb : g_draw_fb; }
#define MAX_BLOCKED 64
static GLuint g_blocked[MAX_BLOCKED];
static volatile int g_nblocked;
static pthread_mutex_t g_block_lock = PTHREAD_MUTEX_INITIALIZER;
static int blocked(GLuint name)
{
    for (int i = 0; i < g_nblocked; i++) if (g_blocked[i] == name) return 1;
    return 0;
}
static void record(GLenum t, GLenum a, unsigned char kind, GLenum tt, GLuint name, GLint level, GLint layer, GLsizei samples)
{
    if (!mine()) {
        // (another context's framebuffer: its texture is never renamed; if it is swapped already, give back)
        if (!name) return;
        pthread_mutex_lock(&g_block_lock);
        if (!blocked(name) && g_nblocked < MAX_BLOCKED) g_blocked[g_nblocked++] = name;
        else if (!blocked(name)) g_foreign = 1;
        pthread_mutex_unlock(&g_block_lock);
        if (to_real(name) != name) g_foreign = 1;
        return;
    }
    GLuint fb = bound_fb(t);
    int s = slot_of(a);
    if (!fb || fb >= MAX_FB || s < 0) return;
    fbo_t *f = &g_fbo[fb];
    f->used = 1;
    if (a == GL_DEPTH_STENCIL_ATTACHMENT) {           // both points
        f->s[8] = f->s[9] = (slot_t){ name ? kind : K_NONE, tt, name, level, layer, samples };
        f->s[10] = f->s[8];
        return;
    }
    f->s[s] = (slot_t){ name ? kind : K_NONE, tt, name, level, layer, samples };
    if (s == 8 || s == 9) f->s[10].kind = K_NONE;
}

// ---- pictures shared by two names, held by one storage
#define MAX_REL 8
typedef struct { GLuint a, b, holder; int w, h; } rel_t;
static rel_t g_rel[MAX_REL];
static int g_nrel;
static int rel_of(GLuint name)
{
    for (int i = 0; i < g_nrel; i++) if (g_rel[i].a == name || g_rel[i].b == name) return i;
    return -1;
}
static void dissolve(int r) { g_rel[r] = g_rel[--g_nrel]; }

// ---- texture parameters (a swap moves them with the names)
static const GLenum g_iparams[] = { GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R,
                                    GL_TEXTURE_BASE_LEVEL, GL_TEXTURE_MAX_LEVEL, GL_TEXTURE_COMPARE_MODE, GL_TEXTURE_COMPARE_FUNC,
                                    GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_G, GL_TEXTURE_SWIZZLE_B, GL_TEXTURE_SWIZZLE_A };
static const GLenum g_fparams[] = { GL_TEXTURE_MIN_LOD, GL_TEXTURE_MAX_LOD };
#define NIP (int)(sizeof g_iparams / sizeof g_iparams[0])
#define NFP (int)(sizeof g_fparams / sizeof g_fparams[0])
static int g_aniso;                  // EXT_texture_filter_anisotropic
typedef struct { GLint i[NIP]; float f[NFP + 1]; } params_t;
// (on the active unit's 2D binding, which the caller restores)
static void params_get(GLuint real, params_t *p)
{
    q_BindTexture(GL_TEXTURE_2D, real);
    for (int k = 0; k < NIP; k++) q_GetTexParameteriv(GL_TEXTURE_2D, g_iparams[k], &p->i[k]);
    for (int k = 0; k < NFP; k++) q_GetTexParameterfv(GL_TEXTURE_2D, g_fparams[k], &p->f[k]);
    p->f[NFP] = 1;
    if (g_aniso) q_GetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, &p->f[NFP]);
}
static void params_set(GLuint real, const params_t *p)
{
    q_BindTexture(GL_TEXTURE_2D, real);
    for (int k = 0; k < NIP; k++) q_TexParameteri(GL_TEXTURE_2D, g_iparams[k], p->i[k]);
    for (int k = 0; k < NFP; k++) q_TexParameterf(GL_TEXTURE_2D, g_fparams[k], p->f[k]);
    if (g_aniso) q_TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, p->f[NFP]);
}

// re-attach every framebuffer that has one of the names (to its storage now)
static void reattach(GLuint a, GLuint b)
{
    GLuint rfb = g_read_fb;
    int bound = 0;
    for (GLuint fb = 1; fb < MAX_FB; fb++) {
        fbo_t *f = &g_fbo[fb];
        if (!f->used) continue;
        for (int s = 0; s < 10; s++) {
            slot_t *e = &f->s[s];
            if (e->kind == K_NONE || (e->name != a && e->name != b)) continue;
            if (!bound || g_read_fb != fb) { r_BindFramebuffer(GL_READ_FRAMEBUFFER, fb); g_read_fb = fb; bound = 1; }
            GLuint real = to_real(e->name);
            GLenum p = s == 8 && f->s[10].kind != K_NONE ? GL_DEPTH_STENCIL_ATTACHMENT : point_of(s);
            switch (e->kind) {
            case K_TEX2D: r_FramebufferTexture2D(GL_READ_FRAMEBUFFER, p, e->textarget, real, e->level); break;
            case K_LAYER: r_FramebufferTextureLayer(GL_READ_FRAMEBUFFER, p, real, e->level, e->layer); break;
            case K_TEX: (r_FramebufferTexture ? r_FramebufferTexture : r_FramebufferTextureEXT ? r_FramebufferTextureEXT : r_FramebufferTextureOES)
                        (GL_READ_FRAMEBUFFER, p, real, e->level); break;
            case K_TEX2DMS: r_FramebufferTexture2DMS(GL_READ_FRAMEBUFFER, p, e->textarget, real, e->level, e->samples); break;
            default: break;
            }
            if (s == 8 && f->s[10].kind != K_NONE) s = 9;     // (one call for both)
            if (fb == g_draw_fb) g_pass_colour = 0;         // (a new pass on it from here)
        }
    }
    if (bound) { r_BindFramebuffer(GL_READ_FRAMEBUFFER, rfb); g_read_fb = rfb; }
}

// a and b exchange storages (and parameters); units and framebuffers follow
static void swap_names(GLuint a, GLuint b)
{
    GLuint ra = to_real(a), rb = to_real(b);
    GLint act = 0, old = 0;
    q_GetIntegerv(GL_ACTIVE_TEXTURE, &act);
    q_GetIntegerv(GL_TEXTURE_BINDING_2D, &old);
    params_t pa, pb;
    params_get(ra, &pa); params_get(rb, &pb);
    params_set(ra, &pb); params_set(rb, &pa);
    q_BindTexture(GL_TEXTURE_2D, (GLuint)old);
    map_set(a, rb); map_set(b, ra);
    for (int u = 0; u < MAX_UNIT; u++) {
        if (g_unit[u] != a && g_unit[u] != b) continue;
        q_ActiveTexture(GL_TEXTURE0 + (GLenum)u);
        q_BindTexture(GL_TEXTURE_2D, to_real(g_unit[u]));
    }
    q_ActiveTexture((GLenum)act);
    reattach(a, b);
    g_swaps++;
}

// ---- copies of our own: a blit (a pass of its own) or a full-screen triangle inside the pass
static GLuint g_tmp_fb[2], g_copy_prog, g_copy_vao;
static GLint g_copy_loc;
static void blit_copy(GLuint from_real, GLuint to_real_, int w, int h)
{
    GLuint dfb = g_draw_fb, rfb = g_read_fb;
    if (!g_tmp_fb[0]) q_GenFramebuffers(2, g_tmp_fb);
    r_BindFramebuffer(GL_READ_FRAMEBUFFER, g_tmp_fb[0]);
    r_FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, from_real, 0);
    r_BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_tmp_fb[1]);
    r_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, to_real_, 0);
    int sc = q_IsEnabled(GL_SCISSOR_TEST), rd = q_IsEnabled(GL_RASTERIZER_DISCARD);
    GLint m[4] = { 1, 1, 1, 1 };
    q_GetIntegerv(GL_COLOR_WRITEMASK, m);
    if (sc) q_Disable(GL_SCISSOR_TEST);
    if (rd) q_Disable(GL_RASTERIZER_DISCARD);
    q_ColorMask(1, 1, 1, 1);
    r_BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    q_ColorMask((GLboolean)(m[0] != 0), (GLboolean)(m[1] != 0), (GLboolean)(m[2] != 0), (GLboolean)(m[3] != 0));
    if (rd) q_Enable(GL_RASTERIZER_DISCARD);
    if (sc) q_Enable(GL_SCISSOR_TEST);
    r_FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    r_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    r_BindFramebuffer(GL_READ_FRAMEBUFFER, rfb);
    r_BindFramebuffer(GL_DRAW_FRAMEBUFFER, dfb);
    g_pass_colour = 0;
    g_copied_pass++;
}
// the relation's picture copied into the owing storage with a pass of its own; the relation ends
static void give_by_blit(int r)
{
    rel_t *e = &g_rel[r];
    GLuint ra = to_real(e->a), rb = to_real(e->b);
    blit_copy(e->holder, ra == e->holder ? rb : ra, e->w, e->h);
    dissolve(r);
}
static int copy_prog(void)
{
    if (g_copy_prog) return g_copy_prog > 0;
    static const char *vs = "#version 300 es\n"
        "void main() { gl_Position = vec4(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0, 0.0, 1.0); }\n";
    static const char *fs = "#version 300 es\nprecision highp float;\nprecision highp sampler2D;\nuniform sampler2D ua_src;\n"
        "layout(location = 0) out highp vec4 ua_out;\nvoid main() { ua_out = texelFetch(ua_src, ivec2(gl_FragCoord.xy), 0); }\n";
    GLuint v = q_CreateShader(GL_VERTEX_SHADER), f = q_CreateShader(GL_FRAGMENT_SHADER), p = q_CreateProgram();
    q_ShaderSource(v, 1, &vs, NULL); q_CompileShader(v);
    q_ShaderSource(f, 1, &fs, NULL); q_CompileShader(f);
    q_AttachShader(p, v); q_AttachShader(p, f); q_LinkProgram(p);
    GLint ok = 0;
    q_GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "[glrename] the copy program does not link: copies are blits\n"); g_copy_prog = (GLuint)-1; return 0; }
    g_copy_prog = p;
    g_copy_loc = q_GetUniformLocation(p, "ua_src");
    q_GenVertexArrays(1, &g_copy_vao);
    return 1;
}
// at the start of a pass on the draw framebuffer, whose colour buffer 0 is the owing storage: the
// tile filled from the holder with one full-screen triangle (no load of the old contents)
static void give_in_pass(int r, int w, int h)
{
    rel_t *e = &g_rel[r];
    GLint prog = 0, vao = 0, act = 0, tex = 0, smp = 0, vp[4] = { 0, 0, 0, 0 }, cm[4] = { 1, 1, 1, 1 }, ppl = 0;
    q_GetIntegerv(GL_CURRENT_PROGRAM, &prog);
    q_GetIntegerv(GL_PROGRAM_PIPELINE_BINDING, &ppl);
    q_GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    q_GetIntegerv(GL_ACTIVE_TEXTURE, &act);
    q_ActiveTexture(GL_TEXTURE0);
    q_GetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    q_GetIntegerv(GL_SAMPLER_BINDING, &smp);
    q_GetIntegerv(GL_VIEWPORT, vp);
    q_GetIntegerv(GL_COLOR_WRITEMASK, cm);
    static const GLenum caps[] = { GL_SCISSOR_TEST, GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE, GL_DITHER, GL_RASTERIZER_DISCARD,
                                   GL_POLYGON_OFFSET_FILL, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE };
    int on[sizeof caps / sizeof caps[0]];
    for (unsigned k = 0; k < sizeof caps / sizeof caps[0]; k++) { on[k] = q_IsEnabled(caps[k]); if (on[k]) q_Disable(caps[k]); }
    static const GLenum c0[1] = { GL_COLOR_ATTACHMENT0 };
    r_Invalidate(GL_DRAW_FRAMEBUFFER, 1, c0);
    q_UseProgram(g_copy_prog);
    q_Uniform1i(g_copy_loc, 0);
    q_BindVertexArray(g_copy_vao);
    q_BindTexture(GL_TEXTURE_2D, e->holder);
    if (smp) q_BindSampler(0, 0);
    q_Viewport(0, 0, w, h);
    q_ColorMask(1, 1, 1, 1);
    r_DrawArrays(GL_TRIANGLES, 0, 3);
    q_ColorMask((GLboolean)(cm[0] != 0), (GLboolean)(cm[1] != 0), (GLboolean)(cm[2] != 0), (GLboolean)(cm[3] != 0));
    q_Viewport(vp[0], vp[1], vp[2], vp[3]);
    if (smp) q_BindSampler(0, (GLuint)smp);
    q_BindTexture(GL_TEXTURE_2D, (GLuint)tex);
    q_BindVertexArray((GLuint)vao);
    q_UseProgram((GLuint)prog);
    (void)ppl;                       // (UseProgram(0) leaves a bound pipeline in effect again)
    for (unsigned k = 0; k < sizeof caps / sizeof caps[0]; k++) if (on[k]) q_Enable(caps[k]);
    q_ActiveTexture((GLenum)act);
    dissolve(r);
    g_copied_tile++;
    g_pass_colour = 1;               // (the pass has a colour write now: ours)
}

// ---- the rules, on the render thread
// the name's storage must be the holder (a read)
static void want_picture(GLuint name)
{
    int r = rel_of(name);
    if (r < 0 || to_real(name) == g_rel[r].holder) return;
    swap_names(g_rel[r].a, g_rel[r].b);
}
// the name is about to be replaced whole: its storage must be the owing one; the relation ends
static void replaced(GLuint name)
{
    int r = rel_of(name);
    if (r < 0) return;
    if (to_real(name) == g_rel[r].holder) swap_names(g_rel[r].a, g_rel[r].b);
    dissolve(r);
}
// part of the name's picture is about to change outside a pass: both need the picture
static void changed_part(GLuint name)
{
    int r = rel_of(name);
    if (r >= 0) give_by_blit(r);
}
static void give_all_back(void);

// the draw framebuffer's colour buffers before the pass's first command (write: a draw, partial
// clear, blit): an owing storage gets its picture now
static void pass_writes(int whole_colour)
{
    if (g_pass_colour || !g_nrel || g_draw_fb == 0 || g_draw_fb >= MAX_FB) return;
    fbo_t *f = &g_fbo[g_draw_fb];
    int colours = 0;
    for (int s = 0; s < 8; s++) colours += f->s[s].kind != K_NONE;
    for (int s = 0; s < 10; s++) {
        slot_t *e = &f->s[s];
        int r;
        if (e->kind == K_NONE || (r = rel_of(e->name)) < 0) continue;
        if (whole_colour && s < 8) { replaced(e->name); continue; }
        if (s == 0 && colours == 1 && e->kind == K_TEX2D && e->level == 0 && copy_prog()) {
            GLint db = 0, tf = 0;
            q_GetIntegerv(GL_DRAW_BUFFER0, &db);
            q_GetIntegerv(GL_TRANSFORM_FEEDBACK_ACTIVE, &tf);
            if (db == GL_COLOR_ATTACHMENT0 && !tf) {
                if (to_real(e->name) == g_rel[r].holder) swap_names(g_rel[r].a, g_rel[r].b);
                give_in_pass(r, g_rel[r].w, g_rel[r].h);
                continue;
            }
        }
        give_by_blit(r);
    }
}
// before a draw or dispatch: what the texture units hold is read
static void unit_reads(void)
{
    for (int r = 0; r < g_nrel; r++) {
        int ua = 0, ub = 0;
        for (int u = 0; u < MAX_UNIT; u++) { ua |= g_unit[u] == g_rel[r].a; ub |= g_unit[u] == g_rel[r].b; }
        if (ua && ub) { give_by_blit(r); r--; continue; }
        if (ua) want_picture(g_rel[r].a);
        else if (ub) want_picture(g_rel[r].b);
    }
}
// the read framebuffer's colour buffers are read
static void fb_reads(GLuint fb)
{
    if (!g_nrel || !fb || fb >= MAX_FB) return;
    for (int s = 0; s < 10; s++) if (g_fbo[fb].s[s].kind != K_NONE) want_picture(g_fbo[fb].s[s].name);
}

// ---- buffers: how often each was written, and its last small write's bytes
#define MAX_BUF 8192
#define SMALL 256
typedef struct { GLuint name; uint32_t gen; int bad; int64_t off; int len; uint32_t small_gen; unsigned char bytes[SMALL]; } buf_t;
static buf_t *g_buf;                 // open addressing by name
static buf_t *buf_find(GLuint name, int add)
{
    if (!name || !g_buf) return NULL;
    unsigned h = (name * 2654435761u) % MAX_BUF;
    for (int i = 0; i < MAX_BUF; i++, h = (h + 1) % MAX_BUF) {
        if (g_buf[h].name == name) return &g_buf[h];
        if (!g_buf[h].name) {
            if (!add) return NULL;
            memset(&g_buf[h], 0, sizeof g_buf[h]);
            g_buf[h].name = name;
            return &g_buf[h];
        }
    }
    return NULL;
}
static GLenum buf_binding(GLenum t)
{
    switch (t) {
    case 0x8892: return GL_ARRAY_BUFFER_BINDING; case 0x8893: return GL_ELEMENT_ARRAY_BUFFER_BINDING; case 0x8A11: return 0x8A28;
    case 0x88EB: return 0x88ED; case 0x88EC: return 0x88EF; case 0x8C8E: return 0x8C8F; case 0x90D2: return 0x90D3;
    case 0x92C0: return 0x92C1; case 0x90EE: return 0x90EF; case 0x8F3F: return 0x8F3F; default: return t;  // (COPY_READ/WRITE: themselves)
    }
}
static pthread_mutex_t g_buf_lock = PTHREAD_MUTEX_INITIALIZER;
static void buf_written(GLenum t, int64_t off, int64_t len, const void *data)
{
    GLint name = 0;
    q_GetIntegerv(buf_binding(t), &name);
    pthread_mutex_lock(&g_buf_lock);
    buf_t *b = buf_find((GLuint)name, 1);
    if (b) {
        b->gen++;
        if (data && len > 0 && len <= SMALL && off >= 0) { b->off = off; b->len = (int)len; memcpy(b->bytes, data, (size_t)len); b->small_gen = b->gen; }
    }
    pthread_mutex_unlock(&g_buf_lock);
}
static void w_BufferData(GLenum t, GLsizeiptr s, const void *d, GLenum u) { r_BufferData(t, s, d, u); buf_written(t, 0, s, d); }
static void w_BufferSubData(GLenum t, GLintptr o, GLsizeiptr s, const void *d) { r_BufferSubData(t, o, s, d); buf_written(t, o, s, d); }
// mapped writes: the bytes are read back at the flush / unmap, for small ranges
// (by buffer: Unity maps several buffers through the same target, rebinding it in between)
typedef struct { GLuint buf; char *ptr; int64_t off, len; int explicit_flush; } map_rec;
static map_rec g_maps[16];
static map_rec *map_of(GLenum t, int add)
{
    GLint name = 0;
    q_GetIntegerv(buf_binding(t), &name);
    if (!name) return NULL;
    for (int i = 0; i < 16; i++) if (g_maps[i].ptr && g_maps[i].buf == (GLuint)name) return &g_maps[i];
    if (!add) return NULL;
    for (int i = 0; i < 16; i++) if (!g_maps[i].ptr) { g_maps[i].buf = (GLuint)name; return &g_maps[i]; }
    return NULL;
}
static void *w_MapBufferRange(GLenum t, GLintptr o, GLsizeiptr l, GLbitfield a)
{
    void *p = r_MapBufferRange(t, o, l, a);
    if (a & 0x0002) {                            // GL_MAP_WRITE_BIT
        if (mine()) {
            map_rec *m = map_of(t, 1);
            if (m && p) { m->ptr = p; m->off = o; m->len = l; m->explicit_flush = (a & 0x0010) != 0; }
        }
        buf_written(t, 0, 0, NULL);              // (counted now: the GPU may not see the old bytes from here)
    }
    return p;
}
static void w_FlushMappedBufferRange(GLenum t, GLintptr o, GLsizeiptr l)
{
    map_rec *m = mine() ? map_of(t, 0) : NULL;
    if (m) buf_written(t, m->off + o, l, m->ptr + o); else buf_written(t, 0, 0, NULL);
    r_FlushMappedBufferRange(t, o, l);
}
static GLboolean w_UnmapBuffer(GLenum t)
{
    map_rec *m = mine() ? map_of(t, 0) : NULL;
    if (m) {
        if (!m->explicit_flush) buf_written(t, m->off, m->len, m->ptr);
        m->ptr = NULL;
    }
    return r_UnmapBuffer(t);
}
static void w_CopyBufferSubData(GLenum rt, GLenum wt, GLintptr ro, GLintptr wo, GLsizeiptr s)
{ r_CopyBufferSubData(rt, wt, ro, wo, s); buf_written(wt, 0, 0, NULL); }
static void w_DeleteBuffers(GLsizei n, const GLuint *b)
{
    r_DeleteBuffers(n, b);
    pthread_mutex_lock(&g_buf_lock);
    for (GLsizei i = 0; b && i < n; i++) { buf_t *e = buf_find(b[i], 0); if (e) { e->gen++; e->small_gen = 0; } }
    pthread_mutex_unlock(&g_buf_lock);
}
// buffers a shader or transform feedback can write: never keys
static void mark_bad(GLenum t, GLuint name)
{
    if (t != 0x90D2 && t != 0x8C8E && t != 0x92C0) return;      // SSBO, transform feedback, atomic counters
    pthread_mutex_lock(&g_buf_lock);
    buf_t *e = buf_find(name, 1);
    if (e) e->bad = 1;
    pthread_mutex_unlock(&g_buf_lock);
}
static void w_BindBufferBase(GLenum t, GLuint i, GLuint b) { r_BindBufferBase(t, i, b); mark_bad(t, b); }
static void w_BindBufferRange(GLenum t, GLuint i, GLuint b, GLintptr o, GLsizeiptr s) { r_BindBufferRange(t, i, b, o, s); mark_bad(t, b); }
static void w_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p)
{
    if (mine()) fb_reads(g_read_fb);
    r_ReadPixels(x, y, w, h, f, t, p);
    GLint pbo = 0;
    q_GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    if (pbo) buf_written(GL_PIXEL_PACK_BUFFER, 0, 0, NULL);
}

// programs: relinked or deleted, a new one for the keys
#define MAX_PROG 65536
static uint16_t *g_prog_gen;
static unsigned char *g_prog_no;     // not an exact copy once (or not usable): not measured again
static void prog_changed(GLuint p) { if (p < MAX_PROG && g_prog_gen) { g_prog_gen[p]++; g_prog_no[p] = 0; } }
static void w_LinkProgram(GLuint p) { r_LinkProgram(p); prog_changed(p); }
static void w_ProgramBinary(GLuint p, GLenum f, const void *b, GLsizei l) { r_ProgramBinary(p, f, b, l); prog_changed(p); }
static void w_DeleteProgram(GLuint p) { r_DeleteProgram(p); prog_changed(p); }

// ---- keys: everything a candidate draw's result depends on, hashed
typedef struct { uint64_t h; } rkey_t;
static void kh(rkey_t *k, const void *p, size_t n)
{
    const unsigned char *s = p;
    for (size_t i = 0; i < n; i++) { k->h ^= s[i]; k->h *= 1099511628211ull; }
}
static void khi(rkey_t *k, int64_t v) { kh(k, &v, sizeof v); }

typedef struct {
    int kind;                        // 0 arrays, 1 elements, 2 elements + base vertex, 3 range
    GLenum mode, type; GLint first; GLsizei count; const void *indices; GLint base; GLuint start, end;
} drawcall;

// the vertex inputs: 0 when they cannot be keyed
static int key_vertices(rkey_t *k, GLuint prog, const drawcall *d)
{
    GLint na = 0;
    q_GetProgramiv(prog, GL_ACTIVE_ATTRIBUTES, &na);
    // the index range read (known only from a small write's bytes, or not needed for arrays)
    int64_t vmin = 0, vmax = -1;
    int index_known = 0;
    GLint ibo = 0;
    if (d->kind == 0) { vmin = d->first; vmax = (int64_t)d->first + d->count - 1; index_known = 1; }
    else {
        q_GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ibo);
        if (!ibo) return 0;                                  // (client-side indices)
        int isz = d->type == GL_UNSIGNED_INT ? 4 : d->type == GL_UNSIGNED_SHORT ? 2 : 1;
        int64_t off = (int64_t)(intptr_t)d->indices, len = (int64_t)d->count * isz;
        pthread_mutex_lock(&g_buf_lock);
        buf_t *b = buf_find((GLuint)ibo, 1);
        if (!b || b->bad) { pthread_mutex_unlock(&g_buf_lock); return 0; }
        if (b->small_gen == b->gen && off >= b->off && off + len <= b->off + b->len) {
            const unsigned char *p = b->bytes + (off - b->off);
            kh(k, p, (size_t)len);
            vmin = INT64_MAX;
            for (int i = 0; i < d->count; i++) {
                int64_t v = isz == 4 ? (int64_t)((const uint32_t *)(const void *)p)[i] : isz == 2 ? ((const uint16_t *)(const void *)p)[i] : p[i];
                if (v < vmin) vmin = v;
                if (v > vmax) vmax = v;
            }
            vmin += d->base; vmax += d->base;
            index_known = 1;
        } else { khi(k, ibo); khi(k, b->gen); }
        pthread_mutex_unlock(&g_buf_lock);
        khi(k, off); khi(k, d->count); khi(k, d->type); khi(k, d->base);
    }
    for (GLint i = 0; i < na; i++) {
        char name[128]; GLsizei len = 0; GLint size = 0; GLenum type = 0;
        q_GetActiveAttrib(prog, (GLuint)i, sizeof name, &len, &size, &type, name);
        if (!strncmp(name, "gl_", 3)) { kh(k, name, (size_t)len); continue; }
        if (size != 1 || (type >= 0x8B5A && type <= 0x8B5C) || (type >= 0x8B65 && type <= 0x8B6A)) return 0;   // (several locations)
        GLint loc = q_GetAttribLocation(prog, name);
        if (loc < 0) return 0;
        GLint en = 0, sz = 0, st = 0, ty = 0, norm = 0, integer = 0, div = 0, buf = 0;
        void *ptr = NULL;
        q_GetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en);
        khi(k, loc); khi(k, en);
        if (!en) {
            float v[4] = { 0, 0, 0, 0 };
            q_GetVertexAttribfv((GLuint)loc, GL_CURRENT_VERTEX_ATTRIB, v);
            kh(k, v, sizeof v);
            continue;
        }
        q_GetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_SIZE, &sz);
        q_GetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &st);
        q_GetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_TYPE, &ty);
        q_GetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &norm);
        q_GetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &integer);
        q_GetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &div);
        q_GetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf);
        q_GetVertexAttribPointerv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
        if (!buf || div) return 0;                           // (client-side arrays; instanced data)
        khi(k, sz); khi(k, st); khi(k, ty); khi(k, norm); khi(k, integer); khi(k, (int64_t)(intptr_t)ptr);
        int esz = ty == GL_FLOAT || ty == GL_INT || ty == GL_UNSIGNED_INT || ty == 0x140C ? 4 : ty == 0x1402 || ty == 0x1403 || ty == 0x140B ? 2 : 1;
        int64_t bytes = ty == 0x8D9F || ty == 0x8368 ? 4 : (int64_t)sz * esz;   // (the 2_10_10_10 formats: 4 bytes in all)
        int64_t stride = st ? st : bytes;
        int64_t lo = (int64_t)(intptr_t)ptr + vmin * stride, hi = (int64_t)(intptr_t)ptr + vmax * stride + bytes;
        pthread_mutex_lock(&g_buf_lock);
        buf_t *b = buf_find((GLuint)buf, 1);
        if (!b || b->bad) { pthread_mutex_unlock(&g_buf_lock); return 0; }
        if (index_known && vmax >= vmin && b->small_gen == b->gen && lo >= b->off && hi <= b->off + b->len) kh(k, b->bytes + (lo - b->off), (size_t)(hi - lo));
        else { khi(k, buf); khi(k, b->gen); }
        pthread_mutex_unlock(&g_buf_lock);
    }
    return 1;
}

// the program's uniforms: values, and its one sampler's unit (-1: not a candidate)
static int key_program(rkey_t *k, GLuint prog)
{
    GLint nu = 0, nb = 0;
    q_GetProgramiv(prog, GL_ACTIVE_UNIFORM_BLOCKS, &nb);
    if (nb) return -1;
    q_GetProgramiv(prog, GL_ACTIVE_UNIFORMS, &nu);
    int unit = -1, samplers = 0;
    khi(k, prog); khi(k, g_prog_gen[prog]);
    for (GLint i = 0; i < nu; i++) {
        char name[128]; GLsizei len = 0; GLint size = 0; GLenum type = 0;
        q_GetActiveUniform(prog, (GLuint)i, sizeof name, &len, &size, &type, name);
        kh(k, name, (size_t)len); khi(k, type); khi(k, size);
        // array uniforms: each element by its own location
        char *br = strchr(name, '[');
        if (br) *br = 0;
        for (GLint e = 0; e < size; e++) {
            char en[160];
            if (size > 1) snprintf(en, sizeof en, "%s[%d]", name, e); else snprintf(en, sizeof en, "%s", name);
            GLint loc = q_GetUniformLocation(prog, en);
            if (loc < 0) return -1;
            if (type == GL_SAMPLER_2D) {
                GLint u = -1;
                q_GetUniformiv(prog, loc, &u);
                samplers++; unit = u;
                khi(k, u);
                continue;
            }
            if ((type >= 0x8B5D && type <= 0x8B62) || (type >= 0x8DC0 && type <= 0x8DC5) || (type >= 0x8DC9 && type <= 0x8DD8) ||
                type == 0x8D66 || (type >= 0x9108 && type <= 0x910D) || (type >= 0x900C && type <= 0x900F) ||
                (type >= 0x904C && type <= 0x906C) || type == 0x92DB)
                return -1;                                  // (other samplers, images, atomic counters: not a plain copy's)
            float v[16];
            memset(v, 0, sizeof v);
            if (type == GL_FLOAT || (type >= 0x8B50 && type <= 0x8B5C) || (type >= 0x8B65 && type <= 0x8B6A)) q_GetUniformfv(prog, loc, v);
            else q_GetUniformiv(prog, loc, (GLint *)(void *)v);
            kh(k, v, sizeof v);
        }
    }
    return samplers == 1 && unit >= 0 && unit < MAX_UNIT ? unit : -1;
}

// ---- probes: the candidate draw made once more on noise
#define MAX_KEYS 256
typedef struct { uint64_t key; signed char result; } known_t;   // result: 1 an exact copy, 0 not, -1 measuring
static known_t g_known[MAX_KEYS];
static int g_nknown;
static known_t *known(uint64_t key) { for (int i = 0; i < g_nknown; i++) if (g_known[i].key == key) return &g_known[i]; return NULL; }
typedef struct { int w, h; GLuint noise, out, fb, pbo; unsigned char *bytes; } probe_tex;
static probe_tex g_ptex[4];
typedef struct { int busy; uint64_t key; GLuint prog; GLsync fence; probe_tex *t; int frames; } probe_t;
static probe_t g_probe;

static probe_tex *probe_textures(int w, int h)
{
    for (int i = 0; i < 4; i++) if (g_ptex[i].w == w && g_ptex[i].h == h) return &g_ptex[i];
    probe_tex *t = NULL;
    for (int i = 0; i < 4 && !t; i++) if (!g_ptex[i].w) t = &g_ptex[i];
    if (!t) return NULL;
    t->bytes = malloc((size_t)w * h * 4);
    if (!t->bytes) return NULL;
    uint32_t x = 0x9e3779b9u ^ (uint32_t)(w * 7919 + h);
    for (size_t i = 0; i < (size_t)w * h * 4; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; t->bytes[i] = (unsigned char)(x >> 7); }
    GLint act = 0, old = 0, unpack = 0;
    q_GetIntegerv(GL_ACTIVE_TEXTURE, &act);
    q_ActiveTexture(GL_TEXTURE0);
    q_GetIntegerv(GL_TEXTURE_BINDING_2D, &old);
    q_GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    if (unpack) q_BindBuffer(0x88EC, 0);
    GLuint tx[2];
    q_GenTextures(2, tx);
    t->noise = tx[0]; t->out = tx[1];
    q_BindTexture(GL_TEXTURE_2D, t->noise);
    q_TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    q_PixelStorei(0x0CF5, 1);                               // GL_UNPACK_ALIGNMENT (restored to Unity's 4 below)
    q_PixelStorei(0x0CF2, 0);                               // GL_UNPACK_ROW_LENGTH
    q_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, t->bytes);
    q_PixelStorei(0x0CF5, 4);
    q_BindTexture(GL_TEXTURE_2D, t->out);
    q_TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    q_BindTexture(GL_TEXTURE_2D, (GLuint)old);
    q_ActiveTexture((GLenum)act);
    if (unpack) q_BindBuffer(0x88EC, (GLuint)unpack);
    q_GenFramebuffers(1, &t->fb);
    GLuint dfb = g_draw_fb;
    r_BindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fb);
    r_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->out, 0);
    r_BindFramebuffer(GL_DRAW_FRAMEBUFFER, dfb);
    GLint pack = 0;
    q_GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    q_GenBuffers(1, &t->pbo);
    q_BindBuffer(GL_PIXEL_PACK_BUFFER, t->pbo);
    q_BufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 4, NULL, GL_STREAM_READ);
    q_BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    t->w = w; t->h = h;
    return t;
}
static void redraw(const drawcall *d)
{
    switch (d->kind) {
    case 0: r_DrawArrays(d->mode, d->first, d->count); break;
    case 1: r_DrawElements(d->mode, d->count, d->type, d->indices); break;
    case 2: (r_DrawElementsBaseVertex ? r_DrawElementsBaseVertex : r_DrawElementsBaseVertexOES ? r_DrawElementsBaseVertexOES :
             r_DrawElementsBaseVertexEXT)(d->mode, d->count, d->type, d->indices, d->base); break;
    case 3: r_DrawRangeElements(d->mode, d->start, d->end, d->count, d->type, d->indices); break;
    }
}
// after the candidate draw was made: the same draw on noise, read back without waiting
static void probe_start(uint64_t key, GLuint prog, const drawcall *d, int unit, GLuint src_real, int w, int h)
{
    probe_tex *t = probe_textures(w, h);
    if (!t) return;
    GLint act = 0, old = 0;
    q_GetIntegerv(GL_ACTIVE_TEXTURE, &act);
    q_GetIntegerv(GL_TEXTURE_BINDING_2D, &old);
    // (on the active unit: the noise gets the source's parameters, the target the noise's
    // complement, so that a pixel the draw leaves alone cannot pass; then that unit as it was)
    unsigned char *inv = malloc((size_t)w * h * 4);
    if (!inv) return;
    for (size_t i = 0; i < (size_t)w * h * 4; i++) inv[i] = (unsigned char)~t->bytes[i];
    params_t p;
    params_get(src_real, &p);
    params_set(t->noise, &p);
    GLint unpack = 0;
    q_GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    if (unpack) q_BindBuffer(0x88EC, 0);
    q_BindTexture(GL_TEXTURE_2D, t->out);
    q_PixelStorei(0x0CF5, 1);
    q_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, inv);
    q_PixelStorei(0x0CF5, 4);
    if (unpack) q_BindBuffer(0x88EC, (GLuint)unpack);
    free(inv);
    q_BindTexture(GL_TEXTURE_2D, (GLuint)old);
    // the draw again, the noise on the sampler's unit, into the probe's target
    q_ActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    q_BindTexture(GL_TEXTURE_2D, t->noise);
    GLuint dfb = g_draw_fb;
    r_BindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fb);
    redraw(d);
    r_BindFramebuffer(GL_DRAW_FRAMEBUFFER, dfb);
    q_BindTexture(GL_TEXTURE_2D, to_real(g_unit[unit]));
    q_ActiveTexture((GLenum)act);
    GLint pack = 0;
    GLuint rfb = g_read_fb;
    q_GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    r_BindFramebuffer(GL_READ_FRAMEBUFFER, t->fb);
    q_BindBuffer(GL_PIXEL_PACK_BUFFER, t->pbo);
    q_PixelStorei(0x0D05, 4);                               // GL_PACK_ALIGNMENT
    q_ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    q_BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    r_BindFramebuffer(GL_READ_FRAMEBUFFER, rfb);
    g_probe = (probe_t){ 1, key, prog, q_FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0), t, 0 };
    if (g_nknown < MAX_KEYS) g_known[g_nknown++] = (known_t){ key, -1 };
    g_probes++;
}
static void probe_poll(void)
{
    if (!g_probe.busy) return;
    GLenum s = q_ClientWaitSync(g_probe.fence, 0, 0);
    if (s != GL_ALREADY_SIGNALED && s != GL_CONDITION_SATISFIED && ++g_probe.frames < 30) return;
    if (s != GL_ALREADY_SIGNALED && s != GL_CONDITION_SATISFIED) q_ClientWaitSync(g_probe.fence, 1, 1000000000ull);
    q_DeleteSync(g_probe.fence);
    probe_tex *t = g_probe.t;
    GLint pack = 0;
    q_GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    q_BindBuffer(GL_PIXEL_PACK_BUFFER, t->pbo);
    const unsigned char *got = q_MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)t->w * t->h * 4, GL_MAP_READ_BIT);
    size_t diff = 0;
    if (got) for (size_t i = 0; i < (size_t)t->w * t->h; i++) diff += memcmp(got + 4 * i, t->bytes + 4 * i, 4) != 0;
    if (got) q_UnmapBuffer(GL_PIXEL_PACK_BUFFER);
    q_BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    known_t *k = known(g_probe.key);
    int yes = got && diff == 0;
    if (k) k->result = (signed char)yes;
    if (yes) g_probe_yes++;
    else { g_probe_no++; if (g_probe.prog < MAX_PROG && g_prog_no[g_probe.prog] < 255) g_prog_no[g_probe.prog]++; }
    fprintf(stderr, "[glrename] program %u, %dx%d: %s (%zu of %d pixels differ from the noise)\n", g_probe.prog, t->w, t->h,
            yes ? "an exact copy: such draws are left out" : "not an exact copy", diff, t->w * t->h);
    g_probe.busy = 0;
}

// ---- texture facts
typedef struct { int w, h, ok, imm; } texinfo;
// a 2D RGBA8 texture with one level (on the active unit's binding, restored)
static texinfo tex_info(GLuint real)
{
    texinfo t = { 0, 0, 0, 0 };
    GLint old = 0, fmt = 0, imm = 0, lv = 0, w1 = 0;
    q_GetIntegerv(GL_TEXTURE_BINDING_2D, &old);
    q_BindTexture(GL_TEXTURE_2D, real);
    q_GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &t.w);
    q_GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &t.h);
    q_GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
    q_GetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_FORMAT, &imm);
    if (imm) q_GetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_LEVELS, &lv);
    else q_GetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &w1);
    q_BindTexture(GL_TEXTURE_2D, (GLuint)old);
    t.ok = t.w > 0 && t.h > 0 && fmt == GL_RGBA8 && (imm ? lv == 1 : w1 == 0);
    t.imm = imm != 0;
    return t;
}

// ---- the candidate check: 0 the caller makes the draw, 1 it was made here, 2 it was left out
static int g_started;                // frames since the first swap (no probes in the first ones)
static int candidate(const drawcall *d)
{
    if (g_draw_fb == 0 || g_draw_fb >= MAX_FB || !g_fbo[g_draw_fb].used) return 0;
    fbo_t *f = &g_fbo[g_draw_fb];
    if (f->s[0].kind != K_TEX2D || f->s[0].textarget != GL_TEXTURE_2D || f->s[0].level != 0) return 0;
    for (int s = 1; s < 8; s++) if (f->s[s].kind != K_NONE) return 0;
    GLuint c = f->s[0].name;
    GLint prog = 0;
    q_GetIntegerv(GL_CURRENT_PROGRAM, &prog);
    if (prog <= 0 || prog >= MAX_PROG || g_prog_no[prog] >= 4) return 0;    // (not a copy four times: not measured again)
    // fixed-function state that a plain copy has
    if (q_IsEnabled(GL_BLEND) || q_IsEnabled(GL_RASTERIZER_DISCARD) || q_IsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE) ||
        q_IsEnabled(GL_SAMPLE_COVERAGE) || q_IsEnabled(GL_STENCIL_TEST)) return 0;
    GLint cm[4] = { 0, 0, 0, 0 }, db = 0, tf = 0, dm = 0, df = 0;
    q_GetIntegerv(GL_COLOR_WRITEMASK, cm);
    q_GetIntegerv(GL_DRAW_BUFFER0, &db);
    q_GetIntegerv(GL_TRANSFORM_FEEDBACK_ACTIVE, &tf);
    if (!(cm[0] && cm[1] && cm[2] && cm[3]) || db != GL_COLOR_ATTACHMENT0 || tf) return 0;
    int dt = q_IsEnabled(GL_DEPTH_TEST);
    if (dt) {
        q_GetIntegerv(GL_DEPTH_FUNC, &df);
        q_GetIntegerv(GL_DEPTH_WRITEMASK, &dm);
        if (df != GL_ALWAYS || dm) return 0;                // (depth left alone, every fragment passes)
    }
    GLint q1 = 0, q2 = 0;
    q_GetQueryiv(GL_ANY_SAMPLES_PASSED, GL_CURRENT_QUERY, &q1);
    q_GetQueryiv(GL_ANY_SAMPLES_PASSED_CONSERVATIVE, GL_CURRENT_QUERY, &q2);
    if (q1 || q2) return 0;
    rkey_t k = { 1469598103934665603ull };
    int unit = key_program(&k, (GLuint)prog);
    if (unit < 0) { g_prog_no[prog] = 255; return 0; }
    GLuint a = g_unit[unit];
    if (!a || a == c || rel_of(a) >= 0 || rel_of(c) >= 0 || blocked(a) || blocked(c)) return 0;
    // the texture on that unit must be a's (another target's binding would be sampled otherwise: the
    // sampler is a sampler2D, so its unit's 2D binding is what it reads)
    GLint act = 0;
    q_GetIntegerv(GL_ACTIVE_TEXTURE, &act);
    GLuint ra = to_real(a), rc = to_real(c);
    texinfo ta = tex_info(ra), tc = tex_info(rc);
    if (!ta.ok || !tc.ok || ta.w != tc.w || ta.h != tc.h || ta.imm != tc.imm) return 0;   // (storages that behave alike)
    GLint vp[4] = { 0, 0, 0, 0 }, box[4] = { 0, 0, 0, 0 };
    q_GetIntegerv(GL_VIEWPORT, vp);
    if (vp[0] || vp[1] || vp[2] != tc.w || vp[3] != tc.h) return 0;
    int sc = q_IsEnabled(GL_SCISSOR_TEST);
    if (sc) {
        q_GetIntegerv(GL_SCISSOR_BOX, box);
        if (box[0] > 0 || box[1] > 0 || box[0] + box[2] < tc.w || box[1] + box[3] < tc.h) return 0;
    }
    // the rest of what the draw's result depends on
    GLint smp = 0, cull = 0, cmode = 0, front = 0;
    q_ActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    q_GetIntegerv(GL_SAMPLER_BINDING, &smp);
    q_ActiveTexture((GLenum)act);
    if (smp) return 0;                                      // (a sampler object's parameters: not followed)
    cull = q_IsEnabled(GL_CULL_FACE);
    q_GetIntegerv(GL_CULL_FACE_MODE, &cmode);
    q_GetIntegerv(GL_FRONT_FACE, &front);
    params_t p;
    {
        GLint old = 0;
        q_GetIntegerv(GL_TEXTURE_BINDING_2D, &old);
        params_get(ra, &p);
        q_BindTexture(GL_TEXTURE_2D, (GLuint)old);
    }
    kh(&k, &p, sizeof p);
    khi(&k, ta.w); khi(&k, ta.h); khi(&k, unit); khi(&k, cull); khi(&k, cmode); khi(&k, front); khi(&k, dt);
    khi(&k, q_IsEnabled(GL_DITHER)); khi(&k, q_IsEnabled(GL_PRIMITIVE_RESTART_FIXED_INDEX)); khi(&k, q_IsEnabled(GL_POLYGON_OFFSET_FILL));
    khi(&k, d->kind); khi(&k, d->mode); khi(&k, d->first); khi(&k, d->count); khi(&k, d->start); khi(&k, d->end);
    if (!key_vertices(&k, (GLuint)prog, d)) return 0;
    known_t *kn = known(k.h);
    if (kn && kn->result == 1) {
        if (g_check) {
            // made, and compared with its source: both read back
            redraw(d);
            static unsigned char *bufs[2];
            static size_t have;
            size_t n = (size_t)tc.w * tc.h * 4;
            if (n > have) { free(bufs[0]); free(bufs[1]); bufs[0] = malloc(n); bufs[1] = malloc(n); have = bufs[0] && bufs[1] ? n : 0; }
            if (n <= have) {
                GLint pack = 0;
                GLuint rfb = g_read_fb;
                q_GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
                if (pack) q_BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                if (!g_tmp_fb[0]) q_GenFramebuffers(2, g_tmp_fb);
                r_BindFramebuffer(GL_READ_FRAMEBUFFER, g_tmp_fb[0]);
                r_FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ra, 0);
                q_ReadPixels(0, 0, tc.w, tc.h, GL_RGBA, GL_UNSIGNED_BYTE, bufs[0]);
                r_FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rc, 0);
                q_ReadPixels(0, 0, tc.w, tc.h, GL_RGBA, GL_UNSIGNED_BYTE, bufs[1]);
                r_FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                r_BindFramebuffer(GL_READ_FRAMEBUFFER, rfb);
                if (pack) q_BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
                g_checked++;
                if (memcmp(bufs[0], bufs[1], n)) {
                    size_t dd = 0;
                    for (size_t i = 0; i < n; i += 4) dd += memcmp(bufs[0] + i, bufs[1] + i, 4) != 0;
                    g_check_bad++;
                    fprintf(stderr, "[glrename] check: program %d's copy (%dx%d) differs from its source on %zu pixels\n", prog, tc.w, tc.h, dd);
                }
            }
            return 1;
        }
        // left out: c gets a's storage (the copy's result), a gets c's (owing it)
        if (g_nrel >= MAX_REL) return 0;
        swap_names(a, c);
        g_rel[g_nrel++] = (rel_t){ a, c, ra, tc.w, tc.h };
        g_renamed++;
        return 2;
    }
    if (kn) return 0;                                       // (being measured, or not a copy)
    if (g_probe.busy || g_started < 30) return 0;
    redraw(d);                                              // the draw as usual, then the probe
    probe_start(k.h, (GLuint)prog, d, unit, ra, tc.w, tc.h);
    return 1;
}

// ---- the draw entry points
static void give_all_back(void)
{
    while (g_nrel) give_by_blit(0);
}
static int foreign_check(void)
{
    if (!g_foreign) return 0;
    if (g_on) {
        give_all_back();
        fprintf(stderr, "[glrename] another thread used a swapped texture name: off from here\n");
    }
    g_on = 0;
    return 1;
}
// before a draw on the render thread; 1: leave it out
static int before_draw(const drawcall *d)
{
    if (!mine()) return 0;
    foreign_check();
    if (g_nrel) { pass_writes(0); unit_reads(); }
    int out = g_on && !g_pass_colour ? candidate(d) : 0;
    if (out != 2) g_pass_colour = 1;    // (left out: the pass has no colour write yet)
    return out != 0;
}
static void w_DrawArrays(GLenum m, GLint f, GLsizei c)
{ drawcall d = { 0, m, 0, f, c, NULL, 0, 0, 0 }; if (!before_draw(&d)) r_DrawArrays(m, f, c); }
static void w_DrawElements(GLenum m, GLsizei c, GLenum t, const void *i)
{ drawcall d = { 1, m, t, 0, c, i, 0, 0, 0 }; if (!before_draw(&d)) r_DrawElements(m, c, t, i); }
static void w_DrawRangeElements(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i)
{ drawcall d = { 3, m, t, 0, c, i, 0, s, e }; if (!before_draw(&d)) r_DrawRangeElements(m, s, e, c, t, i); }
static void w_DrawElementsBaseVertex(GLenum m, GLsizei c, GLenum t, const void *i, GLint b)
{ drawcall d = { 2, m, t, 0, c, i, b, 0, 0 }; if (!before_draw(&d)) r_DrawElementsBaseVertex(m, c, t, i, b); }
static void w_DrawElementsBaseVertexOES(GLenum m, GLsizei c, GLenum t, const void *i, GLint b)
{ drawcall d = { 2, m, t, 0, c, i, b, 0, 0 }; if (!before_draw(&d)) r_DrawElementsBaseVertexOES(m, c, t, i, b); }
static void w_DrawElementsBaseVertexEXT(GLenum m, GLsizei c, GLenum t, const void *i, GLint b)
{ drawcall d = { 2, m, t, 0, c, i, b, 0, 0 }; if (!before_draw(&d)) r_DrawElementsBaseVertexEXT(m, c, t, i, b); }
// the others are never candidates: their reads and writes count
static void plain_draw(void)
{
    if (!mine()) return;
    foreign_check();
    if (g_nrel) { pass_writes(0); unit_reads(); }
    g_pass_colour = 1;
}
#define DRAW(f, args, call) static void (*r_##f) args; static void w_##f args { plain_draw(); r_##f call; }
DRAW(DrawArraysInstanced, (GLenum m, GLint a, GLsizei c, GLsizei n), (m, a, c, n))
DRAW(DrawElementsInstanced, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n), (m, c, t, i, n))
DRAW(DrawArraysInstancedEXT, (GLenum m, GLint a, GLsizei c, GLsizei n), (m, a, c, n))
DRAW(DrawElementsInstancedEXT, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n), (m, c, t, i, n))
DRAW(DrawRangeElementsBaseVertex, (GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i, GLint b), (m, s, e, c, t, i, b))
DRAW(DrawRangeElementsBaseVertexOES, (GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i, GLint b), (m, s, e, c, t, i, b))
DRAW(DrawRangeElementsBaseVertexEXT, (GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i, GLint b), (m, s, e, c, t, i, b))
DRAW(DrawElementsInstancedBaseVertex, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint b), (m, c, t, i, n, b))
DRAW(DrawElementsInstancedBaseVertexOES, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint b), (m, c, t, i, n, b))
DRAW(DrawElementsInstancedBaseVertexEXT, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint b), (m, c, t, i, n, b))
DRAW(DrawArraysInstancedBaseInstanceEXT, (GLenum m, GLint a, GLsizei c, GLsizei n, GLuint b), (m, a, c, n, b))
DRAW(DrawElementsInstancedBaseInstanceEXT, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLuint b), (m, c, t, i, n, b))
DRAW(DrawElementsInstancedBaseVertexBaseInstanceEXT, (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint v, GLuint b),
     (m, c, t, i, n, v, b))
DRAW(MultiDrawArraysEXT, (GLenum m, const GLint *a, const GLsizei *c, GLsizei n), (m, a, c, n))
DRAW(MultiDrawElementsEXT, (GLenum m, const GLsizei *c, GLenum t, const void *const *i, GLsizei n), (m, c, t, i, n))
DRAW(MultiDrawElementsBaseVertexEXT, (GLenum m, const GLsizei *c, GLenum t, const void *const *i, GLsizei n, const GLint *b),
     (m, c, t, i, n, b))
DRAW(MultiDrawElementsBaseVertexOES, (GLenum m, const GLsizei *c, GLenum t, const void *const *i, GLsizei n, const GLint *b),
     (m, c, t, i, n, b))
DRAW(DrawArraysIndirect, (GLenum m, const void *p), (m, p))
DRAW(DrawElementsIndirect, (GLenum m, GLenum t, const void *p), (m, t, p))
#undef DRAW
static void w_DispatchCompute(GLuint x, GLuint y, GLuint z) { if (mine()) { foreign_check(); unit_reads(); } r_DispatchCompute(x, y, z); }
static void w_DispatchComputeIndirect(GLintptr o) { if (mine()) { foreign_check(); unit_reads(); } r_DispatchComputeIndirect(o); }

// ---- clears, invalidation, blits
static int whole_colour_clear(void)
{
    if (q_IsEnabled(GL_RASTERIZER_DISCARD)) return 0;
    GLint m[4] = { 0, 0, 0, 0 };
    q_GetIntegerv(GL_COLOR_WRITEMASK, m);
    if (!(m[0] && m[1] && m[2] && m[3])) return 0;
    if (!q_IsEnabled(GL_SCISSOR_TEST)) return 1;
    // (scissored: whole when the box covers every colour buffer; the relations' sizes are the ones that matter)
    GLint box[4] = { 0, 0, 0, 0 };
    q_GetIntegerv(GL_SCISSOR_BOX, box);
    fbo_t *f = g_draw_fb && g_draw_fb < MAX_FB ? &g_fbo[g_draw_fb] : NULL;
    for (int s = 0; f && s < 8; s++) {
        int r = f->s[s].kind != K_NONE ? rel_of(f->s[s].name) : -1;
        if (r >= 0 && (box[0] > 0 || box[1] > 0 || box[0] + box[2] < g_rel[r].w || box[1] + box[3] < g_rel[r].h)) return 0;
    }
    return 1;
}
static void w_Clear(GLbitfield m)
{
    if (mine()) {
        foreign_check();
        if (g_nrel && (m & GL_COLOR_BUFFER_BIT)) pass_writes(whole_colour_clear());
        if (m & GL_COLOR_BUFFER_BIT) g_pass_colour = 1;
    }
    r_Clear(m);
}
static void cleared_buffer(GLenum b)
{
    if (!mine()) return;
    foreign_check();
    if (g_nrel && b == GL_COLOR) pass_writes(0);
    if (b == GL_COLOR) g_pass_colour = 1;
}
static void w_ClearBufferfv(GLenum b, GLint d, const float *v) { cleared_buffer(b); r_ClearBufferfv(b, d, v); }
static void w_ClearBufferiv(GLenum b, GLint d, const GLint *v) { cleared_buffer(b); r_ClearBufferiv(b, d, v); }
static void w_ClearBufferuiv(GLenum b, GLint d, const GLuint *v) { cleared_buffer(b); r_ClearBufferuiv(b, d, v); }
// the game's invalidation of whole colour buffers: their pictures are not needed
static void invalidated(GLenum t, GLsizei n, const GLenum *a)
{
    if (!mine() || !g_nrel) return;
    foreign_check();
    GLuint fb = t == GL_READ_FRAMEBUFFER ? g_read_fb : g_draw_fb;
    if (!fb || fb >= MAX_FB) return;
    for (GLsizei i = 0; a && i < n; i++) {
        int s = a[i] == GL_COLOR ? 0 : a[i] >= GL_COLOR_ATTACHMENT0 && a[i] < GL_COLOR_ATTACHMENT0 + 8 ? (int)(a[i] - GL_COLOR_ATTACHMENT0) : -1;
        if (s >= 0 && g_fbo[fb].s[s].kind != K_NONE) replaced(g_fbo[fb].s[s].name);
    }
}
static void w_Invalidate(GLenum t, GLsizei n, const GLenum *a) { invalidated(t, n, a); r_Invalidate(t, n, a); }
static void w_Discard(GLenum t, GLsizei n, const GLenum *a) { invalidated(t, n, a); r_Discard(t, n, a); }
static void w_InvalidateSub(GLenum t, GLsizei n, const GLenum *a, GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (mine() && g_nrel) {          // (part of a picture undefined: the pass that follows needs the rest)
        foreign_check();
        GLuint fb = t == GL_READ_FRAMEBUFFER ? g_read_fb : g_draw_fb;
        for (int s = 0; fb && fb < MAX_FB && s < 8; s++) if (g_fbo[fb].s[s].kind != K_NONE) changed_part(g_fbo[fb].s[s].name);
    }
    r_InvalidateSub(t, n, a, x, y, w, h);
}
static void w_BlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield m, GLenum fl)
{
    if (mine()) {
        foreign_check();
        if (g_nrel) { fb_reads(g_read_fb); if (m & GL_COLOR_BUFFER_BIT) pass_writes(0); }
        if (m & GL_COLOR_BUFFER_BIT) g_pass_colour = 1;
    }
    r_BlitFramebuffer(a, b, c, d, e, f, g, h, m, fl);
}

// ---- texture names in and out
static void w_ActiveTexture(GLenum u) { r_ActiveTexture(u); if (mine()) g_active = (int)(u - GL_TEXTURE0); }
static void w_BindTexture(GLenum t, GLuint tex)
{
    r_BindTexture(t, to_real(tex));
    if (mine() && g_active >= 0 && g_active < MAX_UNIT) {
        if (t == GL_TEXTURE_2D) g_unit[g_active] = tex;
        else if (g_unit[g_active] && g_nmap) {
            // (another target on this unit: its 2D binding stays)
        }
    }
}
static GLboolean w_IsTexture(GLuint t) { return r_IsTexture(to_real(t)); }
// a name that the game deletes: its storage must be its own number's (the permutation stays one
// of live names); a picture that lives in that storage is copied out first
static void w_DeleteTextures(GLsizei n, const GLuint *t)
{
    if (!mine()) {
        // (another thread: each name's own storage deleted; a swapped one turns this off)
        GLuint mapped[64];
        if (!g_nmap || n > 64) { r_DeleteTextures(n, t); return; }
        for (GLsizei i = 0; i < n; i++) { mapped[i] = to_real(t[i]); if (mapped[i] != t[i]) g_foreign = 1; }
        r_DeleteTextures(n, mapped);
        return;
    }
    foreign_check();
    for (GLsizei i = 0; t && i < n; i++) {
        GLuint a = t[i];
        if (!a) continue;
        replaced(a);                                       // (its picture is not needed; the other name keeps it)
        GLuint ra = to_real(a);
        if (ra != a) {
            // the name whose storage is a's number gets a's storage, with its picture
            GLuint b = to_app(a);
            texinfo tb = tex_info(a);
            blit_copy(a, ra, tb.w, tb.h);
            map_set(a, a);
            map_set(b, ra);
            GLint act = 0;
            q_GetIntegerv(GL_ACTIVE_TEXTURE, &act);
            for (int u = 0; u < MAX_UNIT; u++) if (g_unit[u] == b) { q_ActiveTexture(GL_TEXTURE0 + (GLenum)u); q_BindTexture(GL_TEXTURE_2D, ra); }
            q_ActiveTexture((GLenum)act);
            // (parameters: b's live in storage a; move them)
            GLint old = 0;
            q_GetIntegerv(GL_TEXTURE_BINDING_2D, &old);
            params_t p;
            params_get(a, &p); params_set(ra, &p);
            q_BindTexture(GL_TEXTURE_2D, (GLuint)old);
            reattach(b, 0);
        }
        for (int u = 0; u < MAX_UNIT; u++) if (g_unit[u] == a) g_unit[u] = 0;
        // framebuffers bound now lose the attachment (GL's rule); others keep a dead one
        for (GLuint fb = 1; fb < MAX_FB; fb++) {
            if (!g_fbo[fb].used) continue;
            for (int s = 0; s < MAX_SLOT; s++) if (g_fbo[fb].s[s].kind != K_NONE && g_fbo[fb].s[s].name == a) {
                if (fb == g_draw_fb || fb == g_read_fb) g_fbo[fb].s[s].kind = K_NONE;
                else g_fbo[fb].s[s].name = 0, g_fbo[fb].s[s].kind = K_OTHER;
            }
        }
    }
    r_DeleteTextures(n, t);
}

// redefinition and uploads through the bound texture
static GLuint bound_app(GLenum target)
{
    if (target != GL_TEXTURE_2D || g_active < 0 || g_active >= MAX_UNIT) return 0;
    return g_unit[g_active];
}
static void tex_redefined(GLenum target) { if (mine() && g_nrel) { foreign_check(); GLuint a = bound_app(target); if (a) replaced(a); } }
static void tex_part(GLenum target) { if (mine() && g_nrel) { foreign_check(); GLuint a = bound_app(target); if (a) changed_part(a); } }
#define TEXCALL(f, args, call, rule) static void (*r_##f) args; static void w_##f args { rule; r_##f call; }
TEXCALL(TexImage2D, (GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLint b, GLenum fm, GLenum ty, const void *p),
        (t, l, f, w, h, b, fm, ty, p), tex_redefined(t))
TEXCALL(TexStorage2D, (GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h), (t, l, f, w, h), tex_redefined(t))
TEXCALL(TexStorage2DEXT, (GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h), (t, l, f, w, h), tex_redefined(t))
TEXCALL(CompressedTexImage2D, (GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei s, const void *d),
        (t, l, f, w, h, b, s, d), tex_redefined(t))
TEXCALL(CopyTexImage2D, (GLenum t, GLint l, GLenum f, GLint x, GLint y, GLsizei w, GLsizei h, GLint b), (t, l, f, x, y, w, h, b),
        { if (mine() && g_nrel) fb_reads(g_read_fb); tex_redefined(t); })
TEXCALL(EGLImageTargetTexture2DOES, (GLenum t, void *img), (t, img), tex_redefined(t))
TEXCALL(TexSubImage2D, (GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *p),
        (t, l, x, y, w, h, f, ty, p), tex_part(t))
TEXCALL(CompressedTexSubImage2D, (GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLsizei s, const void *d),
        (t, l, x, y, w, h, f, s, d), tex_part(t))
TEXCALL(CopyTexSubImage2D, (GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h), (t, l, xo, yo, x, y, w, h),
        { if (mine() && g_nrel) fb_reads(g_read_fb); tex_part(t); })
TEXCALL(GenerateMipmap, (GLenum t), (t), tex_part(t))
TEXCALL(CopyTexSubImage3D, (GLenum t, GLint l, GLint xo, GLint yo, GLint zo, GLint x, GLint y, GLsizei w, GLsizei h),
        (t, l, xo, yo, zo, x, y, w, h), { if (mine() && g_nrel) fb_reads(g_read_fb); })
#undef TEXCALL

static void image_copy(GLuint *s, GLenum st, GLuint *d, GLenum dt)
{
    if (mine() && g_nrel) {
        foreign_check();
        if (st != GL_RENDERBUFFER) want_picture(*s);
        if (dt != GL_RENDERBUFFER) changed_part(*d);
    }
    if (st != GL_RENDERBUFFER) *s = to_real(*s);
    if (dt != GL_RENDERBUFFER) *d = to_real(*d);
}
#define COPYIMG(f) static void w_##f(GLuint s, GLenum st, GLint sl, GLint sx, GLint sy, GLint sz, GLuint d, GLenum dt, GLint dl, \
                                     GLint dx, GLint dy, GLint dz, GLsizei w, GLsizei h, GLsizei dp) \
    { image_copy(&s, st, &d, dt); r_##f(s, st, sl, sx, sy, sz, d, dt, dl, dx, dy, dz, w, h, dp); }
COPYIMG(CopyImageSubData) COPYIMG(CopyImageSubDataEXT) COPYIMG(CopyImageSubDataOES)
#undef COPYIMG
static void w_BindImageTexture(GLuint u, GLuint t, GLint l, GLboolean ly, GLint layer, GLenum a, GLenum f)
{
    if (mine() && g_nrel && t) { foreign_check(); int r = rel_of(t); if (r >= 0) give_by_blit(r); }
    r_BindImageTexture(u, to_real(t), l, ly, layer, a, f);
}
#define GL_TEXTURE_LABEL 0x1702
static void w_ObjectLabel(GLenum id, GLuint n, GLsizei l, const char *s) { r_ObjectLabel(id, id == GL_TEXTURE ? to_real(n) : n, l, s); }
static void w_ObjectLabelKHR(GLenum id, GLuint n, GLsizei l, const char *s) { r_ObjectLabelKHR(id, id == GL_TEXTURE ? to_real(n) : n, l, s); }
static void w_GetObjectLabel(GLenum id, GLuint n, GLsizei b, GLsizei *l, char *s) { r_GetObjectLabel(id, id == GL_TEXTURE ? to_real(n) : n, b, l, s); }
static void w_GetObjectLabelKHR(GLenum id, GLuint n, GLsizei b, GLsizei *l, char *s) { r_GetObjectLabelKHR(id, id == GL_TEXTURE ? to_real(n) : n, b, l, s); }
static void w_GetObjectLabelEXT(GLenum id, GLuint n, GLsizei b, GLsizei *l, char *s) { r_GetObjectLabelEXT(id, id == 0x8DF5 || id == GL_TEXTURE ? to_real(n) : n, b, l, s); }
static void w_LabelObjectEXT(GLenum id, GLuint n, GLsizei l, const char *s) { r_LabelObjectEXT(id, id == 0x8DF5 || id == GL_TEXTURE ? to_real(n) : n, l, s); }
static void w_GetTextureParameteriv(GLuint t, GLenum p, GLint *v) { r_GetTextureParameteriv(to_real(t), p, v); }
static void texture_view(void (*r)(GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint), GLuint t, GLenum tg, GLuint o, GLenum f,
                         GLuint ml, GLuint nl, GLuint mly, GLuint nly)
{
    if (mine() && g_nrel) { foreign_check(); int k = rel_of(o); if (k >= 0) give_by_blit(k); }
    if (mine() && to_real(o) != o) { g_foreign = 1; foreign_check(); }   // (a view of a swapped storage: give names back, stop)
    r(t, tg, to_real(o), f, ml, nl, mly, nly);
}
static void w_TextureView(GLuint t, GLenum tg, GLuint o, GLenum f, GLuint ml, GLuint nl, GLuint mly, GLuint nly) { texture_view(r_TextureView, t, tg, o, f, ml, nl, mly, nly); }
static void w_TextureViewEXT(GLuint t, GLenum tg, GLuint o, GLenum f, GLuint ml, GLuint nl, GLuint mly, GLuint nly) { texture_view(r_TextureViewEXT, t, tg, o, f, ml, nl, mly, nly); }
static void w_TextureViewOES(GLuint t, GLenum tg, GLuint o, GLenum f, GLuint ml, GLuint nl, GLuint mly, GLuint nly) { texture_view(r_TextureViewOES, t, tg, o, f, ml, nl, mly, nly); }

// queries that return texture names: the game's names
static int is_tex_binding(GLenum p)
{
    return p == GL_TEXTURE_BINDING_2D || p == GL_TEXTURE_BINDING_3D || p == GL_TEXTURE_BINDING_2D_ARRAY || p == GL_TEXTURE_BINDING_CUBE_MAP ||
           p == GL_TEXTURE_BINDING_EXTERNAL_OES || p == GL_TEXTURE_BINDING_2D_MULTISAMPLE || p == GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY ||
           p == GL_TEXTURE_BINDING_CUBE_MAP_ARRAY || p == GL_TEXTURE_BINDING_BUFFER;
}
static void w_GetIntegerv(GLenum p, GLint *v)
{
    r_GetIntegerv(p, v);
    if (v && is_tex_binding(p)) *v = (GLint)to_app((GLuint)*v);
}
static void w_GetIntegeri_v(GLenum p, GLuint i, GLint *v)
{
    r_GetIntegeri_v(p, i, v);
    if (v && p == GL_IMAGE_BINDING_NAME) *v = (GLint)to_app((GLuint)*v);
}
static void w_GetFbAttachment(GLenum t, GLenum a, GLenum p, GLint *v)
{
    r_GetFbAttachment(t, a, p, v);
    if (v && p == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) {
        GLint type = 0;
        q_GetFbAttachment(t, a, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        if (type == GL_TEXTURE) *v = (GLint)to_app((GLuint)*v);
    }
}

// ---- framebuffers
static void w_BindFramebuffer(GLenum t, GLuint fb)
{
    r_BindFramebuffer(t, fb);
    if (!mine()) return;
    if (t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER) { if (fb != g_draw_fb) g_pass_colour = 0; g_draw_fb = fb; }
    if (t == GL_FRAMEBUFFER || t == GL_READ_FRAMEBUFFER) g_read_fb = fb;
}
static void w_DeleteFramebuffers(GLsizei n, const GLuint *fbs)
{
    r_DeleteFramebuffers(n, fbs);
    if (!mine()) return;
    for (GLsizei i = 0; fbs && i < n; i++) {
        if (fbs[i] < MAX_FB) memset(&g_fbo[fbs[i]], 0, sizeof g_fbo[0]);
        if (fbs[i] == g_draw_fb) g_draw_fb = 0;
        if (fbs[i] == g_read_fb) g_read_fb = 0;
    }
}
static void attach_rule(GLenum t)
{
    if (!mine()) return;
    foreign_check();
    if (bound_fb(t) == g_draw_fb) g_pass_colour = 0;       // (the driver ends the pass at a change of its buffers)
}
static void w_FramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l)
{ attach_rule(t); r_FramebufferTexture2D(t, a, tt, to_real(tex), l); record(t, a, K_TEX2D, tt, tex, l, 0, 0); }
static void w_FramebufferTexture3D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l, GLint z)
{ attach_rule(t); r_FramebufferTexture3D(t, a, tt, to_real(tex), l, z); record(t, a, K_OTHER, tt, tex, l, z, 0); }
static void w_FramebufferTexture3DOES(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l, GLint z)
{ attach_rule(t); r_FramebufferTexture3DOES(t, a, tt, to_real(tex), l, z); record(t, a, K_OTHER, tt, tex, l, z, 0); }
static void w_FramebufferTextureLayer(GLenum t, GLenum a, GLuint tex, GLint l, GLint layer)
{ attach_rule(t); r_FramebufferTextureLayer(t, a, to_real(tex), l, layer); record(t, a, K_LAYER, 0, tex, l, layer, 0); }
static void w_FramebufferTexture(GLenum t, GLenum a, GLuint tex, GLint l)
{ attach_rule(t); r_FramebufferTexture(t, a, to_real(tex), l); record(t, a, K_TEX, 0, tex, l, 0, 0); }
static void w_FramebufferTextureEXT(GLenum t, GLenum a, GLuint tex, GLint l)
{ attach_rule(t); r_FramebufferTextureEXT(t, a, to_real(tex), l); record(t, a, K_TEX, 0, tex, l, 0, 0); }
static void w_FramebufferTextureOES(GLenum t, GLenum a, GLuint tex, GLint l)
{ attach_rule(t); r_FramebufferTextureOES(t, a, to_real(tex), l); record(t, a, K_TEX, 0, tex, l, 0, 0); }
static void w_FramebufferTexture2DMS(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l, GLsizei s)
{ attach_rule(t); r_FramebufferTexture2DMS(t, a, tt, to_real(tex), l, s); record(t, a, K_TEX2DMS, tt, tex, l, 0, s); }
static void w_FramebufferTexture2DMSIMG(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l, GLsizei s)
{ attach_rule(t); r_FramebufferTexture2DMSIMG(t, a, tt, to_real(tex), l, s); record(t, a, K_OTHER, tt, tex, l, 0, s); }
static void w_FramebufferTextureMultiviewOVR(GLenum t, GLenum a, GLuint tex, GLint l, GLint bv, GLsizei nv)
{ attach_rule(t); r_FramebufferTextureMultiviewOVR(t, a, to_real(tex), l, bv, nv); record(t, a, K_OTHER, 0, tex, l, bv, nv); }
static void w_FramebufferTextureMSMultiviewOVR(GLenum t, GLenum a, GLuint tex, GLint l, GLsizei s, GLint bv, GLsizei nv)
{ attach_rule(t); r_FramebufferTextureMSMultiviewOVR(t, a, to_real(tex), l, s, bv, nv); record(t, a, K_OTHER, 0, tex, l, bv, nv); }
static void w_FramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint rb)
{ attach_rule(t); r_FramebufferRenderbuffer(t, a, rt, rb); record(t, a, K_NONE, 0, 0, 0, 0, 0); }

// swapped names attached in a way a swap cannot redo (3D, IMG, multiview): give everything back, stop
static void check_attachments(void)
{
    if (!g_nmap) return;
    for (GLuint fb = 1; fb < MAX_FB; fb++) {
        if (!g_fbo[fb].used) continue;
        for (int s = 0; s < MAX_SLOT; s++)
            if (g_fbo[fb].s[s].kind == K_OTHER && g_fbo[fb].s[s].name && to_real(g_fbo[fb].s[s].name) != g_fbo[fb].s[s].name) {
                g_foreign = 1;
                return;
            }
    }
}

void glrename_frame(void)
{
    if (!g_on && !g_nmap) return;
    if (!g_have_thread) {
        void *c = q_GetCurrentContext();
        if (g_ctx && c != g_ctx) {
            fprintf(stderr, "[glrename] off: the context that swaps is not the one that made the render textures\n");
            g_on = 0;
        }
        if (!g_ctx) g_ctx = c;
        if (g_multi) fprintf(stderr, "[glrename] another context makes texture calls too: textures it attaches are not renamed\n");
        g_thread = pthread_self(); g_have_thread = 1;
        GLint fb = 0;
        q_GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb); g_draw_fb = (GLuint)fb;
        q_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &fb); g_read_fb = (GLuint)fb;
        GLint act = 0;
        q_GetIntegerv(GL_ACTIVE_TEXTURE, &act); g_active = act - (GLint)GL_TEXTURE0;
        return;
    }
    g_started++;
    check_attachments();
    foreign_check();
    probe_poll();
    static long frames;
    if (++frames % 1200 == 0)
        fprintf(stderr, "[glrename] %ld frames: %ld copies left out; pictures given in a pass %ld, by a blit %ld; %ld swaps; probes %ld (%ld exact, %ld not)%s\n",
                frames, g_renamed, g_copied_tile, g_copied_pass, g_swaps, g_probes, g_probe_yes, g_probe_no, g_check ? "" : "");
    if (g_check && frames % 600 == 0)
        fprintf(stderr, "[glrename] check, %ld frames: %ld copies made and compared, %ld differed from their source\n", frames, g_checked, g_check_bad);
}

static so_shim g_shims[160];

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

const so_shim *glrename_table(void *(*next)(const char *name))
{
    memset(g_shims, 0, sizeof g_shims);
    g_on = 0; g_check = 0; g_have_thread = 0; g_nmap = 0; g_nrel = 0; g_nknown = 0; g_foreign = 0; g_ctx = NULL; g_multi = 0; g_nblocked = 0;
    const char *e = getenv("UA_GL_RENAME");
    if (!e || !strcmp(e, "0")) return g_shims;
    g_check = !strcmp(e, "check");
#define Q(var, name) *(void **)&var = gl(NULL, name)
    Q(q_GetIntegerv, "glGetIntegerv"); Q(q_GetFloatv, "glGetFloatv"); Q(q_IsEnabled, "glIsEnabled");
    Q(q_GetFbAttachment, "glGetFramebufferAttachmentParameteriv"); Q(q_GetTexLevelParameteriv, "glGetTexLevelParameteriv");
    Q(q_GetTexParameteriv, "glGetTexParameteriv"); Q(q_GetTexParameterfv, "glGetTexParameterfv"); Q(q_TexParameteri, "glTexParameteri");
    Q(q_TexParameterf, "glTexParameterf"); Q(q_BindTexture, "glBindTexture"); Q(q_ActiveTexture, "glActiveTexture");
    Q(q_GenTextures, "glGenTextures"); Q(q_DeleteTextures, "glDeleteTextures"); Q(q_TexStorage2D, "glTexStorage2D");
    Q(q_TexSubImage2D, "glTexSubImage2D"); Q(q_PixelStorei, "glPixelStorei"); Q(q_GenFramebuffers, "glGenFramebuffers");
    Q(q_GetProgramiv, "glGetProgramiv"); Q(q_GetActiveUniform, "glGetActiveUniform"); Q(q_GetActiveAttrib, "glGetActiveAttrib");
    Q(q_GetUniformLocation, "glGetUniformLocation"); Q(q_GetAttribLocation, "glGetAttribLocation"); Q(q_GetUniformfv, "glGetUniformfv");
    Q(q_GetUniformiv, "glGetUniformiv"); Q(q_GetVertexAttribiv, "glGetVertexAttribiv"); Q(q_GetVertexAttribfv, "glGetVertexAttribfv");
    Q(q_GetVertexAttribPointerv, "glGetVertexAttribPointerv"); Q(q_GetQueryiv, "glGetQueryiv"); Q(q_Enable, "glEnable");
    Q(q_Disable, "glDisable"); Q(q_ColorMask, "glColorMask"); Q(q_Viewport, "glViewport"); Q(q_UseProgram, "glUseProgram");
    Q(q_BindVertexArray, "glBindVertexArray"); Q(q_GenVertexArrays, "glGenVertexArrays"); Q(q_BindSampler, "glBindSampler");
    Q(q_CreateShader, "glCreateShader"); Q(q_ShaderSource, "glShaderSource"); Q(q_CompileShader, "glCompileShader");
    Q(q_CreateProgram, "glCreateProgram"); Q(q_AttachShader, "glAttachShader"); Q(q_LinkProgram, "glLinkProgram");
    Q(q_Uniform1i, "glUniform1i"); Q(q_GenBuffers, "glGenBuffers"); Q(q_BindBuffer, "glBindBuffer"); Q(q_BufferData, "glBufferData");
    Q(q_MapBufferRange, "glMapBufferRange"); Q(q_UnmapBuffer, "glUnmapBuffer"); Q(q_FenceSync, "glFenceSync");
    Q(q_ClientWaitSync, "glClientWaitSync"); Q(q_DeleteSync, "glDeleteSync"); Q(q_ReadPixels, "glReadPixels"); Q(q_Finish, "glFinish");
#undef Q
    {
        // (the one already loaded first: the driver's, or a test's fake)
        *(void **)&q_GetCurrentContext = dlsym(RTLD_DEFAULT, "eglGetCurrentContext");
        void *egl = q_GetCurrentContext ? NULL : dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!q_GetCurrentContext && !egl) egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
        if (!q_GetCurrentContext && egl) *(void **)&q_GetCurrentContext = dlsym(egl, "eglGetCurrentContext");
        if (!q_GetCurrentContext) { fprintf(stderr, "[glrename] off: no eglGetCurrentContext\n"); return g_shims; }
    }
    int k = 0, ok = 1;
#define REQ(f, n) do { *(void **)&r_##f = gl(next, n); if (r_##f) g_shims[k++] = (so_shim){ n, (void *)w_##f }; else ok = 0; } while (0)
#define OPT(f, n) do { *(void **)&r_##f = gl(next, n); if (r_##f) g_shims[k++] = (so_shim){ n, (void *)w_##f }; } while (0)
    REQ(BindTexture, "glBindTexture"); REQ(ActiveTexture, "glActiveTexture"); REQ(DeleteTextures, "glDeleteTextures");
    REQ(BindFramebuffer, "glBindFramebuffer"); REQ(DeleteFramebuffers, "glDeleteFramebuffers");
    REQ(FramebufferTexture2D, "glFramebufferTexture2D"); REQ(FramebufferTextureLayer, "glFramebufferTextureLayer");
    REQ(FramebufferRenderbuffer, "glFramebufferRenderbuffer"); REQ(GetFbAttachment, "glGetFramebufferAttachmentParameteriv");
    REQ(GetIntegerv, "glGetIntegerv"); REQ(Clear, "glClear"); REQ(ClearBufferfv, "glClearBufferfv");
    REQ(ClearBufferiv, "glClearBufferiv"); REQ(ClearBufferuiv, "glClearBufferuiv"); REQ(Invalidate, "glInvalidateFramebuffer");
    REQ(BlitFramebuffer, "glBlitFramebuffer"); REQ(ReadPixels, "glReadPixels"); REQ(DrawArrays, "glDrawArrays");
    REQ(DrawElements, "glDrawElements"); REQ(DrawRangeElements, "glDrawRangeElements"); REQ(DrawArraysInstanced, "glDrawArraysInstanced");
    REQ(DrawElementsInstanced, "glDrawElementsInstanced"); REQ(BufferData, "glBufferData"); REQ(BufferSubData, "glBufferSubData");
    REQ(MapBufferRange, "glMapBufferRange"); REQ(FlushMappedBufferRange, "glFlushMappedBufferRange"); REQ(UnmapBuffer, "glUnmapBuffer");
    REQ(CopyBufferSubData, "glCopyBufferSubData"); REQ(DeleteBuffers, "glDeleteBuffers"); REQ(BindBufferBase, "glBindBufferBase");
    REQ(BindBufferRange, "glBindBufferRange"); REQ(LinkProgram, "glLinkProgram"); REQ(ProgramBinary, "glProgramBinary");
    REQ(DeleteProgram, "glDeleteProgram"); REQ(TexImage2D, "glTexImage2D"); REQ(TexStorage2D, "glTexStorage2D");
    REQ(CompressedTexImage2D, "glCompressedTexImage2D"); REQ(CopyTexImage2D, "glCopyTexImage2D"); REQ(TexSubImage2D, "glTexSubImage2D");
    REQ(CompressedTexSubImage2D, "glCompressedTexSubImage2D"); REQ(CopyTexSubImage2D, "glCopyTexSubImage2D");
    REQ(GenerateMipmap, "glGenerateMipmap"); REQ(CopyTexSubImage3D, "glCopyTexSubImage3D");
    OPT(IsTexture, "glIsTexture"); OPT(FramebufferTexture3D, "glFramebufferTexture3D"); OPT(FramebufferTexture3DOES, "glFramebufferTexture3DOES");
    OPT(FramebufferTexture, "glFramebufferTexture"); OPT(FramebufferTextureEXT, "glFramebufferTextureEXT");
    OPT(FramebufferTextureOES, "glFramebufferTextureOES"); OPT(FramebufferTexture2DMS, "glFramebufferTexture2DMultisampleEXT");
    OPT(FramebufferTexture2DMSIMG, "glFramebufferTexture2DMultisampleIMG");
    OPT(FramebufferTextureMultiviewOVR, "glFramebufferTextureMultiviewOVR");
    OPT(FramebufferTextureMSMultiviewOVR, "glFramebufferTextureMultisampleMultiviewOVR"); OPT(GetIntegeri_v, "glGetIntegeri_v");
    OPT(CopyImageSubData, "glCopyImageSubData"); OPT(CopyImageSubDataEXT, "glCopyImageSubDataEXT");
    OPT(CopyImageSubDataOES, "glCopyImageSubDataOES"); OPT(BindImageTexture, "glBindImageTexture"); OPT(ObjectLabel, "glObjectLabel");
    OPT(ObjectLabelKHR, "glObjectLabelKHR"); OPT(GetObjectLabel, "glGetObjectLabel"); OPT(GetObjectLabelKHR, "glGetObjectLabelKHR");
    OPT(GetObjectLabelEXT, "glGetObjectLabelEXT"); OPT(LabelObjectEXT, "glLabelObjectEXT");
    OPT(GetTextureParameteriv, "glGetTextureParameteriv"); OPT(TextureView, "glTextureView"); OPT(TextureViewEXT, "glTextureViewEXT");
    OPT(TextureViewOES, "glTextureViewOES"); OPT(TexStorage2DEXT, "glTexStorage2DEXT");
    OPT(EGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES"); OPT(InvalidateSub, "glInvalidateSubFramebuffer");
    OPT(Discard, "glDiscardFramebufferEXT"); OPT(DispatchCompute, "glDispatchCompute");
    OPT(DispatchComputeIndirect, "glDispatchComputeIndirect");
    OPT(DrawElementsBaseVertex, "glDrawElementsBaseVertex"); OPT(DrawElementsBaseVertexOES, "glDrawElementsBaseVertexOES");
    OPT(DrawElementsBaseVertexEXT, "glDrawElementsBaseVertexEXT");
    OPT(DrawArraysInstancedEXT, "glDrawArraysInstancedEXT"); OPT(DrawElementsInstancedEXT, "glDrawElementsInstancedEXT");
    OPT(DrawRangeElementsBaseVertex, "glDrawRangeElementsBaseVertex");
    OPT(DrawRangeElementsBaseVertexOES, "glDrawRangeElementsBaseVertexOES");
    OPT(DrawRangeElementsBaseVertexEXT, "glDrawRangeElementsBaseVertexEXT");
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
    g_fbo = calloc(MAX_FB, sizeof *g_fbo);
    g_buf = calloc(MAX_BUF, sizeof *g_buf);
    g_prog_gen = calloc(MAX_PROG, sizeof *g_prog_gen);
    g_prog_no = calloc(MAX_PROG, 1);
    void **qs[] = { (void **)&q_GetIntegerv, (void **)&q_IsEnabled, (void **)&q_GetFbAttachment, (void **)&q_GetTexLevelParameteriv,
                    (void **)&q_GetTexParameteriv, (void **)&q_GetTexParameterfv, (void **)&q_TexParameteri, (void **)&q_TexParameterf,
                    (void **)&q_BindTexture, (void **)&q_ActiveTexture, (void **)&q_GenTextures, (void **)&q_TexStorage2D,
                    (void **)&q_TexSubImage2D, (void **)&q_PixelStorei, (void **)&q_GenFramebuffers, (void **)&q_GetProgramiv,
                    (void **)&q_GetActiveUniform, (void **)&q_GetActiveAttrib, (void **)&q_GetUniformLocation, (void **)&q_GetAttribLocation,
                    (void **)&q_GetUniformfv, (void **)&q_GetUniformiv, (void **)&q_GetVertexAttribiv, (void **)&q_GetVertexAttribfv,
                    (void **)&q_GetVertexAttribPointerv, (void **)&q_GetQueryiv, (void **)&q_Enable, (void **)&q_Disable, (void **)&q_ColorMask,
                    (void **)&q_Viewport, (void **)&q_UseProgram, (void **)&q_BindVertexArray, (void **)&q_GenVertexArrays,
                    (void **)&q_BindSampler, (void **)&q_CreateShader, (void **)&q_ShaderSource, (void **)&q_CompileShader,
                    (void **)&q_CreateProgram, (void **)&q_AttachShader, (void **)&q_LinkProgram, (void **)&q_Uniform1i, (void **)&q_GenBuffers,
                    (void **)&q_BindBuffer, (void **)&q_BufferData, (void **)&q_MapBufferRange, (void **)&q_UnmapBuffer, (void **)&q_FenceSync,
                    (void **)&q_ClientWaitSync, (void **)&q_DeleteSync, (void **)&q_ReadPixels };
    for (unsigned i = 0; i < sizeof qs / sizeof qs[0]; i++) if (!*qs[i]) ok = 0;
    if (!ok || !g_fbo || !g_buf || !g_prog_gen || !g_prog_no) {
        memset(g_shims, 0, sizeof g_shims);     // (a GLES 2 driver: nothing wrapped)
        return g_shims;
    }
    g_on = 1;
    fprintf(stderr, "[glrename] on%s: copies of one texture into another left out where they are exact (%d entry points)\n",
            g_check ? " (check: made and compared instead)" : "", k);
    return g_shims;
}
