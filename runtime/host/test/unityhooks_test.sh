#!/bin/sh
# unityhooks.c's unit test (test_unityhooks.c: the jump, the trampoline and the row check on a
# stand-in function in a page of its own; fake_adjust.S) and unitypool.c's (test_unitypool.c: its
# hooks over a model of Unity's GLES buffer pool), unityverts.c's (test_unityverts.c),
# unityprefetch.c's (test_unityprefetch.c: its stubs), nativebeh.c's (test_nativebeh.c: its loop
# against a model of Unity's), unityjobs.c's (test_unityjobs.c: its wait against a model of Unity
# 2017.4's job queue) and enginerw.c's (test_anchorwalk.c, test_cullstep1.c: the anchor walk and the culling pass
# against models of Unity's) and unityphys2d.c's (test_unityphys2d.c: the contact-map flag and the
# wake walk against models of Unity 2017.4's). Built and run in ua-build-arm64 (AArch64); the binaries
# also run on the handheld.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-arm64}
[ "$ARCH" = arm64 ] || { echo "ARCH must be arm64 (armhf was removed: tag last-armhf)"; exit 1; }
build='set -e; mkdir -p $O; $CC -O2 -g -Wall -Wextra -o $O/test_unityhooks test/test_unityhooks.c test/fake_adjust.S
  $CC -O2 -g -Wall -Wextra -o $O/test_unitypool test/test_unitypool.c
  $CC -O2 -g -Wall -Wextra -ffp-contract=off -o $O/test_unityverts test/test_unityverts.c
  $CC -O2 -g -Wall -Wextra -o $O/test_unityprefetch test/test_unityprefetch.c
  $CC -O2 -g -Wall -Wextra -o $O/test_nativebeh test/test_nativebeh.c
  $CC -O2 -g -Wall -Wextra -o $O/test_unityjobs test/test_unityjobs.c -lpthread
  $CC -O2 -g -Wall -Wextra -o $O/test_anchorwalk test/test_anchorwalk.c unityprefetch.c
  $CC -O2 -g -Wall -Wextra -o $O/test_cullstep1 test/test_cullstep1.c unityprefetch.c
  $CC -O2 -g -Wall -Wextra -o $O/test_unityphys2d test/test_unityphys2d.c -lpthread'
run='$O/test_unityhooks && $O/test_unitypool && $O/test_unityverts && $O/test_unityprefetch && $O/test_nativebeh && $O/test_unityjobs && $O/test_anchorwalk && $O/test_cullstep1 && $O/test_unityphys2d'
O=out/arm64/test
docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c "O=$O CC=gcc; $build; $run"
