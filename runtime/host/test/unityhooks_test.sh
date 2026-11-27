#!/bin/sh
# unityhooks.c's unit test (test_unityhooks.c: the jump, the trampoline and the row check on a
# stand-in function in a page of its own; fake_adjust.S) and unitypool.c's (test_unitypool.c: its
# hooks over a model of Unity's GLES buffer pool), unityverts.c's (test_unityverts.c), unitymath.c's
# (test_unitymath.c), unitybeh.c's (test_unitybeh.c), unityscripts.c's (test_unityscripts.c), unityicalls.c's
# (test_unityicalls.c) and unitylayers.c's (test_unitylayers.c; these four 32-bit only, the ARMv7 player's layout),
# unityprefetch.c's (test_unityprefetch.c: its stubs, AArch64 only) and nativebeh.c's (test_nativebeh.c: its loop
# against a model of Unity's, AArch64 only) and unityjobs.c's (test_unityjobs.c: its wait against a model of Unity
# 2017.4's job queue).
# ARCH=armhf (default): built in ua-host-armhf (Thumb-2, as ua_host: the jump's interworking), run in
# an arm/v7 container; ARCH=arm64: ua-build-arm64. The binaries also run on the handheld.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-armhf}
build='set -e; mkdir -p $O; $CC -O2 -g -Wall -Wextra -o $O/test_unityhooks test/test_unityhooks.c test/fake_adjust.S
  $CC -O2 -g -Wall -Wextra -o $O/test_unitypool test/test_unitypool.c
  $CC -O2 -g -Wall -Wextra -ffp-contract=off -o $O/test_unityverts test/test_unityverts.c
  $CC -O2 -g -Wall -Wextra -o $O/test_unitymath test/test_unitymath.c -lpthread
  $CC -O2 -g -Wall -Wextra -o $O/test_unitybeh test/test_unitybeh.c
  $CC -O2 -g -Wall -Wextra -o $O/test_unityscripts test/test_unityscripts.c
  $CC -O2 -g -Wall -Wextra -o $O/test_unityicalls test/test_unityicalls.c -lpthread
  $CC -O2 -g -Wall -Wextra -o $O/test_unitylayers test/test_unitylayers.c
  $CC -O2 -g -Wall -Wextra -o $O/test_unityprefetch test/test_unityprefetch.c
  $CC -O2 -g -Wall -Wextra -o $O/test_nativebeh test/test_nativebeh.c
  $CC -O2 -g -Wall -Wextra -o $O/test_unityjobs test/test_unityjobs.c -lpthread'
run='$O/test_unityhooks && $O/test_unitypool && $O/test_unityverts && $O/test_unitymath && $O/test_unitybeh && $O/test_unityscripts && $O/test_unityicalls && $O/test_unitylayers && $O/test_unityprefetch && $O/test_nativebeh && $O/test_unityjobs'
if [ "$ARCH" = arm64 ]; then
  O=out/arm64/test
  docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c "O=$O CC=gcc; $build; $run"
else
  O=out/test
  docker run --rm -v "$PWD/..":/w -w /w/host ua-host-armhf sh -c "O=$O CC='arm-linux-gnueabihf-gcc -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard'; $build"
  docker run --rm --platform linux/arm/v7 -v "$PWD/..":/w -w /w/host debian:bookworm-slim sh -c "O=$O; $run"
fi
