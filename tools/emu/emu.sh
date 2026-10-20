#!/bin/bash
# PC side: runs a game on the runtime emulated (QEMU armv7 or AArch64, Mesa llvmpipe, pbuffer).
# Frame times are meaningless; frame contents, GL work counts and game logic match the device.
#   emu.sh <log name> <seconds> [release|dev]
# ARCH=armhf (default) | arm64   the player's ABI: armeabi-v7a in ua-emu-armhf, arm64-v8a in
#                  ua-emu-arm64 (built from tools/emu/Dockerfile on first use), with that build's ua_host
# EMU_ROOT=<dir>   the emulated device root (lib/ and devlib/ with the player; saves land in data/)
# EMU_ZIP=<dir>    the data tool's zip folder (APK, OBB, android_package.json, ...)
# ZIP2=<dir>       a variant APK for A/B runs (its .apk replaces the one in EMU_ZIP)
# EMU_HOSTBIN=<dir> another ua_host build to run (default runtime/host/out, arm64: runtime/host/out/arm64)
# Environment passed into the container: UA_*, GC_*, ANDROID_*, GALLIUM_*, LP_*, and the prefixes
# in EMU_ENV (space-separated, e.g. EMU_ENV="CUPHEAD_" for a port's own switches).
here=$(cd "$(dirname "$0")/../.." && pwd)
E=${EMU_ROOT:?set EMU_ROOT=<emulated device root>}
Z=${EMU_ZIP:?set EMU_ZIP=<the data tool zip folder>}
case "${ARCH:=armhf}" in
  armhf) platform=linux/arm/v7; base=arm32v7/debian:bookworm-slim; hostbin=$here/runtime/host/out ;;
  arm64) platform=linux/arm64;  base=arm64v8/debian:bookworm-slim; hostbin=$here/runtime/host/out/arm64 ;;
  *) echo "ARCH must be armhf or arm64"; exit 1 ;;
esac
image=ua-emu-$ARCH
docker image inspect "$image" >/dev/null 2>&1 || docker build -q --platform "$platform" --build-arg BASE=$base -t "$image" "$here/tools/emu" >/dev/null
pre="UA_ GC_ ANDROID_ GALLIUM_ LP_ $EMU_ENV"
envs=()
for p in $pre; do for v in $(env | grep -oE "^${p}[A-Z0-9_]*="); do envs+=(-e "${v%=}"); done; done
zip2=(); [ -n "$ZIP2" ] && zip2=(-v "$ZIP2":/zip2:ro)
docker run --rm --platform "$platform" "${envs[@]}" "${zip2[@]}" -v "$E":/emu -v "$Z":/zip:ro -v "${EMU_HOSTBIN:-$hostbin}":/hostbin:ro \
  -v "$here/tools/emu":/emubin:ro "$image" sh /emubin/run_emu.sh "/emu/${1:-emu.log}" "${2:-600}" "${3:-release}"
