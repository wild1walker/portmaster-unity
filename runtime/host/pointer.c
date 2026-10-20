// Pointer mode and key mapping: the handheld's pad standing in for a mouse (or a finger) and for
// keyboard keys, for games whose menus or controls want them. Both are off unless a port turns
// them on; with them off nothing here sends an event or draws anything.
//
//   UA_POINTER=1|mouse|touch   pointer mode: a cursor the host draws over Unity's frame, moved
//                       with the pad; buttons click. mouse (= 1): Android mouse MotionEvents
//                       (SOURCE_MOUSE: hover moves, button state, wheel), so Input.mousePosition
//                       follows the cursor and UI elements highlight under it; touch: one finger
//                       on the screen while the click button is held (no hover).
//   UA_POINTER_MOVE=lstick,dpad   what moves the cursor (lstick, rstick, dpad; default lstick,dpad).
//                       Those controls no longer reach the game as pad input.
//   UA_POINTER_BUTTONS=a:left,b:right   pad button : mouse button (left, right, middle; default
//                       a:left,b:right; touch mode uses the left one). Taken from the game too.
//   UA_POINTER_SCROLL=rstick   the right stick's vertical axis turns the mouse wheel (mouse mode).
//   UA_POINTER_SPEED=<n>  full-deflection speed in screen heights per second (default 1.2); the
//                       stick is progressive, the d-pad starts at a quarter and speeds up.
//   UA_POINTER_START=<x>/<y>   start position as fractions of the screen (default 0.5/0.5).
//   UA_POINTER_TOGGLE=<button>  that button switches pointer mode off and on (the pad is a pad
//                       while it is off; default: none, always on).
//   UA_POINTER_HIDE=<s>   hide the cursor after <s> seconds without movement (default 0: never).
//   UA_POINTER_CURSOR=0   don't draw the cursor (a game that draws its own at the mouse position).
//   UA_POINTER_SIZE=<n>   cursor scale (default: screen height / 400, at least 1).
//   UA_POINTER_SHOTS=1    the cursor is in <root>/shot.req screenshots too (default: not, so
//                       frame comparisons see the game's own picture).
//   UA_KEYMAP=<button>:<key>,...   pad buttons (and up/down/left/right: the d-pad) that press
//                       keyboard keys instead, e.g. start:escape,x:space,select:tab; keys: a-z, 0-9,
//                       f1-f12, escape, enter, space, tab, backspace, delete, up, down, left, right,
//                       shift, ctrl, alt, pageup, pagedown, home, end, back. Works with or without
//                       pointer mode; mapped buttons no longer reach the game as pad buttons.
// Pad button names: a b x y l1 r1 l2 r2 l3 r3 start select mode.
//
// The cursor is drawn on Unity's render thread right before eglSwapBuffers (frames.c's swap hook)
// with scissored clears only: no shader, texture or buffer of Unity's is touched, and the few GL
// states the clears use (scissor, clear colour, colour mask, draw framebuffer, rasterizer
// discard) are put back.
#include "pointer.h"
#include "../jni/unityplayer.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

enum { EV_MOUSE = 1, EV_TOUCH = 2 };
enum { MV_LSTICK = 1, MV_RSTICK = 2, MV_DPAD = 4 };
#define MAX_SLOTS 8
#define MAX_MAP 24

static struct {
    int events;                 // 0: pointer mode not configured
    int on;                     // currently on (UA_POINTER_TOGGLE)
    int move;                   // MV_*
    int scroll_rstick;
    double speed;               // px/s at full deflection
    int toggle_key;
    double hide_s;
    int draw, in_shots, scale;
    int width, height;
    int mouse_dev;
    struct { int key, button; } buttons[8];
    int nbuttons;
} P;

static struct { int pad_key; int dir; int key; int down; } keymap[MAX_MAP];   // dir: hat direction 1-4 (up, down, left, right) or 0
static int nkeymap, kb_dev;

