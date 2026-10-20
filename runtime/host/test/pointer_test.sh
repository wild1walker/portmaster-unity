#!/bin/sh
# pointer.c's unit test (test_pointer.c with fake_gles.c as libGLESv2.so.2), each scenario in its
# own process. ARCH=armhf (default): built in ua-host-armhf, run in an arm/v7 container;
# ARCH=arm64: built and run in ua-build-arm64.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-armhf}
run='set -e
  mkdir -p $O
  $CC -O2 -g -Wall -Wextra -Wno-unused-parameter -shared -fPIC -Wl,-soname,libGLESv2.so.2 -o $O/libGLESv2.so.2 test/fake_gles.c
  $CC -O2 -g -Wall -Wextra -Wno-unused-parameter -I../jni -o $O/test_pointer test/test_pointer.c pointer.c $O/libGLESv2.so.2 -ldl -lm'
if [ "$ARCH" = arm64 ]; then
  docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c "O=out/arm64/test CC=gcc; $run
    for s in off mouse touch keymap toggle; do LD_LIBRARY_PATH=\$O \$O/test_pointer \$s; done"
  exit
fi
docker run --rm -v "$PWD/..":/w -w /w/host ua-host-armhf sh -c "O=out/test CC=arm-linux-gnueabihf-gcc; $run"
docker run --rm --platform linux/arm/v7 -v "$PWD/..":/w -w /w/host debian:bookworm-slim sh -c \
  'for s in off mouse touch keymap toggle; do LD_LIBRARY_PATH=out/test out/test/test_pointer $s; done'
