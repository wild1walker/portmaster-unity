// pointer.c check, one scenario per run (pointer.c reads its switches once): the events it sends
// (recorded by stand-ins for the up_* functions), what it takes from the pad, and the cursor it
// draws into fake_gles.c's window with Unity's GL state put back. "off" checks that with no switch
// set nothing is sent, taken or drawn. Run by pointer_test.sh.
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../pointer.h"
#include "../../jni/unityplayer.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- stand-ins for the runtime ---------------------------------------------------------- */
#define MAXEV 4096
static struct ev { char kind; int dev, action, a, b; float x, y, vs; } evs[MAXEV];
static int nev;
static struct up_input_device devs[8];
static int ndev;
int up_add_input_device(const struct up_input_device *d) { if (ndev < 8) devs[ndev++] = *d; return 0; }
void up_remove_input_device(int id) { (void)id; }
int up_inject_key(int dev, int src, int action, int keycode, int repeat, int meta)
{ (void)repeat; (void)meta; if (nev < MAXEV) evs[nev++] = (struct ev){ 'k', dev, action, keycode, src, 0, 0, 0 }; return 1; }
int up_inject_mouse(int dev, int action, int buttons, float x, float y, float vs, float hs)
{ (void)hs; if (nev < MAXEV) evs[nev++] = (struct ev){ 'm', dev, action, buttons, 0, x, y, vs }; return 1; }
int up_inject_touch(int action, int n, const int *ids, const float *x, const float *y)
{ (void)ids; if (nev < MAXEV) evs[nev++] = (struct ev){ 't', 0, action, n, 0, x[0], y[0], 0 }; return 1; }

/* fake_gles.c */
extern unsigned char fake_fb0[480][640][4];
extern int fake_calls, fake_clears, fake_other_fb_clears, fake_bad_scissor, fake_errors;
extern int fake_scissor_on, fake_discard_on, fake_box[4], fake_draw_fb;
extern float fake_cc[4];
extern unsigned char fake_mask[4];

static struct ev *last(char kind) { for (int i = nev - 1; i >= 0; i--) if (evs[i].kind == kind) return &evs[i]; return NULL; }
static void run_ticks(double secs) { for (double t = 0; t < secs; t += 0.005) { pointer_tick(); usleep(5000); } }
static const unsigned char *px(int x, int y) { return fake_fb0[479 - y][x]; }   // y from the top
static int is(const unsigned char *p, int r, int g, int b) { return p[0] == r && p[1] == g && p[2] == b; }

// Unity's state at the swap, which drawing must leave as it was
static void unity_state(void)
{
    fake_scissor_on = 0; fake_discard_on = 1; fake_draw_fb = 7;
    fake_box[0] = 1; fake_box[1] = 2; fake_box[2] = 3; fake_box[3] = 4;
    fake_cc[0] = 0.25f; fake_cc[1] = 0.5f; fake_cc[2] = 0.75f; fake_cc[3] = 1;
    fake_mask[0] = 1; fake_mask[1] = 0; fake_mask[2] = 1; fake_mask[3] = 0;
    memset(fake_fb0, 128, sizeof fake_fb0);
}
static int unity_state_kept(void)
{
    return !fake_scissor_on && fake_discard_on && fake_draw_fb == 7 && fake_box[0] == 1 && fake_box[1] == 2 &&
           fake_box[2] == 3 && fake_box[3] == 4 && fake_cc[0] == 0.25f && fake_cc[1] == 0.5f && fake_cc[2] == 0.75f &&
           fake_cc[3] == 1 && fake_mask[0] == 1 && fake_mask[1] == 0 && fake_mask[2] == 1 && fake_mask[3] == 0;
}

static float pad[8];
static void axes(float lx, float ly, float hx, float hy) { memset(pad, 0, sizeof pad); pad[0] = lx; pad[1] = ly; pad[4] = hx; pad[5] = hy; pad[2] = 0.3f; pointer_axes(0, pad); }

