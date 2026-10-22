// msaafix.c check against fake_msaa_gl.c (as libGLESv2.so.2): the present resolve of an undrawn
// multisampled framebuffer reads the window instead, every frame; blits of drawn framebuffers, of
// single-sampled ones, of part of the window, and everything with the fix off are left alone.
// Run by pointer_test.sh.
#include <stdio.h>
#include <string.h>
#include "../msaafix.h"

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_BUFFER_BIT 0x4000

extern int fake_read_fb, fake_draw_fb, fake_rb, fake_blits, fake_blit_read, fake_blit_draw, fake_draws, fake_clears;

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void *fn(const so_shim *t, const char *name)
{
    for (; t && t->name; t++) if (!strcmp(t->name, name)) return t->fn;
    return NULL;
}

// what the tracer would be: one more wrapper of glBindFramebuffer the fix must call through
static int traced_binds;
extern void glBindFramebuffer(unsigned, unsigned);
static void traced_bind(unsigned t, unsigned fb) { traced_binds++; glBindFramebuffer(t, fb); }
static void *tracer(const char *name) { return !strcmp(name, "glBindFramebuffer") ? (void *)traced_bind : NULL; }

static void (*Bind)(unsigned, unsigned);
static void (*Blit)(int, int, int, int, int, int, int, int, unsigned, unsigned);
static void (*Clear)(unsigned);
static void (*Draw)(unsigned, int, int);

// one frame of Unity 2017.2's broken present: the scene into the window, then the resolve of its
// own framebuffer 1 into 2, then the copy drawn into the window
static void broken_frame(void)
{
    Bind(GL_FRAMEBUFFER, 0); Clear(GL_COLOR_BUFFER_BIT); Draw(4, 0, 300);
    Bind(GL_FRAMEBUFFER, 1); Bind(GL_DRAW_FRAMEBUFFER, 2);          // as Unity binds the resolve
    Blit(0, 0, 640, 480, 0, 0, 640, 480, GL_COLOR_BUFFER_BIT, 0x2600);
    CHECK(fake_read_fb == 1 && fake_draw_fb == 2);                   // Unity's bindings as it left them
    Bind(GL_FRAMEBUFFER, 0); Draw(4, 0, 3);
    msaafix_frame();                                                  // the swap
}

// the working path: the scene into framebuffer 1, its resolve, the copy into the window
static void good_frame(void)
{
    Bind(GL_FRAMEBUFFER, 1); Clear(GL_COLOR_BUFFER_BIT); Draw(4, 0, 300);
    Bind(GL_READ_FRAMEBUFFER, 1); Bind(GL_DRAW_FRAMEBUFFER, 2);
    Blit(0, 0, 640, 480, 0, 0, 640, 480, GL_COLOR_BUFFER_BIT, 0x2600);
    Bind(GL_FRAMEBUFFER, 0); Draw(4, 0, 3);
    msaafix_frame();
}

int main(int argc, char **argv)
{
    const char *s = argc > 1 ? argv[1] : "";
    if (!strcmp(s, "off")) {
        const so_shim *t = msaafix_table(0, NULL);
        CHECK(t && !t->name);                                         // nothing wrapped
        msaafix_frame();
    } else if (!strcmp(s, "on")) {
        const so_shim *t = msaafix_table(1, tracer);
        msaafix_set_size(640, 480);
        Bind = fn(t, "glBindFramebuffer"); Blit = fn(t, "glBlitFramebuffer");
        Clear = fn(t, "glClear"); Draw = fn(t, "glDrawArrays");
        CHECK(Bind && Blit && Clear && Draw && fn(t, "glDrawElements"));
        msaafix_frame();
        for (int i = 0; i < 3; i++) {
            int n = fake_blits;
            broken_frame();
            CHECK(fake_blits == n + 1 && fake_blit_read == 0 && fake_blit_draw == 2);   // the window was read
        }
        CHECK(traced_binds > 0 && fake_draws == 6 && fake_clears == 3);
        // Unity draws into its own framebuffer: its resolve is its own
        good_frame();
        CHECK(fake_blit_read == 1 && fake_blit_draw == 2);
        // part of the window, a depth blit, a single-sampled source: left alone
        Bind(GL_FRAMEBUFFER, 0); Draw(4, 0, 3);
        Bind(GL_READ_FRAMEBUFFER, 1); Bind(GL_DRAW_FRAMEBUFFER, 2);
        Blit(0, 0, 320, 240, 0, 0, 320, 240, GL_COLOR_BUFFER_BIT, 0x2600);
        CHECK(fake_blit_read == 1);
        Blit(0, 0, 640, 480, 0, 0, 640, 480, 0x0100, 0x2600);
        CHECK(fake_blit_read == 1);
        Bind(GL_READ_FRAMEBUFFER, 3);
        Blit(0, 0, 640, 480, 0, 0, 640, 480, GL_COLOR_BUFFER_BIT, 0x2600);
        CHECK(fake_blit_read == 3);
        msaafix_frame();
        // nothing drawn into the window in a frame: left alone too
        Bind(GL_READ_FRAMEBUFFER, 1); Bind(GL_DRAW_FRAMEBUFFER, 2);
        Blit(0, 0, 640, 480, 0, 0, 640, 480, GL_COLOR_BUFFER_BIT, 0x2600);
        CHECK(fake_blit_read == 1);
        msaafix_frame();
    } else { fprintf(stderr, "usage: test_msaafix off|on\n"); return 2; }
    printf("%s msaafix %s: %d failure(s)\n", failures ? "FAILED" : "PASSED", s, failures);
    return failures != 0;
}
