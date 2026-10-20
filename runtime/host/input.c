// Gamepads for the Android player: SDL2 GameController (the handheld's built-in pad, mapped by
// SDL_GAMECONTROLLERCONFIG, e.g. from PortMaster) presented to Unity as Android gamepads with the
// standard Android layout, the way an Xbox/PlayStation pad shows up on a phone:
//   buttons  A B X Y L1 R1 THUMBL THUMBR START SELECT MODE  -> KeyEvent KEYCODE_BUTTON_*
//   sticks   AXIS_X/AXIS_Y (left), AXIS_Z/AXIS_RZ (right)    -> MotionEvent (SOURCE_JOYSTICK)
//   d-pad    AXIS_HAT_X/AXIS_HAT_Y (-1/0/1)
//   triggers AXIS_LTRIGGER/AXIS_RTRIGGER and AXIS_BRAKE/AXIS_GAS (0..1), plus L2/R2 keys
// UA_PAD_NAME overrides the device name Unity sees (scripts may pick a layout by name).
// UA_INPUT_SCRIPT="<sec>:<button>[~<hold sec>][+<button>...] ..." presses buttons at those times
// after start (held 0.15 s, or <hold sec>; a,b,x,y,l1,r1,l2,r2,start,select,up,down,left,right),
// "<sec>@<period>:..." repeats every <period> s; e.g. "30:a 36:a 70:right~60 75@10:x~9.9 80@1.5:a"
// for unattended tests; lup/ldown/lleft/lright: the left stick, kup..kright: DPAD keys,
// ptr=<x>/<y>: the pointer (UA_POINTER) to that spot (fractions of the screen with a '.', else
// pixels), e.g. "40:ptr=0.9/0.47 41:a~1". Scripted buttons and sticks go through pointer mode and
// UA_KEYMAP like the pad's own (pointer.c).
#include "input.h"
#include "pointer.h"
#include "../jni/unityplayer.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_PADS 4
static struct pad {
    SDL_GameController *gc;
    SDL_JoystickID jid;
    int id;                     // Android device id
    float axes[8];              // last sent, in the order of pad_axes[]
    int l2, r2;                 // trigger key state
    int script_hx, script_hy;   // UA_INPUT_SCRIPT d-pad and left stick, merged into the pad's own state
    int script_lx, script_ly;
    int virt;                   // UA_INPUT_SCRIPT's own pad (no SDL pad present)
} pads[MAX_PADS];

static const int pad_axes[8] = { UP_AXIS_X, UP_AXIS_Y, UP_AXIS_Z, UP_AXIS_RZ,
                                 UP_AXIS_HAT_X, UP_AXIS_HAT_Y, UP_AXIS_LTRIGGER, UP_AXIS_RTRIGGER };
static int next_id = 1;
static int started;

static int keycode(int b)
{
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return UP_KEYCODE_BUTTON_A;
    case SDL_CONTROLLER_BUTTON_B: return UP_KEYCODE_BUTTON_B;
    case SDL_CONTROLLER_BUTTON_X: return UP_KEYCODE_BUTTON_X;
    case SDL_CONTROLLER_BUTTON_Y: return UP_KEYCODE_BUTTON_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return UP_KEYCODE_BUTTON_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return UP_KEYCODE_BUTTON_R1;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return UP_KEYCODE_BUTTON_THUMBL;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return UP_KEYCODE_BUTTON_THUMBR;
    case SDL_CONTROLLER_BUTTON_START: return UP_KEYCODE_BUTTON_START;
    case SDL_CONTROLLER_BUTTON_BACK: return UP_KEYCODE_BUTTON_SELECT;
    case SDL_CONTROLLER_BUTTON_GUIDE: return UP_KEYCODE_BUTTON_MODE;
    default: return 0;          // d-pad: hat axes
    }
}

static struct pad *pad_of(SDL_JoystickID jid)
{
    for (int i = 0; i < MAX_PADS; i++) if (pads[i].gc && pads[i].jid == jid) return &pads[i];
    return NULL;
}