static void off(void)
{
    int id = 1;
    CHECK(pointer_init(640, 480, &id) == 0);
    CHECK(id == 1 && ndev == 0);
    CHECK(!pointer_enabled() && !pointer_in_shots());
    pointer_start();
    CHECK(pointer_key(UP_KEYCODE_BUTTON_A, 1) == 0 && pointer_key(UP_KEYCODE_BUTTON_START, 1) == 0);
    axes(1, -1, 1, 1);
    CHECK(pad[0] == 1 && pad[1] == -1 && pad[2] == 0.3f && pad[4] == 1 && pad[5] == 1);   // the game gets it all
    pointer_warp(0.5, 0.5, 1);
    run_ticks(0.05);
    for (unsigned long f = 0; f < 5; f++) pointer_frames(f);
    unity_state();
    pointer_draw();
    CHECK(nev == 0);
    CHECK(fake_calls == 0 && unity_state_kept());
}

static void mouse(void)
{
    setenv("UA_POINTER", "1", 1);
    setenv("UA_POINTER_START", "0.25/0.5", 1);
    int id = 3;
    CHECK(pointer_init(640, 480, &id) == 1);
    CHECK(id == 4 && ndev == 1 && devs[0].id == 3 && devs[0].sources == UP_SOURCE_MOUSE && devs[0].naxes >= 2);
    pointer_start();
    struct ev *e = last('m');
    CHECK(e && e->action == UP_ACTION_HOVER_MOVE && e->x == 160 && e->y == 240 && e->a == 0 && e->dev == 3);
    // Unity drops what was sent before its first frames: the hover again after the second frame, once,
    // half a pixel beside and then on the cursor (that frame's mouse delta: the last event's)
    int n0 = nev;
    pointer_frames(0); pointer_frames(1);
    CHECK(nev == n0);
    pointer_frames(2);
    CHECK(nev == n0 + 3 && evs[n0].action == UP_ACTION_HOVER_ENTER && evs[n0].x == 160.5f && evs[n0].y == 240 &&
          evs[n0 + 1].action == UP_ACTION_HOVER_MOVE && evs[n0 + 1].x == 160.5f && evs[n0 + 1].y == 240 &&
          evs[n0 + 2].action == UP_ACTION_HOVER_MOVE && evs[n0 + 2].x == 160 && evs[n0 + 2].y == 240 &&
          evs[n0 + 2].a == 0 && evs[n0 + 2].dev == 3);
    pointer_frames(3); pointer_frames(50);
    CHECK(nev == n0 + 3);
    // the left stick and the d-pad are the cursor's; the rest still the game's
    axes(1, 0, 0, 0);
    CHECK(pad[0] == 0 && pad[1] == 0 && pad[4] == 0 && pad[5] == 0 && pad[2] == 0.3f);
    run_ticks(0.3);
    axes(0, 0, 0, 0);
    run_ticks(0.03);
    e = last('m');
    // 1.2 screen heights per second at full deflection: ~173 px in 0.3 s (scheduling slack allowed)
    CHECK(e && e->action == UP_ACTION_HOVER_MOVE && e->x > 160 + 120 && e->x < 600 && e->y == 240);
    float x = e->x;
    n0 = nev;
    run_ticks(0.05);
    CHECK(nev == n0);                                   // still: nothing more is sent
    // d-pad up: moves at a quarter of the speed at first
    axes(0, 0, 0, -1);
    run_ticks(0.2);
    axes(0, 0, 0, 0);
    run_ticks(0.03);
    e = last('m');
    CHECK(e->x == x && e->y < 240 - 10 && e->y > 240 - 120);
    float y = e->y;
    // buttons: A = left, B = right; X stays the game's
    CHECK(pointer_key(UP_KEYCODE_BUTTON_X, 1) == 0);
    n0 = nev;
    CHECK(pointer_key(UP_KEYCODE_BUTTON_A, 1) == 1);
    CHECK(nev == n0 + 2 && evs[n0].action == UP_ACTION_HOVER_EXIT && evs[n0 + 1].action == UP_ACTION_DOWN &&
          evs[n0 + 1].a == UP_BUTTON_PRIMARY && evs[n0 + 1].x == x && evs[n0 + 1].y == y);
    CHECK(pointer_key(UP_KEYCODE_BUTTON_B, 1) == 1);
    e = last('m');
    CHECK(e->action == UP_ACTION_MOVE && e->a == (UP_BUTTON_PRIMARY | UP_BUTTON_SECONDARY));
    axes(-1, 0, 0, 0);                                  // a drag
    run_ticks(0.1);
    e = last('m');
    CHECK(e->action == UP_ACTION_MOVE && e->a == 3 && e->x < x);
    axes(0, 0, 0, 0);
    run_ticks(0.03);
    CHECK(pointer_key(UP_KEYCODE_BUTTON_A, 0) == 1);
    e = last('m');
    CHECK(e->action == UP_ACTION_MOVE && e->a == UP_BUTTON_SECONDARY);
    n0 = nev;
    CHECK(pointer_key(UP_KEYCODE_BUTTON_B, 0) == 1);
    CHECK(nev == n0 + 2 && evs[n0].action == UP_ACTION_UP && evs[n0].a == 0 && evs[n0 + 1].action == UP_ACTION_HOVER_ENTER);
    // warps (UA_INPUT_SCRIPT ptr=), clamped to the screen
    pointer_warp(0.5, 0.5, 1);
    e = last('m');
    CHECK(e->action == UP_ACTION_HOVER_MOVE && e->x == 320 && e->y == 240);
    pointer_warp(5000, -3, 0);
    e = last('m');
    CHECK(e->x == 639 && e->y == 0);
    // drawing at the hot spot (10, 20): outline, fill, untouched beside it; Unity's state kept
    pointer_warp(10, 20, 0);
    unity_state();
    pointer_draw();
    CHECK(fake_clears > 10 && fake_other_fb_clears == 0 && fake_bad_scissor == 0);
    CHECK(unity_state_kept());
    CHECK(is(px(10, 20), 0, 0, 0) && is(px(10, 21), 0, 0, 0));        // outline column
    CHECK(is(px(11, 23), 255, 255, 255) && is(px(12, 23), 255, 255, 255) && is(px(13, 23), 0, 0, 0));   // "XWWX"
    CHECK(is(px(15, 21), 128, 128, 128) && is(px(9, 20), 128, 128, 128));   // beside the arrow
    // at the bottom right corner: clipped, never outside the window
    pointer_warp(639, 479, 0);
    unity_state();
    fake_bad_scissor = 0;
    pointer_draw();
    CHECK(fake_bad_scissor == 0 && unity_state_kept() && is(px(639, 479), 0, 0, 0));
    CHECK(!pointer_in_shots());
    CHECK(fake_errors == 0);
}