// state (UI thread)
static double px, py;                       // cursor, view pixels
static volatile int draw_x, draw_y, shown;  // for the render thread
static float stick[MAX_SLOTS][2], hat[MAX_SLOTS][2], scroll_y[MAX_SLOTS];
static int held;                            // mouse buttons held (UP_BUTTON_*)
static int pressed[8];                      // P.buttons[i] held
static int touching;
static double t_last, t_moved, t_sent, hat_held, t_scroll;
static int sent_x = -1, sent_y = -1;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* ---- names ---------------------------------------------------------------------------------- */
static int pad_button(const char *n)
{
    static const struct { const char *n; int k; } b[] = {
        { "a", UP_KEYCODE_BUTTON_A }, { "b", UP_KEYCODE_BUTTON_B }, { "x", UP_KEYCODE_BUTTON_X },
        { "y", UP_KEYCODE_BUTTON_Y }, { "l1", UP_KEYCODE_BUTTON_L1 }, { "r1", UP_KEYCODE_BUTTON_R1 },
        { "l2", UP_KEYCODE_BUTTON_L2 }, { "r2", UP_KEYCODE_BUTTON_R2 }, { "l3", UP_KEYCODE_BUTTON_THUMBL },
        { "r3", UP_KEYCODE_BUTTON_THUMBR }, { "thumbl", UP_KEYCODE_BUTTON_THUMBL }, { "thumbr", UP_KEYCODE_BUTTON_THUMBR },
        { "start", UP_KEYCODE_BUTTON_START }, { "select", UP_KEYCODE_BUTTON_SELECT }, { "mode", UP_KEYCODE_BUTTON_MODE },
        { "guide", UP_KEYCODE_BUTTON_MODE } };
    for (unsigned i = 0; i < sizeof b / sizeof b[0]; i++) if (!strcasecmp(n, b[i].n)) return b[i].k;
    return 0;
}

static int key_code(const char *n)
{
    static const struct { const char *n; int k; } k[] = {
        { "escape", 111 }, { "esc", 111 }, { "enter", 66 }, { "return", 66 }, { "space", 62 }, { "tab", 61 },
        { "backspace", 67 }, { "delete", 112 }, { "up", 19 }, { "down", 20 }, { "left", 21 }, { "right", 22 },
        { "shift", 59 }, { "ctrl", 113 }, { "alt", 57 }, { "pageup", 92 }, { "pagedown", 93 }, { "home", 122 },
        { "end", 123 }, { "insert", 124 }, { "back", 4 }, { "minus", 69 }, { "equals", 70 }, { "comma", 55 },
        { "period", 56 }, { "slash", 76 }, { "grave", 68 } };
    if (n[0] && !n[1] && n[0] >= 'a' && n[0] <= 'z') return 29 + n[0] - 'a';     // KEYCODE_A..Z
    if (n[0] && !n[1] && n[0] >= '0' && n[0] <= '9') return 7 + n[0] - '0';      // KEYCODE_0..9
    if ((n[0] == 'f' || n[0] == 'F') && atoi(n + 1) >= 1 && atoi(n + 1) <= 12) return 130 + atoi(n + 1);   // F1 = 131
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) if (!strcasecmp(n, k[i].n)) return k[i].k;
    return 0;
}

static int hat_dir(const char *n)
{
    static const char *d[] = { "up", "down", "left", "right" };
    for (int i = 0; i < 4; i++) if (!strcasecmp(n, d[i])) return i + 1;
    return 0;
}

