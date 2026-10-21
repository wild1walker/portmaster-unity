/*
 * up_softinput.c - Unity's soft keyboard, Java side: UnityPlayer.showSoftInput / hideSoftInput /
 * setSoftInputStr / setCharacterLimit / setHideInputField / setSelection / getKeyboardLayout, and
 * the text field of the input dialog Unity's Java player opens for them. Written after the
 * behaviour of that dialog (no Unity code is included): an EditText holding the text the game
 * passed, caret at the end, the character limit as a LengthFilter, the input type's character
 * filter, and every change reported to libunity the way the dialog reports it:
 *
 *   text changed (afterTextChanged)  GL thread: nativeSetInputString(text),
 *                                               nativeSetInputSelection(start, length)   2018.4+
 *   caret moved (onSelectionChanged) GL thread: nativeSetInputSelection(start, length)   2018.4+
 *   OK / Done (Enter, single line)   caret to 0, then GL: nativeSetInputString(text),
 *                                    nativeSoftInputClosed(); the dialog closes
 *   Back                             caret to 0, then GL: nativeSoftInputCanceled(),
 *                                    nativeSoftInputClosed(); the dialog closes
 *   dialog cancelled (lost focus)    UI: nativeSoftInputLostFocus(), GL: nativeSoftInputClosed()
 *   shown / closed                   UI: nativeReportKeyboardConfigChanged()                 2019.4+
 *                                    GL: nativeSetKeyboardIsVisible(b), nativeSetInputArea   2019.4+
 *   the activity pauses              GL: nativeSoftInputCanceled(), nativeSoftInputClosed()
 *
 * Natives a player does not register are not called (2017.4 has only the string, closed,
 * canceled and lost-focus ones). Selection indices are UTF-16 code units, as Java's.
 *
 * The keyboard itself (Android's IME) is the host's: it reads the field with up_soft_input_get()
 * and types with up_soft_input_commit/delete/select/finish (unityplayer.h), on the UI thread,
 * where the dialog lives on Android too. Without a host keyboard nothing types; a game that never
 * opens the keyboard sees no difference at all.
 */
#define _GNU_SOURCE
#include "up_internal.h"

#include <stdlib.h>
#include <string.h>

#define M(c, n, s, f)       fj_method_define(c, n, s, 0, f, NULL)
#define MU(c, n, s, f, u)   fj_method_define(c, n, s, 0, f, (void *)(intptr_t)(u))

/* android.text.InputType */
enum { TYPE_MASK_CLASS = 0xf, TYPE_CLASS_TEXT = 1, TYPE_CLASS_NUMBER = 2, TYPE_CLASS_PHONE = 3,
       TYPE_NUMBER_FLAG_SIGNED = 0x1000, TYPE_NUMBER_FLAG_DECIMAL = 0x2000,
       TYPE_TEXT_FLAG_MULTI_LINE = 0x20000, TYPE_TEXT_FLAG_AUTO_CORRECT = 0x8000,
       TYPE_TEXT_FLAG_NO_SUGGESTIONS = 0x80000, TYPE_TEXT_VARIATION_PASSWORD = 0x80,
       TYPE_NUMBER_VARIATION_PASSWORD = 0x10 };

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int active, kb_type, input_type, multiline, secure, autocorrect, alert, hide_input, limit, nargs;
    unsigned serial;
    uint32_t text[UP_SOFT_INPUT_MAX];
    int len, a, b;                          /* selection a <= b, code points */
    char placeholder[UP_SOFT_INPUT_HINT];
    int area[4];                            /* the host keyboard's rectangle: x, y, w, h */
} E;

static int have(const char *native) { return fj_native_sig(UP_CLS, native) != NULL; }

