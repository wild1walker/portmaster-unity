#!/bin/bash
# build.sh <release tag>: the Cuphead port's codepatch (codepatch/Program.cs of that tag of
# wild1walker/portmaster-cuphead) for WebAssembly, into codepatch/_framework, for tools/build.js.
# Needs the .NET 8 SDK with the wasm-tools workload (`dotnet workload install wasm-tools`) and curl.
# After a new tag: put the CRC-32 of the release zip's setup/codepatch entry into known.json.
set -euo pipefail
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
H=$(cd "$(dirname "$0")" && pwd)
TAG=${1:?release tag, e.g. 2026.10.29}
B=$(mktemp -d); trap 'rm -rf "$B"' EXIT
mkdir -p "$B/codepatch-wasm" "$B/repo/codepatch"
cp "$H"/*.csproj "$H"/*.cs "$B/codepatch-wasm/"
curl -fsSL "https://raw.githubusercontent.com/wild1walker/portmaster-cuphead/$TAG/codepatch/Program.cs" -o "$B/repo/codepatch/Program.cs"
# its one `(dynamic)` (an operand that is a MethodReference or a FieldReference) as the overload the C# binder
# picks at run time: the binder and System.Linq.Expressions stay out of the build (~250 KB gzipped)
DYN='il.Create(getCam.OpCode, (dynamic)getCam.Operand)'
grep -qF "$DYN" "$B/repo/codepatch/Program.cs" || { echo "the (dynamic) line changed in $TAG: update build.sh"; exit 1; }
sed -i 's/il\.Create(getCam\.OpCode, (dynamic)getCam\.Operand)/(getCam.Operand is MethodReference dynM ? il.Create(getCam.OpCode, dynM) : il.Create(getCam.OpCode, (FieldReference)getCam.Operand))/' "$B/repo/codepatch/Program.cs"
if grep -q "dynamic" "$B/repo/codepatch/Program.cs"; then echo "dynamic still in the source"; exit 1; fi
(cd "$B/codepatch-wasm" && dotnet publish -c Release -o "$B/publish" | tail -15)
# the page embeds the plain files (no precompressed .br/.gz, no .pdb)
rm -rf "$H/_framework"; mkdir -p "$H/_framework"
find "$B/publish/wwwroot/_framework" -maxdepth 1 -type f ! -name '*.br' ! -name '*.gz' ! -name '*.pdb' -exec cp {} "$H/_framework/" \;
echo "$TAG" > "$H/tag.txt"
du -sh "$H/_framework"
