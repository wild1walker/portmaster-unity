#!/bin/sh
# Builds the loader in Docker for the player's ABI: libsoloader.a and test_load.
#   ARCH=armhf (default): armeabi-v7a players, cross-compiled (Debian bookworm, glibc 2.36) -> out/
#   ARCH=arm64: arm64-v8a players, native in the ua-build-arm64 image (Ubuntu 20.04, glibc 2.31)
#     -> out/arm64/
# "./build.sh test" also builds the unit tests and the synthetic Android libraries (test/syn_*.c,
# packed by test/pack_relocs.py) into out/[arm64/]test/ and runs them: the armhf ones in an arm/v7
# container (Docker's emulation; the qemu-arm of the build image crashes in pthread_create), the
# arm64 ones natively.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
ARCH=${ARCH:-armhf}
case "$ARCH" in
armhf)
  IMG=unity-android-loader-build
  docker build -q -t $IMG "$DIR" >/dev/null
  T=arm-linux-gnueabihf-
  CFLAGS="-O2 -g -Wall -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard -fPIC"
  # the synthetic libraries as armeabi-v7a code: Thumb-2, softfp, the global stack guard
  SYNFLAGS="-march=armv7-a -mthumb -mfloat-abi=softfp -mfpu=vfpv3-d16"
  EXPORT="-Wl,--export-dynamic-symbol=__gnu_Unwind_Find_exidx"
  OUT=out ;;
arm64)
  IMG=ua-build-arm64
  docker image inspect $IMG >/dev/null 2>&1 || docker build -q -t $IMG "$DIR/../docker/arm64" >/dev/null
  T=
  CFLAGS="-O2 -g -Wall -fPIC"
  # bionic's stack guard: the TLS slot NDK code reads (TLS_SLOT_STACK_GUARD, tpidr_el0 + 0x28)
  SYNFLAGS="-mstack-protector-guard=sysreg -mstack-protector-guard-reg=tpidr_el0 -mstack-protector-guard-offset=40"
  EXPORT=
  OUT=out/arm64 ;;
*)
  echo "ARCH must be armhf or arm64"; exit 1 ;;
esac
TEST=0
[ "$1" = test ] && TEST=1
docker run --rm -v "$DIR/..":/w -w /w/loader -e T="$T" -e CFLAGS="$CFLAGS" -e SYNFLAGS="$SYNFLAGS" \
  -e EXPORT="$EXPORT" -e OUT="$OUT" -e TEST=$TEST $IMG sh -c '
  set -e
  CC=${T}gcc
  mkdir -p $OUT
  for f in so_loader.c softfp.c bionic_setjmp.S; do
    $CC $CFLAGS -c $f -o $OUT/$(basename $f .${f##*.}).o
  done
  # libhybris (../thirdparty/libhybris, Apache 2.0): its hook functions and their helpers
  H=../thirdparty/libhybris/hybris
  HF="$CFLAGS -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -w -Ihybris -I$H/include -I$H/common"
  $CC $HF -c hybris_compat.c -o $OUT/hybris_compat.o
  for f in sysconf hooks_shm logging strlcpy strlcat legacy_properties/properties legacy_properties/cache legacy_properties/runtime_cache; do
    $CC $HF -c $H/common/$f.c -o $OUT/hybris_$(basename $f).o
  done
  $CC $HF -c $H/properties/hybris_properties.c -o $OUT/hybris_hybris_properties.o
  ${T}g++ $HF -c $H/common/dso_handle_counters.cpp -o $OUT/hybris_dso.o
  rm -f $OUT/libsoloader.a
  ${T}ar rcs $OUT/libsoloader.a $OUT/so_loader.o $OUT/softfp.o $OUT/bionic_setjmp.o $OUT/hybris_*.o
  # -lrt: timer_* and shm_open live there before glibc 2.34 (Ubuntu 20.04: 2.31)
  L="$OUT/libsoloader.a $EXPORT -ldl -lpthread -lm -lrt -lstdc++"
  $CC $CFLAGS test_load.c $L -o $OUT/test_load
  [ $TEST = 1 ] || exit 0

  D=$OUT/test
  mkdir -p $D
  for t in test_sem test_sync test_syn; do $CC $CFLAGS test/$t.c $L -o $D/$t; done
  # synthetic Android libraries: freestanding, -nostdlib, DT_NEEDED "libc.so" (an empty stand-in)
  A="-O2 -g -fPIC -ffreestanding -fstack-protector-all $SYNFLAGS"
  S="-shared -nostdlib -Wl,--hash-style=both -Wl,--no-as-needed"
  $CC -shared -nostdlib -Wl,-soname,libc.so -o $D/libc.so -x c /dev/null
  $CC $A -c test/syn_dep.c -o $D/syn_dep.o
  $CC $S -Wl,-soname,libsyn_dep.so -o $D/libsyn_dep.so $D/syn_dep.o -L$D -lc
  $CC $A -c test/syn_lib.c -o $D/syn_lib.o
  # arm64: an unwinder of its own (libgcc_eh, hidden), as NDK libraries carry one
  UNW=
  [ -z "$T" ] && UNW="$(gcc -print-file-name=libgcc_eh.a) -Wl,--exclude-libs,ALL"
  for i in 0 1 2 3 4 x; do
    $CC $S -Wl,-soname,libsyn_$i.so -o $D/libsyn_$i.so $D/syn_lib.o -L$D -lsyn_dep -lc $UNW
  done
  # a library linked against libmediandk.so (an empty stand-in, for the DT_NEEDED entry), as libunity 2022.3
  $CC -shared -nostdlib -Wl,-soname,libmediandk.so -o $D/libmediandk.so -x c /dev/null
  $CC $A -c test/syn_media.c -o $D/syn_media.o
  $CC $S -Wl,-soname,libsyn_media.so -o $D/libsyn_media.so $D/syn_media.o -L$D -lmediandk -lc
  P="python3 test/pack_relocs.py"
  $P --relr $D/libsyn_1.so $D/libsyn_1.so
  $P --aps2 $D/libsyn_2.so $D/libsyn_2.so
  $P --relr --aps2 $D/libsyn_3.so $D/libsyn_3.so
  $P --relr --android-relr --aps2 $D/libsyn_4.so $D/libsyn_4.so
  $P --aps2 --truncate $D/libsyn_x.so $D/libsyn_x.so
'
echo "built: $DIR/$OUT/libsoloader.a $DIR/$OUT/test_load"
[ $TEST = 1 ] || exit 0
if [ "$ARCH" = arm64 ]; then
  docker run --rm -v "$DIR/$OUT/test":/t -w /t $IMG sh -c './test_sem && ./test_sync && ./test_syn .'
else
  docker run --rm --pull never --platform linux/arm/v7 -v "$DIR/$OUT/test":/t -w /t debian:bookworm-slim \
    sh -c './test_sem && ./test_sync && ./test_syn .'
fi
