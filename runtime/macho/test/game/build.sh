#!/bin/bash
# build.sh <editor> <out dir>: the route's test game, from Unity's editor on macOS in batch mode
# (as tools/donor/build.sh builds the donor: same game, same licence needs). Needs the editor's
# Mac Build Support (IL2CPP) module and Xcode. Leaves <out dir>/Pong.app; the bridge's test reads
# Contents/Frameworks/GameAssembly.dylib and Contents/Resources/Data/il2cpp_data from it. The
# game is only built here, never run on the Mac.
set -euo pipefail
export TZ=UTC
ED=${1:?usage: build.sh <editor> <out dir>}; OUT=${2:?}
HERE=$(cd "$(dirname "$0")" && pwd); DONOR=$HERE/../../../../tools/donor
P=$(mktemp -d)/game; mkdir -p "$P/Assets/Editor"
cp -R "$DONOR/Assets/Scripts" "$P/Assets/"; cp "$HERE/MachoChecks.cs" "$P/Assets/Scripts/"; cp "$HERE/MacBuild.cs" "$P/Assets/Editor/"
BIN="$ED/Contents/MacOS/Unity"; [ -x "$BIN" ] || BIN="$ED"
"$BIN" -batchmode -nographics -quit -projectPath "$P" -executeMethod MacBuild.Il2cppArm64 -logFile "$P/build.log" \
  || { tail -60 "$P/build.log"; exit 1; }
mkdir -p "$OUT"; rm -rf "$OUT/Pong.app"; cp -R "$P/out/Pong.app" "$OUT/"; ls -la "$OUT/Pong.app/Contents/Frameworks"