/* ---- cursor picture: an arrow, X = outline, W = fill; hot spot at the top left --------------- */
static const char *arrow[] = {
    "X           ",
    "XX          ",
    "XWX         ",
    "XWWX        ",
    "XWWWX       ",
    "XWWWWX      ",
    "XWWWWWX     ",
    "XWWWWWWX    ",
    "XWWWWWWWX   ",
    "XWWWWWWWWX  ",
    "XWWWWWWWWWX ",
    "XWWWWWWXXXXX",
    "XWWWXWWX    ",
    "XWWXXWWX    ",
    "XWX  XWWX   ",
    "XX   XWWX   ",
    "X     XWWX  ",
    "      XWWX  ",
    "       XX   ",
};
#define ARROW_H (int)(sizeof arrow / sizeof arrow[0])
static struct rect { short x, y, w, h; char c; } rects[96];
static int nrects;

// runs of one colour per row, merged with the same run in the rows below
static void build_cursor(int s)
{
    nrects = 0;
    for (int y = 0; y < ARROW_H; y++)
        for (int x = 0; arrow[y][x]; )
        {
            char c = arrow[y][x];
            int l = 1;
            while (arrow[y][x + l] == c) l++;
            if (c != ' ')
            {
                int merged = 0;
                for (int i = 0; i < nrects && !merged; i++)
                    if (rects[i].c == c && rects[i].x == x * s && rects[i].w == l * s && rects[i].y + rects[i].h == y * s)
                    { rects[i].h += s; merged = 1; }
                if (!merged && nrects < (int)(sizeof rects / sizeof rects[0]))
                    rects[nrects++] = (struct rect){ (short)(x * s), (short)(y * s), (short)(l * s), (short)s, c };
            }
            x += l;
        }
}

/* ---- setup ------------------------------------------------------------------------------------ */
static void add_device(int id, const char *name, int sources)
{
    struct up_input_device d;
    memset(&d, 0, sizeof d);
    d.id = id; d.name = name; d.descriptor = sources == UP_SOURCE_MOUSE ? "ua-virtual-mouse" : "ua-virtual-keyboard";
    d.sources = sources;
    if (sources == UP_SOURCE_MOUSE)
    {
        d.naxes = 4;
        d.axis[0] = UP_AXIS_X; d.min[0] = 0; d.max[0] = (float)(P.width - 1);
        d.axis[1] = UP_AXIS_Y; d.min[1] = 0; d.max[1] = (float)(P.height - 1);
        d.axis[2] = UP_AXIS_VSCROLL; d.min[2] = -1; d.max[2] = 1;
        d.axis[3] = UP_AXIS_HSCROLL; d.min[3] = -1; d.max[3] = 1;
    }
    up_add_input_device(&d);
}

static void parse_keymap(const char *spec)
{
    char buf[512], *save;
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *t = strtok_r(buf, ", ", &save); t && nkeymap < MAX_MAP; t = strtok_r(NULL, ", ", &save))
    {
        char *c = strchr(t, ':');
        if (!c) { fprintf(stderr, "[input] UA_KEYMAP: %s: want <button>:<key>\n", t); continue; }
        *c = 0;
        int b = pad_button(t), d = b ? 0 : hat_dir(t), k = key_code(c + 1);
        if ((!b && !d) || !k) { fprintf(stderr, "[input] UA_KEYMAP: unknown %s %s\n", !b && !d ? "button" : "key", !b && !d ? t : c + 1); continue; }
        keymap[nkeymap++] = (typeof(keymap[0])){ b, d, k, 0 };
    }
}

