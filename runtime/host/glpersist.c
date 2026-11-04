// Unity's dynamic geometry without a map, flush and unmap through the driver for each chunk.
// UA_GL_PERSIST=1 (off by default; a port turns it on).
//
// Unity's GLES device writes its dynamic geometry (sprites, UI, dynamic batches) into buffers of a
// pool: each chunk takes a buffer, maps all of it (glMapBufferRange: write, invalidate range,
// explicit flush, unsynchronized), writes, flushes and unmaps it, and the buffer goes back to the
// pool once the GPU is done with it (Unity's fences). On Hollow Knight's Greenpath that is ~150 maps
// a frame, each a map, a flush and an unmap in the Mali driver: about 6 % of the render thread in
// the map alone. With this layer (Mali-G31, r20p0, muOS): Greenpath 37.8 / 37.5 -> 39.2 / 39.2 fps,
// its worst second 29.7 / 26.7 -> 32.6 / 33.5, 0 buffers redefined, 0 waits.
//
// With GL_EXT_buffer_storage, a buffer the game creates for dynamic use (glBufferData with
// GL_DYNAMIC_DRAW or GL_STREAM_DRAW, bound to the vertex or index target) gets
// persistent, coherent storage instead (glBufferStorageEXT with GL_DYNAMIC_STORAGE_BIT, so its
// glBufferSubData still works) and is mapped once, for good. The game's later maps of it return
// that mapping at the offset asked for; its flushes do nothing (coherent: the GPU sees the CPU's
// writes) and its unmaps return GL_TRUE without the driver. Unity maps these buffers unsynchronized
// only, so it already keeps the GPU's reads apart from its writes with its own fences: the GPU reads
// the same bytes as before.
//
// What it cannot do: storage made this way cannot be redefined. A second glBufferData of such a
// buffer waits for the GPU (glFinish) and copies the data in when it fits, else it is logged and
// the buffer is left as it is (none seen in Hollow Knight). A map that is not unsynchronized (the
// game asking the driver to wait for the GPU) waits with glFinish too. Logged: "[glpersist] ..."
// at the start and every 10 s (buffers made persistent, maps served).
#include "glpersist.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned GLbitfield;
typedef unsigned char GLboolean; typedef intptr_t GLintptr; typedef intptr_t GLsizeiptr;

#define GL_EXTENSIONS 0x1F03
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_UNIFORM_BUFFER_BINDING 0x8A28
#define GL_COPY_READ_BUFFER 0x8F36
#define GL_COPY_WRITE_BUFFER 0x8F37
#define GL_STREAM_DRAW 0x88E0
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#define GL_MAP_PERSISTENT_BIT_EXT 0x0040
#define GL_MAP_COHERENT_BIT_EXT 0x0080
#define GL_DYNAMIC_STORAGE_BIT_EXT 0x0100

