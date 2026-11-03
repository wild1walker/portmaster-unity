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
  # ANDROID_SLES_CAPTURE must hold exactly what SDL's disk driver was given.
  docker run --rm $PLAT -v "$PWD/..":/r -w /r/android/$O/test \
    -e SDL_AUDIODRIVER=disk -e SDL_DISKAUDIOFILE=/tmp/sles.raw -e ANDROID_WINDOW_SIZE=640x480 \
    -e ANDROID_SHIM_TRACE=${ANDROID_SHIM_TRACE:-0} -e ANDROID_SLES_CAPTURE=/tmp/cap.raw $IMG sh -c '
    timeout 300 ./test_shims ./libfakeegl.so || exit 1
    # (an SDL may write a buffer of its own silence into its file, or leave the callback'"'"'s last
    # one out at close: up to one buffer either way, the rest the same at the start or the end)
    a=$(wc -c < /tmp/sles.raw); b=$(wc -c < /tmp/cap.raw)
    if [ "$a" -le "$b" ]; then s=/tmp/sles.raw; l=/tmp/cap.raw; n=$a; else s=/tmp/cap.raw; l=/tmp/sles.raw; n=$b; fi
    d=$((a > b ? a - b : b - a))
    if [ "$d" -le 8192 ] && { cmp -s -n "$n" "$s" "$l" || tail -c "$n" "$l" | cmp -s - "$s"; }; then
      echo "capture: ok ($b bytes, as SDL played them$([ "$d" = 0 ] || echo "; its file $a bytes"))"
    else echo "capture: FAILED"; ls -l /tmp/sles.raw /tmp/cap.raw; exit 1; fi'
  # Same again on SDL's dummy driver (buffer-queue logic and callback pacing only).
  docker run --rm $PLAT -v "$PWD/..":/r -w /r/android/$O/test -e SDL_AUDIODRIVER=dummy -e ANDROID_WINDOW_SIZE=640x480 \
    $IMG timeout 300 ./test_shims ./libfakeegl.so
  # Pipe output on ALSA alone (no sound server): test/fakebin/aplay refuses plughw:0,0 as a busy card
  # does, so the PCM must reach the next device of the aplay client.
  docker run --rm $PLAT -v "$PWD/..":/r -w /r/android/$O/test -e SDL_AUDIODRIVER=dummy -e ANDROID_SLES_OUTPUT=pipe \
    -e ANDROID_WINDOW_SIZE=640x480 -e PATH=/r/android/test/fakebin:/usr/bin:/bin $IMG sh -c '
    unset XDG_RUNTIME_DIR; rm -f /tmp/aplay.args /tmp/aplay.raw
    timeout 300 ./test_shims ./libfakeegl.so > /tmp/out.txt 2>&1 || { cat /tmp/out.txt; echo "pipe/aplay: test_shims failed"; exit 1; }
    if grep -q -- "-D default" /tmp/aplay.args && [ -s /tmp/aplay.raw ]; then
      echo "pipe/aplay: ok ($(wc -c < /tmp/aplay.raw) bytes of PCM on the second device)"
    else
      echo "pipe/aplay: FAILED"; cat /tmp/aplay.args; exit 1
    fi'
  # No SDL audio at all (a 32-bit SDL without a driver it can start, as muOS's has no dummy): the
  # engine still starts and the default output (auto) reaches the pipe client.
  docker run --rm $PLAT -v "$PWD/..":/r -w /r/android/$O/test -e SDL_AUDIODRIVER=nosuchdriver \
    -e ANDROID_WINDOW_SIZE=640x480 -e PATH=/r/android/test/fakebin:/usr/bin:/bin $IMG sh -c '
    unset XDG_RUNTIME_DIR; rm -f /tmp/aplay.args /tmp/aplay.raw
    timeout 300 ./test_shims ./libfakeegl.so > /tmp/out.txt 2>&1 || { cat /tmp/out.txt; echo "no SDL audio: test_shims failed"; exit 1; }
    if grep -q "no SDL audio: the players use the pipe output" /tmp/out.txt && [ -s /tmp/aplay.raw ]; then
      echo "no SDL audio: ok ($(wc -c < /tmp/aplay.raw) bytes of PCM through the pipe)"
    else
      echo "no SDL audio: FAILED"; cat /tmp/out.txt; exit 1
    fi'
fi
