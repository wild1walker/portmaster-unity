#!/bin/sh
# Builds pmspirvcross (pmspirvcross.cpp): Khronos' SPIRV-Cross (https://github.com/KhronosGroup/SPIRV-Cross,
# Apache 2.0 or MIT) and Aras Pranckevicius' SMOL-V decoder (https://github.com/aras-p/smol-v, MIT or
# public domain) with a small command-line front end, for `pmunity shaders --from vulkan`. Both are
# fetched at the pinned commits below (not kept in this repository); any C++11 compiler builds
# them (macOS clang, Linux gcc; e.g. in the ua-build-arm64 image for the handheld's aarch64 userland).
#
#   tools/spirvcross/build.sh [<out dir>]    default tools/spirvcross/out; the binary is <out>/pmspirvcross,
#                                            the licences (SPIRV-Cross, SMOL-V) <out>/pmspirvcross.LICENSE.txt
# For the handheld: the ua-build-arm64 image (aarch64) or ua-host-armhf with
# CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++ (armhf); neither image can fetch over
# HTTPS (no CA certificates), so run this once on the host first: an <out> that already holds
# the sources is not fetched again.
#
# Fetching uses curl, wget, python3 or git, whichever there is. Environment: CXX (compiler),
# CXXFLAGS. The converter finds the binary through $PMSPIRVCROSS, else tools/spirvcross/out/pmspirvcross,
# else pmspirvcross on PATH (or `pmunity shaders --spirv-cross <path>`).
set -e
SPIRV_CROSS_COMMIT=f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7     # tag vulkan-sdk-1.4.363.0, 2026-09-16
SMOLV_COMMIT=55000efe742f56d8b51223b9ea7775a8f0501881           # master, 2026-09-20
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/out}
mkdir -p "$out"
out=$(cd "$out" && pwd)

fetch() {   # fetch <owner/repo> <commit> <dir>: the repository's tree at that commit
  url=https://codeload.github.com/$1/tar.gz/$2
  name=$(basename "$1")-$2
  rm -rf "$3" "$3.tmp"
  mkdir -p "$3.tmp"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url" | tar -xzf - -C "$3.tmp"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO- "$url" | tar -xzf - -C "$3.tmp"
  elif command -v python3 >/dev/null 2>&1; then
    python3 -c 'import sys, urllib.request; sys.stdout.buffer.write(urllib.request.urlopen(sys.argv[1]).read())' "$url" \
      | tar -xzf - -C "$3.tmp"
  else
    git clone -q "https://github.com/$1.git" "$3.tmp/$name"
    git -C "$3.tmp/$name" checkout -q "$2"
  fi
  mv "$3.tmp/$name" "$3"
  rm -rf "$3.tmp"
}

sc=$out/SPIRV-Cross-$SPIRV_CROSS_COMMIT
sv=$out/smol-v-$SMOLV_COMMIT
[ -f "$sc/spirv_glsl.cpp" ] || fetch KhronosGroup/SPIRV-Cross $SPIRV_CROSS_COMMIT "$sc"
[ -f "$sv/source/smolv.cpp" ] || fetch aras-p/smol-v $SMOLV_COMMIT "$sv"

CXX=${CXX:-c++}
CXXFLAGS=${CXXFLAGS:--O2}
obj=$out/obj
mkdir -p "$obj"
objs=
for f in spirv_cfg spirv_cross spirv_cross_parsed_ir spirv_cross_util spirv_glsl spirv_parser; do
  o=$obj/$f.o
  [ "$o" -nt "$sc/$f.cpp" ] || $CXX $CXXFLAGS -std=c++11 -w -I"$sc" -c "$sc/$f.cpp" -o "$o"
  objs="$objs $o"
done
o=$obj/smolv.o
[ "$o" -nt "$sv/source/smolv.cpp" ] || $CXX $CXXFLAGS -std=c++11 -w -c "$sv/source/smolv.cpp" -o "$o"
objs="$objs $o"
$CXX $CXXFLAGS -std=c++11 -Wall -I"$sc" -I"$sv/source" "$here/pmspirvcross.cpp" $objs -o "$out/pmspirvcross"
# the binary contains SPIRV-Cross (Apache 2.0; some headers MIT) and SMOL-V (MIT): ship it with their licences
{
  echo "pmspirvcross contains SPIRV-Cross ($SPIRV_CROSS_COMMIT) and SMOL-V ($SMOLV_COMMIT)."
  echo
  echo "== SPIRV-Cross: Apache License 2.0 (files marked SPDX 'Apache-2.0 OR MIT' are used under Apache 2.0; spirv.hpp and GLSL.std.450.h are MIT, Copyright The Khronos Group Inc.)"
  echo
  cat "$sc/LICENSE"
  echo
  echo "== SPIRV-Cross: the MIT licence of its MIT-licensed headers"
  echo
  cat "$sc/LICENSES/MIT.txt"
  echo
  echo "== SMOL-V (Copyright (c) 2016-2018 Aras Pranckevicius)"
  echo
  cat "$sv/license.txt"
} > "$out/pmspirvcross.LICENSE.txt"
echo "$out/pmspirvcross (SPIRV-Cross $SPIRV_CROSS_COMMIT, SMOL-V $SMOLV_COMMIT)"
