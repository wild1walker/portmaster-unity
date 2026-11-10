// sh_cf.c - the little of CoreFoundation that il2cpp asks: the user's locale, as language,
// country and script codes and as an identifier ("en_US"), read from the environment the way
// a Linux program finds it (LC_ALL, LC_MESSAGES, LANG). The strings handed out are ASCII, so
// every 8-bit encoding CFString knows writes them alike.
#define _GNU_SOURCE
#include "darwin.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CF_MAGIC  0x6a626f6663617564ull
enum { CF_STRING = 1, CF_LOCALE };

struct cfstring { uint64_t magic; int kind; char text[28]; };
struct cflocale {
    uint64_t magic; int kind;
    _Atomic long refs;
    struct cfstring language, country, script, identifier;
    int has_country, has_script;
};

// The keys are CFStringRef variables there: the images read the pointers.
static struct cfstring key_language = { CF_MAGIC, CF_STRING, "kCFLocaleLanguageCodeKey" };
static struct cfstring key_country = { CF_MAGIC, CF_STRING, "kCFLocaleCountryCodeKey" };
static struct cfstring key_script = { CF_MAGIC, CF_STRING, "kCFLocaleScriptCodeKey" };
static const void *kCFLocaleLanguageCode = &key_language, *kCFLocaleCountryCode = &key_country,
                  *kCFLocaleScriptCode = &key_script;

static const struct cfstring *string_of(const void *obj, const char *call)
{
    const struct cfstring *s = obj;
    if (s && s->magic == CF_MAGIC && s->kind == CF_STRING) return s;
    darwin_unanswered(call, "not a string made here");
    return NULL;
}

static void set(struct cfstring *s, const char *text, size_t n)
{
    s->magic = CF_MAGIC;
    s->kind = CF_STRING;
    if (n >= sizeof s->text) n = sizeof s->text - 1;
    memcpy(s->text, text, n);
    s->text[n] = 0;
}

static size_t letters(const char *p) { size_t n = 0; while (isalnum((unsigned char)p[n]) && (unsigned char)p[n] < 128) n++; return n; }

// "de_DE.UTF-8", "sr_RS@latin", "C": language[_COUNTRY][.charset][@modifier]
static void *darwin_CFLocaleCopyCurrent(void)
{
    struct cflocale *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    const char *env = getenv("LC_ALL");
    if (!env || !*env) env = getenv("LC_MESSAGES");
    if (!env || !*env) env = getenv("LANG");
    if (!env || !*env || !strcmp(env, "C") || !strcmp(env, "POSIX") || !strncmp(env, "C.", 2)) env = "en_US";
    l->magic = CF_MAGIC;
    l->kind = CF_LOCALE;
    atomic_store(&l->refs, 1);

    size_t n = letters(env);
    if (n < 2 || n > 3) { env = "en_US"; n = 2; }
    set(&l->language, env, n);
    for (char *c = l->language.text; *c; c++) *c = (char)tolower((unsigned char)*c);
    const char *p = env + n;
    if (*p == '_') {
        size_t m = letters(p + 1);
        if (m >= 2 && m <= 3) {
            set(&l->country, p + 1, m);
            for (char *c = l->country.text; *c; c++) *c = (char)toupper((unsigned char)*c);
            l->has_country = 1;
        }
    }
    const char *mod = strchr(p, '@');
    if (mod && !strcmp(mod, "@latin")) { set(&l->script, "Latn", 4); l->has_script = 1; }
    if (mod && !strcmp(mod, "@cyrillic")) { set(&l->script, "Cyrl", 4); l->has_script = 1; }

    char id[28];
    int len = snprintf(id, sizeof id, "%s%s%s%s%s", l->language.text, l->has_script ? "-" : "", l->script.text,
                       l->has_country ? "_" : "", l->country.text);
    set(&l->identifier, id, (size_t)len);
    return l;
}

static struct cflocale *locale_of(void *obj, const char *call)
{
    struct cflocale *l = obj;
    if (l && l->magic == CF_MAGIC && l->kind == CF_LOCALE) return l;
    darwin_unanswered(call, "not a locale made here");
    return NULL;
}

