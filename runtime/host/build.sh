#!/bin/bash
# Builds the host programs: ua_host (loader + android shims + fake JNI + media core), and the device
# checks egltest / sdltest / audiocheck / shadercheck. Build ../loader, ../android, ../jni and ../media
# first for the same ARCH (../build.sh does).
#   ARCH=armhf (default): cross-compiled for the device's 32-bit userland in the ua-host-armhf image
#     -> out/
#   ARCH=arm64: native build in the ua-build-arm64 image (Ubuntu 20.04, glibc 2.31; its SDL2, EGL
#     and GLES) -> out/arm64/. ua_host is linked once ../loader/out/arm64/libsoloader.a exists;
#     until then its objects are still compiled, so the sources are checked.
set -e
cd "$(dirname "$0")"
ARCH=${ARCH:-armhf}
if [ "$ARCH" = arm64 ]; then
  mkdir -p out/arm64
  docker image inspect ua-build-arm64 >/dev/null 2>&1 || docker build -t ua-build-arm64 ../docker/arm64
  docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 bash -c '
    set -e
    O=out/arm64
    F="-O2 -g -Wall -I../jni/include -I../jni"
    OBJS=
    for f in ua_host input pointer frames glhooks hitches gltrace display display_sdl steamstub plugins plugstubs; do gcc $F -c $f.c -o $O/$f.o; OBJS="$OBJS $O/$f.o"; done
    # No __gnu_Unwind_Find_exidx export: that is ARM EHABI. AArch64 unwinds with .eh_frame, and the
    # unwinders linked into the Android libraries find our modules through the loader'"'"'s
    # dl_iterate_phdr (docs/arm64.md): no flags for that. -lrt: libhybris'"'"'s timer_* and shm_open
    # are there before glibc 2.34. The loader must be in the executable itself (its thread-local
    # block holds bionic'"'"'s stack guard slot, so_loader.h).
    LOADER=../loader/out/arm64/libsoloader.a
    if [ -f $LOADER ]; then
      g++ -o $O/ua_host $OBJS $LOADER ../android/out/arm64/libandroidshims.a ../jni/out/arm64/libfakejni.a \
        ../media/out/arm64/libuamedia.a -lSDL2 -lz -ldl -lpthread -lm -lrt
      file $O/ua_host | sed "s|, BuildID[^,]*||"
    else
      rm -f $O/ua_host
      echo "ua_host not linked: no arm64 loader yet ($LOADER); objects compiled in $O"
    fi
    gcc -O2 -Wall -o $O/egltest egltest.c -lEGL -lGLESv2
    gcc -O2 -Wall -o $O/sdltest sdltest.c -lSDL2
    gcc -O2 -Wall -o $O/audiocheck audiocheck.c -lSDL2
    gcc -O2 -Wall -o $O/shadercheck shadercheck.c -lEGL -lGLESv2'
  exit
fi
[ "$ARCH" = armhf ] || { echo "ARCH must be armhf or arm64"; exit 1; }
mkdir -p out
docker image inspect ua-host-armhf >/dev/null 2>&1 || docker build -q -t ua-host-armhf . >/dev/null
docker run --rm -v "$PWD/..":/w -w /w/host ua-host-armhf bash -c '
  set -e
  F="-O2 -g -Wall -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard -I../jni/include -I../jni"
  arm-linux-gnueabihf-gcc $F -c ua_host.c -o out/ua_host.o
  arm-linux-gnueabihf-gcc $F -c input.c -o out/input.o
  arm-linux-gnueabihf-gcc $F -c pointer.c -o out/pointer.o
  arm-linux-gnueabihf-gcc $F -c frames.c -o out/frames.o
  arm-linux-gnueabihf-gcc $F -c glhooks.c -o out/glhooks.o
  arm-linux-gnueabihf-gcc $F -c hitches.c -o out/hitches.o
  arm-linux-gnueabihf-gcc $F -c gltrace.c -o out/gltrace.o
  arm-linux-gnueabihf-gcc $F -c display.c -o out/display.o
  arm-linux-gnueabihf-gcc $F -c display_sdl.c -o out/display_sdl.o
  arm-linux-gnueabihf-gcc $F -c steamstub.c -o out/steamstub.o
  arm-linux-gnueabihf-gcc $F -c plugins.c -o out/plugins.o
  arm-linux-gnueabihf-gcc $F -c plugstubs.c -o out/plugstubs.o
  arm-linux-gnueabihf-g++ -o out/ua_host out/ua_host.o out/input.o out/pointer.o out/frames.o out/glhooks.o out/hitches.o out/gltrace.o \
    out/display.o out/display_sdl.o out/steamstub.o out/plugins.o out/plugstubs.o ../loader/out/libsoloader.a ../android/out/libandroidshims.a \
    ../jni/out/libfakejni.a ../media/out/libuamedia.a -Wl,--export-dynamic-symbol=__gnu_Unwind_Find_exidx \
    -lSDL2 -lz -ldl -lpthread -lm
  arm-linux-gnueabihf-gcc -O2 -Wall -o out/egltest egltest.c -lEGL -lGLESv2
  arm-linux-gnueabihf-gcc -O2 -Wall -o out/sdltest sdltest.c -lSDL2
  arm-linux-gnueabihf-gcc -O2 -Wall -o out/audiocheck audiocheck.c -lSDL2
  arm-linux-gnueabihf-gcc -O2 -Wall -o out/shadercheck shadercheck.c -lEGL -lGLESv2
  file out/ua_host'
