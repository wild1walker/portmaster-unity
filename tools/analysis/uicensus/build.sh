#!/bin/bash
# build.sh <the game's Managed folder> [out dir]: uicensus.dll (UICensus.cs) -> <out dir> (default: here/out), with the
# .NET SDK (dotnet on PATH or ~/.dotnet). The game's UnityEngine.CoreModule.dll is referenced for the compile only.
# Use: copy it to the handheld and run the game with UA_PROBE=<s> UA_PROBE_DLL=<path of uicensus.dll>.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REF=$(cd "${1:?usage: build.sh <Managed folder> [out dir]}" && pwd)
OUT=${2:-$HERE/out}
[ -f "$REF/UnityEngine.CoreModule.dll" ] || { echo "build.sh: no UnityEngine.CoreModule.dll in $REF (Unity 2017.2 or later)"; exit 1; }
DOTNET=$(command -v dotnet || echo "$HOME/.dotnet/dotnet")
T=$(mktemp -d "${TMPDIR:-/tmp}/uicensus.XXXXXX"); trap 'rm -rf "$T"' EXIT
cp "$HERE/UICensus.cs" "$HERE/uicensus.csproj" "$T/"
"$DOTNET" build "$T/uicensus.csproj" -c Release -nologo -v quiet -p:UnityRef="$REF" -o "$T/out" >/dev/null
mkdir -p "$OUT"; cp "$T/out/uicensus.dll" "$OUT/"
echo "built $OUT/uicensus.dll"
