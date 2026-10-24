// glpass.c check against fake_pass_gl.c (as libGLESv2.so.2): a whole colour clear of an image that
// still holds that colour from its last whole clear is held back, and left out when the pass ends
// without a draw, made right before the pass's next draw otherwise (with the game's state as it was
// afterwards); and it is made whenever anything may have changed the image. Run by
// pointer_test.sh: test_glpass <scenario>
//   same          clear to (0,0,0,0), another framebuffer; again next frame: the second clear left out
//   late          the second time a draw follows: the clear is made right before it, state put back
//   drawn         a draw after the first clear: the second is made
//   colour        the second clear to another colour: made
//   partial       the first clear scissored to part of the image: not followed, the second made
//   mask          the first clear with a channel masked: the second made
//   dither        dithering on for the first clear, off for the second: made
//   upload        glTexSubImage2D into the texture in between: made
//   delete        the texture deleted (and its name back) in between: made
//   invalidate    the game invalidates the colour buffer in between: made
//   blit          a blit into it in between: made
//   otherfb       a draw into another framebuffer the texture is attached to: made
//   depth         the second clear also clears depth: only its depth part made
//   inval_late    the second clear held back, then the game invalidates: the clear is made first
//   renderbuffer  a colour renderbuffer (framebuffer 5): left out
//   foreign       a draw on another thread in between: made
//   nothread      before the first swap (glpass_frame): nothing followed
//   off           UA_GL_SAMECLEAR=0: nothing wrapped
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../glpass.h"

#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_TEXTURE_2D 0x0DE1
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_SCISSOR_TEST 0x0C11
#define GL_DITHER 0x0BD0
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401

