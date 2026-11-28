#!/bin/sh
# gpuidle.c's unit test (test_gpuidle.c: the idle accounting; the fences themselves need a GPU).
# Built and run in ua-build-arm64.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-arm64}
[ "$ARCH" = arm64 ] || { echo "ARCH must be arm64 (armhf was removed: tag last-armhf)"; exit 1; }
build='set -e; mkdir -p $O; $CC -O2 -g -Wall -Wextra -o $O/test_gpuidle test/test_gpuidle.c gpuidle.c -ldl -lpthread -lm'
O=out/arm64/test
docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c "O=$O CC=gcc; $build; $O/test_gpuidle"
