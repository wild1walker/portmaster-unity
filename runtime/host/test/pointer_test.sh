#!/bin/sh
# pointer.c's and osk.c's unit tests (test_pointer.c, test_osk.c with fake_gles.c as libGLESv2.so.2), each scenario in its
# own process, msaafix.c's (test_msaafix.c with fake_msaa_gl.c as its libGLESv2.so.2), gltrace.c's (test_gltrace.c with
# fake_trace_gl.c), glopt.c's (test_glopt.c with fake_opt_gl.c) and glpass.c's (test_glpass.c with fake_pass_gl.c).
# ARCH=armhf (default): built in
# ua-host-armhf, run in an arm/v7 container; ARCH=arm64: built and run in ua-build-arm64.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-armhf}
PASS_TESTS="same late drawn colour partial mask dither upload delete invalidate blit otherfb depth inval_late renderbuffer foreign nothread off"
RENAME_TESTS="grab copyback notcopy params queries delete context near nearoff check off"
run='set -e
  mkdir -p $O
  $CC -O2 -g -Wall -Wextra -Wno-unused-parameter -shared -fPIC -Wl,-soname,libGLESv2.so.2 -o $O/libGLESv2.so.2 test/fake_gles.c
  $CC -O2 -g -Wall -Wextra -Wno-unused-parameter -I../jni -o $O/test_pointer test/test_pointer.c pointer.c $O/libGLESv2.so.2 -ldl -lm
  $CC -O2 -g -Wall -Wextra -Wno-unused-parameter -I../jni -o $O/test_osk test/test_osk.c osk.c $O/libGLESv2.so.2 -ldl -lm -lpthread
  mkdir -p $O/msaa
  $CC -O2 -g -Wall -Wextra -shared -fPIC -Wl,-soname,libGLESv2.so.2 -o $O/msaa/libGLESv2.so.2 test/fake_msaa_gl.c
  $CC -O2 -g -Wall -Wextra -o $O/test_msaafix test/test_msaafix.c msaafix.c $O/msaa/libGLESv2.so.2 -ldl
  mkdir -p $O/trace
  $CC -O2 -g -Wall -Wextra -shared -fPIC -Wl,-soname,libGLESv2.so.2 -o $O/trace/libGLESv2.so.2 test/fake_trace_gl.c
  $CC -O2 -g -Wall -Wextra -Wno-unused-parameter -o $O/test_gltrace test/test_gltrace.c gltrace.c $O/trace/libGLESv2.so.2 -ldl -lpthread
  mkdir -p $O/opt
  $CC -O2 -g -Wall -Wextra -shared -fPIC -Wl,-soname,libGLESv2.so.2 -o $O/opt/libGLESv2.so.2 test/fake_opt_gl.c
  $CC -O2 -g -Wall -Wextra -Wno-unused-parameter -o $O/test_glopt test/test_glopt.c glopt.c $O/opt/libGLESv2.so.2 -ldl -lpthread
  mkdir -p $O/pass
  $CC -O2 -g -Wall -Wextra -shared -fPIC -Wl,-soname,libGLESv2.so.2 -o $O/pass/libGLESv2.so.2 test/fake_pass_gl.c
  $CC -O2 -g -Wall -Wextra -Wno-unused-parameter -o $O/test_glpass test/test_glpass.c glpass.c $O/pass/libGLESv2.so.2 -ldl -lpthread
  mkdir -p $O/rename
  $CC -O2 -g -Wall -Wextra -shared -fPIC -Wl,-soname,libGLESv2.so.2 -o $O/rename/libGLESv2.so.2 test/fake_rename_gl.c
  $CC -O2 -g -Wall -Wextra -Wno-unused-parameter -o $O/test_glrename test/test_glrename.c glrename.c $O/rename/libGLESv2.so.2 -ldl -lpthread'
if [ "$ARCH" = arm64 ]; then
  docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c "O=out/arm64/test CC=gcc; $run
    for s in off mouse touch keymap toggle es2; do LD_LIBRARY_PATH=\$O \$O/test_pointer \$s; done
    for s in off type draw es2 msaa pads done cancel 0; do LD_LIBRARY_PATH=\$O \$O/test_osk \$s; done
    for s in off on; do LD_LIBRARY_PATH=\$O/msaa \$O/test_msaafix \$s; done
    for s in draws off limit gpu gpu2 gpuoff game; do LD_LIBRARY_PATH=\$O/trace \$O/test_gltrace \$s; done
    for s in restore depthwrite stencilwrite stencilkeep partial unityclear colourclear invalidate delete clearbuffer blit renderbuffer sameclear sameclear_off nothread off; do LD_LIBRARY_PATH=\$O/opt \$O/test_glopt \$s; done
    for s in $PASS_TESTS; do LD_LIBRARY_PATH=\$O/pass \$O/test_glpass \$s; done
    for s in $RENAME_TESTS; do LD_LIBRARY_PATH=\$O/rename \$O/test_glrename \$s; done"
  exit
fi
docker run --rm -v "$PWD/..":/w -w /w/host ua-host-armhf sh -c "O=out/test CC=arm-linux-gnueabihf-gcc; $run"
docker run --rm --platform linux/arm/v7 -v "$PWD/..":/w -w /w/host debian:bookworm-slim sh -c \
  'for s in off mouse touch keymap toggle es2; do LD_LIBRARY_PATH=out/test out/test/test_pointer $s; done
   for s in off type draw es2 msaa pads done cancel 0; do LD_LIBRARY_PATH=out/test out/test/test_osk $s; done
   for s in off on; do LD_LIBRARY_PATH=out/test/msaa out/test/test_msaafix $s; done
   for s in draws off limit gpu gpu2 gpuoff game; do LD_LIBRARY_PATH=out/test/trace out/test/test_gltrace $s; done
   for s in restore depthwrite stencilwrite stencilkeep partial unityclear colourclear invalidate delete clearbuffer blit renderbuffer sameclear sameclear_off nothread off; do LD_LIBRARY_PATH=out/test/opt out/test/test_glopt $s; done
   for s in '"$PASS_TESTS"'; do LD_LIBRARY_PATH=out/test/pass out/test/test_glpass $s; done
   for s in '"$RENAME_TESTS"'; do LD_LIBRARY_PATH=out/test/rename out/test/test_glrename $s; done'
