#!/bin/bash
# build.sh <Unity donor APK | a Unity player's Managed folder>: uaflipbook.dll (UAFlipbook.cs, the flip-book stand-in
# ua_host loads with UA_FLIPBOOK) -> ../out/uaflipbook.dll. Compiled with the .NET SDK (dotnet on PATH or ~/.dotnet)
# against the player's own mscorlib, netstandard, System, System.Core, UnityEngine.CoreModule and
# UnityEngine.AnimationModule, taken from the APK (or folder) for the compile only: nothing of Unity's is kept.
# Unity 2021.2+ (netstandard 2.1); the members it uses are the same from then on.
# build.sh probe: uaprobe.dll (UAProbe.cs, the feature probe ua_host loads with UA_PROBE) -> ../out/uaprobe.dll, for
# .NET 3.5 against Microsoft's reference assemblies from NuGet (no APK needed; it reaches Unity by reflection).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:?usage: build.sh <donor APK | Managed folder> | probe}
DOTNET=$(command -v dotnet || echo "$HOME/.dotnet/dotnet")
[ -x "$DOTNET" ] || { echo "build.sh: no dotnet (the .NET SDK) on PATH or in ~/.dotnet"; exit 1; }
T=$(mktemp -d "${TMPDIR:-/tmp}/uaflipbook.XXXXXX"); trap 'rm -rf "$T"' EXIT
if [ "$SRC" = probe ]; then
  cp "$HERE/UAProbe.cs" "$HERE/uaprobe.csproj" "$T/"
  "$DOTNET" build "$T/uaprobe.csproj" -c Release -nologo -v quiet -o "$T/out" >/dev/null
  mkdir -p "$HERE/../out"
  cp "$T/out/uaprobe.dll" "$HERE/../out/uaprobe.dll"
  echo "built $HERE/../out/uaprobe.dll ($(wc -c < "$HERE/../out/uaprobe.dll") bytes)"
  exit 0
fi
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
