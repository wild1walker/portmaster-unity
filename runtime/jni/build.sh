#!/bin/sh
# Build libfakejni.a (+ tests) in docker and run the unit tests (runtime/ is mounted: up_media.c
# includes ../media/um.h): native build and tests in the ua-build-arm64 image (../docker/arm64) ->
# out/arm64/. (32-bit armhf was removed; the last tree with it is the tag last-armhf.)
# Usage: ./build.sh [make targets...]   (default: all + tests)
set -e
cd "$(dirname "$0")"
ARCH=${ARCH:-arm64}
[ "$ARCH" = arm64 ] || { echo "ARCH must be arm64 (armhf was removed: tag last-armhf)"; exit 1; }
docker image inspect ua-build-arm64 >/dev/null 2>&1 || docker build --platform linux/arm64 -t ua-build-arm64 ../docker/arm64
docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/jni ua-build-arm64 make ARCH=arm64 ${@:-all test}
