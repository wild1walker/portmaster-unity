#!/bin/bash
# PC side: runs a game on the runtime emulated (QEMU armv7, Mesa llvmpipe, pbuffer). Frame times
# are meaningless; frame contents, GL work counts and game logic match the device.
#   emu.sh <log name> <seconds> [release|dev]
# EMU_ROOT=<dir>   the emulated device root (lib/ and devlib/ with the player; saves land in data/)
# EMU_ZIP=<dir>    the data tool's zip folder (APK, OBB, android_package.json, ...)
# ZIP2=<dir>       a variant APK for A/B runs (its .apk replaces the one in EMU_ZIP)
# Environment passed into the container: UA_*, GC_*, ANDROID_*, GALLIUM_*, LP_*, and the prefixes
# in EMU_ENV (space-separated, e.g. EMU_ENV="CUPHEAD_" for a port's own switches).
here=$(cd "$(dirname "$0")/../.." && pwd)
E=${EMU_ROOT:?set EMU_ROOT=<emulated device root>}
Z=${EMU_ZIP:?set EMU_ZIP=<the data tool zip folder>}
pre="UA_ GC_ ANDROID_ GALLIUM_ LP_ $EMU_ENV"
envs=()
for p in $pre; do for v in $(env | grep -oE "^${p}[A-Z0-9_]*="); do envs+=(-e "${v%=}"); done; done
zip2=(); [ -n "$ZIP2" ] && zip2=(-v "$ZIP2":/zip2:ro)
docker run --rm --platform linux/arm/v7 "${envs[@]}" "${zip2[@]}" -v "$E":/emu -v "$Z":/zip:ro -v "$here/runtime/host/out":/hostbin:ro \
  -v "$here/tools/emu":/emubin:ro ua-emu-armhf sh /emubin/run_emu.sh "/emu/${1:-emu.log}" "${2:-600}" "${3:-release}"
