// unityverts.c's unit test: its transform's output layout and flags on small cases worked out by
// hand (positions through an affine matrix, the mirrored axes, the constant normal and tangent, the
// colour, the other attributes, the zero padding), and that a call writes exactly out_size bytes. That
// it equals Unity's own function is checked on real game data by UA_FAST_VERTS=check. Run by
// unityhooks_test.sh.
#include <stdio.h>
#include <string.h>

#include "../unityverts.c"

int unityhooks_matches(const struct uh_row *r, uintptr_t base, size_t size) { (void)r; (void)base; (void)size; return 0; }
int unityhooks_hook(uintptr_t fn, char isa, void *to, void **orig) { (void)fn; (void)isa; (void)to; (void)orig; return -1; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static float f_at(const unsigned char *p, int i) { float f; memcpy(&f, p + 4 * i, 4); return f; }

int main(void)
{
    // column-major: scale 2, 3, 4 and move by 10, 20, 30
    float m[16] = { 2, 0, 0, 0,  0, 3, 0, 0,  0, 0, 4, 0,  10, 20, 30, 1 };
    float m3[9] = { 1, 0, 0,  0, 1, 0,  0.5f, -0.25f, 1 };
    float src[2][4] = { { 1, 2, 3, 99 }, { -1, 0.5f, 0, 99 } };      // (a 16-byte stride: the 4th float is not read)
    unsigned char extra[2][12] = { { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 }, { 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24 } };
    unsigned char out[256];
    struct { unsigned flags; int extra_size; const char *what; } cases[] = {
        { 0, 0, "positions" }, { 16, 8, "colour and uv" }, { 8 | 64 | 16 | 32, 12, "normal, tangent, colour, extra, pad" },
        { 512, 0, "mirrored x" }, { 1024 | 256 | 8 | 64, 4, "mirrored y, the 256 signs" }, { 1536 | 64 | 2048, 0, "both mirrored, tangent w" },
    };
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        unsigned fl = cases[k].flags;
        int es = cases[k].extra_size;
        memset(out, 0xEE, sizeof out);
        fast_loop(out, m, m3, 0, 2, (const unsigned char *)src, 16, es ? extra[0] : NULL, 12, es, fl, 0xAABBCCDDu);
        size_t n = out_size(2, es ? extra[0] : NULL, es, fl), per = n / 2;
        CHECK(out[n] == 0xEE, "%s: wrote past %zu bytes", cases[k].what, n);
        for (int v = 0; v < 2; v++) {
            const unsigned char *o = out + per * v;
            float sx = fl & 512 ? -1 : 1, sy = fl & 1024 ? -1 : 1;
            float x = src[v][0], y = src[v][1], z = src[v][2];
            CHECK(f_at(o, 0) == 10 + 2 * sx * x && f_at(o, 1) == 20 + 3 * sy * y && f_at(o, 2) == 30 + 4 * z,
                  "%s: vertex %d at %g %g %g", cases[k].what, v, f_at(o, 0), f_at(o, 1), f_at(o, 2));
            int i = 3;
            if (fl & 8) {
                float s = fl & 256 ? -1 : 1;
                CHECK(f_at(o, i) == s * 0.5f && f_at(o, i + 1) == s * -0.25f && f_at(o, i + 2) == s * 1, "%s: normal", cases[k].what);
                i += 3;
            }
            if (fl & 64) {
                float s = fl & 256 ? -1 : 1, t = (fl & 1536) == 1536 || (fl & 1536) == 512 ? -1 : 1;
                if ((fl & 1536) == 1024 || (fl & 1536) == 512) s = -s;
                float w = fl & 2048 ? -s : s;
                CHECK(f_at(o, i) == t * 2 && f_at(o, i + 1) == 0 && f_at(o, i + 2) == 0 && f_at(o, i + 3) == w,
                      "%s: tangent %g %g %g %g", cases[k].what, f_at(o, i), f_at(o, i + 1), f_at(o, i + 2), f_at(o, i + 3));
                i += 4;
            }
            const unsigned char *p = o + 4 * i;
            if (fl & 16) { unsigned c; memcpy(&c, p, 4); CHECK(c == 0xAABBCCDDu, "%s: colour", cases[k].what); p += 4; }
            if (es) { CHECK(!memcmp(p, extra[v], (size_t)es), "%s: extra", cases[k].what); p += es; }
            if (fl & 32) { static const unsigned char z8[8]; CHECK(!memcmp(p, z8, 8), "%s: padding", cases[k].what); p += 8; }
            CHECK((size_t)(p - o) == per, "%s: %zu bytes a vertex, out_size says %zu", cases[k].what, (size_t)(p - o), per);
        }
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("unityverts: ok\n");
    return 0;
}
