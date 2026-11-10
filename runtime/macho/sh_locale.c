// sh_locale.c - locales, character classes and the wide-character conversions as macOS's libc
// answers them. A Darwin locale_t is glibc's own locale_t, made by newlocale here; its two
// special values (NULL: the C locale, -1: the global one) are replaced on the way in. What
// macOS has and glibc has not (the *_l conversions) runs with the thread's locale switched.
#define _GNU_SOURCE
#include "darwin.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <nl_types.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define PUT(p, off, v) memcpy((unsigned char *)(p) + (off), &(v), sizeof(v))

// ---- locale objects ----------------------------------------------------------------------------

static locale_t c_locale;
static pthread_once_t c_once = PTHREAD_ONCE_INIT;
static _Atomic(locale_t) global_copy;       // the global locale as an object, for the *_l calls

static void c_init(void) { c_locale = newlocale(LC_ALL_MASK, "C", (locale_t)0); }

static locale_t global_locale(void)
{
    locale_t g = atomic_load(&global_copy);
    if (g) return g;
    locale_t made = duplocale(LC_GLOBAL_LOCALE);
    if (!made) { pthread_once(&c_once, c_init); return c_locale; }
    if (atomic_compare_exchange_strong(&global_copy, &g, made)) return made;
    freelocale(made);
    return g;
}

void *darwin_locale_host(void *loc)
{
    if (loc == (void *)DARWIN_LC_C_LOCALE) { pthread_once(&c_once, c_init); return c_locale; }
    if (loc == (void *)DARWIN_LC_GLOBAL_LOCALE) return global_locale();
    return loc;
}
#define HOST(loc) ((locale_t)darwin_locale_host(loc))

static int mask_host(int d)
{
    return ((d & DARWIN_LC_COLLATE_MASK) ? LC_COLLATE_MASK : 0) | ((d & DARWIN_LC_CTYPE_MASK) ? LC_CTYPE_MASK : 0) |
           ((d & DARWIN_LC_MESSAGES_MASK) ? LC_MESSAGES_MASK : 0) | ((d & DARWIN_LC_MONETARY_MASK) ? LC_MONETARY_MASK : 0) |
           ((d & DARWIN_LC_NUMERIC_MASK) ? LC_NUMERIC_MASK : 0) | ((d & DARWIN_LC_TIME_MASK) ? LC_TIME_MASK : 0);
}

static int category_host(int d)
{
    switch (d) {
    case DARWIN_LC_ALL:      return LC_ALL;
    case DARWIN_LC_COLLATE:  return LC_COLLATE;
    case DARWIN_LC_CTYPE:    return LC_CTYPE;
    case DARWIN_LC_MONETARY: return LC_MONETARY;
    case DARWIN_LC_NUMERIC:  return LC_NUMERIC;
    case DARWIN_LC_TIME:     return LC_TIME;
    case DARWIN_LC_MESSAGES: return LC_MESSAGES;
    }
    return -1;
}

// A named UTF-8 locale this system has no data for: its character set is kept (C.UTF-8) and
// its formats are the C locale's. Said once per name's first use; a name of another character
// set is refused as macOS refuses a locale it has not.
static const char *stand_in(const char *name)
{
    const char *dot = name ? strchr(name, '.') : NULL;
    if (!dot || (strcasecmp(dot, ".UTF-8") != 0 && strcasecmp(dot, ".UTF8") != 0)) return NULL;
    darwin_unanswered("locale", "a UTF-8 locale without data here: C.UTF-8 stands in");
    return "C.UTF-8";
}

// locale_t newlocale(int mask, const char *name, locale_t base): the base is used up, as there
static void *darwin_newlocale(int mask, const char *name, void *base)
{
    if (mask & ~DARWIN_LC_ALL_MASK) { errno = EINVAL; return NULL; }
    if (!name) name = "C";                  // macOS takes NULL for the C locale
    int hmask = mask_host(mask);
    locale_t hbase = (locale_t)0, copy = (locale_t)0;
    if (base == (void *)DARWIN_LC_GLOBAL_LOCALE) {
        hbase = copy = duplocale(LC_GLOBAL_LOCALE);
        if (!copy) return NULL;
    } else if (base != (void *)DARWIN_LC_C_LOCALE) {
        hbase = base;
    }
    locale_t made = newlocale(hmask, name, hbase);
    if (!made) {
        const char *other = stand_in(name);
        if (other) made = newlocale(hmask, other, hbase);
    }
    if (!made && copy) { int e = errno; freelocale(copy); errno = e; }
    return made;
}

