// gldriver.c's frame copy (UA_GL_DRIVER=mesa's present: glReadPixels' bottom-up rows into the
// framebuffer's top-down buffer, red and blue swapped for a BGRA framebuffer the driver cannot read
// as BGRA) and its switch staying off unless asked (pointer_test.sh).
#include "../gldriver.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fbdev_buffers(void) { return 0; }
void *egl_glue_real(const char *name) { (void)name; return NULL; }
void egl_glue_set_present(int (*present)(void *, void *), int w, int h) { (void)present; (void)w; (void)h; }

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    // 3 x 2 pixels, RGBA bytes; row 0 is GL's bottom row
    uint8_t src[2][3][4];
    for (int y = 0; y < 2; y++) for (int x = 0; x < 3; x++) { src[y][x][0] = 10 * y + x; src[y][x][1] = 100; src[y][x][2] = 200 + x; src[y][x][3] = 255; }
    uint8_t dst[2][5][4];                                   // a 5-pixel line: the stride is larger
    memset(dst, 0xee, sizeof dst);
    gldriver_copy_rows(&dst[0][0][0], sizeof dst[0], &src[0][0][0], 3, 2, 0);
    for (int x = 0; x < 3; x++) {
        CHECK(!memcmp(dst[0][x], src[1][x], 4));            // the top line is GL's top row
        CHECK(!memcmp(dst[1][x], src[0][x], 4));
    }
    CHECK(dst[0][3][0] == 0xee && dst[1][4][3] == 0xee);    // past the width: untouched
    memset(dst, 0xee, sizeof dst);
    gldriver_copy_rows(&dst[0][0][0], sizeof dst[0], &src[0][0][0], 3, 2, 1);
    for (int x = 0; x < 3; x++) {
        CHECK(dst[0][x][0] == src[1][x][2] && dst[0][x][1] == src[1][x][1] && dst[0][x][2] == src[1][x][0] && dst[0][x][3] == src[1][x][3]);
        CHECK(dst[1][x][0] == src[0][x][2] && dst[1][x][2] == src[0][x][0]);
    }
    unsetenv("UA_GL_DRIVER");
    CHECK(gldriver_mesa() == 0);                            // the blob unless asked
    char *argv[] = { "test_gldriver", NULL };
    setenv("UA_GL_DRIVER", "mesa", 1);
    setenv("UA_MESA_DIR", "/nonexistent", 1);
    gldriver_early(argv);                                   // no Mesa there: returns, the blob
    CHECK(getenv("UA_GL_DRIVER_MESA_DIR") == NULL && getenv("UA_EGL_PBUFFER") == NULL);
    if (fails) return 1;
    printf("ok gldriver\n");
    return 0;
}
