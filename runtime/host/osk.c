// On-screen keyboard: the handheld's stand-in for Android's IME while a game has Unity's soft
// keyboard open (TouchScreenKeyboard.Open -> UnityPlayer.showSoftInput). The text field itself is
// the runtime's (../jni/up_softinput.c: Unity's input dialog, which reports every change to the
// game as Android does); this file draws a keyboard and the field at the bottom of the screen and
// types into the field with the pad. Nothing is drawn and no input is taken unless the game opens
// its keyboard.
//
//   pad                  d-pad / left stick: move over the keys (held: repeats)
//                        A: press the key   B: delete (held: repeats)   X: space   Y: shift (twice:
//                        caps lock, three times: off)   L1 / R1: caret left / right
//                        Start: done (the dialog's OK)   Select: cancel (Back)
//   keys                 letters, a symbols page (?123 / ABC), shift, space, backspace, Enter (a
//                        newline in a multi-line field, else done); number and phone pads for the
//                        game's numeric keyboard types
//   hardware keyboard    (SDL, input.c) types into the field; Backspace, Delete, arrows, Home, End,
//                        Enter, Escape edit it
//
//   UA_OSK=0             no on-screen keyboard: nothing is drawn and the pad stays the game's (a
//                        hardware keyboard still types into the field)
//   UA_OSK=done|cancel   answer every keyboard at once, as if OK (the game keeps the text it
//                        offered) or Back were pressed: for ports where text entry does not matter
//   UA_OSK_SCALE=<n>     glyph scale (default: screen height / 240)
//   UA_OSK_SHOTS=0       keep the keyboard out of <root>/shot.req screenshots (default: in them)
//
// Auto-capitalisation follows the field's input type (CAP_WORDS / CAP_SENTENCES / CAP_CHARACTERS,
// which Unity sets for some keyboard types), as Android's keyboards do; a password field shows *.
//
// Drawing (render thread, right before the swap, like pointer.c's cursor): the keyboard is rendered
// on the CPU when it changes. On a GLES 3 context with a single-sampled window it is uploaded to a
// texture of its own and copied with one glBlitFramebuffer; otherwise (GLES 2, multisampled
// window) it is drawn with scissored glClears, one per run of a colour. The GL state touched is
// put back: framebuffer bindings, scissor test and box, colour mask, clear colour, rasterizer
// discard, and for an upload the active unit's 2D texture, the unpack buffer and unpack
// alignment/row length/skips. No shader, buffer or vertex state of Unity's is used.
#include "osk.h"
#include "osk_font.h"
#include "../jni/unityplayer.h"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ---- colours (sRGB) ------------------------------------------------------------------------ */
enum { C_BG, C_KEY, C_KEY2, C_FOCUS, C_PRESS, C_LABEL, C_DIM, C_FIELD, C_TEXT, C_HINT, C_CARET, C_SEL, C_N };
static const unsigned char palette[C_N][3] = {
    { 0x20, 0x23, 0x28 }, { 0x3c, 0x41, 0x49 }, { 0x2b, 0x2f, 0x35 }, { 0x2f, 0x6f, 0xe0 }, { 0x8f, 0xb8, 0xff },
    { 0xf2, 0xf2, 0xf2 }, { 0x86, 0x8c, 0x96 }, { 0xf4, 0xf4, 0xf4 }, { 0x14, 0x14, 0x14 }, { 0x90, 0x90, 0x90 },
    { 0x1a, 0x5f, 0xe0 }, { 0xb4, 0xcf, 0xff } };

/* ---- icons, in the font's 6x13 cell ------------------------------------------------------------ */
enum { I_NONE, I_SHIFT, I_SHIFT_ON, I_CAPS, I_BKSP, I_ENTER, I_LEFT, I_RIGHT, I_N };
static const char *const icons[I_N][OSK_FONT_H] = {
    { 0 },
    { "", "", "..#...", ".#.#..", "#...#.", "##.##.", ".#.#..", ".#.#..", ".#.#..", ".###..", "", "", "" },
    { "", "", "..#...", ".###..", "#####.", "#####.", ".###..", ".###..", ".###..", ".###..", "", "", "" },
    { "", "", "..#...", ".###..", "#####.", "#####.", ".###..", ".###..", ".###..", "", "#####.", "", "" },
    { "", "", "", "", "..#...", ".##...", "######", ".##...", "..#...", "", "", "", "" },
    { "", "", ".....#", ".....#", "..#..#", ".##..#", "######", ".##...", "..#...", "", "", "", "" },
    { "", "", "", "...#..", "..##..", ".###..", "####..", ".###..", "..##..", "...#..", "", "", "" },
    { "", "", "", ".#....", ".##...", ".###..", ".####.", ".###..", ".##...", ".#....", "", "", "" },
};

/* ---- pages of keys ------------------------------------------------------------------------------ */
enum { A_CHAR, A_SHIFT, A_PAGE, A_BKSP, A_ENTER, A_SPACE, A_LEFT, A_RIGHT };
enum { PG_LETTERS, PG_SYMBOLS, PG_NUMBER, PG_PHONE, PG_N };
#define MAX_ROWS 5
#define MAX_COLS 12
struct key { int action, tenths, page; uint32_t cp; const char *label; int icon; int x, y, w, h; };
static struct page { int rows; int n[MAX_ROWS]; struct key k[MAX_ROWS][MAX_COLS]; } pages[PG_N];

