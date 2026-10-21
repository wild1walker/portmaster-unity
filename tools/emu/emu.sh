#!/bin/bash
# PC side: runs a game on the runtime emulated (QEMU armv7 or AArch64, Mesa llvmpipe, pbuffer).
# Frame times are meaningless; frame contents, GL work counts and game logic match the device.
#   emu.sh <log name> <seconds> [release|dev]
# EMU_ARCH=armhf (default; ARCH works too): the 32-bit route (armeabi-v7a players), QEMU armv7 in ua-emu-armhf.
# EMU_ARCH=arm64: the 64-bit route (arm64-v8a players): an aarch64 container, native on an
#                  Apple-silicon Mac or an arm64 Linux PC (QEMU elsewhere), with the ua-build-arm64
#                  image's Mesa (llvmpipe) and SDL2, and runtime/host/out/arm64/ua_host.
# EMU_ROOT=<dir>   the emulated device root (lib/ and devlib/ with the player; saves land in data/)
# EMU_ZIP=<dir>    the data tool's zip folder (APK, OBB, android_package.json, ...)
# ZIP2=<dir>       a variant APK for A/B runs (its .apk replaces the one in EMU_ZIP)
# EMU_HOSTBIN=<dir> another ua_host build to run (default runtime/host/out)
# EMU_NET=1        give the game network access (default: none, so a game's analytics and other
#                  requests never leave the Mac; a run that tests networking turns it on)
# Environment passed into the container: UA_*, GC_*, ANDROID_*, GALLIUM_*, LP_*, and the prefixes
# in EMU_ENV (space-separated, e.g. EMU_ENV="CUPHEAD_" for a port's own switches).
here=$(cd "$(dirname "$0")/../.." && pwd)
E=${EMU_ROOT:?set EMU_ROOT=<emulated device root>}
Z=${EMU_ZIP:?set EMU_ZIP=<the data tool zip folder>}
pre="UA_ GC_ ANDROID_ GALLIUM_ LP_ $EMU_ENV"
envs=()
for p in $pre; do for v in $(env | grep -oE "^${p}[A-Za-z0-9_]*="); do envs+=(-e "${v%=}"); done; done
zip2=(); [ -n "$ZIP2" ] && zip2=(-v "$ZIP2":/zip2:ro)
case "${EMU_ARCH:-${ARCH:-armhf}}" in
  armhf) plat=linux/arm/v7; img=ua-emu-armhf; hostbin=$here/runtime/host/out
         docker image inspect $img >/dev/null 2>&1 || docker build -q --platform $plat --build-arg BASE=arm32v7/debian:bookworm-slim -t $img "$here/tools/emu" >/dev/null ;;
  arm64) plat=linux/arm64; img=ua-build-arm64; hostbin=$here/runtime/host/out/arm64
         docker image inspect $img >/dev/null 2>&1 || docker build -q --platform $plat -t $img "$here/runtime/docker/arm64" >/dev/null ;;
  *) echo "EMU_ARCH must be armhf or arm64"; exit 1 ;;
esac
net=(--network none); [ "${EMU_NET:-0}" = 1 ] && net=()
docker run --rm --platform $plat "${net[@]}" "${envs[@]}" "${zip2[@]}" -v "$E":/emu -v "$Z":/zip:ro -v "${EMU_HOSTBIN:-$hostbin}":/hostbin:ro \
  -v "$here/tools/emu":/emubin:ro $img sh /emubin/run_emu.sh "/emu/${1:-emu.log}" "${2:-600}" "${3:-release}"