static void pad_open(int index)
{
    struct pad *p = NULL;
    for (int i = 0; i < MAX_PADS && !p; i++) if (!pads[i].gc && !pads[i].virt) p = &pads[i];
    if (!p) return;
    SDL_GameController *gc = SDL_GameControllerOpen(index);
    if (!gc) { fprintf(stderr, "[input] pad %d: %s\n", index, SDL_GetError()); return; }
    SDL_Joystick *js = SDL_GameControllerGetJoystick(gc);
    if (pad_of(SDL_JoystickInstanceID(js))) { SDL_GameControllerClose(gc); return; }
    memset(p, 0, sizeof *p);
    p->gc = gc; p->jid = SDL_JoystickInstanceID(js); p->id = next_id++;

    static char names[MAX_PADS][128], descs[MAX_PADS][64];
    int slot = (int)(p - pads);
    const char *name = getenv("UA_PAD_NAME") ? getenv("UA_PAD_NAME") : SDL_GameControllerName(gc);
    snprintf(names[slot], sizeof names[slot], "%s", name ? name : "Gamepad");
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), descs[slot], sizeof descs[slot]);
    struct up_input_device d;
    memset(&d, 0, sizeof d);
    d.id = p->id; d.name = names[slot]; d.descriptor = descs[slot];
    d.sources = UP_SOURCE_GAMEPAD | UP_SOURCE_JOYSTICK | UP_SOURCE_DPAD;
    d.vendor_id = SDL_GameControllerGetVendor(gc); d.product_id = SDL_GameControllerGetProduct(gc);
    static const int all[] = { UP_AXIS_X, UP_AXIS_Y, UP_AXIS_Z, UP_AXIS_RZ, UP_AXIS_HAT_X, UP_AXIS_HAT_Y,
                               UP_AXIS_LTRIGGER, UP_AXIS_RTRIGGER, UP_AXIS_BRAKE, UP_AXIS_GAS };
    d.naxes = sizeof all / sizeof all[0];
    for (int i = 0; i < d.naxes; i++)
    {
        int trig = all[i] == UP_AXIS_LTRIGGER || all[i] == UP_AXIS_RTRIGGER || all[i] == UP_AXIS_BRAKE || all[i] == UP_AXIS_GAS;
        d.axis[i] = all[i]; d.min[i] = trig ? 0.f : -1.f; d.max[i] = 1.f;
        d.flat[i] = all[i] == UP_AXIS_HAT_X || all[i] == UP_AXIS_HAT_Y || trig ? 0.f : 0.06f;
        d.fuzz[i] = 0.004f;
    }
    up_add_input_device(&d);
    fprintf(stderr, "[input] pad %d: \"%s\" (SDL \"%s\", %04x:%04x) as Android device %d\n", index, names[slot],
            SDL_GameControllerName(gc), d.vendor_id, d.product_id, p->id);
}

static void pad_close(SDL_JoystickID jid)
{
    struct pad *p = pad_of(jid);
    if (!p) return;
    up_remove_input_device(p->id);
    SDL_GameControllerClose(p->gc);
    p->gc = NULL;
    float none[8] = { 0 };
    pointer_axes((int)(p - pads), none);       // its share of the cursor's movement
}

int input_init(int width, int height)
{
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
    { fprintf(stderr, "[input] SDL: %s\n", SDL_GetError()); return -1; }
    SDL_GameControllerEventState(SDL_ENABLE);
    int n = SDL_NumJoysticks(), pads_found = 0;
    for (int i = 0; i < n; i++)
    {
        if (SDL_IsGameController(i)) { pad_open(i); pads_found++; }
        else
        {
            char guid[64];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof guid);
            fprintf(stderr, "[input] joystick %d \"%s\" (GUID %s) has no GameController mapping "
                    "(SDL_GAMECONTROLLERCONFIG / SDL_GAMECONTROLLERCONFIG_FILE)\n", i, SDL_JoystickNameForIndex(i), guid);
        }
    }
    if (!pads_found) fprintf(stderr, "[input] no gamepad\n");
    pointer_init(width, height, &next_id);
    return 0;
}

