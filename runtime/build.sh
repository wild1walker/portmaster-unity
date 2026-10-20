#!/bin/sh
# Builds the runtime in Docker: loader -> android shims -> fake JNI -> media core -> ua_host and the
# device checks.
#   ARCH=armhf (default): 32-bit ARM (armeabi-v7a players), cross-compiled -> */out/, host/out/ua_host
#   ARCH=arm64: AArch64 (arm64-v8a players), native builds in the ua-build-arm64 image -> */out/arm64/,
#     host/out/arm64/ua_host (linked once the loader builds loader/out/arm64/libsoloader.a)
# Each part's build.sh reads ARCH. TESTS=1 also runs the fake JNI's unit tests: armhf under qemu-arm
# (some qemu-user builds crash on their threads; the binaries, jni/out/test_*, also run on the
# handheld itself: LD_LIBRARY_PATH=/lib32:/usr/lib32 ./test_fakejni), arm64 natively.
set -e
cd "$(dirname "$0")"
ARCH=${ARCH:-armhf}
case "$ARCH" in armhf|arm64) ;; *) echo "ARCH must be armhf or arm64"; exit 1 ;; esac
export ARCH
[ -f thirdparty/libhybris/hybris/common/hooks.c ] || {
  echo "libhybris missing: git submodule update --init (runtime/thirdparty/libhybris)"; exit 1; }
loader/build.sh
android/build.sh
T=test-qemu; OUT=host/out
[ "$ARCH" = arm64 ] && { T=test; OUT=host/out/arm64; }
if [ "$TESTS" = 1 ]; then jni/build.sh all $T; else jni/build.sh all; fi
media/build.sh
host/build.sh
echo "runtime built: $OUT/ua_host"
