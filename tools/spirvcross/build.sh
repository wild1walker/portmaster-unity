#!/bin/sh
# Builds pmspirvcross (pmspirvcross.cpp): Khronos' SPIRV-Cross (https://github.com/KhronosGroup/SPIRV-Cross,
# Apache 2.0 or MIT) and Aras Pranckevicius' SMOL-V decoder (https://github.com/aras-p/smol-v, MIT or
# public domain) with a small command-line front end, for `pmunity shaders --from vulkan`. Both are
# fetched at the pinned commits below (not kept in this repository); any C++11 compiler builds
# them (macOS clang, Linux gcc).
#
#   tools/spirvcross/build.sh [<out dir>]    default tools/spirvcross/out; the binary is <out>/pmspirvcross,
#                                            the licences (SPIRV-Cross, SMOL-V) <out>/pmspirvcross.LICENSE.txt
#   ARCH=arm64 tools/spirvcross/build.sh [<out dir>]
#                                            for the handheld (a port's setup translates a Vulkan-only
#                                            build's shaders with it; `pmunity port package` ships it):
#                                            aarch64 Linux, built in Docker in the runtime's ua-build-arm64
#                                            image (Ubuntu 20.04, glibc 2.31; libstdc++ and libgcc linked
#                                            in), default out tools/spirvcross/out/linux-arm64. The
#                                            sources are fetched on the host first (the image has no CA
#                                            certificates) into tools/spirvcross/out/. BUILD_NAME names the
#                                            container.
# An <out> that already holds the sources is not fetched again.
#
# Fetching uses curl, wget, python3 or git, whichever there is. Environment: CXX (compiler),
# CXXFLAGS, LDFLAGS, SPIRV_CROSS_SRC / SMOLV_SRC (source folders fetched before). The converter
# finds the binary through $PMSPIRVCROSS, else tools/spirvcross/out/pmspirvcross, else pmspirvcross
# on PATH (or `pmunity shaders --spirv-cross <path>`).
set -e
SPIRV_CROSS_COMMIT=f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7     # tag vulkan-sdk-1.4.363.0, 2026-09-16
SMOLV_COMMIT=55000efe742f56d8b51223b9ea7775a8f0501881           # master, 2026-09-20
here=$(cd "$(dirname "$0")" && pwd)
if [ "${ARCH:-}" = arm64 ]; then
  # the sources on the host, the build in the arm64 image (this script again, inside it)
  out=${1:-$here/out/linux-arm64}
  mkdir -p "$out" "$here/out"
  out=$(cd "$out" && pwd)
  ARCH= SPIRVCROSS_FETCH_ONLY=1 "$0" "$here/out"
  IMG=ua-build-arm64
  docker image inspect $IMG >/dev/null 2>&1 || \
    docker build -q --platform linux/arm64 -t $IMG "$here/../../runtime/docker/arm64" >/dev/null
  docker run --rm --platform linux/arm64 ${BUILD_NAME:+--name "$BUILD_NAME"} -v "$here":/h:ro -v "$out":/o \
    -e SPIRV_CROSS_SRC=/h/out/SPIRV-Cross-$SPIRV_CROSS_COMMIT -e SMOLV_SRC=/h/out/smol-v-$SMOLV_COMMIT \
    -e LDFLAGS="-static-libstdc++ -static-libgcc -s" $IMG sh /h/build.sh /o
  file "$out/pmspirvcross" 2>/dev/null || true
  exit 0
fi
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

sc=${SPIRV_CROSS_SRC:-$out/SPIRV-Cross-$SPIRV_CROSS_COMMIT}
sv=${SMOLV_SRC:-$out/smol-v-$SMOLV_COMMIT}
[ -f "$sc/spirv_glsl.cpp" ] || fetch KhronosGroup/SPIRV-Cross $SPIRV_CROSS_COMMIT "$sc"
[ -f "$sv/source/smolv.cpp" ] || fetch aras-p/smol-v $SMOLV_COMMIT "$sv"
[ -z "${SPIRVCROSS_FETCH_ONLY:-}" ] || exit 0

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
$CXX $CXXFLAGS -std=c++11 -Wall -I"$sc" -I"$sv/source" "$here/pmspirvcross.cpp" $objs ${LDFLAGS:-} -o "$out/pmspirvcross"
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
