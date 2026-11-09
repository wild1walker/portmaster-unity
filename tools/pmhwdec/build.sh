#!/bin/bash
# build.sh <out dir> [commit]: Knifethrower's libpmhwdec (https://github.com/Knifethrower/pmhwdec,
# LGPL-2.1-or-later) for a port to ship: ua_host takes video pictures from the handheld's decoder
# through it (H.264, HEVC; VP8 from commit 27e1bf1, the default here). Built with this repository's
# images (no clang or Debian sysroot needed): <out>/armhf/libpmhwdec.so.1 for the android route's
# 32-bit ua_host (ua-host-armhf), <out>/arm64/libpmhwdec.so.1 for the arm64 routes (ua-build-arm64),
# and <out>/aarch64/pmhw-cedard, the Allwinner decoder (GPL-2.0-or-later, a separate program the
# library starts next to itself; the H616 / H700 / A133 handhelds run 64-bit kernels, so the 64-bit one
# for both), and pmhw-test, which the setup asks whether VP8 plays on the video engine (the clips then
# stay at full size). Ship the library's folder with pmhw-cedard beside it and point UM_PMHWDEC at the library in
# port.env (docs/porting.md, "Videos"). <out>/armhf/librockchip_mpp.so is Rockchip MPP (Apache-2.0,
# LICENSE.mpp.txt beside it), trimmed as the library's rkmpp/build_mpp.sh does (its source patches,
# H.264 / HEVC / VP8 decoders only), with gcc in ua-host-armhf instead of clang and a sysroot: ship it
# next to libpmhwdec.so.1 for Rockchip handhelds on vendor kernels (dArkOS, ArkOS, AmberELEC), whose
# launcher opens the decoder's nodes (docs/porting.md). MPP_ARM64=1 builds <out>/arm64's too (slow:
# emulated). The sources are cloned into <out>/src; the port ships their licences.
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
# Rockchip MPP 1.1.0, as rkmpp/build_mpp.sh builds it (cmake and ninja put into the throwaway container)
mpp='set -e; apt-get update -qq >/dev/null && apt-get install -y -qq cmake ninja-build >/dev/null
H=/o/src/pmhwdec; M=/o/mpp-src-$A; O=/o/$A; rm -rf $M /o/mpp-build-$A && cp -a /o/src/mpp $M
for p in soc_name hw_list no_enc no_dbg_log; do python3 -I $H/rkmpp/patch_$p.py $M >/dev/null; done
printf "set(CMAKE_SYSTEM_NAME Linux)\nset(CMAKE_SYSTEM_PROCESSOR %s)\nset(CMAKE_C_COMPILER %sgcc)\nset(CMAKE_CXX_COMPILER %sg++)\n" $PROC "$CROSS" "$CROSS" > /o/mpp-$A.cmake
C="-DENABLE_H264D=ON -DENABLE_H265D=ON -DENABLE_VP8D=ON -DMPP_HW_LIST=VDPU2,VDPU341,VDPU34X"
for c in AVSD AVS2D H263D MPEG2D MPEG4D VP9D JPEGD AV1D H264E H265E JPEGE VP8E; do C="$C -DENABLE_$c=OFF"; done
API=$(grep -oE "SYM\([a-z_0-9]+, *\"[a-z_0-9]+\"\)" $H/src/rkmpp.c | grep -oE "\"[a-z_0-9]+\"" | tr -d "\"" | sort -u)
{ echo "{ global:"; echo "$API" | sed "s/$/;/"; echo "local: *; };"; } > /o/mpp-api-$A.map
S="-ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables -DPMHW_NO_DBG_LOG"
cmake -S $M -B /o/mpp-build-$A -G Ninja -DCMAKE_TOOLCHAIN_FILE=/o/mpp-$A.cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_TEST=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON $C -DCMAKE_C_FLAGS_RELEASE="-Os -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-Os -DNDEBUG" \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--no-as-needed -lm -Wl,-z,defs -Wl,--gc-sections -Wl,--version-script=/o/mpp-api-$A.map" \
  -DCMAKE_C_FLAGS="$CPU -fPIC -w $S" -DCMAKE_CXX_FLAGS="$CPU -fPIC -w $S" >/dev/null
ninja -C /o/mpp-build-$A rockchip_mpp >/dev/null
cp -L $(find /o/mpp-build-$A -name "librockchip_mpp.so*" -type f | head -1) $O/librockchip_mpp.so && ${CROSS}strip $O/librockchip_mpp.so
{ echo "librockchip_mpp.so: Rockchip MPP, https://github.com/rockchip-linux/mpp.git (tag 1.1.0), trimmed as libpmhwdec rkmpp/build_mpp.sh does."
  echo; cat $M/LICENSES/Apache-2.0 2>/dev/null || cat $M/LICENSE.md; } > $O/LICENSE.mpp.txt
rm -rf $M /o/mpp-build-$A /o/mpp-$A.cmake /o/mpp-api-$A.map'
docker run --rm -v "$OUT":/o -e A=armhf -e PROC=arm -e CROSS=arm-linux-gnueabihf- \
  -e CPU="-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard" ua-host-armhf bash -c "$mpp"
[ "${MPP_ARM64:-0}" = 1 ] && docker run --rm --platform linux/arm64 -v "$OUT":/o -e A=arm64 -e PROC=aarch64 -e CROSS= \
  -e CPU=-mcpu=cortex-a53 ua-build-arm64 bash -c "$mpp"
echo "libpmhwdec $(git -C "$SRC/pmhwdec" rev-parse --short HEAD):"
ls -l "$OUT"/armhf/libpmhwdec.so.1 "$OUT"/armhf/librockchip_mpp.so "$OUT"/arm64/libpmhwdec.so.1 "$OUT"/aarch64/pmhw-cedard
