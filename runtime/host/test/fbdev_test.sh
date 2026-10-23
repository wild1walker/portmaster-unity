#!/bin/sh
# fbdev.c's flips (test_fbdev.c: a fake Mali fbdev driver and display; the driver's own flips, host
# flips with 3 and 4 buffers, a new window, mailbox flips). ARCH=armhf (default): built in
# ua-host-armhf, run in an arm/v7 container (QEMU on most PCs); ARCH=arm64: built and run in
# ua-build-arm64.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-armhf}
build='set -e; mkdir -p $O; $CC -O2 -g -Wall -Wextra -DFBDEV_TEST -o $O/test_fbdev test/test_fbdev.c fbdev.c -lpthread -ldl'
if [ "$ARCH" = arm64 ]; then
  O=out/arm64/test
  docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c "O=$O CC=gcc; $build; $O/test_fbdev"
else
  O=out/test
  docker run --rm -v "$PWD/..":/w -w /w/host ua-host-armhf sh -c "O=$O CC=arm-linux-gnueabihf-gcc; $build"
  docker run --rm --platform linux/arm/v7 -v "$PWD/..":/w -w /w/host debian:bookworm-slim sh -c "$O/test_fbdev"
fi
