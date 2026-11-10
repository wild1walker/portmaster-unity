#!/bin/sh
# build-mac.sh - on a Mac with Xcode: builds the tests of the Darwin shims as macOS arm64
# images (as a game's are: macOS 11, no chained fixups) and runs each natively, the output
# kept beside it as the expectation for the loader's run on Linux (shims.sh).
#   sh test/build-mac.sh OUT
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build-mac.sh OUT}
mkdir -p "$out"
flags="-arch arm64 -mmacosx-version-min=11.0 -mcpu=generic -O1 -g0 -Wall -Wno-format"
xcrun clang $flags -dynamiclib -Wl,-no_fixup_chains -install_name @rpath/darwin_c.dylib \
    -framework CoreFoundation -o "$out/darwin_c.dylib" "$here/darwin_c.c" "$here/darwin_ctx.c"
xcrun clang $flags -o "$out/darwin_c.native" "$here/native_main.c" "$out/darwin_c.dylib" -Wl,-rpath,@loader_path
xcrun clang++ $flags -std=c++20 -dynamiclib -Wl,-no_fixup_chains -install_name @rpath/darwin_cxx.dylib \
    -o "$out/darwin_cxx.dylib" "$here/darwin_cxx.cpp"
xcrun clang $flags -o "$out/darwin_cxx.native" "$here/native_main.c" "$out/darwin_cxx.dylib" -Wl,-rpath,@loader_path
"$out/darwin_cxx.native" > "$out/darwin_cxx.expect" || echo "darwin_cxx: failures natively (see $out/darwin_cxx.expect)"
"$out/darwin_c.native" > "$out/darwin_c.expect" || echo "darwin_c: failures natively (see $out/darwin_c.expect)"
