#!/bin/sh
# plugins.c's unit test (test_plugins.c, plugins_softfp.c as the "Mono side") with the FMOD
# test double (fmod_double.c) built as an Android library and as a glibc library; the Wwise stub
# (wwise.c) with and without a test backend (wwise_backend.c); the built-in FMOD (fmodstub.c); desktopenv.c's (test_desktopenv.c); monoaot.c's
# (test_monoaot.c); jitlog.c's (test_jitlog.c); jitwarm.c's list (test_jitwarm.c); gcbudget.c's UA_GC_GROW (test_gcgrow.c);
# monoinvoke.c's (test_monoinvoke.c); guiwarm.c's (test_guiwarm.c); lockstep.c's (test_lockstep.c). Build the loader
# first (../loader/build.sh). Built and run in ua-build-arm64.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-arm64}
[ "$ARCH" = arm64 ] || { echo "ARCH must be arm64 (armhf was removed: tag last-armhf)"; exit 1; }
docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c '
  set -e
  O=out/arm64/test/plugins; mkdir -p $O/glibc $O/android
  F="-O2 -g -Wall -Wno-format-truncation"
  gcc $F -o $O/test_plugins test/test_plugins.c test/plugins_softfp.c plugins.c plugstubs.c fmodstub.c fmplay.c wwplay_vorbis.c steamstub.c wwise.c wwise_tramp.S \
    ../loader/out/arm64/libsoloader.a -ldl -lpthread -lm -lrt -lstdc++
  gcc $F -shared -fPIC -o $O/libwwise_backend.so test/wwise_backend.c
  gcc $F -o $O/test_desktopenv test/test_desktopenv.c desktopenv.c ../loader/out/arm64/libsoloader.a -ldl -lpthread -lm -lrt -lstdc++
  gcc $F -o $O/test_monoaot test/test_monoaot.c monoaot.c
  gcc $F -o $O/test_jitlog test/test_jitlog.c jitlog.c -lpthread
  gcc $F -o $O/test_jitwarm test/test_jitwarm.c jitwarm.c -lpthread
  gcc $F -o $O/test_gcgrow test/test_gcgrow.c gcbudget.c -lpthread -ldl
  gcc $F -o $O/test_monoinvoke test/test_monoinvoke.c monoinvoke.c -lpthread
  gcc $F -o $O/test_guiwarm test/test_guiwarm.c guiwarm.c
  gcc $F -o $O/test_lockstep test/test_lockstep.c lockstep.c -lpthread
  gcc $F -shared -fPIC -o $O/glibc/libfmodstudio.so test/fmod_double.c
  # an NDK-style library: freestanding, bionic stack guard in tpidr_el0 + 0x28, DT_NEEDED libc.so
  gcc -O2 -g -fPIC -ffreestanding -DDOUBLE_ANDROID -mstack-protector-guard=sysreg -mstack-protector-guard-reg=tpidr_el0 \
    -mstack-protector-guard-offset=40 -c test/fmod_double.c -o $O/fd_android.o
  gcc -shared -nostdlib -Wl,-soname,libc.so -o $O/libc.so -x c /dev/null
  gcc -shared -nostdlib -Wl,--hash-style=both -Wl,--no-as-needed -Wl,-soname,libfmodstudio.so -o $O/android/libfmodstudio.so $O/fd_android.o \
    -L$O -lc $(gcc -print-libgcc-file-name)
  T=$(mktemp -d); mkdir -p $T/p $T/g $T/a
  $O/test_plugins paths $T/p
  $O/test_plugins lib $O/glibc $T/g
  $O/test_plugins lib $O/android $T/a
  mkdir -p $T/w $T/wb
  $O/test_plugins wwise $T/w
  $O/test_plugins wwise $T/wb $PWD/$O/libwwise_backend.so
  mkdir -p $T/f; $O/test_plugins fmod $T/f
  $O/test_desktopenv
  $O/test_monoaot $T
  $O/test_jitlog none
  $O/test_jitlog legacy
  $O/test_jitlog alloc
  for s in record warm stale patched late latestale legacy badtoken cctor noprobe nodefer inline writer writer0 seed torn garbage garbageseed missing; do d=$(mktemp -d); $O/test_jitwarm $s $d; rm -rf $d; done
  for s in grow playing collected plenty off; do $O/test_gcgrow $s; done
  $O/test_monoinvoke
  for s in on oldmodule nomethod throws nomono off zero; do $O/test_guiwarm $s; done
  L=$(mktemp -d); for s in off hash dump nudge settle record replay; do $O/test_lockstep $s $L; done; rm -rf $L'