// a GLES 2 context: the same cursor, no GLES 3 enum used
static void es2(void)
{
    setenv("FAKE_GLES2", "1", 1);
    setenv("UA_POINTER", "1", 1);
    int id = 1;
    CHECK(pointer_init(640, 480, &id) == 1);
    pointer_warp(10, 20, 0);
    unity_state();
    fake_discard_on = 0;                                // no such state in GLES 2
    pointer_draw();
    CHECK(fake_errors == 0 && fake_clears > 10 && fake_other_fb_clears == 0);
    CHECK(fake_draw_fb == 7 && !fake_scissor_on && fake_box[2] == 3 && fake_cc[1] == 0.5f && fake_mask[1] == 0);
    CHECK(is(px(10, 20), 0, 0, 0) && is(px(11, 23), 255, 255, 255));
}

static void touch(void)
{
    setenv("UA_POINTER", "touch", 1);
    setenv("UA_POINTER_SPEED", "2", 1);
    int id = 1;
    CHECK(pointer_init(640, 480, &id) == 1);
    CHECK(id == 1 && ndev == 0);                        // no mouse device
    pointer_start();
    pointer_frames(2);
    CHECK(nev == 0);                                    // no finger, no event
    axes(1, 0, 0, 0);
    run_ticks(0.1);
    axes(0, 0, 0, 0);
    CHECK(last('t') == NULL && last('m') == NULL);      // moving without a finger down: nothing
    CHECK(pointer_key(UP_KEYCODE_BUTTON_A, 1) == 1);
    struct ev *e = last('t');
    CHECK(e && e->action == UP_ACTION_DOWN && e->x > 320 + 50 && e->y == 240);
    float x = e->x;
    axes(0, 1, 0, 0);
    run_ticks(0.1);
    axes(0, 0, 0, 0);
    run_ticks(0.03);
    e = last('t');
    CHECK(e->action == UP_ACTION_MOVE && e->x == x && e->y > 240);
    CHECK(pointer_key(UP_KEYCODE_BUTTON_B, 1) == 1 && last('t') == e);   // the right button: taken, no touch
    CHECK(pointer_key(UP_KEYCODE_BUTTON_A, 0) == 1);
    e = last('t');
    CHECK(e->action == UP_ACTION_UP);
    CHECK(last('m') == NULL);
}

