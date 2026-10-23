#!/bin/bash
# Builds asmpatch with the .NET 8 SDK (DOTNET, default: dotnet on PATH, else ~/.dotnet/dotnet):
#   out/any/asmpatch.dll           any machine with a .NET 8+ runtime: dotnet out/any/asmpatch.dll ...
#                                  (plus out/any/asmpatch, an apphost for the building machine)
#   out/linux-arm64/asmpatch       self-contained single file for an aarch64 handheld (no .NET there),
#                                  built as portmaster-cuphead builds its code patcher
#   build.sh test                  also builds and runs the tests (test/: fake Rewired assemblies
#                                  made with Cecil, patched, loaded and called)
# Packages come from NuGet (Mono.Cecil 0.11.6; for linux-arm64 the .NET runtime pack and ILLink).
set -euo pipefail
cd "$(dirname "$0")"
DOTNET=${DOTNET:-$(command -v dotnet || echo "$HOME/.dotnet/dotnet")}
export DOTNET_ROOT=${DOTNET_ROOT:-$(dirname "$DOTNET")} DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
"$DOTNET" build asmpatch.csproj -c Release -o out/any -v q -nologo | grep -E "error|warn" || true
[ -f out/any/asmpatch.dll ] || { echo "asmpatch: build failed"; exit 1; }
"$DOTNET" publish asmpatch.csproj -c Release -r linux-arm64 --self-contained -p:PublishSingleFile=true \
  -p:PublishTrimmed=true -p:InvariantGlobalization=true -p:DebugType=none -p:TrimmerRemoveSymbols=true \
  -o out/linux-arm64 -v q -nologo | grep -E "error|warn" || true
[ -f out/linux-arm64/asmpatch ] || { echo "asmpatch: linux-arm64 publish failed"; exit 1; }
# it goes into ports: it must not name this machine's folders (ILLink stamps the trimmed DLLs with
# obj/'s path unless TrimmerRemoveSymbols; an obj/ trimmed before it keeps them: delete obj/)
if LC_ALL=C grep -a -q -F "$PWD" out/linux-arm64/asmpatch; then
  echo "asmpatch: out/linux-arm64/asmpatch names $PWD (delete obj/ and build again)"; exit 1
fi
echo "built out/any/asmpatch.dll, out/linux-arm64/asmpatch"
if [ "${1:-}" = test ]; then
  "$DOTNET" build test/asmpatch-test.csproj -c Release -o out/test -v q -nologo | grep -E "error|warn" || true
  "$DOTNET" out/test/asmpatch-test.dll
fi
