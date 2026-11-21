#!/bin/sh
# A turned panel's present (test_rotate.c: fbdev.c's UA_ROTATE choice, display.c's turned draw of
# glpresent.c's frame, read back) on Mesa llvmpipe's surfaceless platform, aarch64 (ua-display-test,
# made by display_test.sh).
set -e
cd "$(dirname "$0")/../.."
SRC="host/test/test_rotate.c host/display.c host/display_sdl.c host/glpresent.c host/fbdev.c host/gldriver.c android/egl_glue.c android/native_window.c android/log_shim.c"
docker run --rm --platform linux/arm64 -v "$PWD":/w:ro -w /w -e SRC="$SRC" ua-display-test sh -c '
  set -e
  gcc -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -o /tmp/test_rotate $SRC -lSDL2 -ldl -lpthread
  for k in 0 1 2 3 4 5; do EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 /tmp/test_rotate $k; done'
