#!/bin/sh
# threads.c's unit test (test_threads.c: UA_AFFINITY / UA_NICE rules applied to named threads).
# ARCH=armhf (default): built in ua-host-armhf, run in an arm/v7 container; ARCH=arm64: ua-build-arm64.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-armhf}
build='set -e; mkdir -p $O; $CC -O2 -g -Wall -Wextra -o $O/test_threads test/test_threads.c threads.c -lpthread'
if [ "$ARCH" = arm64 ]; then
  O=out/arm64/test
  docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c "O=$O CC=gcc; $build; $O/test_threads"
else
  O=out/test
  docker run --rm -v "$PWD/..":/w -w /w/host ua-host-armhf sh -c "O=$O CC=arm-linux-gnueabihf-gcc; $build"
  docker run --rm --platform linux/arm/v7 -v "$PWD/..":/w -w /w/host debian:bookworm-slim sh -c "$O/test_threads"
fi
