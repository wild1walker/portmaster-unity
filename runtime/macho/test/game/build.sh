#!/bin/bash
# build.sh <editor> <out dir>: the route's test game, from Unity's editor on macOS in batch mode
# (as tools/donor/build.sh builds the donor: same game, same licence needs). Needs the editor's
# Mac Build Support (IL2CPP) module and Xcode. Leaves <out dir>/Pong.app; the bridge's test reads
# Contents/Frameworks/GameAssembly.dylib and Contents/Resources/Data/il2cpp_data from it, and
# Contents/PlugIns/ARM64 has the game's two native plugins (checks_plugin.c). The game is only built
# here, never run on the Mac.
set -euo pipefail
export TZ=UTC
ED=${1:?usage: build.sh <editor> <out dir>}; OUT=${2:?}
HERE=$(cd "$(dirname "$0")" && pwd); DONOR=$HERE/../../../../tools/donor
P=$(mktemp -d)/game; mkdir -p "$P/Assets/Editor"
cp -R "$DONOR/Assets/Scripts" "$P/Assets/"; cp "$HERE/MachoChecks.cs" "$P/Assets/Scripts/"; cp "$HERE/MacBuild.cs" "$P/Assets/Editor/"
# the plugins as a game ships them: a library file, and a bundle folder (MacBuild.cs sets them
# up for the macOS player only, so the editor never loads them)
PL="$P/Assets/Plugins"; B="$PL/checks_bundle.bundle/Contents"; mkdir -p "$B/MacOS"
CF="-arch arm64 -mmacosx-version-min=11.0 -O2 -Wall -Wextra"
xcrun clang $CF -dynamiclib -install_name @rpath/lib_checks.dylib -o "$PL/lib_checks.dylib" "$HERE/checks_plugin.c"
xcrun clang $CF -bundle -DPC_BUNDLE -o "$B/MacOS/checks_bundle" "$HERE/checks_plugin.c"
cat > "$B/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>checks_bundle</string>
<key>CFBundleIdentifier</key><string>org.portmaster-unity.checks-bundle</string>
<key>CFBundlePackageType</key><string>BNDL</string>
</dict></plist>
EOF
BIN="$ED/Contents/MacOS/Unity"; [ -x "$BIN" ] || BIN="$ED"
"$BIN" -batchmode -nographics -quit -projectPath "$P" -executeMethod MacBuild.Il2cppArm64 -logFile "$P/build.log" \
  || { tail -60 "$P/build.log"; exit 1; }
mkdir -p "$OUT"; rm -rf "$OUT/Pong.app"; cp -R "$P/out/Pong.app" "$OUT/"; ls -la "$OUT/Pong.app/Contents/Frameworks"; find "$OUT/Pong.app/Contents/PlugIns" -type f
