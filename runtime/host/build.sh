#!/bin/bash
# Cross-compiles the host programs for the device's 32-bit (armhf) userland in the ua-host-armhf
# image: ua_host (loader + android shims + fake JNI), and the device checks egltest / sdltest /
# audiocheck / shadercheck. Build ../loader, ../android and ../jni first (../build.sh does).
set -e
cd "$(dirname "$0")"
mkdir -p out
docker image inspect ua-host-armhf >/dev/null 2>&1 || docker build -q -t ua-host-armhf . >/dev/null
docker run --rm -v "$PWD/..":/w -w /w/host ua-host-armhf bash -c '
  set -e
  F="-O2 -g -Wall -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard -I../jni/include -I../jni"
  arm-linux-gnueabihf-gcc $F -c ua_host.c -o out/ua_host.o
  arm-linux-gnueabihf-gcc $F -c input.c -o out/input.o
  arm-linux-gnueabihf-gcc $F -c frames.c -o out/frames.o
  arm-linux-gnueabihf-gcc $F -c glhooks.c -o out/glhooks.o
  arm-linux-gnueabihf-gcc $F -c hitches.c -o out/hitches.o
  arm-linux-gnueabihf-gcc $F -c gltrace.c -o out/gltrace.o
  arm-linux-gnueabihf-g++ -o out/ua_host out/ua_host.o out/input.o out/frames.o out/glhooks.o out/hitches.o out/gltrace.o ../loader/out/libsoloader.a ../android/out/libandroidshims.a \
    ../jni/out/libfakejni.a -Wl,--export-dynamic-symbol=__gnu_Unwind_Find_exidx -lSDL2 -lz -ldl -lpthread -lm
  arm-linux-gnueabihf-gcc -O2 -Wall -o out/egltest egltest.c -lEGL -lGLESv2
  arm-linux-gnueabihf-gcc -O2 -Wall -o out/sdltest sdltest.c -lSDL2
  arm-linux-gnueabihf-gcc -O2 -Wall -o out/audiocheck audiocheck.c -lSDL2
  arm-linux-gnueabihf-gcc -O2 -Wall -o out/shadercheck shadercheck.c -lEGL -lGLESv2
  file out/ua_host'
