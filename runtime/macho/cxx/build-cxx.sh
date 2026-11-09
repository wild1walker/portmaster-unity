#!/bin/sh
# build-cxx.sh: the C++ runtime macOS game code links against, built for aarch64 glibc in the
# ua-llvm-arm64 image (runtime/docker/llvm-arm64) -> out/arm64/{libc++,libc++abi,libunwind}.a,
# position-independent and hidden, for the Mach-O bridge to link in (its imports of libc++.1.dylib
# are bound to these). LLVM 19.1.7's runtimes from the release's source tarball (downloaded once
# into $LLVM_CACHE, default ../../../.cache, checked against its sha256), configured as Apple's
# arm64 libc++ is, so the inline code in the game and the library agree:
#   _LIBCPP_ABI_ALTERNATE_STRING_LAYOUT          std::string's arm64 Apple layout
#   _LIBCPP_TYPEINFO_COMPARISON_IMPLEMENTATION=3 type_info's NonUniqueARMRTTIBit (a type name
#                                                pointer with bit 63 set compares by string)
# and libunwind patched (patch-libunwind.py) to read the images' compact unwind info. Atomics are
# inline exclusive loads and stores (-mno-outline-atomics): the handhelds' cores have no LSE, and
# gcc 9's libgcc has no __aarch64_* helpers.
set -e
cd "$(dirname "$0")"
V=19.1.7
SHA=82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501
CACHE=${LLVM_CACHE:-$PWD/../../../.cache}
mkdir -p "$CACHE" out/arm64
T="$CACHE/llvm-project-$V.src.tar.xz"
if [ ! -f "$T" ]; then
  curl -fL --retry 5 -o "$T.part" "https://github.com/llvm/llvm-project/releases/download/llvmorg-$V/llvm-project-$V.src.tar.xz"
  mv "$T.part" "$T"
fi
echo "$SHA  $T" | shasum -a 256 -c - >/dev/null || { echo "$T: sha256 differs" >&2; exit 1; }
docker image inspect ua-llvm-arm64 >/dev/null 2>&1 || docker build --platform linux/arm64 -t ua-llvm-arm64 ../../docker/llvm-arm64
docker run --rm --platform linux/arm64 -v "$PWD":/w -v "$CACHE":/cache:ro -w /tmp ua-llvm-arm64 sh -c '
  set -e
  tar -xJf /cache/llvm-project-'"$V"'.src.tar.xz llvm-project-'"$V"'.src/libunwind llvm-project-'"$V"'.src/libcxx \
    llvm-project-'"$V"'.src/libcxxabi llvm-project-'"$V"'.src/runtimes llvm-project-'"$V"'.src/cmake \
    llvm-project-'"$V"'.src/llvm/cmake llvm-project-'"$V"'.src/llvm/utils/llvm-lit llvm-project-'"$V"'.src/llvm/utils/lit
  S=/tmp/llvm-project-'"$V"'.src
  python3 /w/patch-libunwind.py $S
  cp /w/ua_unwind.h $S/libunwind/src/
  F="-DUA_MACHO_COMPACT -D_LIBCPP_TYPEINFO_COMPARISON_IMPLEMENTATION=3 -fno-omit-frame-pointer -mno-outline-atomics"
  cmake -G Ninja -S $S/runtimes -B /tmp/b -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F" \
    -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
    -DLIBUNWIND_ENABLE_SHARED=OFF -DLIBUNWIND_ENABLE_STATIC=ON -DLIBUNWIND_INCLUDE_TESTS=OFF \
    -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_STATIC=ON -DLIBCXXABI_USE_LLVM_UNWINDER=ON \
    -DLIBCXXABI_HERMETIC_STATIC_LIBRARY=ON -DLIBCXXABI_INCLUDE_TESTS=OFF \
    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_STATIC=ON -DLIBCXX_HERMETIC_STATIC_LIBRARY=ON \
    -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXX_ABI_DEFINES="_LIBCPP_ABI_ALTERNATE_STRING_LAYOUT" \
    -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
    > /tmp/cmake.log || { tail -40 /tmp/cmake.log; exit 1; }
  ninja -C /tmp/b unwind_static cxxabi_static cxx_static > /tmp/ninja.log || { tail -60 /tmp/ninja.log; exit 1; }
  cp /tmp/b/lib/libc++.a /tmp/b/lib/libc++abi.a /tmp/b/lib/libunwind.a /w/out/arm64/
  ls -l /w/out/arm64'
