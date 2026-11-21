#!/bin/bash
# tools/mesa-kbase/build.sh <mesa tree> <out dir>: Mesa Panfrost on kbase for aarch64 (README.md).
#   <mesa tree>  a copy of the Mesa tree mesa0's Step 0 builds (bmdhacks/armsx2-turnip malisx2 b5f8787 +
#                Knifethrower #4 #6 #7); changed in place: bmdhacks/malisx2's src/kbase copied in, kbase.patch
#                applied (once: a tree that has it is left as it is)
#   <out dir>    the installed libraries (libEGL.so.1, libGLESv2.so.2, libgallium-*.so): ship as mesa/ beside
#                ua_host for UA_GL_DRIVER=mesa
# Built in the ua-mesa-build24 image (Ubuntu 24.04, LLVM 18 for mesa_clc only; the driver is built without
# LLVM), build trees in the docker volume ua-mesa-kbase.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "${1:?mesa tree}" && pwd); OUT=${2:?out dir}
MALISX2=0e67eaa1a70ac8fc177c534729895f49e08886e7          # bmdhacks/malisx2 (MIT): src/kbase, UK 11.7 - 11.36
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
if [ ! -f "$SRC/src/panfrost/lib/kmod/kbase_kmod.c" ]; then
  T=$(mktemp -d)
  git -C "$T" init -q && git -C "$T" fetch -q --depth 1 https://github.com/bmdhacks/malisx2.git $MALISX2 && git -C "$T" checkout -q FETCH_HEAD
  cp -R "$T/src/kbase" "$SRC/src/panfrost/lib/kmod/kbase"
  cp "$T/LICENSE" "$SRC/src/panfrost/lib/kmod/kbase/LICENSE"
  rm -rf "$T"
  patch -d "$SRC" -p1 < "$HERE/kbase.patch"
fi
docker image inspect ua-mesa-build24 >/dev/null 2>&1 || { echo "no ua-mesa-build24 image (mesa0's Ubuntu 24.04 Mesa build image)"; exit 1; }
docker run --rm --pull=never --platform linux/arm64 -v "$SRC":/src:ro -v ua-mesa-kbase:/b -v "$OUT":/out ua-mesa-build24 bash -c '
  set -e
  if [ ! -x /b/clc-bin/mesa_clc ]; then
    rm -rf /b/clc
    meson setup /b/clc /src -Dmesa-clc=enabled -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= -Dllvm=enabled \
      -Dshared-llvm=enabled -Dinstall-mesa-clc=true -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dglx=disabled -Dgbm=disabled \
      -Dbuild-tests=false -Dallow-fallback-for=libdrm >/dev/null
    ninja -C /b/clc src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 >/dev/null
    mkdir -p /b/clc-bin && cp /b/clc/src/compiler/clc/mesa_clc /b/clc/src/compiler/spirv/vtn_bindgen2 /b/clc-bin/
  fi
  export PATH=/b/clc-bin:$PATH
  B=/b/o2
  [ -f $B/build.ninja ] || meson setup $B /src --prefix=/opt/mesa --libdir=lib --buildtype=release -Db_ndebug=true -Doptimization=2 \
      -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dllvm=disabled -Dtools= \
      -Dplatforms= -Degl=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=false -Dglx=disabled -Dgbm=disabled \
      -Dglvnd=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dxmlconfig=disabled \
      -Dshader-cache=disabled -Dbuild-tests=false -Dpanfrost-rust=false -Dmesa-clc=system -Dallow-fallback-for=libdrm >/dev/null
  ninja -C $B >/dev/null
  rm -rf /tmp/inst; DESTDIR=/tmp/inst ninja -C $B install >/dev/null
  rm -rf /out/*; cp -a /tmp/inst/opt/mesa/lib/. /out/
  find $B/subprojects -name "libdrm.so*" -exec cp -a {} /out/ \; 2>/dev/null || true
  ls /out'
