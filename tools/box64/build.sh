#!/bin/bash
# Builds box64 and gl4es for aarch64 from their official release tags, in Docker (Debian 11,
# glibc 2.31; native on an Apple-silicon Mac or any arm64 host, emulated elsewhere).
#
#   tools/box64/build.sh [out dir]          (default: tools/box64/out)
#
# Output (nothing of it goes into the repository):
#   out/box64/box64              the emulator (generic ARM64 dynarec build, stripped)
#   out/box64/x64lib/            box64's x86_64 libstdc++/libgcc_s (the game's code loads them emulated)
#   out/gl4es/libGL.so.1         gl4es (OpenGL 2.1 on GLES 2), for Unity players with a GL 2 renderer
#   out/glxes/libGL.so.1         glxes (ours, glxes/glxes.c): GLX for OpenGL ES contexts on EGL, for
#                                Unity's -force-gles mode on GLES-only drivers
#   out/licenses/                LICENSE.box64.txt, LICENSE.gl4es.txt, the GCC runtime licence note
#   out/versions.txt             tags, commits, build flags, the highest glibc symbol version needed
#
# BOX64_TAG / GL4ES_TAG pick other release tags; BOX64_CMAKE adds cmake flags (e.g. -DBOX32=ON for
# 32-bit x86 games, experimental in box64 0.4.x; see docs/box64-route.md). WORK=<dir> keeps the
# sources and the build tree there (about 400 MB; default: inside the container, discarded).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$here/out}
BOX64_TAG=${BOX64_TAG:-v0.4.4}
GL4ES_TAG=${GL4ES_TAG:-v1.1.6}
IMG=pmu-box64-build
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
docker build -q --platform linux/arm64 -t $IMG -f "$here/docker/Dockerfile.build" "$here/docker" >/dev/null
work=(); [ -n "${WORK:-}" ] && { mkdir -p "$WORK"; work=(-v "$(cd "$WORK" && pwd)":/src); }
docker run --rm --platform linux/arm64 -v "$OUT":/out -v "$here/glxes":/glxes:ro ${work[@]+"${work[@]}"} -e BOX64_TAG="$BOX64_TAG" -e GL4ES_TAG="$GL4ES_TAG" \
  -e BOX64_CMAKE="${BOX64_CMAKE:-}" -e JOBS="${JOBS:-3}" $IMG bash -euc '
J=${JOBS:-3}   # the dynarec sources need ~1 GB per compiler; Docker Desktop VMs are small
[ -d box64/.git ] || git clone -q --depth 1 --branch "$BOX64_TAG" https://github.com/ptitSeb/box64.git box64 2>/dev/null
[ -d gl4es/.git ] || git clone -q --depth 1 --branch "$GL4ES_TAG" https://github.com/ptitSeb/gl4es.git gl4es 2>/dev/null
# Debian 11 has CMake 3.18; box64 asks for 3.19 only for file(CHMOD) of its box64-python helper
# (not built into the binary): lower the minimum and set that mode with chmod instead.
sed -i "s/cmake_minimum_required(VERSION 3.19)/cmake_minimum_required(VERSION 3.18)/" box64/CMakeLists.txt
python3 - box64/CMakeLists.txt <<"PY"
import re, sys
p = sys.argv[1]; s = open(p).read()
s = re.sub(r"file\(CHMOD (\S+)\s+FILE_PERMISSIONS[^)]*\)", r"execute_process(COMMAND chmod 755 \1)", s)
open(p, "w").write(s)
PY
cmake -S box64 -B box64/build -DARM64=ON -DARM_DYNAREC=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNOGIT=OFF $BOX64_CMAKE >/dev/null
make -C box64/build -j$J box64 >/tmp/box64.make.log 2>&1 || { grep -B5 -A10 -m3 -E "error:|Error [0-9]" /tmp/box64.make.log | head -60; exit 1; }
cmake -S gl4es -B gl4es/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNOX11=OFF >/dev/null
make -C gl4es/build -j$J >/tmp/gl4es.make.log 2>&1 || { grep -B5 -A10 -m3 -E "error:|Error [0-9]" /tmp/gl4es.make.log | head -60; exit 1; }
rm -rf /out/box64 /out/gl4es /out/glxes /out/licenses; mkdir -p /out/box64/x64lib /out/gl4es /out/glxes /out/licenses
gcc -O2 -Wall -Wextra -fPIC -shared -fvisibility=hidden -Wl,-soname,libGL.so.1 -o /out/glxes/libGL.so.1 \
  /glxes/glxes.c -lEGL -lGLESv2 -lX11 -ldl && strip /out/glxes/libGL.so.1
strip -o /out/box64/box64 box64/build/box64
cp box64/x64lib/libstdc++.so.6 box64/x64lib/libgcc_s.so.1 /out/box64/x64lib/
cp gl4es/lib/libGL.so.1 /out/gl4es/libGL.so.1 && strip /out/gl4es/libGL.so.1
cp box64/LICENSE /out/licenses/LICENSE.box64.txt
cp gl4es/LICENSE /out/licenses/LICENSE.gl4es.txt
cat > /out/licenses/LICENSE.gcc-runtime.txt <<"TXT"
box64/x64lib/libstdc++.so.6 and libgcc_s.so.1 are GCC runtime libraries as box64 ships them
(https://github.com/ptitSeb/box64/tree/main/x64lib): GPL-3.0 with the GCC Runtime Library
Exception 3.1 (https://www.gnu.org/licenses/gcc-exception-3.1.html).
TXT
glibc() { objdump -T "$1" | grep -o "GLIBC_[0-9.]*" | sort -t. -k2,2n -k3,3n -u | tail -1; }
{
  echo "box64 $BOX64_TAG $(git -C box64 rev-parse HEAD) cmake -DARM64=ON -DARM_DYNAREC=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo $BOX64_CMAKE"
  echo "gl4es $GL4ES_TAG $(git -C gl4es rev-parse HEAD) cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo"
  echo "built on $(. /etc/os-release; echo $PRETTY_NAME), $(ldd --version | head -1)"
  echo "box64 needs $(glibc /out/box64/box64), gl4es needs $(glibc /out/gl4es/libGL.so.1), glxes needs $(glibc /out/glxes/libGL.so.1)"
} > /out/versions.txt
cat /out/versions.txt
'