static so_shim g_shims[8];
static void (*r_BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void *(*r_MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
static void (*r_FlushMappedBufferRange)(GLenum, GLintptr, GLsizeiptr);
static GLboolean (*r_UnmapBuffer)(GLenum);
static void (*r_DeleteBuffers)(GLsizei, const GLuint *);
static void (*q_BufferStorageEXT)(GLenum, GLsizeiptr, const void *, GLbitfield);
static void (*q_GetIntegerv)(GLenum, GLint *);
static const unsigned char *(*q_GetString)(GLenum);
static void (*q_Finish)(void);

static int g_ok = -1;                       // -1: the extension not checked yet (no context before)
static long g_made, g_maps, g_respec, g_waits, g_bytes;
static double g_last_log;

// the buffers made persistent: name -> mapping, size
struct pbuf { GLuint name; void *ptr; GLsizeiptr size; };
static struct pbuf *g_tab;
static size_t g_cap, g_n;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static struct pbuf *find(GLuint name)
{
    if (!name || !g_cap) return NULL;
    size_t i = name & (g_cap - 1);
    while (g_tab[i].name) { if (g_tab[i].name == name) return &g_tab[i]; i = (i + 1) & (g_cap - 1); }
    return NULL;
}

static struct pbuf *add(GLuint name)
{
    if ((g_n + 1) * 2 > g_cap) {
        size_t cap = g_cap ? g_cap * 2 : 1024;
        struct pbuf *t = calloc(cap, sizeof *t);
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
    struct pbuf *p = find(name);
    if (!p) return;
    // (open addressing: re-insert the run after it)
    size_t i = (size_t)(p - g_tab);
    p->name = 0; g_n--;
    for (size_t j = (i + 1) & (g_cap - 1); g_tab[j].name; j = (j + 1) & (g_cap - 1)) {
        struct pbuf e = g_tab[j]; g_tab[j].name = 0; g_n--;
        *add(e.name) = e;
    }
}

static GLenum binding_of(GLenum target)
{
    switch (target) {
    case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
    case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
    case GL_UNIFORM_BUFFER: return GL_UNIFORM_BUFFER_BINDING;
    case GL_COPY_READ_BUFFER: return GL_COPY_READ_BUFFER;          // (the binding enums equal the targets)
    case GL_COPY_WRITE_BUFFER: return GL_COPY_WRITE_BUFFER;
    default: return 0;
    }
}

static GLuint bound(GLenum target)
{
    GLenum b = binding_of(target);
    GLint v = 0;
    if (b) q_GetIntegerv(b, &v);
    return (GLuint)v;
}

static int check_ext(void)
{
    if (g_ok >= 0) return g_ok;
    const char *e = q_GetString ? (const char *)q_GetString(GL_EXTENSIONS) : NULL;
    g_ok = e && strstr(e, "GL_EXT_buffer_storage") && q_BufferStorageEXT ? 1 : 0;
    fprintf(stderr, "[glpersist] %s\n", g_ok ? "dynamic buffers get persistent mapped storage (GL_EXT_buffer_storage; UA_GL_PERSIST)"
                                            : "the driver has no GL_EXT_buffer_storage: off");
    g_last_log = now();
    return g_ok;
}

static void log_every(void)
{
    double t = now();
    if (t - g_last_log < 10) return;
    g_last_log = t;
    fprintf(stderr, "[glpersist] %ld buffers persistent (%.1f MB), %ld maps served, %ld redefined, %ld waits\n", g_made,
            g_bytes / 1048576.0, g_maps, g_respec, g_waits);
}

static void w_BufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    if ((target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER) && size > 0 &&
        (usage == GL_DYNAMIC_DRAW || usage == GL_STREAM_DRAW) && check_ext()) {
        GLuint name = bound(target);
        struct pbuf *p = name ? find(name) : NULL;
        if (p) {                                // storage that cannot be redefined: wait, copy in
            g_respec++;
            if (size <= p->size) {
                q_Finish(); g_waits++;
                if (data) memcpy(p->ptr, data, (size_t)size);
            } else if (g_respec <= 20) {
                fprintf(stderr, "[glpersist] buffer %u redefined larger (%ld -> %ld bytes): left at its size\n", name,
                        (long)p->size, (long)size);
            }
            return;
        }
        if (name) {
            q_BufferStorageEXT(target, size, data, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT |
                                                   GL_DYNAMIC_STORAGE_BIT_EXT);
            void *ptr = r_MapBufferRange(target, 0, size, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT);
            if (ptr && (p = add(name))) { p->ptr = ptr; p->size = size; g_made++; g_bytes += size; return; }
            // (not seen): the driver's storage as asked for, if the immutable storage failed too
            if (g_made < 5) fprintf(stderr, "[glpersist] buffer %u: persistent storage failed\n", name);
            if (ptr) r_UnmapBuffer(target);
            else r_BufferData(target, size, data, usage);
            return;
        }
    }
    r_BufferData(target, size, data, usage);
}

static void *w_MapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
    struct pbuf *p = g_ok > 0 ? find(bound(target)) : NULL;
    if (!p) return r_MapBufferRange(target, offset, length, access);
    if (offset < 0 || length < 0 || offset + length > p->size) return r_MapBufferRange(target, offset, length, access);
    if (!(access & GL_MAP_UNSYNCHRONIZED_BIT)) { q_Finish(); g_waits++; }   // the game asked the driver to wait
    g_maps++;
    log_every();
    return (char *)p->ptr + offset;
}

static void w_FlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length)
{
    if (g_ok > 0 && find(bound(target))) return;  // coherent
    r_FlushMappedBufferRange(target, offset, length);
}

static GLboolean w_UnmapBuffer(GLenum target)
{
    if (g_ok > 0 && find(bound(target))) return 1;
    return r_UnmapBuffer(target);
}

static void w_DeleteBuffers(GLsizei n, const GLuint *names)
{
    if (g_ok > 0)
        for (GLsizei i = 0; i < n; i++) {
            struct pbuf *p = find(names[i]);
            if (p) { g_bytes -= p->size; drop(names[i]); }   // (deleting a buffer unmaps it)
        }
    r_DeleteBuffers(n, names);
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

const so_shim *glpersist_table(void *(*next)(const char *name))
{
    memset(g_shims, 0, sizeof g_shims);
    const char *e = getenv("UA_GL_PERSIST");
    if (!e || strcmp(e, "1")) return g_shims;
    *(void **)&q_BufferStorageEXT = gl(NULL, "glBufferStorageEXT");
    *(void **)&q_GetIntegerv = gl(NULL, "glGetIntegerv");
    *(void **)&q_GetString = gl(NULL, "glGetString");
    *(void **)&q_Finish = gl(NULL, "glFinish");
    *(void **)&r_BufferData = gl(next, "glBufferData");
    *(void **)&r_MapBufferRange = gl(next, "glMapBufferRange");
    *(void **)&r_FlushMappedBufferRange = gl(next, "glFlushMappedBufferRange");
    *(void **)&r_UnmapBuffer = gl(next, "glUnmapBuffer");
    *(void **)&r_DeleteBuffers = gl(next, "glDeleteBuffers");
    if (!q_GetIntegerv || !q_GetString || !q_Finish || !r_BufferData || !r_MapBufferRange || !r_FlushMappedBufferRange ||
        !r_UnmapBuffer || !r_DeleteBuffers) {
        fprintf(stderr, "[glpersist] GL functions missing: off\n");
        return g_shims;
    }
    int k = 0;
    g_shims[k++] = (so_shim){ "glBufferData", (void *)w_BufferData };
    g_shims[k++] = (so_shim){ "glMapBufferRange", (void *)w_MapBufferRange };
    g_shims[k++] = (so_shim){ "glFlushMappedBufferRange", (void *)w_FlushMappedBufferRange };
    g_shims[k++] = (so_shim){ "glUnmapBuffer", (void *)w_UnmapBuffer };
    g_shims[k++] = (so_shim){ "glDeleteBuffers", (void *)w_DeleteBuffers };
    return g_shims;
}