static struct key *add(int pg, int row, int action, int tenths, uint32_t cp, const char *label, int icon, int to)
{
    struct page *p = &pages[pg];
    if (row >= p->rows) p->rows = row + 1;
    struct key *k = &p->k[row][p->n[row]++];
    *k = (struct key){ action, tenths, to, cp, label, icon, 0, 0, 0, 0 };
    return k;
}
static void add_chars(int pg, int row, const char *s, int tenths)
{
    for (; *s; s++) add(pg, row, A_CHAR, tenths, (unsigned char)*s, NULL, 0, 0);
}
static void build_pages(void)
{
    memset(pages, 0, sizeof pages);
    add_chars(PG_LETTERS, 0, "1234567890", 10);
    add_chars(PG_LETTERS, 1, "qwertyuiop", 10);
    add_chars(PG_LETTERS, 2, "asdfghjkl'", 10);
    add(PG_LETTERS, 3, A_SHIFT, 15, 0, NULL, I_SHIFT, 0);
    add_chars(PG_LETTERS, 3, "zxcvbnm", 10);
    add(PG_LETTERS, 3, A_BKSP, 15, 0, NULL, I_BKSP, 0);
    add(PG_LETTERS, 4, A_PAGE, 15, 0, "?123", 0, PG_SYMBOLS);
    add(PG_LETTERS, 4, A_CHAR, 10, ',', NULL, 0, 0);          /* '@' or '/' for e-mail / URL fields */
    add(PG_LETTERS, 4, A_SPACE, 50, ' ', "space", 0, 0);
    add(PG_LETTERS, 4, A_CHAR, 10, '.', NULL, 0, 0);
    add(PG_LETTERS, 4, A_ENTER, 15, 0, NULL, I_ENTER, 0);

    add_chars(PG_SYMBOLS, 0, "1234567890", 10);
    add_chars(PG_SYMBOLS, 1, "@#$%&*-+()", 10);
    add_chars(PG_SYMBOLS, 2, "!\"':;/?_=\\", 10);
    add_chars(PG_SYMBOLS, 3, "~^`<>[]{}|", 10);
    add(PG_SYMBOLS, 4, A_PAGE, 15, 0, "ABC", 0, PG_LETTERS);
    add(PG_SYMBOLS, 4, A_CHAR, 10, ',', NULL, 0, 0);
    add(PG_SYMBOLS, 4, A_SPACE, 40, ' ', "space", 0, 0);
    add(PG_SYMBOLS, 4, A_CHAR, 10, '.', NULL, 0, 0);
    add(PG_SYMBOLS, 4, A_BKSP, 10, 0, NULL, I_BKSP, 0);
    add(PG_SYMBOLS, 4, A_ENTER, 15, 0, NULL, I_ENTER, 0);

    add_chars(PG_NUMBER, 0, "123", 25); add(PG_NUMBER, 0, A_BKSP, 25, 0, NULL, I_BKSP, 0);
    add_chars(PG_NUMBER, 1, "456", 25); add_chars(PG_NUMBER, 1, "-", 25);
    add_chars(PG_NUMBER, 2, "789", 25); add_chars(PG_NUMBER, 2, ".", 25);
    add(PG_NUMBER, 3, A_LEFT, 25, 0, NULL, I_LEFT, 0); add_chars(PG_NUMBER, 3, "0", 25);
    add(PG_NUMBER, 3, A_RIGHT, 25, 0, NULL, I_RIGHT, 0); add(PG_NUMBER, 3, A_ENTER, 25, 0, NULL, I_ENTER, 0);

    add_chars(PG_PHONE, 0, "123", 25); add(PG_PHONE, 0, A_BKSP, 25, 0, NULL, I_BKSP, 0);
    add_chars(PG_PHONE, 1, "456", 25); add_chars(PG_PHONE, 1, "+", 25);
    add_chars(PG_PHONE, 2, "789", 25); add(PG_PHONE, 2, A_SPACE, 25, ' ', "space", 0, 0);
    add_chars(PG_PHONE, 3, "*0#", 25); add(PG_PHONE, 3, A_ENTER, 25, 0, NULL, I_ENTER, 0);
}

/* ---- configuration and state --------------------------------------------------------------------- */
enum { M_ON, M_OFF, M_DONE, M_CANCEL };
static struct {
    int mode, W, H, s, hs, in_shots;
    int g, fh, kh, ph_max;                  /* gap, field height, key height, tallest panel */
} P;

static struct up_soft_input fs;             /* the field, as last read (UI thread) */
static struct {
    unsigned serial;
    int active, page, row, col;
    int shift_once, caps, auto_off, pressing;
    int scroll;                             /* first code point shown in the field */
    unsigned char owned[256];               /* pad buttons pressed while open: their release is ours too */
    int dir[8][2], cur_dx, cur_dy;          /* per-pad direction (d-pad / stick), the one in effect */
    double next_move, next_rep;
    int rep_key;                            /* B / L1 / R1 / a DPAD key held: repeats */
    int dirty;
    int field_open, edge;                   /* the field's state as last seen; opened/closed since the last osk_tick */
} K;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* the picture: an indexed image of the panel, written on the UI thread, read on the render thread */
static pthread_mutex_t img_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *img;                  /* P.W x P.ph_max */
static int img_h, img_gen, shown;           /* height in use, generation, drawn at all */

