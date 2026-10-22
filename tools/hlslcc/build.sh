#!/bin/sh
# Builds pmhlslcc (pmhlslcc.cpp): Unity's HLSLcc (https://github.com/Unity-Technologies/HLSLcc,
# MIT) with a small command-line front end, for `pmunity shaders --from d3d11`. HLSLcc is fetched
# at the pinned commit below (not kept in this repository); any C++11 compiler builds it (macOS
# clang, Linux gcc).
#
#   tools/hlslcc/build.sh [<out dir>]        default tools/hlslcc/out; the binary is <out>/pmhlslcc,
#                                            HLSLcc's licence (MIT, bstrlib BSD) <out>/pmhlslcc.LICENSE.txt
#   ARCH=arm64 tools/hlslcc/build.sh [<out dir>]
#                                            for the handheld (a port's setup translates a Windows
#                                            build's shaders with it; `pmunity port package` ships
#                                            it): aarch64 Linux, built in Docker in the runtime's
#                                            ua-build-arm64 image (Ubuntu 20.04, glibc 2.31; libstdc++
#                                            and libgcc linked in), default out tools/hlslcc/out/linux-arm64.
#                                            HLSLcc is fetched on the host first (the image has no CA
#                                            certificates) into tools/hlslcc/out/. BUILD_NAME names the container.
# For an armhf build: the ua-host-armhf image with CC=arm-linux-gnueabihf-gcc
# CXX=arm-linux-gnueabihf-g++; an <out> that already holds HLSLcc-<commit>/ is not fetched again.
#
# Fetching uses curl, wget, python3 or git, whichever there is. Environment: CC, CXX (compilers),
# CXXFLAGS, LDFLAGS, HLSLCC_SRC (an HLSLcc-<commit> folder fetched before). The converter finds the
# binary through $PMHLSLCC, else tools/hlslcc/out/pmhlslcc, else pmhlslcc on PATH (or `pmunity
# shaders --hlslcc <path>`).
set -e
HLSLCC_COMMIT=3ea1fcd6bd0ac445bc078de0bf32f0950188577b      # master, 2020-09-23 ("bring latest changes from upstream")
here=$(cd "$(dirname "$0")" && pwd)
if [ "${ARCH:-}" = arm64 ]; then
  # the source on the host, the build in the arm64 image (this script again, inside it)
  out=${1:-$here/out/linux-arm64}
  mkdir -p "$out" "$here/out"
  out=$(cd "$out" && pwd)
  ARCH= HLSLCC_FETCH_ONLY=1 "$0" "$here/out"
  IMG=ua-build-arm64
  docker image inspect $IMG >/dev/null 2>&1 || \
    docker build -q --platform linux/arm64 -t $IMG "$here/../../runtime/docker/arm64" >/dev/null
  docker run --rm --platform linux/arm64 ${BUILD_NAME:+--name "$BUILD_NAME"} -v "$here":/h:ro -v "$out":/o \
    -e HLSLCC_SRC=/h/out/HLSLcc-$HLSLCC_COMMIT -e LDFLAGS="-static-libstdc++ -static-libgcc -s" $IMG sh /h/build.sh /o
  file "$out/pmhlslcc" 2>/dev/null || true
  exit 0
fi
out=${1:-$here/out}
mkdir -p "$out"
out=$(cd "$out" && pwd)
src=${HLSLCC_SRC:-$out/HLSLcc-$HLSLCC_COMMIT}

if [ ! -f "$src/include/hlslcc.h" ]; then
  url=https://codeload.github.com/Unity-Technologies/HLSLcc/tar.gz/$HLSLCC_COMMIT
  rm -rf "$src" "$src.tmp"
  mkdir -p "$src.tmp"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url" | tar -xzf - -C "$src.tmp"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO- "$url" | tar -xzf - -C "$src.tmp"
  elif command -v python3 >/dev/null 2>&1; then
    python3 -c 'import sys, urllib.request; sys.stdout.buffer.write(urllib.request.urlopen(sys.argv[1]).read())' "$url" \
      | tar -xzf - -C "$src.tmp"
  else
    git clone -q https://github.com/Unity-Technologies/HLSLcc.git "$src.tmp/HLSLcc-$HLSLCC_COMMIT"
    git -C "$src.tmp/HLSLcc-$HLSLCC_COMMIT" checkout -q "$HLSLCC_COMMIT"
  fi
  mv "$src.tmp/HLSLcc-$HLSLCC_COMMIT" "$src"
  rm -rf "$src.tmp"
fi
[ -z "${HLSLCC_FETCH_ONLY:-}" ] || exit 0

CC=${CC:-cc}
CXX=${CXX:-c++}
CXXFLAGS=${CXXFLAGS:--O2}
obj=$out/obj
mkdir -p "$obj"
inc="-I$src -I$src/include -I$src/src -I$src/src/cbstring -I$src/src/internal_includes"
objs=
for f in "$src"/src/cbstring/bsafe.c "$src"/src/cbstring/bstraux.c "$src"/src/cbstring/bstrlib.c; do
  o=$obj/$(basename "$f" .c).o
  [ "$o" -nt "$f" ] || $CC -O2 -w $inc -c "$f" -o "$o"
  objs="$objs $o"
done
for f in "$src"/src/*.cpp; do
  o=$obj/$(basename "$f" .cpp).o
  [ "$o" -nt "$f" ] || $CXX $CXXFLAGS -std=c++11 -w $inc -c "$f" -o "$o"
  objs="$objs $o"
done
$CXX $CXXFLAGS -std=c++11 -Wall $inc "$here/pmhlslcc.cpp" $objs ${LDFLAGS:-} -o "$out/pmhlslcc"
# the binary contains HLSLcc (MIT) and bstrlib (BSD): ship it with their licence text
cp "$src/license.txt" "$out/pmhlslcc.LICENSE.txt"
echo "$out/pmhlslcc (HLSLcc $HLSLCC_COMMIT)"