static void keymap(void)
{
    setenv("UA_KEYMAP", "start:escape,x:space,up:up,select:f1", 1);
    setenv("UA_POINTER_SHOTS", "1", 1);                 // no pointer mode: no effect
    int id = 2;
    CHECK(pointer_init(640, 480, &id) == 0);
    CHECK(id == 3 && ndev == 1 && devs[0].sources == UP_SOURCE_KEYBOARD);
    CHECK(pointer_key(UP_KEYCODE_BUTTON_START, 1) == 1);
    struct ev *e = last('k');
    CHECK(e && e->dev == 2 && e->a == 111 && e->action == UP_ACTION_DOWN && e->b == UP_SOURCE_KEYBOARD);
    CHECK(pointer_key(UP_KEYCODE_BUTTON_START, 0) == 1 && last('k')->action == UP_ACTION_UP);
    CHECK(pointer_key(UP_KEYCODE_BUTTON_SELECT, 1) == 1 && last('k')->a == 131);
    CHECK(pointer_key(UP_KEYCODE_BUTTON_A, 1) == 0);    // unmapped: the game's
    axes(0.5f, 0, 0, -1);                               // d-pad up -> the up arrow; the d-pad is then taken
    e = last('k');
    CHECK(e->a == 19 && e->action == UP_ACTION_DOWN && pad[5] == 0 && pad[4] == 0 && pad[0] == 0.5f);
    int n0 = nev;
    axes(0, 0, 0, -1);
    CHECK(nev == n0);                                   // held: no repeat
    axes(0, 0, 0, 0);
    CHECK(last('k')->a == 19 && last('k')->action == UP_ACTION_UP);
    unity_state();
    pointer_draw();
    CHECK(fake_calls == 0);
}

