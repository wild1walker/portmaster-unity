// input.c's d-pad hold, against stand-ins for the runtime (Unity's input calls, the frame count, the
// on-screen keyboard, pointer mode): an SDL virtual gamepad (SDL 2.0.14+, in the process; no uinput)
// pressed and let go as a quick tap would be, the hat axis Unity was sent read back.
//   a tap let go before the game drew two frames stays sent until it has, or 250 ms went by
//   a press held longer is let go at once; a turn to the other direction waits the same way
//   UA_DPAD_HOLD=0: no hold
// Built and run by input_test.sh (aarch64, SDL 2.26).
#include "../input.h"
#include "../../jni/unityplayer.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- stand-ins
static unsigned long F;                         // frames shown
static float hat_x = -9;                        // the last AXIS_HAT_X Unity was sent
static int sends;
unsigned long frames_presented(void) { return F; }
double frames_t_start(void) { return 0; }
void osk_init(int w, int h) { (void)w; (void)h; }
int osk_tick(void) { return 0; }
int osk_typing(void) { return 0; }
int osk_key(int k, int d) { (void)k; (void)d; return 0; }
void osk_axes(int s, float *v) { (void)s; (void)v; }
void osk_hw_key(int k, int d) { (void)k; (void)d; }
void osk_hw_text(const char *t) { (void)t; }
int pointer_init(int w, int h, int *next_id) { (void)w; (void)h; (void)next_id; return 0; }
void pointer_start(void) {}
void pointer_frames(unsigned long n) { (void)n; }
int pointer_key(int k, int d) { (void)k; (void)d; return 0; }
void pointer_axes(int s, float *v) { (void)s; (void)v; }
void pointer_pad(int s, int l) { (void)s; (void)l; }
int pointer_axes_changed(void) { return 0; }
void pointer_warp(double x, double y, int f) { (void)x; (void)y; (void)f; }
void pointer_tick(void) {}
int up_add_input_device(const struct up_input_device *d) { (void)d; return 1; }
void up_remove_input_device(int id) { (void)id; }
int up_inject_key(int id, int src, int a, int k, int r, int m) { (void)id; (void)src; (void)a; (void)k; (void)r; (void)m; return 1; }
int up_inject_axes(int id, int src, int n, const int *axes, const float *v)
{
    (void)id; (void)src;
    for (int i = 0; i < n; i++) if (axes[i] == UP_AXIS_HAT_X) hat_x = v[i];
    sends++;
    return 1;
}

static SDL_Joystick *J;
static void dpad(int button, int down) { SDL_JoystickSetVirtualButton(J, button, (Uint8)down); }
static void poll(void) { SDL_Delay(5); input_poll(); }

int main(int argc, char **argv)
{
    int nohold = argc > 1 && !strcmp(argv[1], "nohold");
    if (nohold) setenv("UA_DPAD_HOLD", "0", 1);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    int idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, SDL_CONTROLLER_BUTTON_MAX, 0);
    J = idx >= 0 ? SDL_JoystickOpen(idx) : NULL;
    if (!J) { fprintf(stderr, "no virtual joystick: %s\n", SDL_GetError()); return 1; }
    CHECK(input_init(640, 480) == 0);
    input_started();
    poll();
    const int R = SDL_CONTROLLER_BUTTON_DPAD_RIGHT, L = SDL_CONTROLLER_BUTTON_DPAD_LEFT;

    // a tap within one frame: down and up before the game drew a frame
    dpad(R, 1); poll();
    CHECK(hat_x == 1.f);
    dpad(R, 0); poll(); poll();
    CHECK(hat_x == (nohold ? 0.f : 1.f));       // (without the hold the game never sees it)
    F++; poll();
    CHECK(hat_x == (nohold ? 0.f : 1.f));
    F++; poll();
    CHECK(hat_x == 0.f);                        // two frames: let go
    if (nohold) { printf("%d checks, %d failures\n", checks, failures); return failures != 0; }

    // a press held for frames: let go at once
    dpad(R, 1); poll(); F += 3; poll();
    dpad(R, 0); poll();
    CHECK(hat_x == 0.f);

    // no frames at all (a load): let go after 250 ms
    dpad(R, 1); poll(); dpad(R, 0); poll();
    CHECK(hat_x == 1.f);
    SDL_Delay(260); poll();
    CHECK(hat_x == 0.f);

    // right then left at once: right until the game has seen it, then left
    dpad(R, 1); poll(); dpad(R, 0); dpad(L, 1); poll();
    CHECK(hat_x == 1.f);
    F += 2; poll();
    CHECK(hat_x == -1.f);
    dpad(L, 0); F += 2; poll();
    CHECK(hat_x == 0.f);

    // held on while still pressed: nothing re-sent while nothing changed
    int s = sends;
    poll(); poll();
    CHECK(sends == s);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
