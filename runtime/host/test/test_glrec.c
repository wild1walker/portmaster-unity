// glrec.c and glrec_fg.c against fake_rec_gl.c (as libGLESv2.so.2): what reaches the driver, in
// which order, and when. Run by pointer_test.sh: test_glrec <scenario>
//   census      UA_GL_REC=census: every call reaches the driver at once
//   hold        UA_GL_REC=1: a frame's calls reach the driver at the swap, in order; a query (glGetError)
//               submits what is held first
//   copy        uniform data and buffer data are copied when the call is made (changed after: the copy)
//   clientidx   indices in client memory (no element buffer) are copied; with an element buffer, an offset
//   clientarray a draw from a client-side vertex array is submitted at once (with what was held before it)
//   map         a buffer map for writing (flush explicit) is held: the game writes host memory, the flushed
//               range reaches the real mapping at the swap; a map that reads is made at once
//   texsub      a texture upload from client memory is held with its pixels copied (row length, alignment)
//   dead        UA_GL_REC=2: a pass whose colour is cleared again before anything reads it is left out
//               (its draws and clears; its state calls stay)
//   live        the same with a pass in between that samples it: kept
//   cascade     a pass only a dead pass read dies too
//   dsend       a depth/stencil buffer cleared again by the next pass on its framebuffer: invalidated at
//               the end of the first (its colour, sampled in between, stays)
//   window      two passes on the window, the second loading its depth/stencil without testing it: both
//               invalidate depth/stencil at their end, the second at its start too (dead at the swap)
//   sink        a pass that only clears, then another framebuffer, then a draw on the first: the clear is
//               made just before that draw with its own clear colour, the colour put back afterwards
//   fetch       the driver lets shaders read depth/stencil (ARM_shader_framebuffer_fetch_depth_stencil):
//               the window scenario with a program not seen compiled from source keeps the second
//               pass's depth/stencil load; compiled from a source without gl_LastFragDepth, not
//   attach      an attachment changed by a held call is followed into later frames (texture 13 on
//               framebuffer 2: a pass writing it, sampled later, is kept; the driver is not asked again)
//   upload      a texture upload into a cleared target between its clear and its draws: the clear stays
//               before the upload
//   side        a shader storage buffer bound: no pass is left out
//   opt         UA_GL_REC_OPT=end: only invalidates at pass ends
//   file        UA_GL_REC_FILE: every call written after it ran (census), read back: a shader's source, a
//               uniform's values and the location it was asked for, a map's flushed bytes, a texture upload
//               as its size (row length, alignment), client-side indices, a frame per swap, the file closed
//               after UA_GL_REC_FILE_FRAMES; argv[2] keeps the file there for glreplay's test
#include <stdio.h>
#include <stdint.h>
#include <zlib.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <GLES3/gl32.h>
#include "../glrec.h"
#include "../glpresent.h"
#include "../../android/egl_glue.h"

// (glrec.c's UA_GL_REC=thread hands Unity's context over through these; not tested here)
void glopt_rethread(void) {}
void glspec_rethread(void) {}
void glpresent_tstate_take(glpresent_tstate *s) { (void)s; }
void glpresent_tstate_give(const glpresent_tstate *s) { (void)s; }
int glpresent_active(void) { return 0; }
void egl_glue_set_swap_offload(int (*offload)(egl_glue_swap_fn, void *, void *)) { (void)offload; }

extern char fake_log[16384];
static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n  log: %s\n", __FILE__, __LINE__, #c, fake_log); } } while (0)
#define HAS(s) (strstr(fake_log, s) != NULL)