int pointer_init(int width, int height, int *next_id)
{
    P.width = width > 0 ? width : 640; P.height = height > 0 ? height : 480;
    const char *km = getenv("UA_KEYMAP");
    if (km && *km)
    {
        parse_keymap(km);
        if (nkeymap) { kb_dev = (*next_id)++; add_device(kb_dev, "Virtual Keyboard", UP_SOURCE_KEYBOARD);
                       fprintf(stderr, "[input] keymap: %d pad controls press keys (keyboard device %d)\n", nkeymap, kb_dev); }
    }
    const char *e = getenv("UA_POINTER");
    if (!e || !*e || !strcmp(e, "0")) return 0;
    P.events = !strcasecmp(e, "touch") ? EV_TOUCH : EV_MOUSE;
    P.on = 1;
    const char *mv = getenv("UA_POINTER_MOVE") ? getenv("UA_POINTER_MOVE") : "lstick,dpad";
    if (strstr(mv, "lstick")) P.move |= MV_LSTICK;
    if (strstr(mv, "rstick")) P.move |= MV_RSTICK;
    if (strstr(mv, "dpad")) P.move |= MV_DPAD;
    P.scroll_rstick = getenv("UA_POINTER_SCROLL") && !strcasecmp(getenv("UA_POINTER_SCROLL"), "rstick") && !(P.move & MV_RSTICK);
    P.speed = (getenv("UA_POINTER_SPEED") ? atof(getenv("UA_POINTER_SPEED")) : 1.2) * P.height;
    if (P.speed <= 0) P.speed = 1.2 * P.height;
    char buf[256], *save;
    snprintf(buf, sizeof buf, "%s", getenv("UA_POINTER_BUTTONS") ? getenv("UA_POINTER_BUTTONS") : "a:left,b:right");
    for (char *t = strtok_r(buf, ", ", &save); t && P.nbuttons < 8; t = strtok_r(NULL, ", ", &save))
    {
        char *c = strchr(t, ':');
        int mb = !c ? 0 : !strcasecmp(c + 1, "left") ? UP_BUTTON_PRIMARY : !strcasecmp(c + 1, "right") ? UP_BUTTON_SECONDARY
                 : !strcasecmp(c + 1, "middle") ? UP_BUTTON_TERTIARY : 0;
        if (c) *c = 0;
        int k = pad_button(t);
        if (!k || !mb) { fprintf(stderr, "[input] UA_POINTER_BUTTONS: %s: want <button>:left|right|middle\n", t); continue; }
        P.buttons[P.nbuttons].key = k; P.buttons[P.nbuttons++].button = mb;
    }
    P.toggle_key = getenv("UA_POINTER_TOGGLE") ? pad_button(getenv("UA_POINTER_TOGGLE")) : 0;
    P.hide_s = getenv("UA_POINTER_HIDE") ? atof(getenv("UA_POINTER_HIDE")) : 0;
    P.draw = !(getenv("UA_POINTER_CURSOR") && !strcmp(getenv("UA_POINTER_CURSOR"), "0"));
    P.in_shots = getenv("UA_POINTER_SHOTS") && strcmp(getenv("UA_POINTER_SHOTS"), "0") != 0;
    P.scale = getenv("UA_POINTER_SIZE") ? atoi(getenv("UA_POINTER_SIZE")) : (P.height + 200) / 400;
    if (P.scale < 1) P.scale = 1;
    if (P.scale > 8) P.scale = 8;
    build_cursor(P.scale);
    double sx = 0.5, sy = 0.5;
    if (getenv("UA_POINTER_START")) sscanf(getenv("UA_POINTER_START"), "%lf/%lf", &sx, &sy);
    px = sx * (P.width - 1); py = sy * (P.height - 1);
    draw_x = (int)px; draw_y = (int)py; shown = 1;
    if (P.events == EV_MOUSE) { P.mouse_dev = (*next_id)++; add_device(P.mouse_dev, "Virtual Mouse", UP_SOURCE_MOUSE); }
    fprintf(stderr, "[input] pointer mode (%s events): move with%s%s%s, %d buttons, speed %.0f px/s, cursor %s%s\n",
            P.events == EV_MOUSE ? "mouse" : "touch", P.move & MV_LSTICK ? " lstick" : "", P.move & MV_RSTICK ? " rstick" : "",
            P.move & MV_DPAD ? " dpad" : "", P.nbuttons, P.speed, P.draw ? "drawn" : "not drawn", P.toggle_key ? ", toggle button set" : "");
    return 1;
}

int pointer_enabled(void) { return P.events && P.on; }
int pointer_in_shots(void) { return P.in_shots; }

