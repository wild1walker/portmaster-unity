#!/bin/sh
# build-mac.sh - on a Mac with Xcode: builds the tests of the Darwin shims as macOS arm64
# images, in the three forms a linker leaves an image's pointers in: as a game of today is
# (macOS 11: rebases and binds as opcode streams) into OUT; as a build for macOS 12 and later
# is (chained fixups whose targets are offsets, the export trie's own load command,
# initializers as offsets) into OUT/chained; and as a build for macOS 11 that asked for chained
# fixups is (targets as addresses) into OUT/chained11. Each is run natively, the output kept
# beside it as the expectation for the loader's run on Linux (shims.sh).
#   sh test/build-mac.sh OUT
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=${1:?usage: build-mac.sh OUT}

plug() {    # NAME -DWHICH [link flags...]: one of dl_plug.c's; a name that ends in .bundle is one
    name=$1; which=$2; shift 2
    kind="-dynamiclib -install_name @rpath/$name"
    case $name in *.bundle) kind=-bundle ;; esac
    xcrun clang $flags $kind $fix -Wl,-rpath,@loader_path "$which" -o "$out/$name" "$here/dl_plug.c" "$@"
}

build() {   # OUT MIN-VERSION [the linker flag that picks the fixups' form]
    out=$1; fix=$3
    mkdir -p "$out"
    flags="-arch arm64 -mmacosx-version-min=$2 -mcpu=generic -O1 -g0 -Wall -Wno-format"
    xcrun clang $flags -dynamiclib $fix -install_name @rpath/darwin_c.dylib \
        -framework CoreFoundation -o "$out/darwin_c.dylib" "$here/darwin_c.c" "$here/darwin_ctx.c"
    xcrun clang $flags -o "$out/darwin_c.native" "$here/native_main.c" "$out/darwin_c.dylib" -Wl,-rpath,@loader_path
    xcrun clang++ $flags -std=c++20 -dynamiclib $fix -install_name @rpath/darwin_cxx.dylib \
        -o "$out/darwin_cxx.dylib" "$here/darwin_cxx.cpp"
    xcrun clang $flags -o "$out/darwin_cxx.native" "$here/native_main.c" "$out/darwin_cxx.dylib" -Wl,-rpath,@loader_path
    # the dlopen test and what it opens: dylibs that need others (found beside them), a bundle,
    # two that cannot load, one full of pointers for the loader to set
    plug dl_dep.dylib -DDL_DEP
    plug dl_plug.dylib -DDL_PLUG "$out/dl_dep.dylib"
    plug dl_inner.dylib -DDL_INNER "$out/dl_dep.dylib"
    plug dl_bundle.bundle -DDL_BUNDLE
    plug dl_local.dylib -DDL_LOCAL
    plug dl_late.dylib -DDL_LATE
    plug dl_race.dylib -DDL_RACE
    plug dl_bad.dylib -DDL_BAD -Wl,-U,_dl_no_such_variable
    plug dl_dep2.dylib -DDL_DEP2
    plug dl_bad2.dylib -DDL_BAD2 "$out/dl_dep2.dylib" -Wl,-U,_dl_no_such_variable
    plug dl_fixdep.dylib -DDL_FIXDEP
    plug dl_fix.dylib -DDL_FIX "$out/dl_fixdep.dylib" -Wl,-U,_fix_absent
    xcrun clang++ $flags -std=c++20 -dynamiclib $fix -install_name @rpath/dl_cxx.dylib \
        -o "$out/dl_cxx.dylib" "$here/dl_cxx.cpp"
    xcrun clang $flags -dynamiclib $fix -install_name @rpath/darwin_dl.dylib \
        -o "$out/darwin_dl.dylib" "$here/darwin_dl.c"
    xcrun clang $flags -o "$out/darwin_dl.native" "$here/native_main.c" "$out/darwin_dl.dylib" -Wl,-rpath,@loader_path
    # what the setup reads of a game's binary (meta_reg.c), with its symbols and without
    xcrun clang $flags -dynamiclib $fix -install_name @rpath/meta_reg.dylib \
        -o "$out/meta_reg.dylib" "$here/meta_reg.c"
    xcrun strip -x -o "$out/meta_reg_stripped.dylib" "$out/meta_reg.dylib"
    "$out/darwin_dl.native" > "$out/darwin_dl.expect" || echo "darwin_dl: failures natively (see $out/darwin_dl.expect)"
    "$out/darwin_cxx.native" > "$out/darwin_cxx.expect" || echo "darwin_cxx: failures natively (see $out/darwin_cxx.expect)"
    "$out/darwin_c.native" > "$out/darwin_c.expect" || echo "darwin_c: failures natively (see $out/darwin_c.expect)"
    # the form asked for is the form made
    for f in "$out"/*.dylib "$out"/*.bundle; do
        if otool -l "$f" | grep -q LC_DYLD_CHAINED_FIXUPS; then has=chained; else has=streams; fi
        [ "$has" = "$want" ] || { echo "$f: $has, not $want"; exit 1; }
    done
}

want=streams; build "$top" 11.0 -Wl,-no_fixup_chains
want=chained; build "$top/chained" 12.0
want=chained; build "$top/chained11" 11.0 -Wl,-fixup_chains
