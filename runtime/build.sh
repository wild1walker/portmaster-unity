#!/bin/sh
# Builds the runtime in Docker: loader -> android shims -> fake JNI -> media core -> ua_host and the
# device checks, for AArch64 (arm64-v8a players): native builds in the ua-build-arm64 image ->
# */out/arm64/, host/out/arm64/ua_host. (32-bit armhf was removed; the last tree with it is the tag
# last-armhf.)
# TESTS=1 also runs the fake JNI's unit tests.
# OPENH264=0 builds the media core without OpenH264 (media/README.md).
set -e
cd "$(dirname "$0")"
ARCH=${ARCH:-arm64}
[ "$ARCH" = arm64 ] || { echo "ARCH must be arm64 (armhf was removed: tag last-armhf)"; exit 1; }
export ARCH
[ -f thirdparty/libhybris/hybris/common/hooks.c ] || {
  echo "libhybris missing: git submodule update --init (runtime/thirdparty/libhybris)"; exit 1; }
bash loader/build.sh
bash android/build.sh
if [ "$TESTS" = 1 ]; then bash jni/build.sh all test; else bash jni/build.sh all; fi
bash media/build.sh
bash host/build.sh
echo "runtime built: host/out/arm64/ua_host"
