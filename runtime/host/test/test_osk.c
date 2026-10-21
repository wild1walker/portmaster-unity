// osk.c's unit test: the on-screen keyboard against a stand-in of the runtime's soft keyboard
// field (../jni/up_softinput.c: here a plain text buffer that records what the keyboard did) and
// fake_gles.c as libGLESv2.so.2. One scenario per process: test_osk <scenario>.
//   off     no keyboard ever opened: nothing taken, nothing drawn, no GL call
//   type    the pad types: focus moves (d-pad, stick, DPAD keys), A/B/X/Y/L1/R1/Start, shift and
//           caps, the symbols page, Enter, repeats; buttons held from before pass through
//   draw    the picture (GLES 3 copy path): panel at the bottom, focus, field text; Unity's GL state kept
//   es2     the same with scissored clears on a GLES 2 context; msaa: on a multisampled window
//   pads    number / phone pads, password field, auto-capitalisation, a hidden field
//   modes   UA_OSK=done / cancel / 0, a hardware keyboard
#include "../osk.h"
#include "../../jni/unityplayer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

extern unsigned char fake_fb0[480][640][4];
extern int fake_calls, fake_clears, fake_bad_scissor, fake_errors, fake_scissor_on, fake_discard_on, fake_box[4], fake_draw_fb;
extern float fake_cc[4];
extern unsigned char fake_mask[4];
extern int fake_tex_binding, fake_read_fb, fake_pbo, fake_unpack[4], fake_blits, fake_bad_blit, fake_uploads, fake_tex_format;

/* ---- the field (stand-in for up_softinput.c) ------------------------------------------------ */
static struct up_soft_input F;
static int area[4], finished = -1, commits;
unsigned up_soft_input_serial(void) { return F.serial; }
unsigned up_soft_input_get(struct up_soft_input *s) { *s = F; return F.serial; }
int up_soft_input_commit(const char *u)
{
    if (!F.active) return 0;
    int n = 0;
    memmove(F.text + F.sel_start + strlen(u), F.text + F.sel_end, (size_t)(F.len - F.sel_end) * 4);
    F.len -= F.sel_end - F.sel_start;
    for (; *u; u++, n++) F.text[F.sel_start + n] = (unsigned char)*u;     /* ASCII here */
    F.len += n;
    F.sel_start = F.sel_end = F.sel_start + n;
    F.serial++;
    commits++;
    return n;
}
void up_soft_input_delete(int before, int after)
{
    if (!F.active) return;
    if (F.sel_start == F.sel_end) { F.sel_start -= before; F.sel_end += after; }
    if (F.sel_start < 0) F.sel_start = 0;
    if (F.sel_end > F.len) F.sel_end = F.len;
    memmove(F.text + F.sel_start, F.text + F.sel_end, (size_t)(F.len - F.sel_end) * 4);
    F.len -= F.sel_end - F.sel_start;
    F.sel_end = F.sel_start;
    F.serial++;
}
void up_soft_input_select(int a, int b)
{
    if (a < 0) a = 0;
    if (b > F.len) b = F.len;
    F.sel_start = a; F.sel_end = b; F.serial++;
}
void up_soft_input_finish(int how) { if (F.active) { finished = how; F.active = 0; F.serial++; } }
void up_soft_input_set_area(int x, int y, int w, int h) { area[0] = x; area[1] = y; area[2] = w; area[3] = h; }

static void open_field(const char *text, int kb_type, int input_type)
{
    memset(&F.text, 0, sizeof F.text);
    F.active = 1; F.kb_type = kb_type; F.input_type = input_type;
    F.multiline = (input_type & 0x20000) != 0; F.secure = (input_type & 0x80) != 0 || (input_type & 0xff0) == 0x10;
    F.len = (int)strlen(text);
    for (int i = 0; i < F.len; i++) F.text[i] = (unsigned char)text[i];
    F.sel_start = F.sel_end = F.len;
    snprintf(F.placeholder, sizeof F.placeholder, "Name");
    F.serial++;
    finished = -1;
}
static int text_is(const char *want)
{
    if (F.len != (int)strlen(want)) return 0;
    for (int i = 0; i < F.len; i++) if (F.text[i] != (unsigned char)want[i]) return 0;
    return 1;
}
static void show_text(void) { printf("  text: \""); for (int i = 0; i < F.len; i++) putchar((int)F.text[i]); printf("\" caret %d\n", F.sel_end); }

