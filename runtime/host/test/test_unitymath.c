// unitymath.c's unit test: the memo over a stand-in inverse that counts its calls (a diagonal
// matrix's inverse; anything with a zero on the diagonal singular, out left alone): a repeated input
// gets the same bytes and bool without a call, in place (m == out) included; a singular input is
// computed every time; the four last inputs kept, the oldest replaced; each thread its own. Run by
// unityhooks_test.sh.
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "../unitymath.c"

int unityhooks_matches(const struct uh_row *r, uintptr_t base, size_t size) { (void)r; (void)base; (void)size; return 0; }
int unityhooks_hook(uintptr_t fn, char isa, void *to, void **orig) { (void)fn; (void)isa; (void)to; (void)orig; return -1; }

static int fails, calls;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static int fake_inv(const float *m, float *out)
{
    calls++;
    float d[4];
    for (int i = 0; i < 4; i++) { d[i] = m[i * 5]; if (d[i] == 0) return 0; }
    memset(out, 0, 64);
    for (int i = 0; i < 4; i++) out[i * 5] = 1 / d[i];
    return 1;
}
static void diag(float *m, float a, float b, float c, float d) { memset(m, 0, 64); m[0] = a; m[5] = b; m[10] = c; m[15] = d; }

static void *thread_main(void *u)
{
    (void)u;
    float m[16], o[16];
    diag(m, 2, 4, 8, 16);
    int before = calls;
    inv_memo(m, o);                              // (another thread: its own memo, empty)
    CHECK(calls == before + 1, "a new thread's first call was not computed");
    return NULL;
}

int main(void)
{
    o_inv = fake_inv;
    float m[16], o[16], o2[16];
    diag(m, 2, 4, 8, 16);
    CHECK(inv_memo(m, o) == 1 && calls == 1, "first call");
    memset(o2, 0xEE, sizeof o2);
    CHECK(inv_memo(m, o2) == 1 && calls == 1, "a repeated input was computed again");
    CHECK(!memcmp(o, o2, 64), "a repeated input's output differs");
    // in place: the input kept is the one before the call
    float p[16];
    diag(p, 2, 4, 8, 16);
    CHECK(inv_memo(p, p) == 1 && calls == 1 && !memcmp(p, o, 64), "in place from the memo");
    diag(p, 3, 5, 7, 9);
    inv_memo(p, p);
    CHECK(calls == 2, "a new input in place was not computed");
    float q[16]; diag(q, 3, 5, 7, 9);
    float o3[16];
    CHECK(inv_memo(q, o3) == 1 && calls == 2 && !memcmp(o3, p, 64), "the in-place input was not kept as it was before the call");
    // singular: computed every time, out left as it was
    float s[16]; diag(s, 1, 0, 1, 1);
    memset(o2, 0x55, sizeof o2);
    CHECK(inv_memo(s, o2) == 0 && calls == 3, "singular");
    CHECK(inv_memo(s, o2) == 0 && calls == 4, "a singular input came from the memo");
    for (int i = 0; i < 16; i++) { unsigned u; memcpy(&u, &o2[i], 4); CHECK(u == 0x55555555u, "a singular call wrote out"); }
    // four kept: m and q are two of them; three new ones push m out
    for (int k = 0; k < 3; k++) { float n[16]; diag(n, 10 + k, 1, 1, 1); inv_memo(n, o2); }
    int before = calls;
    inv_memo(q, o2);
    CHECK(calls == before, "the fourth newest input was not kept");
    inv_memo(m, o2);
    CHECK(calls == before + 1, "the oldest input was still kept");
    pthread_t t; pthread_create(&t, NULL, thread_main, NULL); pthread_join(t, NULL);
    printf("%s unitymath: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