/* ---- layout ---------------------------------------------------------------------------------------- */
static int field_shown(void) { return !fs.hide_input; }
static int panel_h(int pg)
{
    int rows = pages[pg].rows;
    int h = P.g + rows * P.kh + (rows - 1) * P.g + P.g + OSK_FONT_H * P.hs + P.g;
    if (field_shown()) h += P.fh + P.g;
    return h;
}
static int keys_top(void) { return P.g + (field_shown() ? P.fh + P.g : 0); }
static void layout(int pg)
{
    struct page *p = &pages[pg];
    int top = keys_top(), avail = P.W - P.g;
    for (int r = 0; r < p->rows; r++) {
        int u = 0;
        for (int i = 0; i < p->n[r]; i++) {
            struct key *k = &p->k[r][i];
            int x0 = P.g + u * avail / 100, x1 = P.g + (u + k->tenths) * avail / 100 - P.g;
            k->x = x0; k->w = x1 - x0; k->y = top + r * (P.kh + P.g); k->h = P.kh;
            u += k->tenths;
        }
    }
}

void osk_init(int width, int height)
{
    P.W = width > 0 ? width : 640; P.H = height > 0 ? height : 480;
    const char *m = getenv("UA_OSK");
    P.mode = !m || !*m || !strcmp(m, "1") ? M_ON : !strcasecmp(m, "done") ? M_DONE : !strcasecmp(m, "cancel") ? M_CANCEL : M_OFF;
    P.s = getenv("UA_OSK_SCALE") ? atoi(getenv("UA_OSK_SCALE")) : (P.H + 120) / 240;
    if (P.s < 1) P.s = 1;
    if (P.s > 8) P.s = 8;
    while (P.s > 1 && 10 * (OSK_FONT_W * P.s + 4 * P.s) > P.W) P.s--;   /* ten keys must fit a row */
    P.hs = P.s > 2 ? P.s / 2 : 1;
    P.g = P.s;
    P.fh = (OSK_FONT_H + 4) * P.s;
    P.kh = (OSK_FONT_H + 3) * P.s;
    P.in_shots = !(getenv("UA_OSK_SHOTS") && !strcmp(getenv("UA_OSK_SHOTS"), "0"));
    build_pages();
    fs.hide_input = 0;
    P.ph_max = 0;
    for (int pg = 0; pg < PG_N; pg++) if (panel_h(pg) > P.ph_max) P.ph_max = panel_h(pg);
    if (P.ph_max > P.H) P.ph_max = P.H;
    int ph = panel_h(PG_LETTERS);
    up_soft_input_set_area(0, P.H - ph, P.W, ph);        /* TouchScreenKeyboard.area */
}

int osk_active(void) { return K.active; }
int osk_typing(void) { return fs.active; }
int osk_in_shots(void) { return P.in_shots; }

/* ---- drawing into the image (UI thread) ---------------------------------------------------------------- */
static void fill(int x, int y, int w, int h, int c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > P.W) w = P.W - x;
    if (y + h > img_h) h = img_h - y;
    for (int j = 0; j < h; j++) memset(img + (size_t)(y + j) * P.W + x, c, w > 0 ? (size_t)w : 0);
}
static void glyph(int x, int y, uint32_t cp, int icon, int s, int c, int clip_x1)
{
    for (int r = 0; r < OSK_FONT_H; r++) {
        unsigned bits = 0;
        if (icon) { const char *row = icons[icon][r]; for (int i = 0; row && row[i] && i < 8; i++) if (row[i] == '#') bits |= 0x80 >> i; }
        else {
            int gi = cp >= 32 && cp <= 126 ? (int)cp - 32 : cp >= 160 && cp <= 255 ? (int)cp - 160 + 95 : '?' - 32;
            bits = osk_font[gi][r];
        }
        for (int i = 0; i < OSK_FONT_W; i++)
            if (bits & (0x80 >> i)) {
                int px = x + i * s;
                if (px + s <= clip_x1) fill(px, y + r * s, s, s, c);
            }
    }
}
static void text(int x, int y, const char *t, int s, int c)     /* UTF-8 */
{
    const unsigned char *p = (const unsigned char *)t;
    for (; *p; x += OSK_FONT_W * s) {
        uint32_t cp = *p++;
        int extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
        if (extra) cp &= 0x3f >> extra;
        for (; extra && (*p & 0xc0) == 0x80; extra--) cp = cp << 6 | (*p++ & 0x3f);
        glyph(x, y, cp, 0, s, c, P.W);
    }
}

static int cls(void) { return fs.input_type & 0xf; }
static int variation(void) { return fs.input_type & 0xff0; }
static int shift_state(void);
static uint32_t key_char(const struct key *k)
{
    uint32_t c = k->cp;
    if (k == &pages[PG_LETTERS].k[4][1]) {                    /* the key left of space */
        if (cls() == 1 && variation() == 0x20) c = '@';       /* TYPE_TEXT_VARIATION_EMAIL_ADDRESS */
        else if (cls() == 1 && variation() == 0x10) c = '/';  /* TYPE_TEXT_VARIATION_URI */
    }
    if (c >= 'a' && c <= 'z' && shift_state()) c -= 32;
    return c;
}
static int key_enabled(const struct key *k)          /* the number pad's sign and point */
{
    if (cls() != 2 || k->action != A_CHAR) return 1;
    if (k->cp == '-') return (fs.input_type & 0x1000) != 0;
    if (k->cp == '.') return (fs.input_type & 0x2000) != 0;
    return 1;
}

