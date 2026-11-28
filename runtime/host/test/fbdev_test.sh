#!/bin/sh
# fbdev.c's flips (test_fbdev.c: a fake Mali fbdev driver and display; the driver's own flips, host
# flips with 3 and 4 buffers, a new window, mailbox flips, UA_ASPECT). Built and run in
# ua-build-arm64.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-arm64}
[ "$ARCH" = arm64 ] || { echo "ARCH must be arm64 (armhf was removed: tag last-armhf)"; exit 1; }
build='set -e; mkdir -p $O; $CC -O2 -g -Wall -Wextra -DFBDEV_TEST -o $O/test_fbdev test/test_fbdev.c fbdev.c -lpthread -ldl'
O=out/arm64/test
docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c "O=$O CC=gcc; $build; $O/test_fbdev"