// int freelocale(locale_t)
static long darwin_freelocale(void *loc)
{
    if (loc == (void *)DARWIN_LC_C_LOCALE || loc == (void *)DARWIN_LC_GLOBAL_LOCALE) { errno = EINVAL; return -1; }
    freelocale(loc);
    return 0;
}

// char *setlocale(int category, const char *name). The object that stood for the global locale
// until now is left alone: another thread may be inside a call with it.
static char *darwin_setlocale(int category, const char *name)
{
    int h = category_host(category);
    if (h < 0) { errno = EINVAL; return NULL; }
    char *r = setlocale(h, name);
    if (!r && name) {
        const char *other = stand_in(name);
        if (other) r = setlocale(h, other);
    }
    if (r && name) atomic_store(&global_copy, (locale_t)0);
    return r;
}

// struct lconv *localeconv_l(locale_t): two of the int_* bytes sit in the other order there,
// and "not known" is CHAR_MAX of a signed char, 127: char is unsigned here.
_Static_assert(DARWIN_SIZEOF_LCONV == 96 && sizeof(struct lconv) == 96, "lconv");
static __thread struct { unsigned char lc[DARWIN_SIZEOF_LCONV]; char grouping[32], mon_grouping[32]; } conv
    __attribute__((tls_model("initial-exec")));

static char d_char(char c) { return (unsigned char)c == UCHAR_MAX ? 127 : c; }

static char *d_grouping(const char *g, char *out, size_t n)
{
    size_t i = 0;
    for (; g && g[i] && i + 1 < n; i++) out[i] = d_char(g[i]);
    out[i] = 0;
    return out;
}

static void *darwin_localeconv_l(void *loc)
{
    locale_t old = uselocale(HOST(loc));
    struct lconv *l = localeconv();
    unsigned char *o = conv.lc;
    char *grouping = d_grouping(l->grouping, conv.grouping, sizeof conv.grouping);
    char *mon_grouping = d_grouping(l->mon_grouping, conv.mon_grouping, sizeof conv.mon_grouping);
#define STR(f)  PUT(o, DARWIN_LCONV_##f, l->f)
#define CHR(f)  o[DARWIN_LCONV_##f] = (unsigned char)d_char(l->f)
    STR(decimal_point); STR(thousands_sep); STR(int_curr_symbol); STR(currency_symbol);
    STR(mon_decimal_point); STR(mon_thousands_sep); STR(positive_sign); STR(negative_sign);
    PUT(o, DARWIN_LCONV_grouping, grouping);
    PUT(o, DARWIN_LCONV_mon_grouping, mon_grouping);
    CHR(int_frac_digits); CHR(frac_digits); CHR(p_cs_precedes); CHR(p_sep_by_space); CHR(n_cs_precedes);
    CHR(n_sep_by_space); CHR(p_sign_posn); CHR(n_sign_posn); CHR(int_p_cs_precedes); CHR(int_n_cs_precedes);
    CHR(int_p_sep_by_space); CHR(int_n_sep_by_space); CHR(int_p_sign_posn); CHR(int_n_sign_posn);
#undef STR
#undef CHR
    uselocale(old);
    return o;
}

// ---- character classes -------------------------------------------------------------------------
// _DefaultRuneLocale is the C locale's table: the inline isalpha and its kin read an ASCII
// character's class bits from it and ask __maskrune for every other character. A digit's entry
// also holds its value in the low byte.

static unsigned char rune_locale[DARWIN_SIZEOF_RUNELOCALE] __attribute__((aligned(8)));

static uint32_t c_class(int c)
{
    if (c < 0 || c > 127) return 0;
    if (c < 32 || c == 127)
        return DARWIN__CTYPE_C | ((c >= 9 && c <= 13) ? DARWIN__CTYPE_S : 0) | (c == 9 ? DARWIN__CTYPE_B : 0);
    if (c == ' ') return DARWIN__CTYPE_S | DARWIN__CTYPE_B | DARWIN__CTYPE_R;
    uint32_t t = DARWIN__CTYPE_R | DARWIN__CTYPE_G;
    if (c >= '0' && c <= '9') return t | DARWIN__CTYPE_D | DARWIN__CTYPE_X | (uint32_t)(c - '0');
    if (c >= 'A' && c <= 'Z')
        return t | DARWIN__CTYPE_U | DARWIN__CTYPE_A | (c <= 'F' ? DARWIN__CTYPE_X | (uint32_t)(c - 'A' + 10) : 0);
    if (c >= 'a' && c <= 'z')
        return t | DARWIN__CTYPE_L | DARWIN__CTYPE_A | (c <= 'f' ? DARWIN__CTYPE_X | (uint32_t)(c - 'a' + 10) : 0);
    return t | DARWIN__CTYPE_P;
}