static void send_axes(struct pad *p);

/* ---- scripted presses -------------------------------------------------------------------- */
#define MAX_PRESS 512
#define KEY_PTR (-2)              // ptr=<x>/<y>: hat_x/hat_y unused, at pos[]
static struct press { double at; int key; int hat_x, hat_y; int down; int stick; double period; float pos[2]; int frac; } script[MAX_PRESS];
static int nscript, next_press;
static double t_start;
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int script_key(const char *n, int *hx, int *hy, int *stick)
{
    *stick = 0;
    if (n[0] == 'l' && (!strcasecmp(n + 1, "up") || !strcasecmp(n + 1, "down") || !strcasecmp(n + 1, "left") || !strcasecmp(n + 1, "right")))
    { *stick = 1; return script_key(n + 1, hx, hy, &(int){0}); }        /* lup/ldown/lleft/lright: left stick */
    if (!strcasecmp(n, "kup")) return UP_KEYCODE_DPAD_UP;                /* kup/kdown/kleft/kright: DPAD keys */
    if (!strcasecmp(n, "kdown")) return UP_KEYCODE_DPAD_DOWN;
    if (!strcasecmp(n, "kleft")) return UP_KEYCODE_DPAD_LEFT;
    if (!strcasecmp(n, "kright")) return UP_KEYCODE_DPAD_RIGHT;
    static const struct { const char *n; int k; } keys[] = {
        { "a", UP_KEYCODE_BUTTON_A }, { "b", UP_KEYCODE_BUTTON_B }, { "x", UP_KEYCODE_BUTTON_X },
        { "y", UP_KEYCODE_BUTTON_Y }, { "l1", UP_KEYCODE_BUTTON_L1 }, { "r1", UP_KEYCODE_BUTTON_R1 },
        { "l2", UP_KEYCODE_BUTTON_L2 }, { "r2", UP_KEYCODE_BUTTON_R2 }, { "start", UP_KEYCODE_BUTTON_START },
        { "select", UP_KEYCODE_BUTTON_SELECT } };
    *hx = *hy = 0;
    if (!strcasecmp(n, "up")) { *hy = -1; return 0; }
    if (!strcasecmp(n, "down")) { *hy = 1; return 0; }
    if (!strcasecmp(n, "left")) { *hx = -1; return 0; }
    if (!strcasecmp(n, "right")) { *hx = 1; return 0; }
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) if (!strcasecmp(n, keys[i].n)) return keys[i].k;
    fprintf(stderr, "[input] script: unknown button %s\n", n);
    return -1;
}
static int cmp_press(const void *a, const void *b)
{
    double d = ((const struct press *)a)->at - ((const struct press *)b)->at;
    return d < 0 ? -1 : d > 0;
}
static void script_parse(const char *spec)
{
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", spec);
    char *save;
    for (char *tok = strtok_r(buf, " ,;", &save); tok && nscript < MAX_PRESS - 4; tok = strtok_r(NULL, " ,;", &save))
    {
        char *colon = strchr(tok, ':');
        if (!colon) continue;
        double at = atof(tok), period = strchr(tok, '@') && strchr(tok, '@') < colon ? atof(strchr(tok, '@') + 1) : 0;
        for (char *b = strtok_r(colon + 1, "+", &colon); b && nscript < MAX_PRESS - 4; b = strtok_r(NULL, "+", &colon))
        {
            if (!strncasecmp(b, "ptr=", 4))
            {
                float x = 0, y = 0;
                if (sscanf(b + 4, "%f/%f", &x, &y) != 2) { fprintf(stderr, "[input] script: %s: want ptr=<x>/<y>\n", b); continue; }
                script[nscript++] = (struct press){ at, KEY_PTR, 0, 0, 1, 0, period, { x, y }, strchr(b, '.') != NULL };
                continue;
            }
            double hold = 0.15;
            char *tilde = strchr(b, '~');
            if (tilde) { *tilde = 0; hold = atof(tilde + 1); }
            int hx, hy, stick, k = script_key(b, &hx, &hy, &stick);
            if (k < 0) continue;
            if (period > 0 && hold >= period) hold = period * 0.9;
            script[nscript++] = (struct press){ at, k, hx, hy, 1, stick, period, { 0, 0 }, 0 };
            script[nscript++] = (struct press){ at + hold, k, hx, hy, 0, stick, period, { 0, 0 }, 0 };
        }
    }
    qsort(script, nscript, sizeof script[0], cmp_press);
}
static struct pad *scripted_pad(void)
{
    for (int i = 0; i < MAX_PADS; i++) if (pads[i].gc) return &pads[i];
    /* no real pad: a virtual one, presented like a real pad (UA_PAD_NAME, the same axes), since
       Rewired maps Android pads by name */
    static struct pad *vp;
    if (vp) return vp;
    for (int i = 0; i < MAX_PADS && !vp; i++) if (!pads[i].gc && !pads[i].virt) vp = &pads[i];
    if (!vp) return NULL;
    memset(vp, 0, sizeof *vp);
    vp->virt = 1;
    struct up_input_device d;
    memset(&d, 0, sizeof d);
    d.id = next_id++; d.name = getenv("UA_PAD_NAME") ? getenv("UA_PAD_NAME") : "Virtual Gamepad"; d.descriptor = "ua-virtual-pad";
    d.sources = UP_SOURCE_GAMEPAD | UP_SOURCE_JOYSTICK | UP_SOURCE_DPAD;
    static const int all[] = { UP_AXIS_X, UP_AXIS_Y, UP_AXIS_Z, UP_AXIS_RZ, UP_AXIS_HAT_X, UP_AXIS_HAT_Y,
                               UP_AXIS_LTRIGGER, UP_AXIS_RTRIGGER, UP_AXIS_BRAKE, UP_AXIS_GAS };
    d.naxes = sizeof all / sizeof all[0];
    for (int i = 0; i < d.naxes; i++)
    {
        int trig = all[i] == UP_AXIS_LTRIGGER || all[i] == UP_AXIS_RTRIGGER || all[i] == UP_AXIS_BRAKE || all[i] == UP_AXIS_GAS;
        d.axis[i] = all[i]; d.min[i] = trig ? 0.f : -1.f; d.max[i] = 1.f;
    }
    up_add_input_device(&d);
    vp->id = d.id;
    fprintf(stderr, "[input] virtual pad \"%s\" as Android device %d\n", d.name, d.id);
    return vp;
}
static void script_run(void)
{
    static struct pad *sp;
    double t = now_s() - t_start;
    while (next_press < nscript && script[next_press].at <= t)
    {
        if (next_press >= MAX_PRESS / 2)                    // drop what was played
        {
            memmove(script, script + next_press, (nscript - next_press) * sizeof script[0]);
            nscript -= next_press; next_press = 0;
        }
        struct press done = script[next_press++], *p = &done;
        if (p->period > 0 && nscript < MAX_PRESS)          // repeating: back in, one period later
        {
            struct press again = *p;
            again.at += p->period;
            int i = nscript++;
            while (i > next_press && script[i - 1].at > again.at) { script[i] = script[i - 1]; i--; }
            script[i] = again;
        }
        if (p->key == KEY_PTR) { pointer_warp(p->pos[0], p->pos[1], p->frac); continue; }
        if (!sp) sp = scripted_pad();
        if (!sp) continue;
        if (p->period <= 0)
            fprintf(stderr, "[input] script %.2f s: %s %d/%d,%d\n", t, p->down ? "down" : "up", p->key, p->hat_x, p->hat_y);
        if (p->key)
        {
            if (!pointer_key(p->key, p->down))
                up_inject_key(sp->id, UP_SOURCE_GAMEPAD, p->down ? UP_ACTION_DOWN : UP_ACTION_UP, p->key, 0, 0);
        }
        else
        {
            int *sx = p->stick ? &sp->script_lx : &sp->script_hx, *sy = p->stick ? &sp->script_ly : &sp->script_hy;
            if (p->hat_x) *sx = p->down ? p->hat_x : 0;
            if (p->hat_y) *sy = p->down ? p->hat_y : 0;
            send_axes(sp);
        }
    }
}

