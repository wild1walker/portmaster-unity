#!/bin/sh
# gpuidle.c's unit test (test_gpuidle.c: the idle accounting; the fences themselves need a GPU).
# ARCH=armhf (default): built in ua-host-armhf, run in an arm/v7 container; ARCH=arm64: ua-build-arm64.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-armhf}
build='set -e; mkdir -p $O; $CC -O2 -g -Wall -Wextra -o $O/test_gpuidle test/test_gpuidle.c gpuidle.c -ldl -lpthread -lm'
if [ "$ARCH" = arm64 ]; then
  O=out/arm64/test
  docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c "O=$O CC=gcc; $build; $O/test_gpuidle"
else
  O=out/test
  docker run --rm -v "$PWD/..":/w -w /w/host ua-host-armhf sh -c "O=$O CC=arm-linux-gnueabihf-gcc; $build"
  docker run --rm --platform linux/arm/v7 -v "$PWD/..":/w -w /w/host debian:bookworm-slim sh -c "$O/test_gpuidle"
fi
