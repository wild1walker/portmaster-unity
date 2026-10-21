#!/bin/bash
# Builds the Android shims in Docker (runtime/ is mounted: media_ndk.c includes ../media/um.h):
#   ARCH=armhf (default): Debian bookworm cross toolchain (glibc 2.36), image ua-android-shims ->
#     out/libandroidshims.a (+ out/obj/*.o), out/test/test_shims, out/test/libfakeegl.so
#   ARCH=arm64: native gcc of the ua-build-arm64 image (Ubuntu 20.04, glibc 2.31) -> the same
#     files under out/arm64/
# Link with -lSDL2 -ldl -lpthread -lm.
# ./build.sh test   also runs the unit tests (no GPU): armhf under QEMU (linux/arm/v7 container),
# arm64 natively.
set -e
cd "$(dirname "$0")"
ARCH=${ARCH:-armhf}
case "$ARCH" in
  armhf) IMG=ua-android-shims; PLAT=; O=out; CROSS=arm-linux-gnueabihf-
         docker image inspect $IMG >/dev/null 2>&1 || docker build -t $IMG docker ;;
  arm64) IMG=ua-build-arm64; PLAT="--platform linux/arm64"; O=out/arm64; CROSS=
         docker image inspect $IMG >/dev/null 2>&1 || docker build $PLAT -t $IMG ../docker/arm64 ;;
  *) echo "ARCH must be armhf or arm64"; exit 1 ;;
esac
mkdir -p $O/obj $O/test
docker run --rm $PLAT -v "$PWD/..":/r -w /r/android -e O=$O -e CROSS=$CROSS $IMG bash -c '
  set -e
  CC=${CROSS}gcc
  CFLAGS="-O2 -g -fPIC -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_FILE_OFFSET_BITS=64"
  for f in log_shim native_window asset_manager looper_input_sensor choreographer ndk_newer opensles media_ndk egl_glue shim_tables; do
    $CC $CFLAGS -c $f.c -o $O/obj/$f.o
  done
  rm -f $O/libandroidshims.a
  ${CROSS}ar rcs $O/libandroidshims.a $O/obj/*.o
  $CC $CFLAGS -shared -o $O/test/libfakeegl.so test/fake_egl.c
  $CC $CFLAGS -I. -o $O/test/test_shims test/test_shims.c $O/libandroidshims.a -lSDL2 -ldl -lpthread -lm
  file $O/libandroidshims.a $O/test/test_shims | sed "s|, BuildID[^,]*||"
  ${CROSS}objdump -T $O/test/test_shims | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1 | sed "s/^/max glibc symbol version: /"
'
if [ "$1" = test ]; then
  # armhf: the test binary runs in the same image, which carries the armhf multiarch runtime
  # (libc6:armhf, libSDL2:armhf); Docker Desktop runs 32-bit ARM code with QEMU user emulation.
  # arm64: native.
  docker run --rm $PLAT -v "$PWD/..":/r -w /r/android/$O/test \
    -e SDL_AUDIODRIVER=disk -e SDL_DISKAUDIOFILE=/tmp/sles.raw -e ANDROID_WINDOW_SIZE=640x480 \
    -e ANDROID_SHIM_TRACE=${ANDROID_SHIM_TRACE:-0} \
    $IMG timeout 300 ./test_shims ./libfakeegl.so
  # Same again on SDL's dummy driver (buffer-queue logic and callback pacing only).
  docker run --rm $PLAT -v "$PWD/..":/r -w /r/android/$O/test -e SDL_AUDIODRIVER=dummy -e ANDROID_WINDOW_SIZE=640x480 \
    $IMG timeout 300 ./test_shims ./libfakeegl.so
fi
