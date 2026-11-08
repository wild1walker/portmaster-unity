#!/bin/sh
# plugins.c's unit test (test_plugins.c, plugins_softfp.c as the softfp "Mono side") with the FMOD
# test double (fmod_double.c) built as an Android library and as a glibc library; the Wwise stub
# (wwise.c) with and without a test backend (wwise_backend.c); desktopenv.c's (test_desktopenv.c); monoaot.c's
# (test_monoaot.c); jitlog.c's (test_jitlog.c); monoinvoke.c's (test_monoinvoke.c). Build the loader
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
    gcc $F -o $O/test_plugins test/test_plugins.c test/plugins_softfp.c plugins.c plugstubs.c steamstub.c wwise.c wwise_tramp.S \
      ../loader/out/arm64/libsoloader.a -ldl -lpthread -lm -lrt -lstdc++
    gcc $F -shared -fPIC -o $O/libwwise_backend.so test/wwise_backend.c
    gcc $F -o $O/test_desktopenv test/test_desktopenv.c desktopenv.c ../loader/out/arm64/libsoloader.a -ldl -lpthread -lm -lrt -lstdc++
    gcc $F -o $O/test_monoaot test/test_monoaot.c monoaot.c
    gcc $F -o $O/test_jitlog test/test_jitlog.c jitlog.c -lpthread
    gcc $F -o $O/test_monoinvoke test/test_monoinvoke.c monoinvoke.c -lpthread
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
    $O/test_plugins lib $O/android $T/a
    mkdir -p $T/w $T/wb
    $O/test_plugins wwise $T/w
    $O/test_plugins wwise $T/wb $PWD/$O/libwwise_backend.so
    $O/test_desktopenv
    $O/test_monoaot $T
    $O/test_jitlog none
    $O/test_jitlog legacy
    $O/test_monoinvoke'
  exit
fi
docker run --rm -v "$PWD/..":/w -w /w/host ua-host-armhf sh -c '
  set -e
  O=out/test/plugins; mkdir -p $O/glibc $O/android
  CC=arm-linux-gnueabihf-gcc
  F="-O2 -g -Wall -Wno-format-truncation -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard"
  # the Mono side: armeabi-v7a code passes floats in core registers
  $CC -O2 -g -Wall -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=softfp -ffreestanding -c test/plugins_softfp.c -o $O/plugins_softfp.o
  arm-linux-gnueabihf-g++ $F -o $O/test_plugins -x c test/test_plugins.c -x c plugins.c -x c plugstubs.c -x c steamstub.c -x c wwise.c \
    -x assembler-with-cpp wwise_tramp.S -x none \
    $O/plugins_softfp.o ../loader/out/libsoloader.a -Wl,--no-warn-mismatch -Wl,--export-dynamic-symbol=__gnu_Unwind_Find_exidx \
    -ldl -lpthread -lm
  $CC $F -shared -fPIC -o $O/glibc/libfmodstudio.so test/fmod_double.c
  $CC $F -shared -fPIC -o $O/libwwise_backend.so test/wwise_backend.c
  arm-linux-gnueabihf-g++ $F -o $O/test_desktopenv -x c test/test_desktopenv.c -x c desktopenv.c -x none ../loader/out/libsoloader.a \
    -Wl,--export-dynamic-symbol=__gnu_Unwind_Find_exidx -ldl -lpthread -lm
  $CC $F -o $O/test_monoaot test/test_monoaot.c monoaot.c
  $CC $F -o $O/test_jitlog test/test_jitlog.c jitlog.c -lpthread -latomic
  arm-linux-gnueabihf-g++ $F -o $O/test_monoinvoke -x c test/test_monoinvoke.c -x c monoinvoke.c -x none ../loader/out/libsoloader.a \
    -Wl,--export-dynamic-symbol=__gnu_Unwind_Find_exidx -ldl -lpthread -lm
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
  $O/test_plugins lib $O/android $T/a
  mkdir -p $T/w $T/wb
  $O/test_plugins wwise $T/w
  $O/test_plugins wwise $T/wb $PWD/$O/libwwise_backend.so
  $O/test_desktopenv
  $O/test_monoaot $T
  $O/test_jitlog none
  $O/test_jitlog legacy
  $O/test_monoinvoke'