static void render(void)
{
    if (!img) return;
    int pg = K.page, ph = panel_h(pg);
    if (ph > P.ph_max) ph = P.ph_max;
    pthread_mutex_lock(&img_mu);
    img_h = ph;
    layout(pg);
    fill(0, 0, P.W, ph, C_BG);
    int s = P.s;
    if (field_shown()) {                               /* the text field */
        int fx = P.g, fy = P.g, fw = P.W - 2 * P.g, pad = 2 * s, ty = fy + (P.fh - OSK_FONT_H * s) / 2;
        fill(fx, fy, fw, P.fh, C_FIELD);
        int cols = (fw - 2 * pad) / (OSK_FONT_W * s);
        if (cols < 1) cols = 1;
        int caret = fs.sel_end;
        if (caret < K.scroll) K.scroll = caret;
        if (caret > K.scroll + cols - 1) K.scroll = caret - cols + 1;
        if (K.scroll > fs.len) K.scroll = fs.len;
        if (K.scroll < 0) K.scroll = 0;
        int x1 = fx + fw - pad;
        if (!fs.len && fs.placeholder[0]) text(fx + pad, ty, fs.placeholder, s, C_HINT);
        for (int i = K.scroll; i < fs.len && i < K.scroll + cols; i++) {
            int x = fx + pad + (i - K.scroll) * OSK_FONT_W * s;
            if (i >= fs.sel_start && i < fs.sel_end) fill(x, ty, OSK_FONT_W * s, OSK_FONT_H * s, C_SEL);
            uint32_t c = fs.secure ? '*' : fs.text[i] == '\n' ? 0xb6 : fs.text[i] == '\t' ? ' ' : fs.text[i];
            glyph(x, ty, c, 0, s, C_TEXT, x1);
        }
        if (fs.sel_start == fs.sel_end) {
            int x = fx + pad + (caret - K.scroll) * OSK_FONT_W * s;
            fill(x > fx + 1 ? x - 1 : x, ty, s > 1 ? s : 1, OSK_FONT_H * s, C_CARET);
        }
    }
    struct page *p = &pages[pg];
    for (int r = 0; r < p->rows; r++)
        for (int i = 0; i < p->n[r]; i++) {
            struct key *k = &p->k[r][i];
            int focus = r == K.row && i == K.col;
            int bg = focus ? (K.pressing ? C_PRESS : C_FOCUS) : k->action == A_CHAR ? C_KEY : C_KEY2;
            fill(k->x, k->y, k->w, k->h, bg);
            int fg = key_enabled(k) ? C_LABEL : C_DIM;
            int ly = k->y + (k->h - OSK_FONT_H * s) / 2;
            if (k->action == A_SHIFT) {
                int st = shift_state();
                glyph(k->x + (k->w - OSK_FONT_W * s) / 2, ly, 0, st == 2 ? I_CAPS : st ? I_SHIFT_ON : I_SHIFT, s, fg, P.W);
            } else if (k->icon) {
                glyph(k->x + (k->w - OSK_FONT_W * s) / 2, ly, 0, k->icon, s, fg, P.W);
            } else if (k->label) {
                int tw = (int)strlen(k->label) * OSK_FONT_W * s;
                text(k->x + (k->w - tw) / 2, ly, k->label, s, fg);
            } else {
                glyph(k->x + (k->w - OSK_FONT_W * s) / 2, ly, key_char(k), 0, s, fg, P.W);
            }
        }
    const char *help = K.page == PG_NUMBER || K.page == PG_PHONE
        ? "A type  B delete  L1/R1 move  START done  SELECT cancel"
        : "A type  B delete  X space  Y shift  L1/R1 move  START done  SELECT cancel";
    int hw = (int)strlen(help) * OSK_FONT_W * P.hs;
    int hy = keys_top() + p->rows * P.kh + (p->rows - 1) * P.g + P.g;
    if (hw <= P.W) text((P.W - hw) / 2, hy, help, P.hs, C_DIM);
    img_gen++;
    pthread_mutex_unlock(&img_mu);
}

/* ---- typing (UI thread) --------------------------------------------------------------------------------- */
static void sync(void)                      /* re-read the field after a change */
{
    up_soft_input_get(&fs);
    K.serial = fs.serial;
    K.dirty = 1;
}
static int auto_cap(void)
{
    int t = fs.input_type, c = fs.sel_start;
    if ((t & 0xf) != 1 || fs.secure) return 0;
    if (t & 0x1000) return 1;                                   /* CAP_CHARACTERS */
    if (t & 0x2000) return c == 0 || fs.text[c - 1] == ' ' || fs.text[c - 1] == '\n';   /* CAP_WORDS */
    if (t & 0x4000) {                                           /* CAP_SENTENCES */
        int i = c;
        while (i > 0 && fs.text[i - 1] == ' ') i--;
        return i == 0 || fs.text[i - 1] == '.' || fs.text[i - 1] == '!' || fs.text[i - 1] == '?' || fs.text[i - 1] == '\n';
    }
    return 0;
}
static int shift_state(void) { return K.caps ? 2 : K.shift_once || (!K.auto_off && auto_cap()) ? 1 : 0; }

