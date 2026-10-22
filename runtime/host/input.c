// Gamepads for the Android player: SDL2 GameController (the handheld's built-in pad, mapped by
// SDL_GAMECONTROLLERCONFIG, e.g. from PortMaster) presented to Unity as Android gamepads with the
// standard Android layout, the way an Xbox/PlayStation pad shows up on a phone:
//   buttons  A B X Y L1 R1 THUMBL THUMBR START SELECT MODE  -> KeyEvent KEYCODE_BUTTON_*
//   sticks   AXIS_X/AXIS_Y (left), AXIS_Z/AXIS_RZ (right)    -> MotionEvent (SOURCE_JOYSTICK)
//   d-pad    AXIS_HAT_X/AXIS_HAT_Y (-1/0/1)
//   triggers AXIS_LTRIGGER/AXIS_RTRIGGER and AXIS_BRAKE/AXIS_GAS (0..1), plus L2/R2 keys
// UA_PAD_NAME overrides the device name Unity sees (scripts may pick a layout by name).
// <root>/input.req: more presses in the same syntax while the game runs (times from when it is read)
// UA_INPUT_SCRIPT="<sec>:<button>[~<hold sec>][+<button>...] ..." presses buttons at those times
// after start (held 0.15 s, or <hold sec>; a,b,x,y,l1,r1,l2,r2,start,select,up,down,left,right),
// "<sec>@<period>:..." repeats every <period> s; e.g. "30:a 36:a 70:right~60 75@10:x~9.9 80@1.5:a"
// for unattended tests (the pad they press is the first real one, else a virtual pad registered at
// the first press; UA_VIRTUAL_PAD=1 registers it at start, as a pad present at boot would be);
// lup/ldown/lleft/lright: the left stick, kup..kright: DPAD keys,
// ptr=<x>/<y>: the pointer (UA_POINTER) to that spot (fractions of the screen with a '.', else
// pixels), e.g. "40:ptr=0.9/0.47 41:a~1"; text=<chars>: typed into an open soft keyboard as a
// hardware keyboard would ('_' types a space). Scripted buttons and sticks go through the
// on-screen keyboard, pointer mode and UA_KEYMAP like the pad's own (osk.c, pointer.c).
// The soft keyboard: while a game has it open the pad drives the on-screen keyboard (osk.c).
// A hardware keyboard (SDL key events: an SDL window) types into an open soft keyboard; with
// UA_KEYBOARD=1 its keys also reach the game as an Android keyboard (KeyEvents from a
// "Hardware Keyboard" device) while none is open.
#include "input.h"
#include "frames.h"
#include "osk.h"
#include "pointer.h"
#include "../jni/unityplayer.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
static int kb_dev;              // UA_KEYBOARD=1: the hardware keyboard's Android device

static struct pad *scripted_pad(void);

// a pad button: the on-screen keyboard first, then pointer mode / UA_KEYMAP; 1 if taken
static int take_key(int keycode, int down) { return osk_key(keycode, down) || pointer_key(keycode, down); }

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
    if (!pads_found && getenv("UA_VIRTUAL_PAD") && !strcmp(getenv("UA_VIRTUAL_PAD"), "1")) scripted_pad();
    pointer_init(width, height, &next_id);
    osk_init(width, height);
    if (getenv("UA_KEYBOARD") && !strcmp(getenv("UA_KEYBOARD"), "1"))
    {
        struct up_input_device d;
        memset(&d, 0, sizeof d);
        d.id = kb_dev = next_id++; d.name = "Hardware Keyboard"; d.descriptor = "ua-hardware-keyboard";
        d.sources = UP_SOURCE_KEYBOARD;
        up_add_input_device(&d);
        fprintf(stderr, "[input] hardware keyboard: keys reach the game as Android device %d\n", kb_dev);
    }
    return 0;
}

static void send_axes(struct pad *p);

