#!/bin/bash
# build.sh <Unity donor APK | a Unity player's Managed folder>: uaflipbook.dll (UAFlipbook.cs, the flip-book stand-in
# ua_host loads with UA_FLIPBOOK) -> ../out/uaflipbook.dll. Compiled with the .NET SDK (dotnet on PATH or ~/.dotnet)
# against the player's own mscorlib, netstandard, System, System.Core, UnityEngine.CoreModule and
# UnityEngine.AnimationModule, taken from the APK (or folder) for the compile only: nothing of Unity's is kept.
# Unity 2021.2+ (netstandard 2.1); the members it uses are the same from then on.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:?usage: build.sh <donor APK | Managed folder>}
DOTNET=$(command -v dotnet || echo "$HOME/.dotnet/dotnet")
[ -x "$DOTNET" ] || { echo "build.sh: no dotnet (the .NET SDK) on PATH or in ~/.dotnet"; exit 1; }
T=$(mktemp -d "${TMPDIR:-/tmp}/uaflipbook.XXXXXX"); trap 'rm -rf "$T"' EXIT
REFS="mscorlib netstandard System System.Core UnityEngine.CoreModule UnityEngine.AnimationModule"
mkdir -p "$T/ref"
for r in $REFS; do
  if [ -d "$SRC" ]; then cp "$SRC/$r.dll" "$T/ref/"
  else unzip -q -j -o "$SRC" "assets/bin/Data/Managed/$r.dll" -d "$T/ref"; fi
done
cp "$HERE/UAFlipbook.cs" "$HERE/uaflipbook.csproj" "$T/"
"$DOTNET" build "$T/uaflipbook.csproj" -c Release -nologo -v quiet -p:UnityRef="$T/ref" -o "$T/out" >/dev/null
mkdir -p "$HERE/../out"
cp "$T/out/uaflipbook.dll" "$HERE/../out/uaflipbook.dll"
echo "built $HERE/../out/uaflipbook.dll ($(wc -c < "$HERE/../out/uaflipbook.dll") bytes)"