/* ---- events ----------------------------------------------------------------------------------- */
static void send_move(void)
{
    int x = (int)lround(px), y = (int)lround(py);
    if (x == sent_x && y == sent_y) return;
    sent_x = x; sent_y = y;
    if (P.events == EV_MOUSE)
        up_inject_mouse(P.mouse_dev, held ? UP_ACTION_MOVE : UP_ACTION_HOVER_MOVE, held, (float)x, (float)y, 0, 0);
    else if (touching)
    {
        float fx = (float)x, fy = (float)y;
        up_inject_touch(UP_ACTION_MOVE, 1, NULL, &fx, &fy);
    }
}

static void set_buttons(int now_held)
{
    float x = (float)lround(px), y = (float)lround(py);
    if (P.events == EV_TOUCH)
    {
        int t = (now_held & UP_BUTTON_PRIMARY) != 0;
        if (t != touching) up_inject_touch(t ? UP_ACTION_DOWN : UP_ACTION_UP, 1, NULL, &x, &y);
        touching = t;
        held = now_held;
        return;
    }
    if (now_held == held) return;
    int action = !held ? UP_ACTION_DOWN : !now_held ? UP_ACTION_UP : UP_ACTION_MOVE;
    if (!held) up_inject_mouse(P.mouse_dev, UP_ACTION_HOVER_EXIT, 0, x, y, 0, 0);
    held = now_held;
    up_inject_mouse(P.mouse_dev, action, held, x, y, 0, 0);
    if (!held) up_inject_mouse(P.mouse_dev, UP_ACTION_HOVER_ENTER, 0, x, y, 0, 0);
    sent_x = (int)x; sent_y = (int)y;
}

static void set_on(int on)
{
    if (on == P.on) return;
    if (!on)
    {
        set_buttons(0);                         // what is held is let go, and the mouse leaves
        memset(pressed, 0, sizeof pressed);
        if (P.events == EV_MOUSE) up_inject_mouse(P.mouse_dev, UP_ACTION_HOVER_EXIT, 0, (float)lround(px), (float)lround(py), 0, 0);
    }
    P.on = on;
    shown = on;
    fprintf(stderr, "[input] pointer mode %s\n", on ? "on" : "off");
    if (on) { sent_x = sent_y = -1; t_moved = now_s(); send_move(); }
}

int pointer_key(int keycode, int down)
{
    for (int i = 0; i < nkeymap; i++)
        if (keymap[i].pad_key == keycode)
        {
            if (down != keymap[i].down) up_inject_key(kb_dev, UP_SOURCE_KEYBOARD, down ? UP_ACTION_DOWN : UP_ACTION_UP, keymap[i].key, 0, 0);
            keymap[i].down = down;
            return 1;
        }
    if (!P.events) return 0;
    if (P.toggle_key && keycode == P.toggle_key) { if (down) set_on(!P.on); return 1; }
    if (!P.on) return 0;
    for (int i = 0; i < P.nbuttons; i++)
        if (P.buttons[i].key == keycode)
        {
            int h = 0;
            pressed[i] = down;
            for (int j = 0; j < P.nbuttons; j++) if (pressed[j]) h |= P.buttons[j].button;
            set_buttons(h);
            t_moved = now_s();
            return 1;
        }
    return 0;
}

// hat directions of the keymap: v[4], v[5] (-1/0/1) -> key presses; those axes are then taken
static void keymap_hat(float *v)
{
    int any = 0;
    for (int i = 0; i < nkeymap; i++)
    {
        if (!keymap[i].dir) continue;
        any = 1;
        int d = keymap[i].dir;
        int down = d == 1 ? v[5] < -0.5f : d == 2 ? v[5] > 0.5f : d == 3 ? v[4] < -0.5f : v[4] > 0.5f;
        if (down != keymap[i].down) up_inject_key(kb_dev, UP_SOURCE_KEYBOARD, down ? UP_ACTION_DOWN : UP_ACTION_UP, keymap[i].key, 0, 0);
        keymap[i].down = down;
    }
    if (any) v[4] = v[5] = 0;
}

