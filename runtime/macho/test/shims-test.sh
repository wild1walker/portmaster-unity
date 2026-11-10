#!/bin/sh
# shims-test.sh - on a Mac with Xcode and Docker: builds the tests natively (build-mac.sh),
# runs each through the loader in the arm64 Linux image, and compares the two outputs. Every
# form of build: test/out (opcode streams), test/out/chained (chained fixups, as a build for
# macOS 12 has them) and test/out/chained11 (as a build for macOS 11 that asks for them).
set -e
cd "$(dirname "$0")/.."
sh test/build-mac.sh test/out
status=0
cxx="cxx/out/macos-arm64/libc++abi.1.dylib cxx/out/macos-arm64/libc++.1.dylib"
for o in test/out test/out/chained test/out/chained11; do
    case $o in */chained) form="chained fixups" ;; */chained11) form="chained fixups, macOS 11" ;; *) form="opcode streams" ;; esac
    for t in darwin_c darwin_cxx darwin_dl; do
        pre=; [ $t = darwin_cxx ] && pre=$cxx       # the C++ test: with the libc++ built as Mach-O
        docker run --rm --platform linux/arm64 -e UA_A64FIX=all -v "$PWD":/w -w /w ua-llvm-arm64 \
            sh test/shims.sh run $pre $o/$t.dylib > $o/$t.got 2> $o/$t.err || { echo "$t ($form): exit $?"; status=1; }
        if diff $o/$t.expect $o/$t.got > $o/$t.diff; then echo "$t ($form): same as on macOS ($(wc -l < $o/$t.got | tr -d ' ') lines)"
        else echo "$t ($form): differs"; head -40 $o/$t.diff; status=1; fi
        sed 's/^/  /' $o/$t.err | head -20
    done
    # the names a game's code may have for a plugin: macOS itself opens none of them without the
    # bundle's own search paths, so this run is compared with what the loader must answer
    docker run --rm --platform linux/arm64 -e UA_A64FIX=all -v "$PWD":/w -w /w ua-llvm-arm64 \
        sh test/shims.sh run $o/darwin_dl.dylib -- probe > $o/darwin_dl.probe.got 2> $o/darwin_dl.probe.err || { echo "darwin_dl probe ($form): exit $?"; status=1; }
    if diff test/darwin_dl.probe $o/darwin_dl.probe.got > $o/darwin_dl.probe.diff; then echo "darwin_dl probe ($form): as expected ($(wc -l < $o/darwin_dl.probe.got | tr -d ' ') lines)"
    else echo "darwin_dl probe ($form): differs"; head -40 $o/darwin_dl.probe.diff; status=1; fi
    sed 's/^/  /' $o/darwin_dl.probe.err | head -20
done
exit $status
