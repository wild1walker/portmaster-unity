#!/bin/sh
# gen.sh - on a Mac with Xcode's command line tools: writes ../darwin_abi.h from the SDK's headers.
set -e
cd "$(dirname "$0")"
T=$(mktemp -d)
xcrun clang -arch arm64 -Wall -Wno-deprecated-declarations -o "$T/gen" gen.c -framework CoreFoundation
"$T/gen" > ../darwin_abi.h
rm -rf "$T"
wc -l ../darwin_abi.h