/* ---- helpers -------------------------------------------------------------------------------- */
static void tap(int key) { osk_key(key, 1); osk_tick(); osk_key(key, 0); osk_tick(); }
static void taps(int key, int n) { for (int i = 0; i < n; i++) tap(key); }
static float pad[8];
static void hat(float hx, float hy) { memset(pad, 0, sizeof pad); pad[4] = hx; pad[5] = hy; pad[0] = 0.2f; pad[6] = 0.7f; osk_axes(0, pad); osk_tick(); }
static void sleep_ticks(double secs) { for (double t = 0; t < secs; t += 0.01) { osk_tick(); usleep(10000); } }
static const unsigned char *px(int x, int y) { return fake_fb0[479 - y][x]; }   // y from the top
static int is(const unsigned char *p, int r, int g, int b) { return p[0] == r && p[1] == g && p[2] == b; }
static void dump(const char *name)          /* OSK_DUMP=<dir>: the fake window as a PPM, to look at */
{
    const char *d = getenv("OSK_DUMP");
    if (!d) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", d, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n640 480\n255\n");
    for (int y = 0; y < 480; y++) for (int x = 0; x < 640; x++) fwrite(px(x, y), 1, 3, f);
    fclose(f);
}
static void unity_state(void)
{
    fake_scissor_on = 0; fake_discard_on = !getenv("FAKE_GLES2"); fake_draw_fb = 7; fake_read_fb = getenv("FAKE_GLES2") ? 7 : 6;
    fake_box[0] = 1; fake_box[1] = 2; fake_box[2] = 3; fake_box[3] = 4;
    fake_cc[0] = 0.25f; fake_cc[1] = 0.5f; fake_cc[2] = 0.75f; fake_cc[3] = 1;
    fake_mask[0] = 1; fake_mask[1] = 0; fake_mask[2] = 1; fake_mask[3] = 0;
    fake_tex_binding = 5; fake_pbo = 3; fake_unpack[0] = 1; fake_unpack[1] = 7; fake_unpack[2] = 2; fake_unpack[3] = 3;
    memset(fake_fb0, 128, sizeof fake_fb0);
}
static int unity_state_kept(void)
{
    return !fake_scissor_on && fake_discard_on == !getenv("FAKE_GLES2") && fake_draw_fb == 7 && fake_read_fb == (getenv("FAKE_GLES2") ? 7 : 6) && fake_box[0] == 1 && fake_box[1] == 2 &&
           fake_box[2] == 3 && fake_box[3] == 4 && fake_cc[0] == 0.25f && fake_cc[1] == 0.5f && fake_cc[2] == 0.75f &&
           fake_cc[3] == 1 && fake_mask[0] == 1 && fake_mask[1] == 0 && fake_mask[2] == 1 && fake_mask[3] == 0 &&
           fake_tex_binding == 5 && fake_pbo == 3 && fake_unpack[0] == 1 && fake_unpack[1] == 7 && fake_unpack[2] == 2 && fake_unpack[3] == 3;
}

/* 640x480: glyph scale 2, gap 2, field 34, keys 32 high; letters page 5 rows: panel 223 high */
#define PANEL_TOP (480 - 223)
#define KEY_ROW_Y(r) (PANEL_TOP + 2 + 34 + 2 + (r) * 34)

static void off(void)
{
    osk_init(640, 480);
    CHECK(area[0] == 0 && area[1] == PANEL_TOP && area[2] == 640 && area[3] == 223);
    CHECK(osk_tick() == 0 && !osk_active() && !osk_typing());
    for (int k = 1; k < 256; k++) CHECK(osk_key(k, 1) == 0 && osk_key(k, 0) == 0);
    float v[8] = { 1, -1, 0.5f, 0.5f, 1, 1, 0.9f, 0.9f };
    osk_axes(0, v);
    CHECK(v[0] == 1 && v[1] == -1 && v[4] == 1 && v[6] == 0.9f);          /* the game gets it all */
    osk_hw_text("abc");
    osk_hw_key(67, 1);
    unity_state();
    osk_draw();
    CHECK(fake_calls == 0 && unity_state_kept() && commits == 0);
}

