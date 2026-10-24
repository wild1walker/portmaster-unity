#!/bin/sh
# futex.c's unit test (test_futex.c: ping-pong and a counting semaphore through the syscall wrapper,
# with the user-space wait off and on: no lost wake-ups, a lone wake makes no system call).
# ARCH=armhf (default): built in ua-host-armhf, run in an arm/v7 container (QEMU: it cannot read the
# ARM timer, so run out/test/test_futex on the handheld too); ARCH=arm64: ua-build-arm64.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-armhf}
build='set -e; mkdir -p $O; $CC -O2 -g -Wall -Wextra -o $O/test_futex test/test_futex.c futex.c -lpthread'
run='for s in 0 20 100; do $O/test_futex $s 1; done'
if [ "$ARCH" = arm64 ]; then
  O=out/arm64/test
  docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c "O=$O CC=gcc; $build; $run"
else
  O=out/test
  docker run --rm -v "$PWD/..":/w -w /w/host ua-host-armhf sh -c "O=$O CC=arm-linux-gnueabihf-gcc; $build"
  docker run --rm --platform linux/arm/v7 -v "$PWD/..":/w -w /w/host debian:bookworm-slim sh -c "O=$O; $run"
fi
