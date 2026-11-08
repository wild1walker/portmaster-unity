#!/bin/sh
# unityhooks.c's unit test (test_unityhooks.c: the jump, the trampoline and the row check on a
# stand-in function in a page of its own; fake_adjust.S) and unitypool.c's (test_unitypool.c: its
# hooks over a model of Unity's GLES buffer pool), unityverts.c's (test_unityverts.c) and unitymath.c's
# (test_unitymath.c).
# ARCH=armhf (default): built in ua-host-armhf (Thumb-2, as ua_host: the jump's interworking), run in
# an arm/v7 container; ARCH=arm64: ua-build-arm64. The binaries also run on the handheld.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-armhf}
build='set -e; mkdir -p $O; $CC -O2 -g -Wall -Wextra -o $O/test_unityhooks test/test_unityhooks.c test/fake_adjust.S
  $CC -O2 -g -Wall -Wextra -o $O/test_unitypool test/test_unitypool.c
  $CC -O2 -g -Wall -Wextra -ffp-contract=off -o $O/test_unityverts test/test_unityverts.c
  $CC -O2 -g -Wall -Wextra -o $O/test_unitymath test/test_unitymath.c -lpthread'
run='$O/test_unityhooks && $O/test_unitypool && $O/test_unityverts && $O/test_unitymath'
if [ "$ARCH" = arm64 ]; then
  O=out/arm64/test
  docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c "O=$O CC=gcc; $build; $run"
else
  O=out/test
  docker run --rm -v "$PWD/..":/w -w /w/host ua-host-armhf sh -c "O=$O CC='arm-linux-gnueabihf-gcc -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard'; $build"
  docker run --rm --platform linux/arm/v7 -v "$PWD/..":/w -w /w/host debian:bookworm-slim sh -c "O=$O; $run"
fi
