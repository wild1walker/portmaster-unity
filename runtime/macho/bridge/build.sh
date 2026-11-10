#!/bin/sh
# Builds the Mach-O route's bridge for aarch64 glibc in the ua-llvm-arm64 image (clang, as the
# unwinder beside it): out/arm64/libil2cpp_macho.so, which a port's lib folder holds as
# libil2cpp.so next to the game's GameAssembly.dylib and ../cxx/out/macos-arm64's two dylibs.
set -e
cd "$(dirname "$0")/.."
mkdir -p bridge/out/arm64
# No build of the image from here: an inspect that fails for a moment would start a pull.
docker image inspect ua-llvm-arm64 >/dev/null 2>&1 ||
  { echo "no ua-llvm-arm64 image: docker build --platform linux/arm64 -t ua-llvm-arm64 runtime/docker/llvm-arm64" >&2; exit 1; }
docker run --rm --pull=never --platform linux/arm64 -v "$PWD":/w -w /w ua-llvm-arm64 sh -c '
  set -e
  SRC="macho.c a64fix.c dyld.c tlv.c tlv_thunk.S sh_asm.S sh_table.c sh_errno.c sh_mem.c sh_misc.c sh_signal.c sh_context.c
       sh_stdio.c sh_file.c sh_time.c sh_thread.c sh_locale.c sh_net.c sh_cf.c bridge/bridge.c bridge/bridge.S"
  clang-19 -O2 -g -Wall -Wextra -Wno-unused-parameter -fPIC -fvisibility=hidden -pthread -shared \
    -Wl,-soname,libil2cpp.so -Wl,-z,now -Wl,-z,defs -Wl,--exclude-libs,ALL \
    -o bridge/out/arm64/libil2cpp_macho.so $SRC cxx/out/arm64/libunwind.a -ldl -lm
  n=$(nm -D --defined-only bridge/out/arm64/libil2cpp_macho.so | grep -c " T il2cpp_")
  echo "bridge/out/arm64/libil2cpp_macho.so: $n il2cpp_* functions"
  nm -D --defined-only bridge/out/arm64/libil2cpp_macho.so | grep -v " T il2cpp_" || true
  readelf -lW bridge/out/arm64/libil2cpp_macho.so | grep -E "TLS|GNU_STACK"'