__attribute__((constructor)) static void rune_init(void)
{
    int32_t invalid = 0xfffd;
    memcpy(rune_locale + DARWIN_RUNELOCALE___magic, "RuneMagA", 8);
    memcpy(rune_locale + DARWIN_RUNELOCALE___magic + 8, "NONE", 5);
    PUT(rune_locale, DARWIN_RUNELOCALE___invalid_rune, invalid);
    for (int c = 0; c < DARWIN__CACHED_RUNES; c++) {
        uint32_t type = c_class(c);
        int32_t lower = (c >= 'A' && c <= 'Z') ? c + 32 : c, upper = (c >= 'a' && c <= 'z') ? c - 32 : c;
        PUT(rune_locale, DARWIN_RUNELOCALE___runetype + 4 * c, type);
        PUT(rune_locale, DARWIN_RUNELOCALE___maplower + 4 * c, lower);
        PUT(rune_locale, DARWIN_RUNELOCALE___mapupper + 4 * c, upper);
    }
}

// The class bits of a character in a locale, of those asked for
static long class_in(int c, unsigned long want, locale_t l)
{
    if (c < 0) return 0;
    wint_t w = (wint_t)c;
    unsigned long t = 0;
    if ((want & DARWIN__CTYPE_A) && iswalpha_l(w, l)) t |= DARWIN__CTYPE_A;
    if ((want & DARWIN__CTYPE_C) && iswcntrl_l(w, l)) t |= DARWIN__CTYPE_C;
    if ((want & DARWIN__CTYPE_D) && c >= '0' && c <= '9') t |= DARWIN__CTYPE_D;
    if ((want & DARWIN__CTYPE_G) && iswgraph_l(w, l)) t |= DARWIN__CTYPE_G;
    if ((want & DARWIN__CTYPE_L) && iswlower_l(w, l)) t |= DARWIN__CTYPE_L;
    if ((want & DARWIN__CTYPE_P) && iswpunct_l(w, l)) t |= DARWIN__CTYPE_P;
    if ((want & DARWIN__CTYPE_S) && iswspace_l(w, l)) t |= DARWIN__CTYPE_S;
    if ((want & DARWIN__CTYPE_U) && iswupper_l(w, l)) t |= DARWIN__CTYPE_U;
    if ((want & DARWIN__CTYPE_X) && iswxdigit_l(w, l)) t |= DARWIN__CTYPE_X;
    if ((want & DARWIN__CTYPE_B) && iswblank_l(w, l)) t |= DARWIN__CTYPE_B;
    if ((want & DARWIN__CTYPE_R) && iswprint_l(w, l)) t |= DARWIN__CTYPE_R;
    if (c < 128) t |= c_class(c) & want & 0xff;        // a digit's value (digittoint asks for it)
    return (long)t;
}

static locale_t current(void)
{
    locale_t l = uselocale((locale_t)0);
    return l == LC_GLOBAL_LOCALE ? global_locale() : l;
}

// int __maskrune(__darwin_ct_rune_t, unsigned long), int __maskrune_l(.., locale_t)
static long darwin___maskrune(int c, unsigned long want) { return class_in(c, want, current()); }
static long darwin___maskrune_l(int c, unsigned long want, void *loc) { return class_in(c, want, HOST(loc)); }

static long darwin___tolower(int c) { return c < 0 ? c : (long)towlower_l((wint_t)c, current()); }
static long darwin___toupper(int c) { return c < 0 ? c : (long)towupper_l((wint_t)c, current()); }
static long darwin____tolower_l(int c, void *loc) { return c < 0 ? c : (long)towlower_l((wint_t)c, HOST(loc)); }
static long darwin____toupper_l(int c, void *loc) { return c < 0 ? c : (long)towupper_l((wint_t)c, HOST(loc)); }

static long darwin____mb_cur_max_l(void *loc)
{
    long n;
    DARWIN_WITH_LOCALE(loc, n = (long)MB_CUR_MAX);
    return n > 4 ? 4 : n;               // UTF-8: glibc says 6 (the old 31-bit form), macOS 4
}

// ---- conversions with a locale -----------------------------------------------------------------
// mbstate_t: 128 bytes there, glibc's 8 inside them; all zero is the first state on both.
_Static_assert(sizeof(mbstate_t) <= DARWIN_SIZEOF_MBSTATE_T && sizeof(wchar_t) == DARWIN_SIZEOF_WCHAR_T, "mbstate_t");