static void type(void)
{
    osk_init(640, 480);
    CHECK(osk_key(UP_KEYCODE_BUTTON_A, 1) == 0);           /* held from before the keyboard opens */
    open_field("ab", 0, 0x8001);
    CHECK(osk_tick() == 1 && osk_active() && osk_typing());
    CHECK(osk_key(UP_KEYCODE_BUTTON_A, 0) == 0);           /* its release goes to the game */
    float v[8] = { 0.9f, 0, 0.5f, 0.5f, 0, 0, 0.9f, 0.9f };
    osk_axes(0, v);
    for (int i = 0; i < 8; i++) CHECK(v[i] == 0);          /* the game sees a centred pad */
    hat(0, 0);
    tap(UP_KEYCODE_BUTTON_A);                               /* focus starts on 'q' */
    CHECK(text_is("abq"));
    hat(1, 0); hat(0, 0); hat(1, 0); hat(0, 0);             /* d-pad right twice: 'e' */
    tap(UP_KEYCODE_BUTTON_A);
    CHECK(text_is("abqe"));
    tap(UP_KEYCODE_DPAD_DOWN);                              /* DPAD keys move too: 'd' (below 'e') */
    tap(UP_KEYCODE_BUTTON_A);
    CHECK(text_is("abqed"));
    memset(pad, 0, sizeof pad); pad[1] = -0.9f; osk_axes(0, pad); osk_tick();   /* stick up: back to 'e' */
    memset(pad, 0, sizeof pad); osk_axes(0, pad); osk_tick();
    tap(UP_KEYCODE_BUTTON_Y);                               /* shift: one capital */
    tap(UP_KEYCODE_BUTTON_A);
    tap(UP_KEYCODE_BUTTON_A);
    CHECK(text_is("abqedEe"));
    taps(UP_KEYCODE_BUTTON_Y, 2);                           /* caps lock */
    tap(UP_KEYCODE_BUTTON_A); tap(UP_KEYCODE_BUTTON_A);
    tap(UP_KEYCODE_BUTTON_Y);                               /* off */
    tap(UP_KEYCODE_BUTTON_A);
    CHECK(text_is("abqedEeEEe"));
    tap(UP_KEYCODE_BUTTON_X);                               /* space */
    taps(UP_KEYCODE_BUTTON_B, 3);                           /* delete */
    CHECK(text_is("abqedEeE"));
    taps(UP_KEYCODE_BUTTON_L1, 2);                          /* caret left, type there */
    tap(UP_KEYCODE_BUTTON_A);
    CHECK(text_is("abqedEeeE") && F.sel_end == 7);
    tap(UP_KEYCODE_BUTTON_R1);
    CHECK(F.sel_end == 8);
    tap(UP_KEYCODE_BUTTON_R1);
    /* the symbols page: down to the bottom row, the ?123 key, then '@' (row 1, first key) */
    taps(UP_KEYCODE_DPAD_DOWN, 3);                          /* 'e' -> 'd' -> 'z' -> ',' */
    tap(UP_KEYCODE_DPAD_LEFT);                              /* ?123 */
    tap(UP_KEYCODE_BUTTON_A);
    taps(UP_KEYCODE_DPAD_UP, 3);
    tap(UP_KEYCODE_BUTTON_A);
    CHECK(text_is("abqedEeeE@"));
    taps(UP_KEYCODE_DPAD_DOWN, 3);                          /* back: ABC */
    tap(UP_KEYCODE_BUTTON_A);
    taps(UP_KEYCODE_DPAD_RIGHT, 2);                         /* ABC -> ',' -> space */
    tap(UP_KEYCODE_BUTTON_A);
    CHECK(text_is("abqedEeeE@ "));
    /* B held repeats */
    osk_key(UP_KEYCODE_BUTTON_B, 1); osk_tick();
    sleep_ticks(0.65);
    osk_key(UP_KEYCODE_BUTTON_B, 0); osk_tick();
    CHECK(F.len <= 9 && F.len >= 1);
    /* the d-pad held repeats */
    tap(UP_KEYCODE_DPAD_UP);                                /* row 3 */
    int before = F.len;
    hat(-1, 0); sleep_ticks(0.7); hat(0, 0);
    tap(UP_KEYCODE_BUTTON_A);
    CHECK(F.len >= before - 1 && F.len <= before + 1);      /* landed on a letter, shift or delete */
    /* Enter on a single-line field is done; the press that closes it keeps its release */
    CHECK(osk_key(UP_KEYCODE_BUTTON_START, 1) == 1);
    CHECK(osk_tick() == 1 && finished == UP_SOFT_INPUT_DONE && !osk_active());
    CHECK(osk_key(UP_KEYCODE_BUTTON_START, 0) == 1);
    CHECK(osk_key(UP_KEYCODE_BUTTON_A, 1) == 0 && osk_key(UP_KEYCODE_BUTTON_A, 0) == 0);
    float w[8] = { 0.9f, 0, 0, 0, 1, 0, 0, 0 };
    osk_axes(0, w);
    CHECK(w[0] == 0.9f && w[4] == 1);
    /* Select cancels; Enter in a multi-line field is a newline */
    open_field("", 0, 0x28001);
    osk_tick();
    taps(UP_KEYCODE_DPAD_DOWN, 3);                          /* 'q' -> 'a' -> shift -> ?123 */
    taps(UP_KEYCODE_DPAD_RIGHT, 20);                        /* wraps around the row: ?123 again */
    tap(UP_KEYCODE_DPAD_LEFT);                              /* Enter */
    tap(UP_KEYCODE_BUTTON_A);
    CHECK(osk_active() && text_is("\n"));
    tap(UP_KEYCODE_BUTTON_SELECT);
    CHECK(finished == UP_SOFT_INPUT_CANCEL && !osk_active());
    if (failures) show_text();
}

