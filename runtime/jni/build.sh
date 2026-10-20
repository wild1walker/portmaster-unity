#!/bin/sh
# Build libfakejni.a (+ tests) in docker and run the unit tests (runtime/ is mounted: up_media.c
# includes ../media/um.h).
#   ARCH=armhf (default): Debian bookworm armhf cross compiler -> out/; the tests run in an arm/v7
#     container (Docker's emulation; the qemu-arm 7.2 inside the build image crashes in pthread_create)
#   ARCH=arm64: native build and tests in the ua-build-arm64 image (../docker/arm64) -> out/arm64/
# Usage: [ARCH=arm64] ./build.sh [make targets...]   (default: all + tests)
set -e
cd "$(dirname "$0")"
ARCH=${ARCH:-armhf}
if [ "$ARCH" = arm64 ]; then
  docker image inspect ua-build-arm64 >/dev/null 2>&1 || docker build --platform linux/arm64 -t ua-build-arm64 ../docker/arm64
  docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/jni ua-build-arm64 make ARCH=arm64 ${@:-all test}
  exit
fi
docker image inspect ua-jni-build >/dev/null 2>&1 || docker build -t ua-jni-build docker
docker run --rm -v "$PWD/..":/w -w /w/jni ua-jni-build make ${@:-all}
if [ $# -eq 0 ]; then
  docker run --rm --platform linux/arm/v7 -v "$PWD":/src -w /src debian:bookworm-slim \
    sh -c './out/test_fakejni && ./out/test_unityplayer' 2>&1 | grep -E 'PASSED|FAILED|FAIL '
fi