static const so_shim *g_t;
static void *fn(const char *name)
{
    for (const so_shim *t = g_t; t && t->name; t++) if (!strcmp(t->name, name)) return t->fn;
    printf("FAIL: no wrapper for %s\n", name);
    exit(1);
}
#define CALL(f, T, ...) ((T)fn(#f))(__VA_ARGS__)
#define BIND(fb) CALL(glBindFramebuffer, void (*)(GLenum, GLuint), GL_FRAMEBUFFER, fb)
#define CLEAR(m) CALL(glClear, void (*)(GLbitfield), m)
#define DRAW(n) CALL(glDrawArrays, void (*)(GLenum, GLint, GLsizei), GL_TRIANGLES, 0, n)
#define USE(p) CALL(glUseProgram, void (*)(GLuint), p)
#define TEX(t) CALL(glBindTexture, void (*)(GLenum, GLuint), GL_TEXTURE_2D, t)
#define SWAP() glrec_swap()
#define RESET() (fake_log[0] = 0)
// glClearColor's wrapper takes the player's convention: on armhf softfp, the floats in core registers
#if defined(__arm__)
static unsigned fbits(float f) { unsigned u; memcpy(&u, &f, 4); return u; }
#define CLEARCOLOR(r) ((void (*)(unsigned, unsigned, unsigned, unsigned))fn("glClearColor"))(fbits(r), 0, 0, 0)
#else
#define CLEARCOLOR(r) ((void (*)(GLfloat, GLfloat, GLfloat, GLfloat))fn("glClearColor"))(r, 0, 0, 0)
#endif

// the position of a token in the log (-1: not there)
static int at(const char *s) { const char *p = strstr(fake_log, s); return p ? (int)(p - fake_log) : -1; }

