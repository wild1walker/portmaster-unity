#!/bin/sh
# futex.c's unit test (test_futex.c: ping-pong and a counting semaphore through the syscall wrapper,
# with the user-space wait off and on: no lost wake-ups, a lone wake makes no system call).
# Built and run in ua-build-arm64 (out/arm64/test/test_futex also runs on the handheld).
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-arm64}
[ "$ARCH" = arm64 ] || { echo "ARCH must be arm64 (armhf was removed: tag last-armhf)"; exit 1; }
build='set -e; mkdir -p $O; $CC -O2 -g -Wall -Wextra -o $O/test_futex test/test_futex.c futex.c -lpthread'
run='for s in 0 20 100; do $O/test_futex $s 1; done'
O=out/arm64/test
docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c "O=$O CC=gcc; $build; $run"