static void draw(int path)
{
    osk_init(640, 480);
    open_field("Bob", 0, 0x8001);
    osk_tick();
    unity_state();
    osk_draw();
    CHECK(unity_state_kept() && fake_errors == 0 && fake_bad_scissor == 0 && fake_bad_blit == 0);
    dump(path ? "clears" : "copy");
    if (failures) printf("  errors %d bad scissor %d bad blit %d clears %d blits %d\n", fake_errors, fake_bad_scissor, fake_bad_blit, fake_clears, fake_blits);
    if (path == 0) CHECK(fake_blits == 1 && fake_uploads == 1 && fake_clears == 0);
    else CHECK(fake_blits == 0 && fake_clears > 50);
    CHECK(is(px(320, 100), 128, 128, 128) && is(px(5, PANEL_TOP - 1), 128, 128, 128));   /* the game's picture above */
    CHECK(is(px(320, PANEL_TOP), 0x20, 0x23, 0x28));                                       /* panel background */
    CHECK(is(px(320, PANEL_TOP + 3), 0xf4, 0xf4, 0xf4) || is(px(320, PANEL_TOP + 3), 0x90, 0x90, 0x90)); /* field */
    int dark = 0;                                                                         /* "Bob" in the field */
    for (int y = PANEL_TOP + 2; y < PANEL_TOP + 36; y++) for (int x = 6; x < 6 + 36; x++) dark += is(px(x, y), 0x14, 0x14, 0x14);
    CHECK(dark > 40);
    CHECK(is(px(3, KEY_ROW_Y(1) + 1), 0x2f, 0x6f, 0xe0));                                   /* 'q' has the focus */
    CHECK(is(px(70, KEY_ROW_Y(1) + 1), 0x3c, 0x41, 0x49));                                  /* 'w' does not */
    CHECK(is(px(3, KEY_ROW_Y(3) + 1), 0x2b, 0x2f, 0x35));                                   /* shift: a special key */
    int lab = 0;
    for (int y = KEY_ROW_Y(1); y < KEY_ROW_Y(1) + 32; y++) for (int x = 2; x < 63; x++) lab += is(px(x, y), 0xf2, 0xf2, 0xf2);
    CHECK(lab > 10);                                                                      /* a label on it */
    /* unchanged: no new upload; a move: one */
    int up = fake_uploads, cl = fake_clears;
    unity_state();
    osk_draw();
    CHECK(unity_state_kept() && (path ? fake_clears > cl : fake_uploads == up));
    tap(UP_KEYCODE_DPAD_RIGHT);
    unity_state();
    osk_draw();
    CHECK(unity_state_kept() && is(px(70, KEY_ROW_Y(1) + 1), 0x2f, 0x6f, 0xe0));
    if (path == 0) CHECK(fake_uploads == up + 1);
    /* closed: nothing drawn */
    tap(UP_KEYCODE_BUTTON_START);
    int calls = fake_calls;
    unity_state();
    osk_draw();
    CHECK(fake_calls == calls && is(px(320, PANEL_TOP), 128, 128, 128));
}

