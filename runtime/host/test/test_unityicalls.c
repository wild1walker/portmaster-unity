// unityicalls.c's unit test: Shader.PropertyToID's kept numbers against a stand-in for Unity's call (a table of
// names that gives each new name the next number): every answer equals the stand-in's for the same name, the
// stand-in is asked once per name on UnityMain, every time on another thread, for a null string and for a name of
// more than 64 characters; strings of the same characters at other addresses share the number. Mono 2.6 string
// layout: header 8 bytes, length at +8, UTF-16 at +12. Run by unityhooks_test.sh (32-bit only: the player's layout).
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "../unityicalls.c"

#if UINTPTR_MAX > 0xffffffffu
int main(void) { printf("SKIPPED unityicalls: the ARMv7 player's layout (32-bit only)\n"); return 0; }
#else
static int fails, asked;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static char names[200][80]; static int nnames;
static int32_t f_unity(const void *str)
{
    asked++;
    if (!str) return -1;
    int32_t len = *(const int32_t *)((const char *)str + 8);
    const uint16_t *c = (const uint16_t *)((const char *)str + 12);
    char a[200]; for (int i = 0; i < len && i < 199; i++) a[i] = (char)c[i]; a[len < 199 ? len : 199] = 0;
    for (int i = 0; i < nnames; i++) if (!strcmp(names[i], a)) return i;
    snprintf(names[nnames], sizeof names[nnames], "%s", a); return nnames++;
}
static uint8_t buf[64][12 + 2 * 100];
static const void *mstr(int slot, const char *s)
{
    int32_t len = (int32_t)strlen(s);
    memset(buf[slot], 0, sizeof buf[slot]);
    memcpy(buf[slot] + 8, &len, 4);
    for (int i = 0; i < len; i++) { uint16_t ch = (uint8_t)s[i]; memcpy(buf[slot] + 12 + 2 * i, &ch, 2); }
    return buf[slot];
}
static int32_t want(const char *s) { for (int i = 0; i < nnames; i++) if (!strcmp(names[i], s)) return i; return -2; }
static void *other(void *u)
{
    (void)u;
    int before = asked;
    property_to_id(mstr(40, "_MainTex"));
    property_to_id(mstr(41, "_MainTex"));
    CHECK(asked == before + 2, "another thread's calls were answered from the kept numbers");
    return NULL;
}

int main(void)
{
    o_property_to_id = f_unity;
    // before the first frame: Unity's every time
    property_to_id(mstr(0, "_Color")); property_to_id(mstr(1, "_Color"));
    CHECK(asked == 2, "kept before the first frame");
    unityicalls_frame();
    const char *words[] = { "_Color", "_MainTex", "_Cutoff", "_Glow", "_LightColor0", "_Time", "_Tint", "_Color2", "_Fade" };
    int n = sizeof words / sizeof *words;
    for (int round = 0; round < 5; round++)
        for (int k = 0; k < n; k++) {
            int before = asked;
            int32_t got = property_to_id(mstr(2 + (round * n + k) % 30, words[k]));
            CHECK(got == want(words[k]), "%s: %d, Unity's %d", words[k], got, want(words[k]));
            if (round > 0) CHECK(asked == before, "%s asked Unity again (round %d)", words[k], round);
        }
    int before = asked;
    CHECK(property_to_id(NULL) == -1 && asked == before + 1, "a null string");
    char longname[100]; memset(longname, 'a', 70); longname[70] = 0;
    before = asked;
    int32_t l1 = property_to_id(mstr(50, longname)), l2 = property_to_id(mstr(51, longname));
    CHECK(l1 == l2 && asked == before + 2, "a long name was kept");
    pthread_t t; pthread_create(&t, NULL, other, NULL); pthread_join(t, NULL);
    printf("%s unityicalls: %d failure(s) (%d asked of Unity)\n", fails ? "FAILED" : "PASSED", fails, asked);
    return fails != 0;
}
#endif
