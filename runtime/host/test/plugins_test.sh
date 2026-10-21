#!/bin/sh
# plugins.c's unit test (test_plugins.c, plugins_softfp.c as the softfp "Mono side") with the FMOD
# test double (fmod_double.c) built as an Android library and as a glibc library. Build the loader
# first (../loader/build.sh, same ARCH). ARCH=armhf (default): built in ua-host-armhf, run in an
# arm/v7 container; ARCH=arm64: built and run in ua-build-arm64.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-armhf}
if [ "$ARCH" = arm64 ]; then
  docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c '
    set -e
    O=out/arm64/test/plugins; mkdir -p $O/glibc $O/android
    F="-O2 -g -Wall -Wno-format-truncation"
    gcc $F -o $O/test_plugins test/test_plugins.c test/plugins_softfp.c plugins.c plugstubs.c steamstub.c \
      ../loader/out/arm64/libsoloader.a -ldl -lpthread -lm -lrt -lstdc++
    gcc $F -shared -fPIC -o $O/glibc/libfmodstudio.so test/fmod_double.c
    # an NDK-style library: freestanding, bionic stack guard in tpidr_el0 + 0x28, DT_NEEDED libc.so
    gcc -O2 -g -fPIC -ffreestanding -DDOUBLE_ANDROID -mstack-protector-guard=sysreg -mstack-protector-guard-reg=tpidr_el0 \
      -mstack-protector-guard-offset=40 -c test/fmod_double.c -o $O/fd_android.o
    gcc -shared -nostdlib -Wl,-soname,libc.so -o $O/libc.so -x c /dev/null
    gcc -shared -nostdlib -Wl,--hash-style=both -Wl,--no-as-needed -Wl,-soname,libfmodstudio.so -o $O/android/libfmodstudio.so $O/fd_android.o \
      -L$O -lc $(gcc -print-libgcc-file-name)
    T=$(mktemp -d); mkdir -p $T/p $T/g $T/a
    $O/test_plugins thunks
    $O/test_plugins paths $T/p
    $O/test_plugins lib $O/glibc $T/g
    $O/test_plugins lib $O/android $T/a'
  exit
fi
docker run --rm -v "$PWD/..":/w -w /w/host ua-host-armhf sh -c '
  set -e
  O=out/test/plugins; mkdir -p $O/glibc $O/android
  CC=arm-linux-gnueabihf-gcc
  F="-O2 -g -Wall -Wno-format-truncation -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard"
  # the Mono side: armeabi-v7a code passes floats in core registers
  $CC -O2 -g -Wall -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=softfp -ffreestanding -c test/plugins_softfp.c -o $O/plugins_softfp.o
  arm-linux-gnueabihf-g++ $F -o $O/test_plugins -x c test/test_plugins.c -x c plugins.c -x c plugstubs.c -x c steamstub.c -x none \
    $O/plugins_softfp.o ../loader/out/libsoloader.a -Wl,--no-warn-mismatch -Wl,--export-dynamic-symbol=__gnu_Unwind_Find_exidx \
    -ldl -lpthread -lm
  $CC $F -shared -fPIC -o $O/glibc/libfmodstudio.so test/fmod_double.c
  # an NDK-style armeabi-v7a library: Thumb-2, softfp, freestanding, DT_NEEDED libc.so
  $CC -O2 -g -fPIC -ffreestanding -fno-stack-protector -march=armv7-a -mthumb -mfloat-abi=softfp -mfpu=vfpv3-d16 \
    -DDOUBLE_ANDROID -c test/fmod_double.c -o $O/fd_android.o
  $CC -shared -nostdlib -Wl,-soname,libc.so -o $O/libc.so -x c /dev/null
  $CC -shared -nostdlib -Wl,--hash-style=both -Wl,--no-as-needed -Wl,-soname,libfmodstudio.so -o $O/android/libfmodstudio.so $O/fd_android.o \
    -L$O -lc $($CC -mfloat-abi=softfp -print-libgcc-file-name)'
docker run --rm --platform linux/arm/v7 -v "$PWD/..":/w -w /w/host debian:bookworm-slim sh -c '
  set -e
  O=out/test/plugins
  T=$(mktemp -d); mkdir -p $T/p $T/g $T/a
  $O/test_plugins thunks
  $O/test_plugins paths $T/p
  $O/test_plugins lib $O/glibc $T/g
  $O/test_plugins lib $O/android $T/a'