static void pads(void)
{
    osk_init(640, 480);
    open_field("", 4, 2);                                   /* number pad: focus on '1' */
    osk_tick();
    tap(UP_KEYCODE_BUTTON_A);
    tap(UP_KEYCODE_DPAD_RIGHT); tap(UP_KEYCODE_BUTTON_A);
    tap(UP_KEYCODE_DPAD_DOWN); tap(UP_KEYCODE_DPAD_DOWN); tap(UP_KEYCODE_DPAD_DOWN); tap(UP_KEYCODE_BUTTON_A);   /* '0' */
    taps(UP_KEYCODE_DPAD_RIGHT, 2); taps(UP_KEYCODE_DPAD_UP, 2); tap(UP_KEYCODE_BUTTON_A);   /* '-': not signed */
    CHECK(text_is("120"));
    tap(UP_KEYCODE_BUTTON_Y);                               /* no shift on a number pad */
    tap(UP_KEYCODE_BUTTON_START);
    open_field("", 5, 3);                                   /* phone pad: '*' bottom left */
    osk_tick();
    taps(UP_KEYCODE_DPAD_UP, 1); tap(UP_KEYCODE_BUTTON_A);
    CHECK(text_is("*"));
    tap(UP_KEYCODE_BUTTON_SELECT);
    open_field("", 6, 0x2061);                              /* CAP_WORDS (name): capitals at word starts */
    osk_tick();
    tap(UP_KEYCODE_BUTTON_A); tap(UP_KEYCODE_BUTTON_A); tap(UP_KEYCODE_BUTTON_X); tap(UP_KEYCODE_BUTTON_A);
    CHECK(text_is("Qq Q"));
    tap(UP_KEYCODE_BUTTON_START);
    open_field("", 1, 0x4001);                              /* CAP_SENTENCES */
    osk_tick();
    tap(UP_KEYCODE_BUTTON_A); tap(UP_KEYCODE_BUTTON_A);
    CHECK(text_is("Qq"));
    tap(UP_KEYCODE_BUTTON_START);
    open_field("pw", 0, 0x80081);                           /* password: drawn as '*' (not checked here) */
    F.hide_input = 1;                                       /* hidden field: the panel is lower */
    osk_tick();
    unity_state();
    osk_draw();
    CHECK(unity_state_kept() && is(px(320, PANEL_TOP), 128, 128, 128) && is(px(320, PANEL_TOP + 40), 0x20, 0x23, 0x28));
    dump("hidden-field");
    tap(UP_KEYCODE_BUTTON_START);
    open_field("", 4, 0x1002);                              /* signed number pad, to look at */
    osk_tick();
    unity_state();
    osk_draw();
    dump("number");
    tap(UP_KEYCODE_BUTTON_START);
    open_field("Hello wo", 0, 0x8001);                      /* symbols page, a selection */
    osk_tick();
    taps(UP_KEYCODE_DPAD_DOWN, 3); tap(UP_KEYCODE_BUTTON_A);   /* 'q' -> 'a' -> shift -> ?123 */
    up_soft_input_select(2, 5);
    osk_tick();
    unity_state();
    osk_draw();
    dump("symbols");
    tap(UP_KEYCODE_BUTTON_START);
}

static void modes(const char *m)
{
    setenv("UA_OSK", m, 1);
    osk_init(640, 480);
    open_field("hi", 0, 1);
    osk_tick();
    if (!strcmp(m, "done")) CHECK(finished == UP_SOFT_INPUT_DONE && !F.active && text_is("hi"));
    if (!strcmp(m, "cancel")) CHECK(finished == UP_SOFT_INPUT_CANCEL && !F.active);
    if (!strcmp(m, "0")) {
        CHECK(F.active && !osk_active() && osk_typing());
        CHECK(osk_key(UP_KEYCODE_BUTTON_A, 1) == 0 && osk_key(UP_KEYCODE_BUTTON_START, 1) == 0);
        osk_hw_text("!");                                   /* a hardware keyboard still types */
        osk_hw_key(21, 1); osk_hw_key(67, 1);               /* left, backspace */
        CHECK(text_is("h!"));
        osk_hw_key(66, 1);                                  /* Enter: done */
        CHECK(finished == UP_SOFT_INPUT_DONE);
    }
    unity_state();
    osk_draw();
    CHECK(fake_calls == 0 && unity_state_kept());
}

int main(int argc, char **argv)
{
    const char *s = argc > 1 ? argv[1] : "";
    if (!strcmp(s, "off")) off();
    else if (!strcmp(s, "type")) type();
    else if (!strcmp(s, "draw")) draw(0);
    else if (!strcmp(s, "es2")) { setenv("FAKE_GLES2", "1", 1); draw(1); }
    else if (!strcmp(s, "msaa")) { setenv("FAKE_SAMPLES", "1", 1); draw(1); }
    else if (!strcmp(s, "pads")) pads();
    else if (!strcmp(s, "done") || !strcmp(s, "cancel") || !strcmp(s, "0")) modes(s);
    else { printf("usage: test_osk off|type|draw|es2|msaa|pads|done|cancel|0\n"); return 2; }
    printf("osk %s: %s\n", s, failures ? "FAILED" : "ok");
    return failures != 0;
}