typedef struct { int fb; unsigned mask; float c[4]; int scissor, discard, dither, cmask[4]; } fake_clear_t;
extern fake_clear_t fake_clears[64];
extern int fake_nclears, fake_draws, fake_scissor, fake_dither, fake_mask[4], fake_box[4], fake_order[128], fake_norder, fake_tex2d;
extern float fake_cc[4];
extern void glEnable(unsigned), glDisable(unsigned), glScissor(int, int, int, int), glClearColor(float, float, float, float);
extern void glColorMask(unsigned char, unsigned char, unsigned char, unsigned char), glBindTexture(unsigned, unsigned);

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void *fn(const so_shim *t, const char *name)
{
    for (; t && t->name; t++) if (!strcmp(t->name, name)) return t->fn;
    return NULL;
}
static void (*Bind)(unsigned, unsigned);
static void (*Clear)(unsigned);
static void (*Draw)(unsigned, int, int);
static void *other_thread(void *a) { (void)a; Draw(4, 0, 3); return NULL; }

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "same";
    if (!strcmp(mode, "off")) setenv("UA_GL_SAMECLEAR", "0", 1);
    const so_shim *t = glpass_table(NULL);
    Bind = fn(t, "glBindFramebuffer"); Clear = fn(t, "glClear"); Draw = fn(t, "glDrawArrays");
    void (*Attach)(unsigned, unsigned, unsigned, unsigned, int) = fn(t, "glFramebufferTexture2D");
    void (*Invalidate)(unsigned, int, const unsigned *) = fn(t, "glInvalidateFramebuffer");
    void (*Delete)(int, const unsigned *) = fn(t, "glDeleteTextures");
    void (*SubImage)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *) = fn(t, "glTexSubImage2D");
    void (*Blit)(int, int, int, int, int, int, int, int, unsigned, unsigned) = fn(t, "glBlitFramebuffer");
    if (!strcmp(mode, "off")) {
        CHECK(!Bind && !Clear && !Draw);
        printf("test_glpass %s: %s\n", mode, failures ? "FAILED" : "ok");
        return failures != 0;
    }
    CHECK(Bind && Clear && Draw && Attach && Invalidate && Delete && SubImage && Blit);
    if (!(Bind && Clear && Draw && Attach && Invalidate && Delete && SubImage && Blit)) return 1;
    if (strcmp(mode, "nothread")) glpass_frame();     // the first swap: this thread renders

    int fb = !strcmp(mode, "renderbuffer") ? 5 : 3;
    // frame 1: the texture attached, a whole clear (scissor on, covering the 64x32 image)
    Bind(GL_FRAMEBUFFER, (unsigned)fb);
    if (fb == 3) Attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 9, 0);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, !strcmp(mode, "partial") ? 32 : 64, 32);
    glColorMask(1, 1, !strcmp(mode, "mask") ? 0 : 1, 1);
    glClearColor(0, 0, 0, 0);
    Clear(GL_COLOR_BUFFER_BIT);
    glColorMask(1, 1, 1, 1);
    if (!strcmp(mode, "drawn")) Draw(4, 0, 6);
    if (!strcmp(mode, "blit")) Blit(0, 0, 64, 32, 0, 0, 64, 32, GL_COLOR_BUFFER_BIT, 0x2600);
    Bind(GL_FRAMEBUFFER, 0);
    Draw(4, 0, 6);
    if (!strcmp(mode, "otherfb")) { Bind(GL_FRAMEBUFFER, 4); Draw(4, 0, 3); Bind(GL_FRAMEBUFFER, 0); }
    if (!strcmp(mode, "upload")) { static unsigned char px[4]; glBindTexture(GL_TEXTURE_2D, 9); SubImage(GL_TEXTURE_2D, 0, 1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); }
    if (!strcmp(mode, "delete")) { static const unsigned d[1] = { 9 }; Delete(1, d); }
    if (!strcmp(mode, "invalidate")) {
        static const unsigned a[1] = { GL_COLOR_ATTACHMENT0 };
        Bind(GL_FRAMEBUFFER, 3); Invalidate(GL_FRAMEBUFFER, 1, a); Bind(GL_FRAMEBUFFER, 0);
    }
    if (!strcmp(mode, "foreign")) { pthread_t th; pthread_create(&th, NULL, other_thread, NULL); pthread_join(th, NULL); }
    glpass_frame();
    // frame 2: the same clear (scissor off this time); then the game's state changes (scissor, colour
    // mask, clear colour) before the pass goes on
    if (!strcmp(mode, "dither")) glDisable(GL_DITHER);
    Bind(GL_FRAMEBUFFER, (unsigned)fb);
    if (!strcmp(mode, "delete") && fb == 3) Attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 9, 0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, !strcmp(mode, "colour") ? 1.0f : 0.0f);
    int before = fake_nclears, order0 = fake_norder;
    Clear(!strcmp(mode, "depth") ? GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(1, 2, 3, 4);
    glClearColor(0.5f, 0.5f, 0.5f, 0.5f);
    glColorMask(1, 0, 1, 0);
    if (!strcmp(mode, "late")) Draw(4, 0, 6);
    if (!strcmp(mode, "inval_late")) { static const unsigned a[1] = { GL_COLOR_ATTACHMENT0 }; Invalidate(GL_FRAMEBUFFER, 1, a); }
    int made = fake_nclears - before;
    Bind(GL_FRAMEBUFFER, 0);

    int left_out = !strcmp(mode, "same") || !strcmp(mode, "renderbuffer");
    int later = !strcmp(mode, "late") || !strcmp(mode, "inval_late");
    CHECK(made == (left_out ? 0 : 1));
    if (made == 1) {
        fake_clear_t *c = &fake_clears[before];
        // (depth: the colour part held back, the depth part passed on)
        CHECK(c->fb == fb && (!strcmp(mode, "depth") ? c->mask == GL_DEPTH_BUFFER_BIT : (c->mask & GL_COLOR_BUFFER_BIT) != 0));
        if (later) {
            // made with the first clear's colour and a whole, unmasked clear; then before the draw
            CHECK(c->c[0] == 0 && c->c[3] == 0 && !c->scissor && c->cmask[0] && c->cmask[1] && c->cmask[2] && c->cmask[3]);
            CHECK(fake_order[order0] == 'c' && fake_norder > order0 + 1 && fake_order[order0 + 1] == (!strcmp(mode, "late") ? 'd' : 'i'));
        }
    }
    // the game's state as it was
    CHECK(fake_scissor == 1 && fake_box[0] == 1 && fake_box[3] == 4);
    CHECK(fake_mask[0] == 1 && fake_mask[1] == 0 && fake_mask[2] == 1 && fake_mask[3] == 0);
    CHECK(fake_cc[0] == 0.5f && fake_cc[3] == 0.5f);
    CHECK(fake_dither == (!strcmp(mode, "dither") ? 0 : 1));
    printf("test_glpass %s: %s\n", mode, failures ? "FAILED" : "ok");
    return failures != 0;
}