void input_started(void)
{
    started = 1;
    t_start = now_s();
    if (getenv("UA_INPUT_SCRIPT")) script_parse(getenv("UA_INPUT_SCRIPT"));
    pointer_start();
}

static void send_axes(struct pad *p)
{
    float v[8] = { 0 };
    SDL_GameController *gc = p->gc;
    if (gc)
    {
        v[0] = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) / 32767.f;
        v[1] = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) / 32767.f;
        v[2] = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.f;
        v[3] = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.f;
        v[4] = (float)(SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) - SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT));
        v[5] = (float)(SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN) - SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP));
        v[6] = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.f;
        v[7] = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.f;
    }
    if (p->script_lx) v[0] = (float)p->script_lx;
    if (p->script_ly) v[1] = (float)p->script_ly;
    if (p->script_hx) v[4] = (float)p->script_hx;
    if (p->script_hy) v[5] = (float)p->script_hy;
    for (int i = 0; i < 8; i++) { if (v[i] < -1.f) v[i] = -1.f; if (v[i] > 1.f) v[i] = 1.f; }
    pointer_axes((int)(p - pads), v);           // pointer mode / UA_KEYMAP take their controls
    if (!memcmp(v, p->axes, sizeof v)) return;
    memcpy(p->axes, v, sizeof v);
    int ax[10]; float val[10];
    for (int i = 0; i < 8; i++) { ax[i] = pad_axes[i]; val[i] = v[i]; }
    ax[8] = UP_AXIS_BRAKE; val[8] = v[6];
    ax[9] = UP_AXIS_GAS;   val[9] = v[7];
    up_inject_axes(p->id, UP_SOURCE_JOYSTICK, 10, ax, val);
    // pads with analog triggers also report L2/R2 as keys past half travel
    int l2 = v[6] > 0.5f, r2 = v[7] > 0.5f;
    if (l2 != p->l2) up_inject_key(p->id, UP_SOURCE_GAMEPAD, l2 ? UP_ACTION_DOWN : UP_ACTION_UP, UP_KEYCODE_BUTTON_L2, 0, 0);
    if (r2 != p->r2) up_inject_key(p->id, UP_SOURCE_GAMEPAD, r2 ? UP_ACTION_DOWN : UP_ACTION_UP, UP_KEYCODE_BUTTON_R2, 0, 0);
    p->l2 = l2; p->r2 = r2;
}

void input_poll(void)
{
    SDL_Event e;
    int moved[MAX_PADS] = {0};
    while (SDL_PollEvent(&e))
    {
        struct pad *p;
        switch (e.type) {
        case SDL_CONTROLLERDEVICEADDED: pad_open(e.cdevice.which); break;
        case SDL_CONTROLLERDEVICEREMOVED: pad_close(e.cdevice.which); break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            if (!started || !(p = pad_of(e.cbutton.which))) break;
            if (keycode(e.cbutton.button))
            {
                int down = e.type == SDL_CONTROLLERBUTTONDOWN;
                if (!pointer_key(keycode(e.cbutton.button), down))
                    up_inject_key(p->id, UP_SOURCE_GAMEPAD, down ? UP_ACTION_DOWN : UP_ACTION_UP, keycode(e.cbutton.button), 0, 0);
            }
            else moved[p - pads] = 1;     // d-pad -> hat axes
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (started && (p = pad_of(e.caxis.which))) moved[p - pads] = 1;
            break;
        }
    }
    for (int i = 0; i < MAX_PADS; i++) if (moved[i] && pads[i].gc) send_axes(&pads[i]);
    if (next_press < nscript) script_run();
    pointer_tick();
}
