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
# the dlopen test and what it opens: dylibs that need others (found beside them), a bundle, two
# that cannot load
plug() {    # NAME -DWHICH [link flags...]: one of dl_plug.c's; a name that ends in .bundle is one
    name=$1; which=$2; shift 2
    kind="-dynamiclib -install_name @rpath/$name"
    case $name in *.bundle) kind=-bundle ;; esac
    xcrun clang $flags $kind -Wl,-no_fixup_chains -Wl,-rpath,@loader_path "$which" -o "$out/$name" "$here/dl_plug.c" "$@"
}
plug dl_dep.dylib -DDL_DEP
plug dl_plug.dylib -DDL_PLUG "$out/dl_dep.dylib"
plug dl_inner.dylib -DDL_INNER "$out/dl_dep.dylib"
plug dl_bundle.bundle -DDL_BUNDLE
plug dl_local.dylib -DDL_LOCAL
plug dl_late.dylib -DDL_LATE
plug dl_race.dylib -DDL_RACE
plug dl_bad.dylib -DDL_BAD -undefined dynamic_lookup
plug dl_dep2.dylib -DDL_DEP2
plug dl_bad2.dylib -DDL_BAD2 "$out/dl_dep2.dylib" -undefined dynamic_lookup
xcrun clang++ $flags -std=c++20 -dynamiclib -Wl,-no_fixup_chains -install_name @rpath/dl_cxx.dylib \
    -o "$out/dl_cxx.dylib" "$here/dl_cxx.cpp"
xcrun clang $flags -dynamiclib -Wl,-no_fixup_chains -install_name @rpath/darwin_dl.dylib \
    -o "$out/darwin_dl.dylib" "$here/darwin_dl.c"
xcrun clang $flags -o "$out/darwin_dl.native" "$here/native_main.c" "$out/darwin_dl.dylib" -Wl,-rpath,@loader_path
"$out/darwin_dl.native" > "$out/darwin_dl.expect" || echo "darwin_dl: failures natively (see $out/darwin_dl.expect)"
"$out/darwin_cxx.native" > "$out/darwin_cxx.expect" || echo "darwin_cxx: failures natively (see $out/darwin_cxx.expect)"
"$out/darwin_c.native" > "$out/darwin_c.expect" || echo "darwin_c: failures natively (see $out/darwin_c.expect)"
