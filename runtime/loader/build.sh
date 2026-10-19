#!/bin/sh
# Cross-compiles the loader (armhf, Debian bookworm glibc 2.36) into out/.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
docker build -q -t unity-android-loader-build "$DIR" >/dev/null
docker run --rm -v "$DIR/..":/w -w /w/loader unity-android-loader-build sh -c '
  set -e
  CFLAGS="-O2 -g -Wall -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard -fPIC"
  mkdir -p out
  for f in so_loader.c softfp.c bionic_setjmp.S; do
    arm-linux-gnueabihf-gcc $CFLAGS -c $f -o out/$(basename $f .${f##*.}).o
  done
  # libhybris (../thirdparty/libhybris, Apache 2.0): its hook functions and their helpers
  H=../thirdparty/libhybris/hybris
  HF="$CFLAGS -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -w -Ihybris -I$H/include -I$H/common"
  arm-linux-gnueabihf-gcc $HF -c hybris_compat.c -o out/hybris_compat.o
  for f in sysconf hooks_shm logging strlcpy strlcat legacy_properties/properties legacy_properties/cache legacy_properties/runtime_cache; do
    arm-linux-gnueabihf-gcc $HF -c $H/common/$f.c -o out/hybris_$(basename $f).o
  done
  arm-linux-gnueabihf-gcc $HF -c $H/properties/hybris_properties.c -o out/hybris_hybris_properties.o
  arm-linux-gnueabihf-g++ $HF -c $H/common/dso_handle_counters.cpp -o out/hybris_dso.o
  arm-linux-gnueabihf-ar rcs out/libsoloader.a out/so_loader.o out/softfp.o out/bionic_setjmp.o out/hybris_*.o
  arm-linux-gnueabihf-gcc $CFLAGS test_load.c out/libsoloader.a -o out/test_load \
    -Wl,--export-dynamic-symbol=__gnu_Unwind_Find_exidx -ldl -lpthread -lm -lstdc++
'
echo "built: $DIR/out/libsoloader.a $DIR/out/test_load"
