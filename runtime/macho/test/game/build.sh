#!/bin/bash
# build.sh <editor> <out dir> [oldest macOS]: the route's test game, from Unity's editor on macOS in batch mode
# (as tools/donor/build.sh builds the donor: same game, same licence needs). Needs the editor's
# Mac Build Support (IL2CPP) module and Xcode. Leaves <out dir>/Pong.app; the bridge's test reads
# Contents/Frameworks/GameAssembly.dylib and Contents/Resources/Data/il2cpp_data from it, and
# Contents/PlugIns has the game's native plugins (checks_plugin.c: a library, a bundle, and a
# stand-in of the name Steamworks' library has) and the library Burst compiled the checks' jobs
# into, and <out dir>/build.log is the editor's log. The game is only
# built here, never run on the Mac. The project is made in a temporary folder inside <out dir>
# (gigabytes with the packages) and removed when the script ends. The third argument is the
# oldest macOS the game and its plugins ask for (left out: 11.0, the editor's own; with 12.0 or
# later the linker writes chained fixups, the form a newer game has), and the last lines say
# which form of fixups each binary came out with.
set -euo pipefail
export TZ=UTC
ED=${1:?usage: build.sh <editor> <out dir> [oldest macOS]}; OUT=${2:?}; MIN=${3:-}
HERE=$(cd "$(dirname "$0")" && pwd); DONOR=$HERE/../../../../tools/donor
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
T=$(mktemp -d "$OUT/project.XXXXXX"); trap 'rm -rf "$T"' EXIT
P=$T/game; mkdir -p "$P/Assets/Editor" "$P/Packages"
cp -R "$DONOR/Assets/Scripts" "$P/Assets/"; cp "$HERE/MachoChecks.cs" "$P/Assets/Scripts/"; cp "$HERE/MacBuild.cs" "$P/Assets/Editor/"
# The packages a new project has (the editor's own list) and Burst, at the version the editor
# carries: all of them are inside the editor, nothing is fetched.
python3 -I - "$ED/Contents/Resources/PackageManager/Editor/manifest.json" > "$P/Packages/manifest.json" <<'EOF'
import json, sys
packages = json.load(open(sys.argv[1]))["packages"]
deps = {name: p["version"] for name, p in packages.items() if p.get("isDefault") or name == "com.unity.burst"}
if "com.unity.burst" not in deps: sys.exit("the editor carries no com.unity.burst")
json.dump({"dependencies": dict(sorted(deps.items()))}, sys.stdout, indent=2)
EOF
# the plugins as a game ships them: a library file, and bundle folders (MacBuild.cs sets them
# up for the macOS player only, so the editor never loads them)
PL="$P/Assets/Plugins"; mkdir -p "$PL"
CF="-arch arm64 -mmacosx-version-min=${MIN:-11.0} -O2 -Wall -Wextra"
xcrun clang $CF -dynamiclib -install_name @rpath/lib_checks.dylib -o "$PL/lib_checks.dylib" "$HERE/checks_plugin.c"
bundle() {    # bundle <name> <define> <identifier>: a bundle folder as Xcode makes one
  local b="$PL/$1.bundle/Contents"; mkdir -p "$b/MacOS"
  xcrun clang $CF -bundle "-D$2" -o "$b/MacOS/$1" "$HERE/checks_plugin.c"
  cat > "$b/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>$1</string>
<key>CFBundleIdentifier</key><string>org.portmaster-unity.$3</string>
<key>CFBundlePackageType</key><string>BNDL</string>
</dict></plist>
EOF
}
bundle checks_bundle PC_BUNDLE checks-bundle
bundle steam_api PC_STEAM checks-steam-stand-in
BIN="$ED/Contents/MacOS/Unity"; [ -x "$BIN" ] || BIN="$ED"
PONG_MACOS_MIN=$MIN "$BIN" -batchmode -nographics -quit -projectPath "$P" -executeMethod MacBuild.Il2cppArm64 -logFile "$OUT/build.log" \
  || { tail -60 "$OUT/build.log"; exit 1; }
rm -rf "$OUT/Pong.app"; mv "$P/out/Pong.app" "$OUT/"; ls -la "$OUT/Pong.app/Contents/Frameworks"
{ echo "$OUT/Pong.app/Contents/Frameworks/GameAssembly.dylib"; find "$OUT/Pong.app/Contents/PlugIns" -type f; } | while read -r f; do
  file -b "$f" | grep -q Mach-O || continue
  if xcrun otool -l "$f" | grep -q LC_DYLD_CHAINED_FIXUPS; then form="chained fixups"; else form="opcode streams"; fi
  echo "${f#"$OUT/"}: $form"
done
