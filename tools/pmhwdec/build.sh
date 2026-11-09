#!/bin/bash
# build.sh <out dir> [commit]: Knifethrower's libpmhwdec (https://github.com/Knifethrower/pmhwdec,
# LGPL-2.1-or-later) for a port to ship: ua_host takes video pictures from the handheld's decoder
# through it (H.264, HEVC; VP8 from commit 27e1bf1, the default here). Built with this repository's
# images (no clang or Debian sysroot needed): <out>/armhf/libpmhwdec.so.1 for the android route's
# 32-bit ua_host (ua-host-armhf), <out>/arm64/libpmhwdec.so.1 for the arm64 routes (ua-build-arm64),
# and <out>/aarch64/pmhw-cedard, the Allwinner decoder (GPL-2.0-or-later, a separate program the
# library starts next to itself; the H616 / H700 / A133 handhelds run 64-bit kernels, so the 64-bit one
# for both). Ship the library's folder with pmhw-cedard beside it and point UM_PMHW at the library in
# port.env (docs/porting.md, "Videos"); Rockchip MPP is a separate build (the library's
# rkmpp/build_mpp.sh). The sources are cloned into <out>/src; the port ships their licences.
set -euo pipefail
OUT=$(mkdir -p "$1" && cd "$1" && pwd); REV=${2:-27e1bf1}
SRC=$OUT/src
[ -d "$SRC/pmhwdec/.git" ] || git clone -q https://github.com/Knifethrower/pmhwdec "$SRC/pmhwdec"
git -C "$SRC/pmhwdec" fetch -q origin && git -C "$SRC/pmhwdec" checkout -q "$REV"
# Rockchip MPP's headers, which rkmpp.c compiles against (the library dlopens MPP at run time)
[ -d "$SRC/mpp/inc" ] || git clone -q --depth 1 --branch 1.1.0 https://github.com/rockchip-linux/mpp.git "$SRC/mpp"
build='set -e; H=/o/src/pmhwdec; O=/o/$A; mkdir -p $O/obj
F="-O2 -fPIC -Wall -Wextra -Werror=implicit-function-declaration -I$H/src/uapi -I$H/include -I$H/src"
if [ $A = aarch64 ]; then
  for s in cedard cedar stateless h264 hevc vp8; do gcc $F -mcpu=cortex-a53 -c $H/src/$s.c -o $O/obj/$s.o; done
  gcc -o $O/pmhw-cedard $O/obj/*.o -lpthread && strip $O/pmhw-cedard
else
  for s in pmhwdec selftest stateless h264 hevc vp8 cedar_client rkmpp v4l2req v4l2m2m; do
    $CC $F -I/o/src/mpp/inc -c $H/src/$s.c -o $O/obj/$s.o; done
  $CC -shared -o $O/libpmhwdec.so.1 -Wl,-soname,libpmhwdec.so.1 -Wl,--version-script=$H/src/exports.map $O/obj/*.o -lpthread -ldl
  $CC -O2 -I$H/include -o $O/pmhw-test $H/tools/pmhw_test.c $O/obj/*.o -lpthread -ldl
  $STRIP $O/libpmhwdec.so.1 $O/pmhw-test
fi
rm -rf $O/obj'
docker run --rm -v "$OUT":/o -e A=armhf -e CC="arm-linux-gnueabihf-gcc -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard" \
  -e STRIP=arm-linux-gnueabihf-strip ua-host-armhf bash -c "$build" 2>&1 | grep -v "warning:\|note:\|^ \|^In \|^$" || true
docker run --rm --platform linux/arm64 -v "$OUT":/o -e A=arm64 -e CC=gcc -e STRIP=strip ua-build-arm64 bash -c "$build" 2>&1 \
  | grep -v "warning:\|note:\|^ \|^In \|^$" || true
docker run --rm --platform linux/arm64 -v "$OUT":/o -e A=aarch64 ua-build-arm64 bash -c "$build" 2>&1 | grep -v "warning:\|note:\|^ \|^In \|^$" || true
echo "libpmhwdec $(git -C "$SRC/pmhwdec" rev-parse --short HEAD):"
ls -l "$OUT"/armhf/libpmhwdec.so.1 "$OUT"/arm64/libpmhwdec.so.1 "$OUT"/aarch64/pmhw-cedard