static const void *darwin_CFLocaleGetValue(void *locale, const void *key)
{
    struct cflocale *l = locale_of(locale, "CFLocaleGetValue");
    if (!l) return NULL;
    if (key == &key_language) return &l->language;
    if (key == &key_country) return l->has_country ? &l->country : NULL;
    if (key == &key_script) return l->has_script ? &l->script : NULL;
    darwin_unanswered("CFLocaleGetValue", "a key other than the language, country and script codes");
    return NULL;
}

static const void *darwin_CFLocaleGetIdentifier(void *locale)
{
    struct cflocale *l = locale_of(locale, "CFLocaleGetIdentifier");
    return l ? &l->identifier : NULL;
}

// The strings belong to their locale or are constants: only a locale is counted.
static void darwin_CFRelease(void *obj)
{
    struct cflocale *l = obj;
    if (!l || l->magic != CF_MAGIC) { darwin_unanswered("CFRelease", "not an object made here"); return; }
    if (l->kind != CF_LOCALE) return;
    if (atomic_fetch_sub(&l->refs, 1) != 1) return;
    l->magic = 0;
    free(l);
}

static long darwin_CFStringGetLength(const void *str)
{
    const struct cfstring *s = string_of(str, "CFStringGetLength");
    return s ? (long)strlen(s->text) : 0;
}

#define ENC_MACROMAN 0u
#define ENC_LATIN1   0x0201u
#define ENC_ASCII    0x0600u
#define ENC_UTF8     0x08000100u

static int byte_encoding(uint32_t enc, const char *call)
{
    if (enc == ENC_MACROMAN || enc == ENC_LATIN1 || enc == ENC_ASCII || enc == ENC_UTF8) return 1;
    darwin_unanswered(call, "an encoding other than MacRoman, Latin-1, ASCII and UTF-8");
    return 0;
}

// CFIndex CFStringGetMaximumSizeForEncoding(CFIndex length, CFStringEncoding)
static long darwin_CFStringGetMaximumSizeForEncoding(long length, uint32_t enc)
{
    if (!byte_encoding(enc, "CFStringGetMaximumSizeForEncoding")) return -1;    // kCFNotFound
    return enc == ENC_UTF8 ? length * 3 : length;
}

// CFIndex CFStringGetBytes(CFStringRef, CFRange, CFStringEncoding, UInt8 lossByte, Boolean isExternal,
//                          UInt8 *buffer, CFIndex maxBufLen, CFIndex *usedBufLen)
// The range is two longs in two registers. The characters converted; without a buffer the
// bytes they need are counted.
static long darwin_CFStringGetBytes(const void *str, long location, long length, uint32_t enc, uint32_t loss,
                                    uint32_t external, unsigned char *buffer, long max, long *used)
{
    const struct cfstring *s = string_of(str, "CFStringGetBytes");
    if (used) *used = 0;
    if (!s || !byte_encoding(enc, "CFStringGetBytes")) return 0;
    long have = (long)strlen(s->text);
    if (location < 0 || length < 0 || location > have || length > have - location) return 0;
    long n = length;
    if (buffer) {
        if (n > max) n = max < 0 ? 0 : max;
        memcpy(buffer, s->text + location, (size_t)n);
    }
    if (used) *used = n;
    return n;
}

// Boolean CFStringGetCString(CFStringRef, char *buffer, CFIndex bufferSize, CFStringEncoding)
static long darwin_CFStringGetCString(const void *str, char *buffer, long size, uint32_t enc)
{
    const struct cfstring *s = string_of(str, "CFStringGetCString");
    if (!s || !buffer || !byte_encoding(enc, "CFStringGetCString")) return 0;
    size_t n = strlen(s->text);
    if (size <= 0 || n + 1 > (size_t)size) return 0;
    memcpy(buffer, s->text, n + 1);
    return 1;
}

const struct darwin_sym darwin_syms_cf[] = {
    SHIM(CFLocaleCopyCurrent), SHIM(CFLocaleGetValue), SHIM(CFLocaleGetIdentifier), SHIM(CFRelease),
    SHIM(CFStringGetLength), SHIM(CFStringGetMaximumSizeForEncoding), SHIM(CFStringGetBytes), SHIM(CFStringGetCString),
    NAMED("_kCFLocaleLanguageCode", &kCFLocaleLanguageCode), NAMED("_kCFLocaleCountryCode", &kCFLocaleCountryCode),
    NAMED("_kCFLocaleScriptCode", &kCFLocaleScriptCode),
    { NULL, NULL }
};