int main(int argc, char **argv)
{
    const char *m = argc > 1 ? argv[1] : "hold";
    const char *mode = !strcmp(m, "census") || !strcmp(m, "file") ? "census" : (!strcmp(m, "hold") || !strcmp(m, "copy") || !strcmp(m, "clientidx") ||
                                                          !strcmp(m, "clientarray") || !strcmp(m, "map") || !strcmp(m, "texsub") ||
                                                          !strcmp(m, "blackbox")) ? "1" : "2";
    setenv("UA_GL_REC", mode, 1);
    setenv("UA_GL_REC_LOG", "0", 1);
    if (!strcmp(m, "opt")) setenv("UA_GL_REC_OPT", "end", 1);
    if (!strcmp(m, "fetch")) setenv("FAKE_DS_FETCH", "1", 1);
    char bbf[] = "/tmp/test_glrec_bbXXXXXX";
    if (!strcmp(m, "blackbox")) {
        int fd = mkstemp(bbf);
        CHECK(fd >= 0);
        close(fd);
        setenv("UA_GL_REC_BLACKBOX", bbf, 1);
        setenv("UA_GL_REC_BLACKBOX_LEVEL", "2", 1);
    }
    char trf[512] = "/tmp/test_glrec_fileXXXXXX";
    if (!strcmp(m, "file")) {
        if (argc > 2) snprintf(trf, sizeof trf, "%s", argv[2]);
        else { int fd = mkstemp(trf); CHECK(fd >= 0); close(fd); }
        setenv("UA_GL_REC_FILE", trf, 1);
        setenv("UA_GL_REC_FILE_FRAMES", "2", 1);
    }
    g_t = glrec_table(NULL);
    CHECK(g_t && g_t[0].name);
    SWAP();                                     // (the render thread: this one; recording from now on)
    RESET();
    if (!strcmp(m, "census")) {
        BIND(1); CLEAR(GL_COLOR_BUFFER_BIT); DRAW(3);
        CHECK(HAS("bind(1) clear(C fb1") && HAS("draw(fb1 3)"));
        SWAP();
    } else if (!strcmp(m, "hold")) {
        BIND(1); CLEAR(GL_COLOR_BUFFER_BIT); DRAW(3);
        CHECK(fake_log[0] == 0);
        CALL(glGetError, GLenum (*)(void));
        CHECK(at("bind(1)") >= 0 && at("bind(1)") < at("clear(C fb1") && at("clear(C fb1") < at("draw(fb1 3)") && at("draw(fb1 3)") < at("geterror"));
        RESET();
        DRAW(6);
        CHECK(fake_log[0] == 0);
        SWAP();
        CHECK(HAS("draw(fb1 6)"));
    } else if (!strcmp(m, "copy")) {
        GLfloat v[4] = { 0.5f, 0, 0, 0 };
        unsigned char d[4] = { 7, 0, 0, 0 };
        CALL(glUniform4fv, void (*)(GLint, GLsizei, const GLfloat *), 2, 1, v);
        CALL(glBufferSubData, void (*)(GLenum, GLintptr, GLsizeiptr, const void *), GL_ARRAY_BUFFER, 0, 4, d);
        v[0] = 0.25f; d[0] = 9;
        SWAP();
        CHECK(HAS("u4fv(2 0.5)") && HAS("bufsub(4 7)"));
    } else if (!strcmp(m, "clientidx")) {
        unsigned short idx[3] = { 42, 1, 2 };
        CALL(glBindVertexArray, void (*)(GLuint), 1);
        CALL(glDrawElements, void (*)(GLenum, GLsizei, GLenum, const void *), GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, idx);
        idx[0] = 99;
        CALL(glBindBuffer, void (*)(GLenum, GLuint), GL_ELEMENT_ARRAY_BUFFER, 4);
        CALL(glDrawElements, void (*)(GLenum, GLsizei, GLenum, const void *), GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, (const void *)64);
        SWAP();
        CHECK(HAS("drawel(fb0 3 idx 42)") && HAS("drawel(fb0 3 @64)"));
    } else if (!strcmp(m, "map")) {
        extern unsigned char fake_store[256];
        CALL(glBindBuffer, void (*)(GLenum, GLuint), GL_ARRAY_BUFFER, 4);
        unsigned char *p = CALL(glMapBufferRange, void *(*)(GLenum, GLintptr, GLsizeiptr, GLbitfield), GL_ARRAY_BUFFER, 16, 32,
                                GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        CHECK(p && p != fake_store + 16 && fake_log[0] == 0);
        p[0] = 7; p[4] = 9; p[20] = 5;
        CALL(glFlushMappedBufferRange, void (*)(GLenum, GLintptr, GLsizeiptr), GL_ARRAY_BUFFER, 0, 8);
        CHECK(CALL(glUnmapBuffer, GLboolean (*)(GLenum), GL_ARRAY_BUFFER) == GL_TRUE);
        p = NULL;
        DRAW(3);
        CHECK(fake_log[0] == 0 && fake_store[16] == 0);
        SWAP();
        CHECK(HAS("map(16 32) flushmap(0 8) unmap draw(fb0 3)") && fake_store[16] == 7 && fake_store[20] == 9 && fake_store[36] == 0);
        RESET();
        unsigned char *q = CALL(glMapBufferRange, void *(*)(GLenum, GLintptr, GLsizeiptr, GLbitfield), GL_ARRAY_BUFFER, 0, 8, GL_MAP_READ_BIT);
        CHECK(q == fake_store && HAS("map(0 8 read)"));
        CALL(glUnmapBuffer, GLboolean (*)(GLenum), GL_ARRAY_BUFFER);
        CHECK(HAS("unmap"));
        SWAP();
    } else if (!strcmp(m, "texsub")) {
        unsigned char px[3 * 8 * 4];
        for (size_t i = 0; i < sizeof px; i++) px[i] = (unsigned char)i;
        CALL(glPixelStorei, void (*)(GLenum, GLint), GL_UNPACK_ROW_LENGTH, 8);
        CALL(glTexSubImage2D, void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *),
             GL_TEXTURE_2D, 0, 0, 0, 5, 3, GL_RGBA, GL_UNSIGNED_BYTE, px);
        memset(px, 0xee, sizeof px);
        CHECK(fake_log[0] == 0);
        SWAP();
        CHECK(HAS("texsub(5x3 0..83)"));
    } else if (!strcmp(m, "clientarray")) {
        CALL(glBindVertexArray, void (*)(GLuint), 2);
        CALL(glBindBuffer, void (*)(GLenum, GLuint), GL_ARRAY_BUFFER, 0);
        static float verts[6];
        CALL(glVertexAttribPointer, void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *), 0, 2, GL_FLOAT, 0, 0, verts);
        CALL(glEnableVertexAttribArray, void (*)(GLuint), 0);
        BIND(2);
        DRAW(3);
        CHECK(HAS("bind(2) draw(fb2 3)"));
        RESET();
        SWAP();
    } else if (!strcmp(m, "dead") || !strcmp(m, "live") || !strcmp(m, "side") || !strcmp(m, "opt")) {
        if (!strcmp(m, "side")) CALL(glBindBufferBase, void (*)(GLenum, GLuint, GLuint), GL_SHADER_STORAGE_BUFFER, 0, 1);
        USE(6);
        BIND(2); CLEAR(GL_COLOR_BUFFER_BIT); DRAW(1);            // tex 11, overwritten below
        if (!strcmp(m, "live")) { BIND(3); USE(5); TEX(11); DRAW(2); USE(6); }
        BIND(1); CLEAR(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT); DRAW(4);
        BIND(2); CLEAR(GL_COLOR_BUFFER_BIT); DRAW(5);
        BIND(0); USE(5); TEX(11); DRAW(6);
        SWAP();
        if (!strcmp(m, "dead")) CHECK(!HAS("draw(fb2 1)") && at("clear(C fb2") > at("clear(CDS fb1") && HAS("draw(fb2 5)") && HAS("draw(fb0 6)"));
        else CHECK(HAS("draw(fb2 1)") && HAS("draw(fb2 5)"));
    } else if (!strcmp(m, "cascade")) {
        USE(6);
        BIND(2); CLEAR(GL_COLOR_BUFFER_BIT); DRAW(1);            // tex 11: read only by the dead pass below
        BIND(3); USE(5); TEX(11); CLEAR(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT); DRAW(2);   // tex 12, 21
        BIND(1); USE(6); CLEAR(GL_COLOR_BUFFER_BIT); DRAW(7);
        BIND(3); CLEAR(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT); DRAW(3);
        BIND(2); CLEAR(GL_COLOR_BUFFER_BIT); DRAW(4);
        BIND(0); USE(5); TEX(12); DRAW(5);
        SWAP();
        CHECK(!HAS("draw(fb3 2)") && !HAS("draw(fb2 1)") && HAS("draw(fb3 3)") && HAS("draw(fb2 4)"));
    } else if (!strcmp(m, "dsend")) {
        USE(6);
        BIND(1); CLEAR(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT); DRAW(1);
        BIND(2); USE(5); TEX(10); DRAW(2); USE(6);               // samples fb1's colour
        BIND(1); CLEAR(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT); DRAW(3);
        BIND(0); TEX(10); DRAW(4);
        SWAP();
        CHECK(HAS("draw(fb1 1) inval(fb1 DS) bind(2)") && HAS("draw(fb1 3)"));
    } else if (!strcmp(m, "fetch")) {
        for (int round = 0; round < 2; round++) {
            if (round == 1) {
                GLuint sh = CALL(glCreateShader, GLuint (*)(GLenum), GL_FRAGMENT_SHADER);
                const char *src = "void main() { gl_FragColor = vec4(1.0); }";
                CALL(glShaderSource, void (*)(GLuint, GLsizei, const GLchar *const *, const GLint *), sh, 1, &src, NULL);
                CALL(glAttachShader, void (*)(GLuint, GLuint), 6, sh);
                CALL(glLinkProgram, void (*)(GLuint), 6);
                RESET();
            }
            USE(6);
            BIND(0); DRAW(1);
            BIND(2); CLEAR(GL_COLOR_BUFFER_BIT); DRAW(2);
            BIND(0); DRAW(3);
            SWAP();
            if (round == 0) CHECK(HAS("draw(fb0 1) bind(2)") && !HAS("inval(fb0 DS) draw(fb0 3)") && HAS("draw(fb0 3) inval(fb0 DS)"));
            else CHECK(HAS("draw(fb0 1) inval(fb0 DS) bind(2)") && HAS("inval(fb0 DS) draw(fb0 3) inval(fb0 DS)"));
            RESET();
        }
    } else if (!strcmp(m, "upload")) {
        static unsigned char px[2 * 2 * 4] = { 77 };
        USE(6);
        BIND(2); CLEAR(GL_COLOR_BUFFER_BIT);
        BIND(1); CLEAR(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT); DRAW(1);
        TEX(11);
        CALL(glTexSubImage2D, void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *),
             GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, px);
        BIND(2); DRAW(2);
        BIND(0); USE(5); TEX(11); DRAW(3);
        SWAP();
        CHECK(at("clear(C fb2") >= 0 && at("clear(C fb2") < at("texsub(") && at("texsub(") < at("draw(fb2 2)"));
    } else if (!strcmp(m, "attach")) {
        USE(6);
        BIND(2); CLEAR(GL_COLOR_BUFFER_BIT); DRAW(1);            // (framebuffer 2 asked about: texture 11)
        SWAP();
        BIND(2); CALL(glFramebufferTexture2D, void (*)(GLenum, GLenum, GLenum, GLuint, GLint), GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 13, 0);
        SWAP();
        RESET();
        BIND(2); CLEAR(GL_COLOR_BUFFER_BIT); DRAW(2);            // texture 13
        BIND(0); USE(5); TEX(13); DRAW(3); USE(6);               // read
        BIND(2); CLEAR(GL_COLOR_BUFFER_BIT); DRAW(4);
        BIND(0); USE(5); TEX(13); DRAW(5);
        SWAP();
        CHECK(HAS("draw(fb2 2)") && HAS("draw(fb2 4)"));
    } else if (!strcmp(m, "window")) {
        USE(6);
        BIND(0); DRAW(1);
        BIND(2); CLEAR(GL_COLOR_BUFFER_BIT); DRAW(2);
        BIND(0); USE(5); TEX(11); DRAW(3);
        SWAP();
        CHECK(HAS("draw(fb0 1) inval(fb0 DS) bind(2)"));
        CHECK(at("inval(fb0 DS) draw(fb0 3) inval(fb0 DS)") >= 0);
    } else if (!strcmp(m, "sink")) {
        USE(6);
        BIND(2);
        CALL(glInvalidateFramebuffer, void (*)(GLenum, GLsizei, const GLenum *), GL_FRAMEBUFFER, 1, (const GLenum[]){ GL_COLOR_ATTACHMENT0 });
        CLEARCOLOR(0.5f);                                       // (as URP: invalidate, then set the clear colour, then clear)
        CLEAR(GL_COLOR_BUFFER_BIT);
        CLEARCOLOR(0.25f);
        BIND(1); CLEAR(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT); DRAW(1);
        BIND(2); DRAW(2);
        BIND(3); CLEAR(GL_COLOR_BUFFER_BIT); DRAW(3);
        BIND(0); USE(5); TEX(11); DRAW(4);
        SWAP();
        CHECK(at("clear(CDS fb1 0.25)") >= 0 && at("clear(CDS fb1 0.25)") < at("clear(C fb2 0.5) draw(fb2 2)") && HAS("clear(C fb3 0.25)"));
    } else if (!strcmp(m, "blackbox")) {
        // (UA_GL_REC_BLACKBOX: the file holds the last stage reached, rewritten in place, 4 KB)
        BIND(1); CLEAR(GL_COLOR_BUFFER_BIT); DRAW(3);
        SWAP();
        BIND(2); DRAW(4);
        SWAP();
        char buf[8192] = { 0 };
        FILE *f = fopen(bbf, "r");
        size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        if (f) fclose(f);
        unlink(bbf);
        CHECK(n == 4096);
        CHECK(strstr(buf, "frame 1, stage: render thread: frame submitted, swapping") != NULL);
        CHECK(strstr(buf, "glBindFramebuffer glDrawArrays") != NULL || strstr(buf, "glBindFramebuffer glDrawElements") != NULL);
        if (failures) printf("%s\n", buf);
    } else if (!strcmp(m, "file")) {
        GLuint sh = CALL(glCreateShader, GLuint (*)(GLenum), GL_VERTEX_SHADER);
        const GLchar *src[2] = { "void main() {", " }" };
        CALL(glShaderSource, void (*)(GLuint, GLsizei, const GLchar *const *, const GLint *), sh, 2, src, NULL);
        GLuint pr = CALL(glCreateProgram, GLuint (*)(void));
        CALL(glAttachShader, void (*)(GLuint, GLuint), pr, sh);
        CALL(glLinkProgram, void (*)(GLuint), pr);
        USE(pr);
        GLint loc = CALL(glGetUniformLocation, GLint (*)(GLuint, const GLchar *), pr, "_Color");
        GLfloat v[4] = { 0.5f, 0.25f, 0, 1 };
        CALL(glUniform4fv, void (*)(GLint, GLsizei, const GLfloat *), loc, 1, v);
        CALL(glBindBuffer, void (*)(GLenum, GLuint), GL_ARRAY_BUFFER, 4);
        unsigned char *p = CALL(glMapBufferRange, void *(*)(GLenum, GLintptr, GLsizeiptr, GLbitfield), GL_ARRAY_BUFFER, 16, 32,
                                GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
        p[0] = 7; p[4] = 9;
        CALL(glFlushMappedBufferRange, void (*)(GLenum, GLintptr, GLsizeiptr), GL_ARRAY_BUFFER, 0, 8);
        CALL(glUnmapBuffer, GLboolean (*)(GLenum), GL_ARRAY_BUFFER);
        // a buffer kept mapped while another is mapped, written and unmapped on the same target, then bound
        // again and flushed (Unity's particle buffers): its bytes are in the trace too
        CALL(glBindBuffer, void (*)(GLenum, GLuint), GL_ARRAY_BUFFER, 5);
        unsigned char *q = CALL(glMapBufferRange, void *(*)(GLenum, GLintptr, GLsizeiptr, GLbitfield), GL_ARRAY_BUFFER, 48, 8,
                                GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
        CALL(glBindBuffer, void (*)(GLenum, GLuint), GL_ARRAY_BUFFER, 6);
        unsigned char *r2 = CALL(glMapBufferRange, void *(*)(GLenum, GLintptr, GLsizeiptr, GLbitfield), GL_ARRAY_BUFFER, 64, 8,
                                 GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
        r2[0] = 3; r2[4] = 4;
        CALL(glFlushMappedBufferRange, void (*)(GLenum, GLintptr, GLsizeiptr), GL_ARRAY_BUFFER, 0, 8);
        CALL(glUnmapBuffer, GLboolean (*)(GLenum), GL_ARRAY_BUFFER);
        CALL(glBindBuffer, void (*)(GLenum, GLuint), GL_ARRAY_BUFFER, 5);
        q[0] = 21; q[4] = 22;
        CALL(glFlushMappedBufferRange, void (*)(GLenum, GLintptr, GLsizeiptr), GL_ARRAY_BUFFER, 0, 8);
        CALL(glUnmapBuffer, GLboolean (*)(GLenum), GL_ARRAY_BUFFER);
        unsigned char px[3 * 8 * 4] = { 1 };
        CALL(glPixelStorei, void (*)(GLenum, GLint), GL_UNPACK_ROW_LENGTH, 8);
        CALL(glTexSubImage2D, void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *),
             GL_TEXTURE_2D, 0, 0, 0, 5, 3, GL_RGBA, GL_UNSIGNED_BYTE, px);
        CALL(glPixelStorei, void (*)(GLenum, GLint), GL_UNPACK_ROW_LENGTH, 0);
        TEX(12);
        unsigned short idx[3] = { 42, 1, 2 };
        CALL(glBindVertexArray, void (*)(GLuint), 1);
        CALL(glDrawElements, void (*)(GLenum, GLsizei, GLenum, const void *), GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, idx);
        BIND(1); CLEAR(GL_COLOR_BUFFER_BIT); DRAW(3);
        CHECK(HAS("bind(1) clear(C fb1") && HAS("draw(fb1 3)") && HAS("u4fv(7 0.5)"));       // (census: reached the driver)
        SWAP();
        DRAW(5);                                // (after UA_GL_REC_FILE_FRAMES: not written)
        SWAP();
        // read it back
        gzFile f = gzopen(trf, "rb");
        char magic[8] = { 0 }; uint32_t h[3] = { 0 };
        CHECK(f && gzread(f, magic, 8) == 8 && !memcmp(magic, "UAGLTRC1", 8) && gzread(f, h, 12) == 12 && h[0] == 1);
        char names[1024][64];
        for (uint32_t i = 0; f && i < h[1] && i < 1024; i++) {
            uint16_t l = 0; names[i][0] = 0;
            if (gzread(f, &l, 2) != 2 || l >= 64 || gzread(f, names[i], l) != (int)l) { CHECK(0); break; }
            names[i][l] = 0;
        }
        int frames = 0, got_src = 0, got_u = 0, got_loc = 0, got_flush = 0, got_tex = 0, got_idx = 0, got_draw5 = 0, got_gen = 0;
        uint64_t w;
        while (f && gzread(f, &w, 8) == 8) {
            unsigned op = (unsigned)(w & 0xffffff), na = (unsigned)(w >> 24 & 0xff), nb = (unsigned)(w >> 32 & 0xffff), hr = (unsigned)(w >> 48 & 1);
            uint64_t a[32] = { 0 };
            if (na + hr > 32 || gzread(f, a, (na + hr) * 8) != (int)((na + hr) * 8)) { CHECK(0); break; }
            unsigned char *bl[8] = { 0 }; uint64_t bn[8] = { 0 };
            for (unsigned i = 0; i < nb; i++) {
                uint64_t len = 0;
                gzread(f, &len, 8);
                size_t n = (size_t)(len & ~(1ull << 63)), pad = len >> 63 ? 0 : (n + 7) / 8 * 8;
                unsigned char *b = calloc(1, pad + 1);
                if (pad) gzread(f, b, (unsigned)pad);
                if (i < 8) { bl[i] = b; bn[i] = len; } else free(b);
            }
            const char *nm = op == 0xFFFFF0u ? "frame" : op < h[1] ? names[op] : "?";
            if (!strcmp(nm, "frame")) frames++;
            if (!strcmp(nm, "glCreateShader") && hr && a[1] == 40) got_gen = 1;
            if (!strcmp(nm, "glShaderSource") && nb == 1 && !strcmp((char *)bl[0], "void main() { }")) got_src = 1;
            if (!strcmp(nm, "glGetUniformLocation") && hr && a[2] == 7 && nb == 1 && !strcmp((char *)bl[0], "_Color")) got_loc = 1;
            if (!strcmp(nm, "glUniform4fv") && a[0] == 7 && nb == 1) { float x; memcpy(&x, bl[0], 4); got_u = x == 0.5f; }
            if (!strcmp(nm, "glFlushMappedBufferRange") && nb == 1 && bn[0] == 8 && bl[0][0] == 7 && bl[0][4] == 9) got_flush = 1;
            if (!strcmp(nm, "glTexSubImage2D") && nb == 1 && bn[0] == ((1ull << 63) | 84)) got_tex = 1;
            if (!strcmp(nm, "glDrawElements") && nb == 1 && bn[0] == 6 && ((unsigned short *)bl[0])[0] == 42) got_idx = 1;
            if (!strcmp(nm, "glDrawArrays") && a[2] == 5) got_draw5 = 1;
            for (unsigned i = 0; i < nb && i < 8; i++) free(bl[i]);
        }
        if (f) gzclose(f);
        if (argc <= 2) unlink(trf);
        CHECK(frames == 2 && got_gen && got_src && got_loc && got_u && got_flush && got_tex && got_idx && !got_draw5);
        if (failures) printf("frames %d gen %d src %d loc %d u %d flush %d tex %d idx %d draw5 %d\n", frames, got_gen, got_src, got_loc, got_u, got_flush, got_tex, got_idx, got_draw5);
    } else { printf("unknown scenario %s\n", m); return 2; }
    printf("%s %s\n", failures ? "FAIL" : "ok", m);
    return failures != 0;
}
