// glopt.c check against fake_opt_gl.c (as libGLESv2.so.2): a depth/stencil buffer that holds only its
// last whole clear's values is cleared to them again at the start of the next pass on its
// framebuffer (with the game's state as it was afterwards), and not when anything may have changed
// it. Run by pointer_test.sh: test_glopt <scenario>
//   restore       clear, draws that test but do not write, another framebuffer, back: one clear (1.0, 0)
//   depthwrite    a draw with depth writes on in between: no clear
//   stencilwrite  a draw with a stencil operation that writes: no clear
//   stencilkeep   stencil test on, operations GL_KEEP: the clear
//   partial       the first clear scissored to part of the buffer: no clear
//   unityclear    the game's own first command clears depth and stencil: only its clear
//   colourclear   the game's own first command clears colour only: the restoring clear too
//   invalidate    the game invalidates depth/stencil: no clear
//   delete        the texture deleted: no clear
//   clearbuffer   glClearBufferfi on it: no clear
//   blit          a blit of depth into it: no clear
//   renderbuffer  a depth-only renderbuffer (framebuffer 5): its clear restores depth only
//   nothread      before the first swap (glopt_frame): nothing followed
//   sameclear     the game clears depth and stencil again to the values they hold: left out, the
//                 next draw's restoring clear made
//   sameclear_off the same with UA_GL_SAMECLEAR=0: the game's clear made
//   off           UA_GL_DSCLEAR=0: nothing wrapped
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../glopt.h"

#define GL_FRAMEBUFFER 0x8D40
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_TEXTURE_2D 0x0DE1
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_SCISSOR_TEST 0x0C11
#define GL_KEEP 0x1E00
#define GL_REPLACE 0x1E01
#define GL_DEPTH_STENCIL 0x84F9
#define GL_DEPTH 0x1801
#define GL_RENDERBUFFER 0x8D41

