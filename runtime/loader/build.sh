#!/bin/sh
# Builds the loader in Docker for the player's ABI (arm64-v8a players): libsoloader.a and test_load,
# native in the ua-build-arm64 image (Ubuntu 20.04, glibc 2.31) -> out/arm64/.
# "./build.sh test" also builds the unit tests and the synthetic Android libraries (test/syn_*.c,
# packed by test/pack_relocs.c) into out/arm64/test/ and runs them.
# (32-bit armhf was removed; the last tree with it is the tag last-armhf.)
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
ARCH=${ARCH:-arm64}
[ "$ARCH" = arm64 ] || { echo "ARCH must be arm64 (armhf was removed: tag last-armhf)"; exit 1; }
IMG=ua-build-arm64; PLAT="--platform linux/arm64"   # also on an x86 PC (QEMU)
docker image inspect $IMG >/dev/null 2>&1 || docker build -q $PLAT -t $IMG "$DIR/../docker/arm64" >/dev/null
CFLAGS="-O2 -g -Wall -fPIC -ffunction-sections"   # (one section a function: host/build.sh groups the hot ones)
# bionic's stack guard: the TLS slot NDK code reads (TLS_SLOT_STACK_GUARD, tpidr_el0 + 0x28)
SYNFLAGS="-mstack-protector-guard=sysreg -mstack-protector-guard-reg=tpidr_el0 -mstack-protector-guard-offset=40"
OUT=out/arm64
TEST=0
[ "$1" = test ] && TEST=1
docker run --rm $PLAT -v "$DIR/..":/w -w /w/loader -e CFLAGS="$CFLAGS" -e SYNFLAGS="$SYNFLAGS" \
  -e OUT="$OUT" -e TEST=$TEST $IMG sh -c '
  set -e
  CC=gcc
  mkdir -p $OUT
  for f in so_loader.c relayout.c eglproc.c fastclock.c bionic_setjmp.S; do
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
  g++ $HF -c $H/common/dso_handle_counters.cpp -o $OUT/hybris_dso.o
  rm -f $OUT/libsoloader.a
  ar rcs $OUT/libsoloader.a $OUT/so_loader.o $OUT/relayout.o $OUT/eglproc.o $OUT/fastclock.o $OUT/bionic_setjmp.o $OUT/hybris_*.o
  # -lrt: timer_* and shm_open live there before glibc 2.34 (Ubuntu 20.04: 2.31)
  L="$OUT/libsoloader.a -ldl -lpthread -lm -lrt -lstdc++"
  $CC $CFLAGS test_load.c $L -o $OUT/test_load
  [ $TEST = 1 ] || exit 0

  D=$OUT/test
  mkdir -p $D
  for t in test_sem test_sync test_syn test_clock; do $CC $CFLAGS test/$t.c $L -o $D/$t; done
  # synthetic Android libraries: freestanding, -nostdlib, DT_NEEDED "libc.so" (an empty stand-in)
  A="-O2 -g -fPIC -ffreestanding -fstack-protector-all $SYNFLAGS"
  S="-shared -nostdlib -Wl,--hash-style=both -Wl,--no-as-needed"
  $CC -shared -nostdlib -Wl,-soname,libc.so -o $D/libc.so -x c /dev/null
  $CC $A -c test/syn_dep.c -o $D/syn_dep.o
  $CC $S -Wl,-soname,libsyn_dep.so -o $D/libsyn_dep.so $D/syn_dep.o -L$D -lc
  # no DT_NEEDED at all (as the AOT images of Mono): served by so_set_dlopen_rewrite
  $CC $A -c test/syn_plain.c -o $D/syn_plain.o
  $CC -shared -nostdlib -Wl,--hash-style=both -Wl,-soname,libplain.so -o $D/libplain.so $D/syn_plain.o
  $CC $A -c test/syn_lib.c -o $D/syn_lib.o
  # an unwinder of its own (libgcc_eh, hidden), as NDK libraries carry one
  UNW="$(gcc -print-file-name=libgcc_eh.a) -Wl,--exclude-libs,ALL"
  for i in 0 1 2 3 4 x; do
    $CC $S -Wl,-soname,libsyn_$i.so -o $D/libsyn_$i.so $D/syn_lib.o -L$D -lsyn_dep -lc $UNW
  done
  # a library linked against libmediandk.so (an empty stand-in, for the DT_NEEDED entry), as libunity 2022.3
  $CC -shared -nostdlib -Wl,-soname,libmediandk.so -o $D/libmediandk.so -x c /dev/null
  $CC $A -c test/syn_media.c -o $D/syn_media.o
  $CC $S -Wl,-soname,libsyn_media.so -o $D/libsyn_media.so $D/syn_media.o -L$D -lmediandk -lc
  # the relayout test library (every pc-relative class, C cleanups unwound by its own libgcc_eh)
  $CC $A -fexceptions -fno-omit-frame-pointer -c test/syn_rl.c -o $D/syn_rl.o
  $CC -c test/syn_rl_asm.S -o $D/syn_rl_asm.o
  $CC $S -Wl,-Bsymbolic -Wl,--build-id -Wl,-soname,libsyn_rl.so -o $D/libsyn_rl.so $D/syn_rl_asm.o $D/syn_rl.o -L$D -lc $UNW
  $CC $CFLAGS test/test_relayout.c $L -o $D/test_relayout
  $CC -O2 -static -o $D/pack_relocs test/pack_relocs.c
  P=$D/pack_relocs
  $P --relr $D/libsyn_1.so $D/libsyn_1.so
  $P --aps2 $D/libsyn_2.so $D/libsyn_2.so
  $P --relr --aps2 $D/libsyn_3.so $D/libsyn_3.so
  $P --relr --android-relr --aps2 $D/libsyn_4.so $D/libsyn_4.so
  $P --aps2 --truncate $D/libsyn_x.so $D/libsyn_x.so
'
echo "built: $DIR/$OUT/libsoloader.a $DIR/$OUT/test_load"
[ $TEST = 1 ] || exit 0
docker run --rm $PLAT -v "$DIR/$OUT/test":/t -w /t $IMG sh -c './test_sem && UA_FUTEX_SPIN=50 ./test_sem && ./test_sync && ./test_syn . && ./test_relayout . && ./test_clock 6'
