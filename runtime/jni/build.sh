#!/bin/sh
# Build out/libfakejni.a (+ tests) with the Debian bookworm armhf cross compiler in docker and
# run the unit tests in an arm/v7 container (Docker's emulation; the qemu-arm 7.2 inside the
# build image crashes in pthread_create). Usage: ./build.sh [make targets...]   (default: all + tests)
set -e
cd "$(dirname "$0")"
docker image inspect ua-jni-build >/dev/null 2>&1 || docker build -t ua-jni-build docker
docker run --rm -v "$PWD":/src -w /src ua-jni-build make ${@:-all}
if [ $# -eq 0 ]; then
  docker run --rm --platform linux/arm/v7 -v "$PWD":/src -w /src debian:bookworm-slim \
    sh -c './out/test_fakejni && ./out/test_unityplayer' 2>&1 | grep -E 'PASSED|FAILED|FAIL '
fi