static void commit_cp(uint32_t c)
{
    char u[5] = { 0 };
    if (c < 0x80) u[0] = (char)c;
    else if (c < 0x800) { u[0] = (char)(0xc0 | c >> 6); u[1] = (char)(0x80 | (c & 0x3f)); }
    else { u[0] = (char)(0xe0 | c >> 12); u[1] = (char)(0x80 | (c >> 6 & 0x3f)); u[2] = (char)(0x80 | (c & 0x3f)); }
    up_soft_input_commit(u);
    K.shift_once = 0; K.auto_off = 0;
    sync();
}
static void caret(int d)
{
    int c = (d < 0 ? fs.sel_start : fs.sel_end) + (fs.sel_start == fs.sel_end ? d : 0);
    up_soft_input_select(c, c);
    sync();
}
static void backspace(void) { up_soft_input_delete(1, 0); K.auto_off = 0; sync(); }
static void finish(int how) { up_soft_input_finish(how); sync(); }
static void enter(void)
{
    if (fs.multiline) commit_cp('\n');
    else finish(UP_SOFT_INPUT_DONE);
}

static void press(void)
{
    struct page *p = &pages[K.page];
    if (K.row >= p->rows || K.col >= p->n[K.row]) return;
    struct key *k = &p->k[K.row][K.col];
    switch (k->action) {
    case A_CHAR: if (key_enabled(k)) commit_cp(key_char(k)); break;
    case A_SPACE: commit_cp(' '); break;
    case A_SHIFT: { int st = (shift_state() + 1) % 3; K.caps = st == 2; K.shift_once = st == 1; K.auto_off = st == 0; K.dirty = 1; break; }
    case A_PAGE: {
        int x = k->x + k->w / 2;
        K.page = k->page;
        layout(K.page);
        struct page *q = &pages[K.page];
        if (K.row >= q->rows) K.row = q->rows - 1;
        int best = 0;
        for (int i = 0; i < q->n[K.row]; i++)
            if (abs(q->k[K.row][i].x + q->k[K.row][i].w / 2 - x) < abs(q->k[K.row][best].x + q->k[K.row][best].w / 2 - x)) best = i;
        K.col = best;
        K.dirty = 1;
        break;
    }
    case A_BKSP: backspace(); break;
    case A_ENTER: enter(); break;
    case A_LEFT: caret(-1); break;
    case A_RIGHT: caret(1); break;
    }
}

static void move(int dx, int dy)
{
    struct page *p = &pages[K.page];
    layout(K.page);
    if (dy) {
        struct key *cur = &p->k[K.row][K.col];
        int x = cur->x + cur->w / 2;
        K.row = (K.row + dy + p->rows) % p->rows;
        int best = 0;
        for (int i = 0; i < p->n[K.row]; i++)
            if (abs(p->k[K.row][i].x + p->k[K.row][i].w / 2 - x) < abs(p->k[K.row][best].x + p->k[K.row][best].w / 2 - x)) best = i;
        K.col = best;
    }
    if (dx) K.col = (K.col + dx + p->n[K.row]) % p->n[K.row];
    K.dirty = 1;
}

static void open_kb(void)
{
    K.active = 1;
    K.page = cls() == 2 ? PG_NUMBER : cls() == 3 ? PG_PHONE : PG_LETTERS;
    K.row = K.page == PG_LETTERS ? 1 : 0;
    K.col = 0;
    K.shift_once = K.caps = K.auto_off = K.pressing = 0;
    K.scroll = 0;
    K.rep_key = 0;
    memset(K.dir, 0, sizeof K.dir);
    K.cur_dx = K.cur_dy = 0;
    if (!img) img = calloc((size_t)P.W * P.ph_max, 1);
    fprintf(stderr, "[osk] keyboard open (keyboard type %d, %s page%s%s)\n", fs.kb_type,
            K.page == PG_NUMBER ? "number" : K.page == PG_PHONE ? "phone" : "letters",
            fs.multiline ? ", multi-line" : "", fs.secure ? ", password" : "");
    pthread_mutex_lock(&img_mu);
    shown = 1;
    pthread_mutex_unlock(&img_mu);
    K.dirty = 1;
}
static void close_kb(void)
{
    K.active = 0;
    K.pressing = 0;
    K.rep_key = 0;
    memset(K.dir, 0, sizeof K.dir);
    pthread_mutex_lock(&img_mu);
    shown = 0;
    pthread_mutex_unlock(&img_mu);
    fprintf(stderr, "[osk] keyboard closed\n");
}

