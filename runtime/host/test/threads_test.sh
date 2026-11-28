#!/bin/sh
# threads.c's unit test (test_threads.c: UA_AFFINITY / UA_NICE rules applied to named threads).
# Built and run in ua-build-arm64.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-arm64}
[ "$ARCH" = arm64 ] || { echo "ARCH must be arm64 (armhf was removed: tag last-armhf)"; exit 1; }
build='set -e; mkdir -p $O; $CC -O2 -g -Wall -Wextra -o $O/test_threads test/test_threads.c threads.c -lpthread'
O=out/arm64/test
docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c "O=$O CC=gcc; $build; $O/test_threads"