/* ---- scripted presses -------------------------------------------------------------------- */
#define MAX_PRESS 512
#define KEY_PTR (-2)              // ptr=<x>/<y>: hat_x/hat_y unused, at pos[]
#define KEY_TEXT (-3)             // text=<chars>: text[]
static struct press { double at; int key; int hat_x, hat_y; int down; int stick; double period; float pos[2]; int frac; char text[24]; } script[MAX_PRESS];
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
static void script_parse_at(const char *spec, double base);
static void script_parse(const char *spec) { script_parse_at(spec, 0); }
/* presses at <sec> after base (0: the start; live input: now), merged in among the unplayed ones */
static void script_parse_at(const char *spec, double base)
{
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", spec);
    char *save;
    for (char *tok = strtok_r(buf, " ,;", &save); tok && nscript < MAX_PRESS - 4; tok = strtok_r(NULL, " ,;", &save))
    {
        char *colon = strchr(tok, ':');
        if (!colon) continue;
        double at = base + atof(tok), period = strchr(tok, '@') && strchr(tok, '@') < colon ? atof(strchr(tok, '@') + 1) : 0;
        for (char *b = strtok_r(colon + 1, "+", &colon); b && nscript < MAX_PRESS - 4; b = strtok_r(NULL, "+", &colon))
        {
            if (!strncasecmp(b, "text=", 5))
            {
                struct press t = { at, KEY_TEXT, 0, 0, 1, 0, period, { 0, 0 }, 0, "" };
                snprintf(t.text, sizeof t.text, "%s", b + 5);
                for (char *c = t.text; *c; c++) if (*c == '_') *c = ' ';
                script[nscript++] = t;
                continue;
            }
            if (!strncasecmp(b, "ptr=", 4))
            {
                float x = 0, y = 0;
                if (sscanf(b + 4, "%f/%f", &x, &y) != 2) { fprintf(stderr, "[input] script: %s: want ptr=<x>/<y>\n", b); continue; }
                script[nscript++] = (struct press){ at, KEY_PTR, 0, 0, 1, 0, period, { x, y }, strchr(b, '.') != NULL, "" };
                continue;
            }
            double hold = 0.15;
            char *tilde = strchr(b, '~');
            if (tilde) { *tilde = 0; hold = atof(tilde + 1); }
            int hx, hy, stick, k = script_key(b, &hx, &hy, &stick);
            if (k < 0) continue;
            if (period > 0 && hold >= period) hold = period * 0.9;
            script[nscript++] = (struct press){ at, k, hx, hy, 1, stick, period, { 0, 0 }, 0, "" };
            script[nscript++] = (struct press){ at + hold, k, hx, hy, 0, stick, period, { 0, 0 }, 0, "" };
        }
    }
    qsort(script + next_press, nscript - next_press, sizeof script[0], cmp_press);
}

/* live input: <root>/input.req holds presses in UA_INPUT_SCRIPT's syntax, their times counted
   from when it is read; it is read (every 0.25 s) and deleted. Write it elsewhere and rename it
   into place, so it is never read half-written. For driving a game from a test script (emulated
   rig or handheld) by what its screenshots show. */