static void tick(void)
{
    if (up_soft_input_serial() != K.serial) sync();
    if (fs.active != K.field_open) {                /* the game opened or closed its keyboard */
        K.field_open = fs.active;
        if (fs.active) {
            if (P.mode == M_DONE || P.mode == M_CANCEL) {
                fprintf(stderr, "[osk] keyboard answered at once (UA_OSK=%s)\n", P.mode == M_DONE ? "done" : "cancel");
                finish(P.mode == M_DONE ? UP_SOFT_INPUT_DONE : UP_SOFT_INPUT_CANCEL);
                return;
            }
            if (P.mode == M_OFF) return;
            open_kb();
            K.edge = 1;
        } else if (K.active) { close_kb(); K.edge = 1; }
    }
    if (!K.active) return;
    double t = now_s();
    /* the d-pad / stick: a step at once, then repeats */
    int dx = 0, dy = 0;
    for (int i = 0; i < 8 && !dx && !dy; i++) { dx = K.dir[i][0]; dy = K.dir[i][1]; }
    if (dx != K.cur_dx || dy != K.cur_dy) {
        K.cur_dx = dx; K.cur_dy = dy;
        if (dx || dy) { move(dx, dy); K.next_move = t + 0.4; }
    } else if ((dx || dy) && t >= K.next_move) { move(dx, dy); K.next_move = t + 0.09; }
    if (K.rep_key && t >= K.next_rep) {
        K.next_rep = t + 0.07;
        switch (K.rep_key) {
        case UP_KEYCODE_BUTTON_B: backspace(); break;
        case UP_KEYCODE_BUTTON_L1: caret(-1); break;
        case UP_KEYCODE_BUTTON_R1: caret(1); break;
        case UP_KEYCODE_DPAD_UP: move(0, -1); break;
        case UP_KEYCODE_DPAD_DOWN: move(0, 1); break;
        case UP_KEYCODE_DPAD_LEFT: move(-1, 0); break;
        case UP_KEYCODE_DPAD_RIGHT: move(1, 0); break;
        }
    }
    if (K.dirty && K.active) { K.dirty = 0; render(); }
}

int osk_tick(void)
{
    tick();
    int e = K.edge;
    K.edge = 0;
    return e;
}

/* ---- pad and keyboard input (UI thread) ----------------------------------------------------------------- */
static void start_repeat(int key) { K.rep_key = key; K.next_rep = now_s() + 0.4; }

int osk_key(int keycode, int down)
{
    if (keycode <= 0 || keycode > 255) return 0;
    if (!down) {
        if (!K.owned[keycode]) return 0;
        K.owned[keycode] = 0;
        if (keycode == UP_KEYCODE_BUTTON_A && K.pressing) { K.pressing = 0; K.dirty = 1; }
        if (K.rep_key == keycode) K.rep_key = 0;
        tick();
        return 1;
    }
    if (!K.active) return 0;
    K.owned[keycode] = 1;
    switch (keycode) {
    case UP_KEYCODE_BUTTON_A: K.pressing = 1; press(); break;
    case UP_KEYCODE_BUTTON_B: backspace(); start_repeat(keycode); break;
    case UP_KEYCODE_BUTTON_X: commit_cp(' '); break;
    case UP_KEYCODE_BUTTON_Y: {
        int st = (shift_state() + 1) % 3;
        K.caps = st == 2; K.shift_once = st == 1; K.auto_off = st == 0; K.dirty = 1;
        break;
    }
    case UP_KEYCODE_BUTTON_L1: caret(-1); start_repeat(keycode); break;
    case UP_KEYCODE_BUTTON_R1: caret(1); start_repeat(keycode); break;
    case UP_KEYCODE_BUTTON_START: finish(UP_SOFT_INPUT_DONE); break;
    case UP_KEYCODE_BUTTON_SELECT: case UP_KEYCODE_BACK: finish(UP_SOFT_INPUT_CANCEL); break;
    case UP_KEYCODE_DPAD_UP: move(0, -1); start_repeat(keycode); break;
    case UP_KEYCODE_DPAD_DOWN: move(0, 1); start_repeat(keycode); break;
    case UP_KEYCODE_DPAD_LEFT: move(-1, 0); start_repeat(keycode); break;
    case UP_KEYCODE_DPAD_RIGHT: move(1, 0); start_repeat(keycode); break;
    case UP_KEYCODE_DPAD_CENTER: case UP_KEYCODE_ENTER: K.pressing = 1; press(); break;
    default: break;                         /* the rest of the pad does nothing while typing */
    }
    tick();
    return 1;
}

void osk_axes(int slot, float *v)
{
    if (slot < 0 || slot >= 8) return;
    if (!K.active) { K.dir[slot][0] = K.dir[slot][1] = 0; return; }
    int dx = v[4] > 0.5f ? 1 : v[4] < -0.5f ? -1 : 0, dy = v[5] > 0.5f ? 1 : v[5] < -0.5f ? -1 : 0;
    if (!dx && !dy) {                        /* the left stick, its larger axis */
        float ax = fabsf(v[0]), ay = fabsf(v[1]);
        if (ax > 0.5f && ax >= ay) dx = v[0] > 0 ? 1 : -1;
        else if (ay > 0.5f) dy = v[1] > 0 ? 1 : -1;
    }
    K.dir[slot][0] = dx; K.dir[slot][1] = dy;
    for (int i = 0; i < 8; i++) v[i] = 0;    /* the game sees a centred pad while typing */
}