/* ---- UTF-8 <-> code points ----------------------------------------------------------------- */
static int utf8_decode(const char *s, uint32_t *out, int max)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    int n = 0;
    while (*p && n < max) {
        uint32_t c = *p++;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (extra) c &= 0x3f >> extra;
        for (; extra && (*p & 0xc0) == 0x80; extra--) c = c << 6 | (*p++ & 0x3f);
        if (extra) c = 0xfffd;              /* truncated sequence */
        out[n++] = c;
    }
    return n;
}
static char *utf8_encode(const uint32_t *t, int n)
{
    char *s = malloc((size_t)n * 4 + 1), *o = s;
    for (int i = 0; i < n; i++) {
        uint32_t c = t[i];
        if (c < 0x80) *o++ = (char)c;
        else if (c < 0x800) { *o++ = (char)(0xc0 | c >> 6); *o++ = (char)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { *o++ = (char)(0xe0 | c >> 12); *o++ = (char)(0x80 | (c >> 6 & 0x3f)); *o++ = (char)(0x80 | (c & 0x3f)); }
        else { *o++ = (char)(0xf0 | c >> 18); *o++ = (char)(0x80 | (c >> 12 & 0x3f)); *o++ = (char)(0x80 | (c >> 6 & 0x3f)); *o++ = (char)(0x80 | (c & 0x3f)); }
    }
    *o = 0;
    return s;
}
/* UTF-16 length of the first n code points (Java String indices) */
static int u16(const uint32_t *t, int n) { int k = n; for (int i = 0; i < n; i++) k += t[i] > 0xffff; return k; }
static int cp_index(const uint32_t *t, int len, int u)      /* code point index of UTF-16 index u */
{
    int i = 0, k = 0;
    while (i < len && k + 1 + (t[i] > 0xffff) <= u) { k += 1 + (t[i] > 0xffff); i++; }
    return i;
}

/* ---- reports to libunity (GL thread jobs, as UnityPlayer.queueGLThreadEvent) --------------- */
enum { J_STR, J_SEL, J_CANCELED, J_CLOSED, J_VISIBLE, J_AREA };
struct job { int kind; char *s; int v[4]; };
static void gl_job(void *arg)
{
    struct job *j = arg;
    jvalue a[4];
    if (up_is_finishing()) { free(j->s); free(j); return; }    /* queueGLThreadEvent drops them */
    fj_push_frame();
    switch (j->kind) {
    case J_STR: a[0].l = fj_string(j->s); up_native("nativeSetInputString", "(Ljava/lang/String;)V", a); break;
    case J_SEL: a[0] = fj_i(j->v[0]); a[1] = fj_i(j->v[1]); up_native("nativeSetInputSelection", "(II)V", a); break;
    case J_CANCELED: up_native("nativeSoftInputCanceled", "()V", NULL); break;
    case J_CLOSED: up_native("nativeSoftInputClosed", "()V", NULL); break;
    case J_VISIBLE: a[0] = fj_z((jboolean)j->v[0]); up_native("nativeSetKeyboardIsVisible", "(Z)V", a); break;
    case J_AREA: for (int i = 0; i < 4; i++) a[i] = fj_i(j->v[i]); up_native("nativeSetInputArea", "(IIII)V", a); break;
    }
    fj_pop_frame();
    free(j->s);
    free(j);
}
static void post(int kind, char *s, int v0, int v1, int v2, int v3)
{
    static const char *const natives[] = { "nativeSetInputString", "nativeSetInputSelection", "nativeSoftInputCanceled",
                                           "nativeSoftInputClosed", "nativeSetKeyboardIsVisible", "nativeSetInputArea" };
    if (!have(natives[kind]) || up_is_finishing()) { free(s); return; }
    struct job *j = malloc(sizeof *j);
    *j = (struct job){ kind, s, { v0, v1, v2, v3 } };
    up_gl_post(gl_job, j, NULL);
}
static void ui_native(const char *name)          /* natives Java calls on the UI thread itself */
{
    if (have(name)) up_native(name, "()V", NULL);
}

/* the selection as Java reports it: start, length in UTF-16 units (state locked by the caller) */
static void sel_report_locked(int *start, int *length)
{
    *start = u16(E.text, E.a);
    *length = u16(E.text, E.b) - *start;
}
static void report_text(void)                     /* afterTextChanged */
{
    int s, l;
    pthread_mutex_lock(&g_mu);
    char *t = utf8_encode(E.text, E.len);
    sel_report_locked(&s, &l);
    pthread_mutex_unlock(&g_mu);
    post(J_STR, t, 0, 0, 0, 0);
    post(J_SEL, NULL, s, l, 0, 0);
}
static void report_selection(void)                /* onSelectionChanged */
{
    int s, l;
    pthread_mutex_lock(&g_mu);
    sel_report_locked(&s, &l);
    pthread_mutex_unlock(&g_mu);
    post(J_SEL, NULL, s, l, 0, 0);
}
static void report_layout(int visible)            /* the dialog's global layout listener */
{
    if (visible) {
        pthread_mutex_lock(&g_mu);
        int x = E.area[0], y = E.area[1], w = E.area[2], h = E.area[3];
        pthread_mutex_unlock(&g_mu);
        if (w <= 0 || h <= 0) { w = UPC.width; h = UPC.height / 2; x = 0; y = UPC.height - h; }
        post(J_VISIBLE, NULL, 1, 0, 0, 0);
        /* left, top of the input view; its width; the part of the view below the display (0) */
        post(J_AREA, NULL, x, y, w, 0);
    } else {
        post(J_AREA, NULL, 0, 0, 0, 0);
        post(J_VISIBLE, NULL, 0, 0, 0, 0);
    }
}

/* hideSoftInput's UI runnable: area and visibility, then (with a dialog) dismiss and the keyboard
   configuration */
static void hide_ui(void)
{
    pthread_mutex_lock(&g_mu);
    int was = E.active;
    E.active = 0;
    E.serial++;
    pthread_mutex_unlock(&g_mu);
    report_layout(0);
    if (was) {
        up_log("soft input: closed");
        ui_native("nativeReportKeyboardConfigChanged");
    }
}

/* reportSoftInputStr(text, kind, canceled): kind 1 closes the dialog. Java posts the hide to the
   UI thread and queues the GL event right away, so libunity gets the text (or the cancel) and
   the close before the area and visibility; the field takes no more input from here on. */
static void report_str(char *text, int kind, int canceled)
{
    int was = 0;
    if (kind == 1) {
        pthread_mutex_lock(&g_mu);
        was = E.active;
        E.active = 0;
        E.serial++;
        pthread_mutex_unlock(&g_mu);
    }
    if (canceled) { free(text); post(J_CANCELED, NULL, 0, 0, 0, 0); }
    else if (text) post(J_STR, text, 0, 0, 0, 0);
    else free(text);
    if (kind != 1) return;
    post(J_CLOSED, NULL, 0, 0, 0, 0);
    report_layout(0);
    if (was) {
        up_log("soft input: closed");
        ui_native("nativeReportKeyboardConfigChanged");
    }
}

/* ---- the EditText's filters: the input type's characters and the LengthFilter -------------- */
static int accept_locked(uint32_t c, int at, const uint32_t *ins, int nins)
{
    int cls = E.input_type & TYPE_MASK_CLASS;
    if (c < 0x20 && c != '\n' && c != '\t') return 0;
    if (c == 0x7f) return 0;
    if (c == '\n' && !E.multiline) return 0;
    if (cls == TYPE_CLASS_NUMBER) {                   /* DigitsKeyListener */
        if (c >= '0' && c <= '9') return 1;
        int has_sign = 0, has_dot = 0;
        for (int i = 0; i < E.len; i++) { if (i < E.a || i >= E.b) { has_sign |= E.text[i] == '-' || E.text[i] == '+'; has_dot |= E.text[i] == '.'; } }
        for (int i = 0; i < nins; i++) { has_sign |= ins[i] == '-' || ins[i] == '+'; has_dot |= ins[i] == '.'; }
        if ((c == '-' || c == '+') && (E.input_type & TYPE_NUMBER_FLAG_SIGNED)) return at == 0 && !has_sign;
        if (c == '.' && (E.input_type & TYPE_NUMBER_FLAG_DECIMAL)) return !has_dot;
        return 0;
    }
    if (cls == TYPE_CLASS_PHONE)                      /* DialerKeyListener */
        return (c >= '0' && c <= '9') || (c && strchr("#*+-(),/N. ;", (int)c));
    return 1;
}

/* replace the selection with ins[0..n) after filtering; returns code points inserted */
static int replace_locked(const uint32_t *in, int n)
{
    uint32_t ok[256];
    int k = 0;
    for (int i = 0; i < n && k < 256; i++)
        if (accept_locked(in[i], E.a + k, ok, k)) ok[k++] = in[i];
    if (E.limit > 0) {                                /* LengthFilter, in UTF-16 units */
        int keep = E.limit - (u16(E.text, E.len) - (u16(E.text, E.b) - u16(E.text, E.a)));
        int m = 0, units = 0;
        while (m < k && units + 1 + (ok[m] > 0xffff) <= keep) units += 1 + (ok[m++] > 0xffff);
        k = m;
    }
    if (E.len - (E.b - E.a) + k > UP_SOFT_INPUT_MAX) k = UP_SOFT_INPUT_MAX - (E.len - (E.b - E.a));
    if (k <= 0 && E.a == E.b) return 0;
    memmove(E.text + E.a + k, E.text + E.b, (size_t)(E.len - E.b) * sizeof E.text[0]);
    memcpy(E.text + E.a, ok, (size_t)k * sizeof ok[0]);
    E.len += k - (E.b - E.a);
    E.a = E.b = E.a + k;
    E.serial++;
    return k > 0 ? k : 0;
}

static void set_text_locked(const char *utf8)       /* setText: through the filters */
{
    static uint32_t tmp[UP_SOFT_INPUT_MAX];
    int n = utf8_decode(utf8, tmp, UP_SOFT_INPUT_MAX);
    E.len = 0; E.a = E.b = 0;
    int limit = E.limit;
    /* setText runs the InputFilters set so far (the LengthFilter), not the input type's
       character filter (the KeyListener filters edits only) */
    if (limit > 0) {
        int m = 0, units = 0;
        while (m < n && units + 1 + (tmp[m] > 0xffff) <= limit) units += 1 + (tmp[m++] > 0xffff);
        n = m;
    }
    memcpy(E.text, tmp, (size_t)n * sizeof tmp[0]);
    E.len = n;
    E.serial++;
}

/* ---- the dialog: UnityPlayer methods libunity calls (on UnityMain) -------------------------- */
struct show { char *text, *hint; int type, autocorrect, multiline, secure, alert, limit, hide, extra, nargs; };

/* the inputType Unity's Java side derives from TouchScreenKeyboardType and the flags */
static int input_type(int type, int autocorrect, int multiline, int secure)
{
    static const int types[12] = { 1, 16385, 12290, 17, 2, 3, 8289, 33, 1, 16417, 17, 8194 };
    int t = (autocorrect ? TYPE_TEXT_FLAG_AUTO_CORRECT : TYPE_TEXT_FLAG_NO_SUGGESTIONS) |
            (multiline ? TYPE_TEXT_FLAG_MULTI_LINE : 0) | (secure ? TYPE_TEXT_VARIATION_PASSWORD : 0);
    if (type < 0 || type > 11) return t;
    if (types[type] & 2)                     /* number and phone classes take no text flags */
        return types[type] | ((types[type] & TYPE_MASK_CLASS) == TYPE_CLASS_NUMBER && secure ? TYPE_NUMBER_VARIATION_PASSWORD : 0);
    return t | types[type];
}

static void ui_show(void *arg)
{
    struct show *s = arg;
    pthread_mutex_lock(&g_mu);
    E.kb_type = s->type; E.autocorrect = s->autocorrect; E.multiline = s->multiline; E.secure = s->secure;
    E.alert = s->alert; E.hide_input = s->nargs >= 9 && s->hide; E.nargs = s->nargs;
    E.input_type = input_type(s->type, s->autocorrect, s->multiline, s->secure);
    E.limit = 0;                             /* setText comes before the LengthFilter is set */
    set_text_locked(s->text);
    E.limit = s->nargs >= 9 ? s->limit : 0;
    snprintf(E.placeholder, sizeof E.placeholder, "%s", s->hint ? s->hint : "");
    E.a = E.b = E.len;                       /* setSelection(length) */
    E.active = 1;
    E.serial++;
    int type = E.input_type, len = E.len;
    pthread_mutex_unlock(&g_mu);
    up_log("soft input: shown (keyboard type %d, inputType 0x%x%s%s%s, limit %d, %d chars)", s->type, type,
           s->multiline ? ", multiline" : "", s->secure ? ", secure" : "", E.hide_input ? ", field hidden" : "", E.limit, len);
    report_selection();                      /* onSelectionChanged of that setSelection */
    ui_native("nativeReportKeyboardConfigChanged");
    report_layout(1);
    free(s->text); free(s->hint); free(s);
}

H(si_show)       /* showSoftInput: 7 (2017.4), 9 (2018.4) or 10 (2019.4+) arguments */
{
    struct show *s = calloc(1, sizeof *s);
    const char *t = fj_cstr(a[0].l), *h = fj_cstr(a[6].l);
    s->text = strdup(t ? t : ""); s->hint = strdup(h ? h : "");
    s->type = a[1].i; s->autocorrect = a[2].z; s->multiline = a[3].z; s->secure = a[4].z; s->alert = a[5].z;
    s->nargs = (int)(intptr_t)ud;
    if (s->nargs >= 9) { s->limit = a[7].i; s->hide = a[8].z; }
    if (s->nargs >= 10) s->extra = a[9].z;
    looper_post(&L_ui, ui_show, s, NULL, 0);
    return fj_v();
}

static void ui_hide(void *arg) { (void)arg; hide_ui(); }
H(si_hide) { looper_post(&L_ui, ui_hide, NULL, NULL, 0); return fj_v(); }

static void ui_set_str(void *arg)          /* setSoftInputStr: setText, then the caret to the end */
{
    char *t = arg;
    pthread_mutex_lock(&g_mu);
    int on = E.active;
    if (on) { set_text_locked(t); E.a = E.b = 0; }
    pthread_mutex_unlock(&g_mu);
    if (on) {
        report_text();
        pthread_mutex_lock(&g_mu);
        E.a = E.b = E.len;
        E.serial++;
        pthread_mutex_unlock(&g_mu);
        report_selection();
    }
    free(t);
}
H(si_set_str)
{
    if (!a[0].l) return fj_v();
    looper_post(&L_ui, ui_set_str, strdup(fj_cstr(a[0].l)), NULL, 0);
    return fj_v();
}

struct ints { int v[2]; };
static void ui_limit(void *arg)
{
    struct ints *x = arg;
    pthread_mutex_lock(&g_mu);
    if (E.active) { E.limit = x->v[0] > 0 ? x->v[0] : 0; E.serial++; }
    pthread_mutex_unlock(&g_mu);
    free(x);
}
static void ui_hide_field(void *arg)
{
    struct ints *x = arg;
    pthread_mutex_lock(&g_mu);
    if (E.active) { E.hide_input = x->v[0]; E.serial++; }
    pthread_mutex_unlock(&g_mu);
    free(x);
}
static void ui_selection(void *arg)        /* setSelection(start, length), UTF-16 units */
{
    struct ints *x = arg;
    int changed = 0;
    pthread_mutex_lock(&g_mu);
    if (E.active && x->v[0] >= 0 && x->v[1] >= 0 && u16(E.text, E.len) >= x->v[0] + x->v[1]) {
        int na = cp_index(E.text, E.len, x->v[0]), nb = cp_index(E.text, E.len, x->v[0] + x->v[1]);
        changed = na != E.a || nb != E.b;
        E.a = na; E.b = nb;
        if (changed) E.serial++;
    }
    pthread_mutex_unlock(&g_mu);
    if (changed) report_selection();
    free(x);
}
static jvalue post_ints(void (*fn)(void *), int v0, int v1)
{
    struct ints *x = malloc(sizeof *x);
    x->v[0] = v0; x->v[1] = v1;
    looper_post(&L_ui, fn, x, NULL, 0);
    return fj_v();
}
H(si_limit)      { return post_ints(ui_limit, a[0].i, 0); }
H(si_hide_field) { return post_ints(ui_hide_field, a[0].z, 0); }
H(si_selection)  { return post_ints(ui_selection, a[0].i, a[1].i); }
H(si_layout)     /* getKeyboardLayout: the IME's locale while the dialog is up */
{
    pthread_mutex_lock(&g_mu);
    int on = E.active;
    pthread_mutex_unlock(&g_mu);
    return fj_l(on ? fj_string("en_US") : NULL);
}
H(si_report_str) /* UnityPlayer.reportSoftInputStr (Java's own) */
{
    const char *t = fj_cstr(a[0].l);
    report_str(t ? strdup(t) : NULL, a[1].i, a[2].z);
    return fj_v();
}

void up_softinput_register(void)
{
    MU(UP_CLS, "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;)V", si_show, 7);      /* 2017.4 */
    MU(UP_CLS, "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;IZ)V", si_show, 9);    /* 2018.4 */
    MU(UP_CLS, "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;IZZ)V", si_show, 10);  /* 2019.4+ */
    M(UP_CLS, "hideSoftInput", "()V", si_hide);
    M(UP_CLS, "setSoftInputStr", "(Ljava/lang/String;)V", si_set_str);
    M(UP_CLS, "reportSoftInputStr", "(Ljava/lang/String;IZ)V", si_report_str);
    M(UP_CLS, "setCharacterLimit", "(I)V", si_limit);
    M(UP_CLS, "setHideInputField", "(Z)V", si_hide_field);
    M(UP_CLS, "setSelection", "(II)V", si_selection);
    M(UP_CLS, "getKeyboardLayout", "()Ljava/lang/String;", si_layout);
}

/* UnityPlayer.pause(): reportSoftInputStr(null, 1, true), dialog or not */
void up_softinput_pause(void)
{
    report_str(NULL, 1, 1);
}

/* ---- the host's keyboard (the IME), UI thread ------------------------------------------------ */
unsigned up_soft_input_serial(void)
{
    pthread_mutex_lock(&g_mu);
    unsigned s = E.serial;
    pthread_mutex_unlock(&g_mu);
    return s;
}

unsigned up_soft_input_get(struct up_soft_input *o)
{
    pthread_mutex_lock(&g_mu);
    o->serial = E.serial; o->active = E.active; o->kb_type = E.kb_type; o->input_type = E.input_type;
    o->multiline = E.multiline; o->secure = E.secure; o->hide_input = E.hide_input; o->char_limit = E.limit;
    o->len = E.len; o->sel_start = E.a; o->sel_end = E.b;
    memcpy(o->text, E.text, (size_t)E.len * sizeof E.text[0]);
    memcpy(o->placeholder, E.placeholder, sizeof o->placeholder);
    unsigned s = E.serial;
    pthread_mutex_unlock(&g_mu);
    return s;
}

int up_soft_input_commit(const char *utf8)
{
    uint32_t in[256];
    int n = utf8_decode(utf8, in, 256);
    pthread_mutex_lock(&g_mu);
    int on = E.active, had_sel = E.a != E.b;
    int k = on ? replace_locked(in, n) : 0;
    pthread_mutex_unlock(&g_mu);
    if (on && (k > 0 || had_sel)) report_text();
    return k;
}

void up_soft_input_delete(int before, int after)
{
    pthread_mutex_lock(&g_mu);
    int on = E.active, changed = 0;
    if (on) {
        if (E.a == E.b) {
            int a = E.a - (before > 0 ? before : 0), b = E.b + (after > 0 ? after : 0);
            if (a < 0) a = 0;
            if (b > E.len) b = E.len;
            E.a = a; E.b = b;
        }
        changed = E.a != E.b;
        if (changed) replace_locked(NULL, 0);
    }
    pthread_mutex_unlock(&g_mu);
    if (changed) report_text();
}

void up_soft_input_select(int start, int end)
{
    pthread_mutex_lock(&g_mu);
    int changed = 0;
    if (E.active) {
        if (start < 0) start = 0;
        if (end < 0) end = 0;
        if (start > E.len) start = E.len;
        if (end > E.len) end = E.len;
        if (start > end) { int t = start; start = end; end = t; }
        changed = start != E.a || end != E.b;
        E.a = start; E.b = end;
        if (changed) E.serial++;
    }
    pthread_mutex_unlock(&g_mu);
    if (changed) report_selection();
}

void up_soft_input_finish(int how)
{
    int lost = how == UP_SOFT_INPUT_LOST_FOCUS;
    pthread_mutex_lock(&g_mu);
    int on = E.active, moved = on && !lost && (E.a || E.b);
    char *t = on && !lost ? utf8_encode(E.text, E.len) : NULL;
    if (moved) { E.a = E.b = 0; E.serial++; }      /* OK and Back put the caret at 0 first */
    pthread_mutex_unlock(&g_mu);
    if (!on) return;
    up_log("soft input: %s", how == UP_SOFT_INPUT_CANCEL ? "cancelled" : lost ? "lost focus" : "done");
    if (moved) report_selection();
    if (lost) {                                    /* the dialog's OnCancelListener */
        ui_native("nativeSoftInputLostFocus");
        report_str(NULL, 1, 0);
    }
    else report_str(t, 1, how == UP_SOFT_INPUT_CANCEL);
}

void up_soft_input_set_area(int x, int y, int w, int h)
{
    pthread_mutex_lock(&g_mu);
    E.area[0] = x; E.area[1] = y; E.area[2] = w; E.area[3] = h;
    pthread_mutex_unlock(&g_mu);
}