// UA_DPAD_SWAP: the d-pad steers (arrow keys) until select swaps it with the stick, which moves the cursor
static void swap(void)
{
    setenv("UA_KEYMAP", "left:left,right:right", 1);
    setenv("UA_POINTER", "mouse", 1);
    setenv("UA_POINTER_MOVE", "lstick", 1);
    setenv("UA_DPAD_SWAP", "select", 1);
    int id = 1;
    CHECK(pointer_init(640, 480, &id) == 1);
    pointer_start();
    axes(0, 0, -1, 0);                                  // d-pad left: the left arrow
    CHECK(last('k')->a == 21 && last('k')->action == UP_ACTION_DOWN);
    CHECK(pointer_key(UP_KEYCODE_BUTTON_SELECT, 1) == 1);    // swapped while held: the key is let go
    CHECK(last('k')->a == 21 && last('k')->action == UP_ACTION_UP);
    CHECK(pointer_key(UP_KEYCODE_BUTTON_SELECT, 0) == 1);
    int nk = 0;
    for (int i = 0; i < nev; i++) nk += evs[i].kind == 'k';
    axes(0, 0, 1, 0);                                   // d-pad right now moves the cursor
    CHECK(pad[4] == 0 && pad[0] == 0);
    run_ticks(0.2);
    axes(0, 0, 0, 0);
    run_ticks(0.03);
    struct ev *e = last('m');
    CHECK(e && e->action == UP_ACTION_HOVER_MOVE && e->x > 320 + 50);
    int nk2 = 0;
    for (int i = 0; i < nev; i++) nk2 += evs[i].kind == 'k';
    CHECK(nk2 == nk);                                   // no key for it
    axes(1, 0, 0, 0);                                   // the stick now steers
    CHECK(last('k')->a == 22 && last('k')->action == UP_ACTION_DOWN && pad[0] == 0);
    axes(0, 0, 0, 0);
    CHECK(last('k')->a == 22 && last('k')->action == UP_ACTION_UP);
    CHECK(pointer_key(UP_KEYCODE_BUTTON_SELECT, 1) == 1);    // back
    axes(0, 0, -1, 0);
    CHECK(last('k')->a == 21 && last('k')->action == UP_ACTION_DOWN);
}

static void toggle(void)
{
    setenv("UA_POINTER", "mouse", 1);
    setenv("UA_POINTER_TOGGLE", "select", 1);
    setenv("UA_POINTER_SHOTS", "1", 1);
    setenv("UA_POINTER_BUTTONS", "r1:left", 1);
    int id = 1;
    CHECK(pointer_init(640, 480, &id) == 1);
    CHECK(pointer_in_shots());
    pointer_start();
    CHECK(pointer_key(UP_KEYCODE_BUTTON_A, 1) == 0);    // A is not a mouse button here
    CHECK(pointer_key(UP_KEYCODE_BUTTON_R1, 1) == 1 && last('m')->action == UP_ACTION_DOWN);
    int n0 = nev;
    CHECK(pointer_key(UP_KEYCODE_BUTTON_SELECT, 1) == 1);    // off: the held button is released, the mouse leaves
    CHECK(nev > n0 && evs[n0].action == UP_ACTION_UP && evs[n0].a == 0 && last('m')->action == UP_ACTION_HOVER_EXIT && !pointer_enabled());
    CHECK(pointer_key(UP_KEYCODE_BUTTON_SELECT, 0) == 1);
    CHECK(pointer_key(UP_KEYCODE_BUTTON_R1, 0) == 0);   // off: the pad is a pad
    axes(1, 0, 0, 1);
    CHECK(pad[0] == 1 && pad[5] == 1);
    unity_state();
    pointer_draw();
    CHECK(fake_clears == 0 && unity_state_kept());
    CHECK(pointer_key(UP_KEYCODE_BUTTON_SELECT, 1) == 1 && pointer_enabled());
    axes(1, 0, 0, 1);
    CHECK(pad[0] == 0 && pad[5] == 0);
}

int main(int argc, char **argv)
{
    const char *s = argc > 1 ? argv[1] : "";
    if (!strcmp(s, "off")) off();
    else if (!strcmp(s, "mouse")) mouse();
    else if (!strcmp(s, "touch")) touch();
    else if (!strcmp(s, "keymap")) keymap();
    else if (!strcmp(s, "swap")) swap();
    else if (!strcmp(s, "toggle")) toggle();
    else if (!strcmp(s, "es2")) es2();
    else { fprintf(stderr, "usage: test_pointer off|mouse|touch|keymap|swap|toggle|es2\n"); return 2; }
    printf("%s %s: %d failure(s)\n", failures ? "FAILED" : "PASSED", s, failures);
    return failures != 0;
}