void osk_hw_key(int keycode, int down)
{
    if (!down || !up_soft_input_serial()) return;
    up_soft_input_get(&fs);
    if (!fs.active) return;
    switch (keycode) {
    case 67: backspace(); break;                                          /* KEYCODE_DEL */
    case 112: up_soft_input_delete(0, 1); sync(); break;                  /* KEYCODE_FORWARD_DEL */
    case UP_KEYCODE_DPAD_LEFT: caret(-1); break;
    case UP_KEYCODE_DPAD_RIGHT: caret(1); break;
    case 122: up_soft_input_select(0, 0); sync(); break;                  /* KEYCODE_MOVE_HOME */
    case 123: up_soft_input_select(fs.len, fs.len); sync(); break;        /* KEYCODE_MOVE_END */
    case UP_KEYCODE_ENTER: case 160: enter(); break;                      /* KEYCODE_NUMPAD_ENTER */
    case UP_KEYCODE_ESCAPE: finish(UP_SOFT_INPUT_CANCEL); break;
    }
    tick();
}

void osk_hw_text(const char *utf8)
{
    if (!utf8 || !*utf8 || !up_soft_input_serial()) return;
    up_soft_input_commit(utf8);
    K.shift_once = 0; K.auto_off = 0;
    sync();
    tick();
}

/* ---- drawing (render thread, Unity's context current, right before the swap) --------------------------- */
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_NEAREST 0x2600
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define GL_SCISSOR_BOX 0x0C10
#define GL_SCISSOR_TEST 0x0C11
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING 0x8210
#define GL_SRGB 0x8C40
#define GL_BACK 0x0405
#define GL_SAMPLE_BUFFERS 0x80A8
#define GL_RASTERIZER_DISCARD 0x8C89
#define GL_VERSION 0x1F02
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
    const unsigned char *(*GetString)(unsigned);
    /* GLES 3 copy */
    void (*GenTextures)(int, unsigned *);
    void (*BindTexture)(unsigned, unsigned);
    void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (*TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (*TexParameteri)(unsigned, unsigned, int);
    void (*PixelStorei)(unsigned, int);
    void (*BindBuffer)(unsigned, unsigned);
    void (*GenFramebuffers)(int, unsigned *);
    void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    unsigned (*CheckFramebufferStatus)(unsigned);
    void (*BlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
    void (*GetFramebufferAttachmentParameteriv)(unsigned, unsigned, unsigned, int *);
} gl;
static int gl_state;                        /* 0 unresolved, 1 ok, -1 missing */
static int gl_es2, gl_copy, gl_blit = -1, gl_srgb;   /* GLES 2, copy functions, copy path (-1 undecided), sRGB window */
static unsigned tex, fbo;
static int drawn_gen = -1;
static struct rect { short x, y, w, h; unsigned char c; } *rects;
static int nrects, maxrects;
static unsigned char *rgba, *img_rt;        /* the render thread's copy of the picture, its RGBA */
static int img_rt_h;

static int gl_resolve(void)
{
    if (gl_state) return gl_state > 0;
    void *h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
#define G(n) (*(void **)&gl.n = h ? dlsym(h, "gl" #n) : NULL, gl.n != NULL)
    int ok = G(Enable) & G(Disable) & G(IsEnabled) & G(Scissor) & G(ClearColor) & G(Clear) & G(GetIntegerv) &
             G(GetFloatv) & G(GetBooleanv) & G(ColorMask) & G(BindFramebuffer) & G(GetString);
    int copy = G(GenTextures) & G(BindTexture) & G(TexImage2D) & G(TexSubImage2D) & G(TexParameteri) & G(PixelStorei) &
               G(BindBuffer) & G(GenFramebuffers) & G(FramebufferTexture2D) & G(CheckFramebufferStatus) &
               G(BlitFramebuffer) & G(GetFramebufferAttachmentParameteriv);
#undef G
    gl_state = ok ? 1 : -1;
    if (!ok) { fprintf(stderr, "[osk] no GLES functions: the keyboard is not drawn\n"); return 0; }
    const char *v = (const char *)gl.GetString(GL_VERSION);
    gl_es2 = !v || strstr(v, "OpenGL ES 2") != NULL;
    gl_copy = copy && !gl_es2;
    return 1;
}

static float lin(unsigned char c) { float x = c / 255.f; return x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f); }

static void rebuild_rects(void)             /* runs of one colour per row, merged with the rows below */
{
    nrects = 0;
    for (int y = 0; y < img_rt_h; y++) {
        const unsigned char *row = img_rt + (size_t)y * P.W;
        for (int x = 0; x < P.W; ) {
            int c = row[x], l = 1;
            while (x + l < P.W && row[x + l] == c) l++;
            int merged = 0;
            for (int i = nrects - 1; i >= 0 && !merged && rects[i].y + rects[i].h >= y; i--)
                if (rects[i].x == x && rects[i].w == l && rects[i].c == c && rects[i].y + rects[i].h == y) { rects[i].h++; merged = 1; }
            if (!merged) {
                if (nrects == maxrects) { maxrects = maxrects ? maxrects * 2 : 1024; rects = realloc(rects, sizeof *rects * maxrects); }
                rects[nrects++] = (struct rect){ (short)x, (short)y, (short)l, 1, (unsigned char)c };
            }
            x += l;
        }
    }
}

