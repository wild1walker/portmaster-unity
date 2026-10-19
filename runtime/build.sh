#!/bin/sh
# Builds the runtime for armhf in Docker: loader -> android shims -> fake JNI -> ua_host and the
# device checks (runtime/host/out/). TESTS=1 also runs the fake JNI's unit tests under qemu-arm
# (some qemu-user builds crash on their threads; the binaries, jni/out/test_*, also run on the
# handheld itself: LD_LIBRARY_PATH=/lib32:/usr/lib32 ./test_fakejni).
set -e
cd "$(dirname "$0")"
[ -f thirdparty/libhybris/hybris/common/hooks.c ] || {
  echo "libhybris missing: git submodule update --init (runtime/thirdparty/libhybris)"; exit 1; }
loader/build.sh
android/build.sh
if [ "$TESTS" = 1 ]; then jni/build.sh all test-qemu; else jni/build.sh all; fi
host/build.sh
echo "runtime built: host/out/ua_host"