typedef struct { int fb; unsigned mask; float depth; int stencil, scissor, depth_mask, stencil_mask, stencil_back_mask; } fake_clear_t;
extern fake_clear_t fake_clears[64];
extern int fake_nclears, fake_draws, fake_scissor, fake_depth_mask, fake_stencil_mask, fake_stencil_back_mask, fake_box[4];
extern float fake_clear_depth;
extern int fake_clear_stencil;
extern void glEnable(unsigned), glDisable(unsigned), glDepthMask(unsigned char), glStencilMask(unsigned), glStencilOp(unsigned, unsigned, unsigned);
extern void glScissor(int, int, int, int), glClearDepthf(float), glClearStencil(int);

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void *fn(const so_shim *t, const char *name)
{
    for (; t && t->name; t++) if (!strcmp(t->name, name)) return t->fn;
    return NULL;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "restore";
    if (!strcmp(mode, "off")) setenv("UA_GL_DSCLEAR", "0", 1);
    if (!strcmp(mode, "sameclear_off")) setenv("UA_GL_SAMECLEAR", "0", 1);
    const so_shim *t = glopt_table(NULL);
    void (*Bind)(unsigned, unsigned) = fn(t, "glBindFramebuffer");
    void (*Clear)(unsigned) = fn(t, "glClear");
    void (*Draw)(unsigned, int, int) = fn(t, "glDrawArrays");
    void (*Attach)(unsigned, unsigned, unsigned, unsigned, int) = fn(t, "glFramebufferTexture2D");
    void (*Invalidate)(unsigned, int, const unsigned *) = fn(t, "glInvalidateFramebuffer");
    void (*Delete)(int, const unsigned *) = fn(t, "glDeleteTextures");
    void (SO_ABI *ClearBufferfi)(unsigned, int, float, int) = fn(t, "glClearBufferfi");   // (softfp on armhf, as the player calls it)
    void (*Blit)(int, int, int, int, int, int, int, int, unsigned, unsigned) = fn(t, "glBlitFramebuffer");
    if (!strcmp(mode, "off")) {
        CHECK(!Bind && !Clear && !Draw);
        printf("test_glopt %s: %s\n", mode, failures ? "FAILED" : "ok");
        return failures != 0;
    }
    CHECK(Bind && Clear && Draw && Attach && Invalidate && Delete && ClearBufferfi && Blit);
    if (!(Bind && Clear && Draw && Attach && Invalidate && Delete && ClearBufferfi && Blit)) return 1;
    if (strcmp(mode, "nothread")) glopt_frame();     // the first swap: this thread renders

    int fb = !strcmp(mode, "renderbuffer") ? 5 : 3;
    Bind(GL_FRAMEBUFFER, (unsigned)fb);
    if (fb == 3) Attach(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 20, 0);
    // the camera: clears with the scissor on, over the whole 64x32 buffer (or part of it)
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, !strcmp(mode, "partial") ? 32 : 64, 32);
    glDepthMask(1); glStencilMask(0xff); glClearDepthf(1); glClearStencil(0);
    Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    // sprites: depth test without writes, stencil test that keeps
    glEnable(GL_DEPTH_TEST); glDepthMask(0);
    glEnable(GL_STENCIL_TEST); glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    Draw(4, 0, 6);
    if (!strcmp(mode, "depthwrite")) { glDepthMask(1); Draw(4, 0, 6); glDepthMask(0); }
    if (!strcmp(mode, "stencilwrite")) { glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE); Draw(4, 0, 6); glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP); }
    if (!strcmp(mode, "clearbuffer")) ClearBufferfi(GL_DEPTH_STENCIL, 0, 0.5f, 1);
    if (!strcmp(mode, "blit")) Blit(0, 0, 64, 32, 0, 0, 64, 32, GL_DEPTH_BUFFER_BIT, 0x2600);
    if (!strcmp(mode, "invalidate")) { static const unsigned a[1] = { GL_DEPTH_STENCIL_ATTACHMENT }; Invalidate(GL_FRAMEBUFFER, 1, a); }
    // a grab: another framebuffer, then back
    Bind(GL_FRAMEBUFFER, 4);
    Draw(4, 0, 3);
    if (!strcmp(mode, "delete")) { static const unsigned d[1] = { 20 }; Delete(1, d); }
    Bind(GL_FRAMEBUFFER, (unsigned)fb);
    // the game's state now: other clear values, a small scissor, masks off
    glScissor(1, 2, 3, 4); glClearDepthf(0.25f); glClearStencil(7); glStencilMask(0x0f);
    int before = fake_nclears;
    if (!strcmp(mode, "unityclear")) { glDepthMask(1); glStencilMask(0xff); Clear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT); glDepthMask(0); glStencilMask(0x0f); }
    else if (!strcmp(mode, "colourclear")) Clear(GL_COLOR_BUFFER_BIT);
    else if (!strncmp(mode, "sameclear", 9)) {
        // the game clears it again to the values it holds: left out (the pass that goes on restores it)
        glClearDepthf(1); glClearStencil(0); glDepthMask(1); glStencilMask(0xff);
        Clear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glDepthMask(0); glStencilMask(0x0f); glClearDepthf(0.25f); glClearStencil(7);
        Draw(4, 0, 6);
    }
    else Draw(4, 0, 6);
    int added = fake_nclears - before;

    int restores = !strcmp(mode, "restore") || !strcmp(mode, "stencilkeep") || !strcmp(mode, "renderbuffer") || !strcmp(mode, "sameclear") ? 1 :
                   !strcmp(mode, "colourclear") ? 1 : 0;
    int game_clears = !strcmp(mode, "unityclear") || !strcmp(mode, "colourclear") || !strcmp(mode, "sameclear_off");
    CHECK(added == restores + game_clears);
    if (restores) {
        fake_clear_t *c = &fake_clears[before];
        unsigned want = fb == 5 ? GL_DEPTH_BUFFER_BIT : GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
        CHECK(c->fb == fb && c->mask == want && c->depth == 1.0f && c->stencil == 0);
        CHECK(!c->scissor && c->depth_mask == 1 && (c->stencil_mask & 0xff) == 0xff && (c->stencil_back_mask & 0xff) == 0xff);
        if (game_clears) CHECK(fake_clears[before + 1].mask == GL_COLOR_BUFFER_BIT);
    }
    // the game's state as it was
    CHECK(fake_scissor == 1 && fake_box[0] == 1 && fake_box[3] == 4);
    CHECK(fake_depth_mask == 0 && fake_stencil_mask == 0x0f && fake_stencil_back_mask == 0x0f);
    CHECK(fake_clear_depth == 0.25f && fake_clear_stencil == 7);
    CHECK(fake_draws >= 2);
    printf("test_glopt %s: %s\n", mode, failures ? "FAILED" : "ok");
    return failures != 0;
}
