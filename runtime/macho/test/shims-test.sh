#!/bin/sh
# shims-test.sh - on a Mac with Xcode and Docker: builds the tests natively (build-mac.sh),
# runs each through the loader in the arm64 Linux image, and compares the two outputs.
set -e
cd "$(dirname "$0")/.."
sh test/build-mac.sh test/out
status=0
cxx="cxx/out/macos-arm64/libc++abi.1.dylib cxx/out/macos-arm64/libc++.1.dylib"
for t in darwin_c darwin_cxx; do
    pre=; [ $t = darwin_cxx ] && pre=$cxx       # the C++ test: with the libc++ built as Mach-O
    docker run --rm --platform linux/arm64 -e UA_A64FIX=all -v "$PWD":/w -w /w ua-llvm-arm64 \
        sh test/shims.sh run $pre test/out/$t.dylib > test/out/$t.got 2> test/out/$t.err || { echo "$t: exit $?"; status=1; }
    if diff test/out/$t.expect test/out/$t.got > test/out/$t.diff; then echo "$t: same as on macOS ($(wc -l < test/out/$t.got | tr -d ' ') lines)"
    else echo "$t: differs"; head -40 test/out/$t.diff; status=1; fi
    sed 's/^/  /' test/out/$t.err | head -20
done
exit $status
