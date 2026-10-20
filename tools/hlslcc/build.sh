#!/bin/sh
# Builds pmhlslcc (pmhlslcc.cpp): Unity's HLSLcc (https://github.com/Unity-Technologies/HLSLcc,
# MIT) with a small command-line front end, for `pmunity shaders --from d3d11`. HLSLcc is fetched
# at the pinned commit below (not kept in this repository); any C++11 compiler builds it (macOS
# clang, Linux gcc; e.g. in the ua-build-arm64 image for the handheld's aarch64 userland).
#
#   tools/hlslcc/build.sh [<out dir>]        default tools/hlslcc/out; the binary is <out>/pmhlslcc,
#                                            HLSLcc's licence (MIT, bstrlib BSD) <out>/pmhlslcc.LICENSE.txt
# For the handheld: the ua-build-arm64 image (aarch64) or ua-host-armhf with
# CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++ (armhf); neither image can fetch over
# HTTPS (no CA certificates), so run this once on the host first: an <out> that already holds
# HLSLcc-<commit>/ is not fetched again.
#
# Fetching uses curl, wget, python3 or git, whichever there is. Environment: CC, CXX (compilers),
# CXXFLAGS. The converter finds the binary through $PMHLSLCC, else tools/hlslcc/out/pmhlslcc,
# else pmhlslcc on PATH (or `pmunity shaders --hlslcc <path>`).
set -e
HLSLCC_COMMIT=3ea1fcd6bd0ac445bc078de0bf32f0950188577b      # master, 2020-09-23 ("bring latest changes from upstream")
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/out}
mkdir -p "$out"
out=$(cd "$out" && pwd)
src=$out/HLSLcc-$HLSLCC_COMMIT

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
$CXX $CXXFLAGS -std=c++11 -Wall $inc "$here/pmhlslcc.cpp" $objs -o "$out/pmhlslcc"
# the binary contains HLSLcc (MIT) and bstrlib (BSD): ship it with their licence text
cp "$src/license.txt" "$out/pmhlslcc.LICENSE.txt"
echo "$out/pmhlslcc (HLSLcc $HLSLCC_COMMIT)"