void pointer_axes(int slot, float *v)
{
    if (slot < 0 || slot >= MAX_SLOTS) return;
    keymap_hat(v);
    if (!P.events) return;
    stick[slot][0] = stick[slot][1] = hat[slot][0] = hat[slot][1] = scroll_y[slot] = 0;
    if (!P.on) return;
    if (P.move & MV_LSTICK) { stick[slot][0] += v[0]; stick[slot][1] += v[1]; v[0] = v[1] = 0; }
    if (P.move & MV_RSTICK) { stick[slot][0] += v[2]; stick[slot][1] += v[3]; v[2] = v[3] = 0; }
    if (P.move & MV_DPAD) { hat[slot][0] = v[4]; hat[slot][1] = v[5]; v[4] = v[5] = 0; }
    if (P.scroll_rstick) { scroll_y[slot] = v[3]; v[2] = v[3] = 0; }
}

void pointer_warp(double x, double y, int fraction)
{
    if (!P.events) return;
    px = fraction ? x * (P.width - 1) : x;
    py = fraction ? y * (P.height - 1) : y;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px > P.width - 1) px = P.width - 1;
    if (py > P.height - 1) py = P.height - 1;
    draw_x = (int)lround(px); draw_y = (int)lround(py);
    t_moved = now_s();
    fprintf(stderr, "[input] pointer at %d,%d\n", draw_x, draw_y);
    if (P.on) send_move();
}

void pointer_start(void)
{
    if (!pointer_enabled()) return;
    t_last = t_moved = now_s();
    if (P.events == EV_MOUSE) up_inject_mouse(P.mouse_dev, UP_ACTION_HOVER_ENTER, 0, (float)lround(px), (float)lround(py), 0, 0);
    send_move();
}

void pointer_tick(void)
{
    if (!pointer_enabled()) return;
    double t = now_s(), dt = t - t_last;
    t_last = t;
    if (dt > 0.1) dt = 0.1;
    if (dt < 0) dt = 0;
    double vx = 0, vy = 0, hx = 0, hy = 0, sc = 0;
    for (int i = 0; i < MAX_SLOTS; i++) { vx += stick[i][0]; vy += stick[i][1]; hx += hat[i][0]; hy += hat[i][1]; sc += scroll_y[i]; }
    // the stick: radial dead zone, then progressive (half deflection = a quarter of the speed)
    double m = sqrt(vx * vx + vy * vy), dx = 0, dy = 0;
    const double dz = 0.15;
    if (m > dz)
    {
        double k = (m > 1 ? 1 : m - dz) / (1 - dz);
        double s = P.speed * k * k / m;
        dx += vx * s * dt; dy += vy * s * dt;
    }
    // the d-pad: a quarter of the speed at first, full speed after 0.8 s held
    if (hx || hy)
    {
        hat_held += dt;
        double hm = sqrt(hx * hx + hy * hy);
        double s = P.speed * (0.25 + 0.75 * (hat_held > 0.8 ? 1 : hat_held / 0.8)) / hm;
        dx += hx * s * dt; dy += hy * s * dt;
    }
    else hat_held = 0;
    if (dx || dy)
    {
        px += dx; py += dy;
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        if (px > P.width - 1) px = P.width - 1;
        if (py > P.height - 1) py = P.height - 1;
        draw_x = (int)lround(px); draw_y = (int)lround(py);
        t_moved = t;
        if (t - t_sent >= 0.016) { t_sent = t; send_move(); }
    }
    else if (t - t_sent >= 0.016) { t_sent = t; send_move(); }   // the last position of a move
    // the wheel: one notch per 0.15 s past half deflection (stick up = wheel up)
    if (P.events == EV_MOUSE && (sc > 0.5 || sc < -0.5) && t - t_scroll >= 0.15)
    {
        t_scroll = t;
        up_inject_mouse(P.mouse_dev, UP_ACTION_SCROLL, held, (float)lround(px), (float)lround(py), sc < 0 ? 1.f : -1.f, 0);
    }
    shown = P.hide_s <= 0 || t - t_moved < P.hide_s;
}