static uint32_t darwin_btowc_l(int c, void *loc) { wint_t r; DARWIN_WITH_LOCALE(loc, r = btowc(c)); return r; }
static long darwin_wctob_l(uint32_t c, void *loc) { int r; DARWIN_WITH_LOCALE(loc, r = wctob(c)); return r; }

static size_t darwin_mbrlen_l(const char *s, size_t n, mbstate_t *ps, void *loc)
{
    size_t r; DARWIN_WITH_LOCALE(loc, r = mbrlen(s, n, ps)); return r;
}

static size_t darwin_mbrtowc_l(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps, void *loc)
{
    size_t r; DARWIN_WITH_LOCALE(loc, r = mbrtowc(pwc, s, n, ps)); return r;
}

static long darwin_mbtowc_l(wchar_t *pwc, const char *s, size_t n, void *loc)
{
    int r; DARWIN_WITH_LOCALE(loc, r = mbtowc(pwc, s, n)); return r;
}

static size_t darwin_wcrtomb_l(char *s, uint32_t wc, mbstate_t *ps, void *loc)
{
    size_t r; DARWIN_WITH_LOCALE(loc, r = wcrtomb(s, (wchar_t)wc, ps)); return r;
}

static size_t darwin_mbsrtowcs_l(wchar_t *dst, const char **src, size_t len, mbstate_t *ps, void *loc)
{
    size_t r; DARWIN_WITH_LOCALE(loc, r = mbsrtowcs(dst, src, len, ps)); return r;
}

static size_t darwin_mbsnrtowcs_l(wchar_t *dst, const char **src, size_t nms, size_t len, mbstate_t *ps, void *loc)
{
    size_t r; DARWIN_WITH_LOCALE(loc, r = mbsnrtowcs(dst, src, nms, len, ps)); return r;
}

static size_t darwin_wcsnrtombs_l(char *dst, const wchar_t **src, size_t nwc, size_t len, mbstate_t *ps, void *loc)
{
    size_t r; DARWIN_WITH_LOCALE(loc, r = wcsnrtombs(dst, src, nwc, len, ps)); return r;
}

static long darwin_strcoll_l(const char *a, const char *b, void *loc) { return strcoll_l(a, b, HOST(loc)); }
static size_t darwin_strxfrm_l(char *d, const char *s, size_t n, void *loc) { return strxfrm_l(d, s, n, HOST(loc)); }
static long darwin_wcscoll_l(const wchar_t *a, const wchar_t *b, void *loc) { return wcscoll_l(a, b, HOST(loc)); }
static size_t darwin_wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *loc) { return wcsxfrm_l(d, s, n, HOST(loc)); }

// long double is double there
static double darwin_strtod_l(const char *s, char **end, void *loc) { return strtod_l(s, end, HOST(loc)); }
static float darwin_strtof_l(const char *s, char **end, void *loc) { return strtof_l(s, end, HOST(loc)); }
static double darwin_strtold_l(const char *s, char **end, void *loc) { return strtod_l(s, end, HOST(loc)); }
static long long darwin_strtoll_l(const char *s, char **end, int base, void *loc) { return strtoll_l(s, end, base, HOST(loc)); }
static unsigned long long darwin_strtoull_l(const char *s, char **end, int base, void *loc)
{
    return strtoull_l(s, end, base, HOST(loc));
}
static double darwin_wcstold(const wchar_t *s, wchar_t **end) { return wcstod(s, end); }

const struct darwin_sym darwin_syms_locale[] = {
    NAMED("__DefaultRuneLocale", rune_locale),
    SHIM(newlocale), SHIM(freelocale), SHIM(setlocale), SHIM(localeconv_l),
    SHIM(__maskrune), SHIM(__maskrune_l), SHIM(__tolower), SHIM(__toupper), SHIM(___tolower_l), SHIM(___toupper_l),
    SHIM(___mb_cur_max_l),
    SHIM(btowc_l), SHIM(wctob_l), SHIM(mbrlen_l), SHIM(mbrtowc_l), SHIM(mbtowc_l), SHIM(wcrtomb_l),
    SHIM(mbsrtowcs_l), SHIM(mbsnrtowcs_l), SHIM(wcsnrtombs_l),
    SHIM(strcoll_l), SHIM(strxfrm_l), SHIM(wcscoll_l), SHIM(wcsxfrm_l),
    SHIM(strtod_l), SHIM(strtof_l), SHIM(strtold_l), SHIM(strtoll_l), SHIM(strtoull_l), SHIM(wcstold),
    SAME(wcstod), SAME(wcstof), SAME(wcstol), SAME(wcstoll), SAME(wcstoul), SAME(wcstoull),
    SAME(catopen), SAME(catgets), SAME(catclose),
    { NULL, NULL }
};
