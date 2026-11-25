#!/bin/bash
# run.sh <libmonobdwgc-2.0.so> <class library folder (mscorlib.dll, System.dll)>: the cached invoke's exactness test.
# FiTest.dll's methods (exceptions of every kind, finally blocks, rethrows, inner exceptions, a failing type initialiser,
# a virtual method called on a subclass, allocation and a collection, a return value, a static method, arguments) are
# called through mono_runtime_invoke and then through the runtime-invoke wrapper mono_unity_runtime_invoke_info gives,
# each way on a fresh object; the side effects and the exception (type, message, stack trace, inner) must be the same
# ("RESULT 0 differ"). A 6th argument to fihost compares mono_method_get_unmanaged_thunk instead (it differs: virtual
# dispatch and its own frame in every stack trace). Builds FiTest.dll with the .NET SDK (DOTNET, default dotnet on
# PATH, else ~/.dotnet/dotnet), runs in the ua-build-arm64 container.
set -euo pipefail
cd "$(dirname "$0")"
RT=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); CL=$(cd "$2" && pwd)
DOTNET=${DOTNET:-$(command -v dotnet || echo "$HOME/.dotnet/dotnet")}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 "$DOTNET" build FiTest.csproj -c Release -p:CLASSLIBS="$CL" -o "$W/out" -v q -nologo >/dev/null
cp fihost.c "$W/"
docker run --rm -v "$W":/f -v "$RT":/rt/libmonobdwgc-2.0.so:ro -v "$CL":/cl:ro ua-build-arm64 sh -c \
  'cd /f && gcc -O1 -o fihost fihost.c -ldl && timeout 120 ./fihost /rt/libmonobdwgc-2.0.so /cl /f/out/FiTest.dll 17' | grep -E "^SAME|^DIFF|^  |RESULT"
rm -rf obj bin