/* ---- drawing (render thread, Unity's context current, right before the swap) ------------------- */
#define GL_SCISSOR_BOX 0x0C10
#define GL_SCISSOR_TEST 0x0C11
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_RASTERIZER_DISCARD 0x8C89
static struct {
    void (*Enable)(unsigned), (*Disable)(unsigned);
    unsigned char (*IsEnabled)(unsigned);
    void (*Scissor)(int, int, int, int);
    void (*ClearColor)(float, float, float, float);
    void (*Clear)(unsigned);
    void (*GetIntegerv)(unsigned, int *);
    void (*GetFloatv)(unsigned, float *);
    void (*GetBooleanv)(unsigned, unsigned char *);
    void (*ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
    void (*BindFramebuffer)(unsigned, unsigned);
} gl;
static int gl_state;   // 0 unresolved, 1 ok, -1 missing

static int gl_resolve(void)
{
    if (gl_state) return gl_state > 0;
    void *h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
#define G(n) (*(void **)&gl.n = h ? dlsym(h, "gl" #n) : NULL, gl.n != NULL)
    int ok = G(Enable) & G(Disable) & G(IsEnabled) & G(Scissor) & G(ClearColor) & G(Clear) & G(GetIntegerv) &
             G(GetFloatv) & G(GetBooleanv) & G(ColorMask) & G(BindFramebuffer);
#undef G
    gl_state = ok ? 1 : -1;
    if (!ok) fprintf(stderr, "[input] pointer: no GLES functions, the cursor is not drawn\n");
    return ok;
}

void pointer_draw(void)
{
    if (!P.events || !P.draw || !P.on || !shown || !gl_resolve()) return;
    int x0 = draw_x, y0 = draw_y, H = P.height;
    int fb = 0, box[4];
    float cc[4];
    unsigned char mask[4];
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    gl.GetIntegerv(GL_SCISSOR_BOX, box);
    gl.GetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    gl.GetBooleanv(GL_COLOR_WRITEMASK, mask);
    unsigned char sc = gl.IsEnabled(GL_SCISSOR_TEST), rd = gl.IsEnabled(GL_RASTERIZER_DISCARD);
    if (fb) gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (!sc) gl.Enable(GL_SCISSOR_TEST);
    if (rd) gl.Disable(GL_RASTERIZER_DISCARD);
    gl.ColorMask(1, 1, 1, 1);
    for (int pass = 0; pass < 2; pass++)
    {
        char c = pass ? 'W' : 'X';
        if (pass) gl.ClearColor(1, 1, 1, 1); else gl.ClearColor(0, 0, 0, 1);
        for (int i = 0; i < nrects; i++)
        {
            const struct rect *r = &rects[i];
            if (r->c != c) continue;
            int x = x0 + r->x, y = y0 + r->y, w = r->w, h = r->h;
            if (x >= P.width || y >= H) continue;
            if (x + w > P.width) w = P.width - x;
            if (y + h > H) h = H - y;
            gl.Scissor(x, H - y - h, w, h);          // GL's rows count from the bottom
            gl.Clear(GL_COLOR_BUFFER_BIT);
        }
    }
    gl.ClearColor(cc[0], cc[1], cc[2], cc[3]);
    gl.ColorMask(mask[0], mask[1], mask[2], mask[3]);
    gl.Scissor(box[0], box[1], box[2], box[3]);
    if (rd) gl.Enable(GL_RASTERIZER_DISCARD);
    if (!sc) gl.Disable(GL_SCISSOR_TEST);
    if (fb) gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned)fb);
}