static char g_live[600];
void input_set_root(const char *root) { snprintf(g_live, sizeof g_live, "%s/input.req", root); }
static void live_input(void)
{
    static double last;
    if (!started || !g_live[0]) return;
    double t = now_s() - t_start;
    if (t - last < 0.25) return;
    last = t;
    FILE *f = fopen(g_live, "r");
    if (!f) return;
    char spec[2048];
    size_t n = fread(spec, 1, sizeof spec - 1, f);
    fclose(f);
    unlink(g_live);
    spec[n] = 0;
    for (char *c = spec; *c; c++) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
    int before = nscript;
    script_parse_at(spec, t);
    fprintf(stderr, "[input] input.req at %.1f s: %d events (%s)\n", t, nscript - before, spec);
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
// A game reads the pad once a frame, so a press released within one frame never reaches it: in the
// emulated rig (about 1 fps) or during a long load. A scripted release therefore waits until
// SCRIPT_HOLD_FRAMES frames were presented since the last press, and the first press waits
// SCRIPT_PAD_FRAMES frames after the pad is taken (a virtual pad appears then, and games such as
// Rewired's take a frame or two to list it). At 60 fps that is 33 ms; real pads are not affected.
#define SCRIPT_HOLD_FRAMES 2
#define SCRIPT_PAD_FRAMES 3
static void script_run(void)
{
    static struct pad *sp;
    static unsigned long pad_frame, down_frame;
    double t = now_s() - t_start;
    while (next_press < nscript && script[next_press].at <= t)
    {
        const struct press *q = &script[next_press];
        if (q->key != KEY_PTR && q->key != KEY_TEXT)
        {
            if (!sp && (sp = scripted_pad())) pad_frame = frames_presented();
            unsigned long f = frames_presented();
            if (sp && (f < pad_frame + SCRIPT_PAD_FRAMES || (!q->down && f < down_frame + SCRIPT_HOLD_FRAMES)))
                break;                                      // again at the next poll, order kept
        }
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
        if (p->key == KEY_TEXT)
        {
            fprintf(stderr, "[input] script %.2f s: text \"%s\"%s\n", t, p->text, osk_typing() ? "" : " (no soft keyboard open)");
            osk_hw_text(p->text);
            continue;
        }
        if (!sp) sp = scripted_pad();
        if (!sp) continue;
        if (p->period <= 0)
            fprintf(stderr, "[input] script %.2f s: %s %d/%d,%d\n", t, p->down ? "down" : "up", p->key, p->hat_x, p->hat_y);
        if (p->down) down_frame = frames_presented();
        if (p->key)
        {
            if (!take_key(p->key, p->down))
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
    osk_axes((int)(p - pads), v);               // the on-screen keyboard takes the pad while open
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

/* ---- a hardware keyboard (SDL key events) ------------------------------------------------- */
static int android_key(SDL_Keycode k)
{
    if (k >= SDLK_a && k <= SDLK_z) return 29 + (int)(k - SDLK_a);          // KEYCODE_A..Z
    if (k >= SDLK_0 && k <= SDLK_9) return 7 + (int)(k - SDLK_0);           // KEYCODE_0..9
    if (k >= SDLK_F1 && k <= SDLK_F12) return 131 + (int)(k - SDLK_F1);     // KEYCODE_F1..F12
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return 145 + (int)(k - SDLK_KP_1); // KEYCODE_NUMPAD_1..9
    static const struct { SDL_Keycode s; int a; } m[] = {
        { SDLK_RETURN, 66 }, { SDLK_ESCAPE, 111 }, { SDLK_BACKSPACE, 67 }, { SDLK_TAB, 61 }, { SDLK_SPACE, 62 },
        { SDLK_MINUS, 69 }, { SDLK_EQUALS, 70 }, { SDLK_LEFTBRACKET, 71 }, { SDLK_RIGHTBRACKET, 72 },
        { SDLK_BACKSLASH, 73 }, { SDLK_SEMICOLON, 74 }, { SDLK_QUOTE, 75 }, { SDLK_BACKQUOTE, 68 },
        { SDLK_COMMA, 55 }, { SDLK_PERIOD, 56 }, { SDLK_SLASH, 76 }, { SDLK_CAPSLOCK, 115 },
        { SDLK_PRINTSCREEN, 120 }, { SDLK_SCROLLLOCK, 116 }, { SDLK_PAUSE, 121 }, { SDLK_INSERT, 124 },
        { SDLK_HOME, 122 }, { SDLK_PAGEUP, 92 }, { SDLK_DELETE, 112 }, { SDLK_END, 123 }, { SDLK_PAGEDOWN, 93 },
        { SDLK_RIGHT, 22 }, { SDLK_LEFT, 21 }, { SDLK_DOWN, 20 }, { SDLK_UP, 19 }, { SDLK_NUMLOCKCLEAR, 143 },
        { SDLK_KP_DIVIDE, 154 }, { SDLK_KP_MULTIPLY, 155 }, { SDLK_KP_MINUS, 156 }, { SDLK_KP_PLUS, 157 },
        { SDLK_KP_ENTER, 160 }, { SDLK_KP_0, 144 }, { SDLK_KP_PERIOD, 158 }, { SDLK_MENU, 82 },
        { SDLK_LCTRL, 113 }, { SDLK_LSHIFT, 59 }, { SDLK_LALT, 57 }, { SDLK_LGUI, 117 },
        { SDLK_RCTRL, 114 }, { SDLK_RSHIFT, 60 }, { SDLK_RALT, 58 }, { SDLK_RGUI, 118 } };
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) if (m[i].s == k) return m[i].a;
    return 0;
}
static int android_meta(Uint16 mod)
{
    int m = 0;
    if (mod & KMOD_LSHIFT) m |= 0x41;          // META_SHIFT_ON | META_SHIFT_LEFT_ON
    if (mod & KMOD_RSHIFT) m |= 0x81;
    if (mod & KMOD_LALT) m |= 0x12;
    if (mod & KMOD_RALT) m |= 0x22;
    if (mod & KMOD_LCTRL) m |= 0x3000;
    if (mod & KMOD_RCTRL) m |= 0x5000;
    if (mod & KMOD_CAPS) m |= 0x100000;
    if (mod & KMOD_NUM) m |= 0x200000;
    return m;
}
static void hw_key(const SDL_KeyboardEvent *k)
{
    int code = android_key(k->keysym.sym), down = k->type == SDL_KEYDOWN;
    if (!code) return;
    if (osk_typing()) { osk_hw_key(code, down); return; }   // the soft keyboard's field has the focus
    if (kb_dev)
        up_inject_key(kb_dev, UP_SOURCE_KEYBOARD, down ? UP_ACTION_DOWN : UP_ACTION_UP, code, k->repeat, android_meta(k->keysym.mod));
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
                if (!take_key(keycode(e.cbutton.button), down))
                    up_inject_key(p->id, UP_SOURCE_GAMEPAD, down ? UP_ACTION_DOWN : UP_ACTION_UP, keycode(e.cbutton.button), 0, 0);
            }
            else moved[p - pads] = 1;     // d-pad -> hat axes
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (started && (p = pad_of(e.caxis.which))) moved[p - pads] = 1;
            break;
        case SDL_TEXTINPUT:
            if (started && osk_typing()) osk_hw_text(e.text.text);
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (started) hw_key(&e.key);
            break;
        }
    }
    for (int i = 0; i < MAX_PADS; i++) if (moved[i] && pads[i].gc) send_axes(&pads[i]);
    live_input();                       // (cheap: a stat of input.req every 0.25 s)
    if (next_press < nscript) script_run();
    if (osk_tick())                             // the soft keyboard opened or closed: the game
        for (int i = 0; i < MAX_PADS; i++)      // sees a centred pad while it is open
            if (pads[i].gc || pads[i].virt) send_axes(&pads[i]);
    pointer_frames(frames_presented());
    pointer_tick();
}