static int setup_copy(void)                 /* texture + framebuffer of our own (read binding only) */
{
    int prev_tex = 0;
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    gl.GenTextures(1, &tex);
    gl.BindTexture(GL_TEXTURE_2D, tex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, gl_srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, P.W, P.ph_max, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.BindTexture(GL_TEXTURE_2D, (unsigned)prev_tex);
    gl.GenFramebuffers(1, &fbo);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return gl.CheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static void upload(void)
{
    if (!rgba) rgba = malloc((size_t)P.W * P.ph_max * 4);
    for (int y = 0; y < img_rt_h; y++) {                   /* GL rows count from the bottom */
        const unsigned char *src = img_rt + (size_t)y * P.W;
        unsigned char *dst = rgba + (size_t)(img_rt_h - 1 - y) * P.W * 4;
        for (int x = 0; x < P.W; x++) {
            const unsigned char *c = palette[src[x]];
            dst[4 * x] = c[0]; dst[4 * x + 1] = c[1]; dst[4 * x + 2] = c[2]; dst[4 * x + 3] = 255;
        }
    }
    int prev_tex = 0, pbo = 0, align = 4, rowlen = 0, skiprows = 0, skippix = 0;
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &rowlen);
    gl.GetIntegerv(GL_UNPACK_SKIP_ROWS, &skiprows);
    gl.GetIntegerv(GL_UNPACK_SKIP_PIXELS, &skippix);
    if (pbo) gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (align != 4) gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (rowlen) gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (skiprows) gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    if (skippix) gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    gl.BindTexture(GL_TEXTURE_2D, tex);
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, P.W, img_rt_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    gl.BindTexture(GL_TEXTURE_2D, (unsigned)prev_tex);
    if (skippix) gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, skippix);
    if (skiprows) gl.PixelStorei(GL_UNPACK_SKIP_ROWS, skiprows);
    if (rowlen) gl.PixelStorei(GL_UNPACK_ROW_LENGTH, rowlen);
    if (align != 4) gl.PixelStorei(GL_UNPACK_ALIGNMENT, align);
    if (pbo) gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, (unsigned)pbo);
}

void osk_draw(void)
{
    pthread_mutex_lock(&img_mu);
    int on = shown && img_h > 0 && img_gen > 0;
    pthread_mutex_unlock(&img_mu);
    if (!on || P.mode != M_ON || !gl_resolve()) return;
    int dfb = 0, rfb = 0, box[4];
    float cc[4];
    unsigned char mask[4];
    unsigned dtarget = gl_es2 ? GL_FRAMEBUFFER : GL_DRAW_FRAMEBUFFER;
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
    if (!gl_es2) gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    gl.GetIntegerv(GL_SCISSOR_BOX, box);
    gl.GetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    gl.GetBooleanv(GL_COLOR_WRITEMASK, mask);
    unsigned char sc = gl.IsEnabled(GL_SCISSOR_TEST), rd = gl_es2 ? 0 : gl.IsEnabled(GL_RASTERIZER_DISCARD);
    if (dfb) gl.BindFramebuffer(dtarget, 0);
    if (rd) gl.Disable(GL_RASTERIZER_DISCARD);
    gl.ColorMask(1, 1, 1, 1);
    if (gl_blit < 0) {                                    /* first time: which way, what encoding */
        int samples = 0, enc = 0;
        if (!gl_es2) {
            gl.GetIntegerv(GL_SAMPLE_BUFFERS, &samples);
            if (gl.GetFramebufferAttachmentParameteriv)
                gl.GetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_BACK, GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &enc);
        }
        gl_srgb = enc == GL_SRGB;
        gl_blit = gl_copy && samples == 0 && setup_copy();
        if (gl_copy) gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)rfb);
        fprintf(stderr, "[osk] drawn with %s%s\n", gl_blit ? "a framebuffer copy" : "scissored clears",
                gl_srgb ? " (sRGB window)" : "");
    }
    int changed = 0;
    pthread_mutex_lock(&img_mu);                          /* take a copy: the UI thread draws on */
    if (img_gen != drawn_gen) {
        if (!img_rt) img_rt = malloc((size_t)P.W * P.ph_max);
        memcpy(img_rt, img, (size_t)P.W * img_h);
        img_rt_h = img_h;
        drawn_gen = img_gen;
        changed = 1;
    }
    pthread_mutex_unlock(&img_mu);
    if (changed) { if (gl_blit) upload(); else rebuild_rects(); }
    int h = img_rt_h;
    if (gl_blit) {
        if (sc) gl.Disable(GL_SCISSOR_TEST);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        gl.BlitFramebuffer(0, 0, P.W, h, 0, 0, P.W, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)rfb);
        if (sc) gl.Enable(GL_SCISSOR_TEST);
    } else {
        if (!sc) gl.Enable(GL_SCISSOR_TEST);
        int last = -1;
        for (int i = 0; i < nrects; i++) {
            const struct rect *r = &rects[i];
            if (r->c != last) {
                const unsigned char *c = palette[r->c];
                if (gl_srgb) gl.ClearColor(lin(c[0]), lin(c[1]), lin(c[2]), 1);
                else gl.ClearColor(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, 1);
                last = r->c;
            }
            gl.Scissor(r->x, h - r->y - r->h, r->w, r->h);
            gl.Clear(GL_COLOR_BUFFER_BIT);
        }
        gl.ClearColor(cc[0], cc[1], cc[2], cc[3]);
        gl.Scissor(box[0], box[1], box[2], box[3]);
        if (!sc) gl.Disable(GL_SCISSOR_TEST);
    }
    gl.ColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (rd) gl.Enable(GL_RASTERIZER_DISCARD);
    if (dfb) gl.BindFramebuffer(dtarget, (unsigned)dfb);
}
