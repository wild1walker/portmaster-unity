#!/bin/bash
# Builds the Android shims for armhf (Debian bookworm cross toolchain, glibc 2.36) in Docker:
#   out/libandroidshims.a   (+ out/obj/*.o)   link with -lSDL2 -ldl -lpthread -lm
#   out/test/test_shims, out/test/libfakeegl.so
# ./build.sh test   also runs the unit tests under QEMU (linux/arm/v7 container, no GPU).
set -e
cd "$(dirname "$0")"
IMG=ua-android-shims
docker image inspect $IMG >/dev/null 2>&1 || docker build -t $IMG docker
mkdir -p out/obj out/test
docker run --rm -v "$PWD":/w -w /w $IMG bash -c '
  set -e
  CC=arm-linux-gnueabihf-gcc
  CFLAGS="-O2 -g -fPIC -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_FILE_OFFSET_BITS=64"
  for f in log_shim native_window asset_manager looper_input_sensor opensles egl_glue shim_tables; do
    $CC $CFLAGS -c $f.c -o out/obj/$f.o
  done
  rm -f out/libandroidshims.a
  arm-linux-gnueabihf-ar rcs out/libandroidshims.a out/obj/*.o
  $CC $CFLAGS -shared -o out/test/libfakeegl.so test/fake_egl.c
  $CC $CFLAGS -I. -o out/test/test_shims test/test_shims.c out/libandroidshims.a -lSDL2 -ldl -lpthread -lm
  file out/libandroidshims.a out/test/test_shims | sed "s|, BuildID[^,]*||"
  arm-linux-gnueabihf-objdump -T out/test/test_shims | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1 | sed "s/^/max glibc symbol version: /"
'
if [ "$1" = test ]; then
  # The armhf test binary runs in the same (arm64) image: it carries the armhf multiarch runtime
  # (libc6:armhf, libSDL2:armhf), and Docker Desktop runs 32-bit ARM code with QEMU user emulation.
  docker run --rm -v "$PWD":/w -w /w/out/test \
    -e SDL_AUDIODRIVER=disk -e SDL_DISKAUDIOFILE=/tmp/sles.raw -e ANDROID_WINDOW_SIZE=640x480 \
    -e ANDROID_SHIM_TRACE=${ANDROID_SHIM_TRACE:-0} \
    $IMG timeout 300 ./test_shims ./libfakeegl.so
  # Same again on SDL's dummy driver (buffer-queue logic and callback pacing only).
  docker run --rm -v "$PWD":/w -w /w/out/test -e SDL_AUDIODRIVER=dummy -e ANDROID_WINDOW_SIZE=640x480 \
    $IMG timeout 300 ./test_shims ./libfakeegl.so
fi
