#!/bin/bash
# Emulated handheld for the box64 route: runs a game's own Linux x86_64 build under box64 in an
# arm64 container (native on an Apple-silicon Mac) with Xvfb and Mesa, takes screenshots and
# measures frames and CPU per thread. Frame rates here are rough: the Mac's cores are several times
# faster than a handheld's Cortex-A53 and llvmpipe is not a Mali (docs/box64-route.md, "Test rig").
#
#   tools/box64/rig.sh <game folder> <out dir> [seconds]
#
# The game folder is mounted read-only, the container has no network and no extra privileges.
# Env:
#   BIN=<dir>       tools/box64/build.sh output (box64/, gl4es/), default tools/box64/out
#   EXE=<name>      the executable in the game folder (default: the x86_64 ELF next to <Game>_Data)
#   GL=mesa         Mesa's desktop GL (llvmpipe, GL 4.5: a Panfrost/Turnip-like full GL driver)
#      noop         Mesa's no-op driver: draws accepted, nothing rasterized (CPU side only, as a
#                   handheld's GPU would take it off the CPU); screenshots are black
#      gl4es        gl4es (GL 2.1) on Mesa's GLES 2 (for Unity's legacy GL 2 renderer)
#      gles         no desktop GL at all (libGL/libGLX/libOpenGL removed), only EGL + GLES 3.2, as on
#                   a Mali driver: for the player's -force-gles mode (gles-noop: and nothing drawn)
#   RES=WxH         screen (default 640x480, PortMaster's most common)
#   SHOTS="t ..."   screenshot times in seconds (default: 4 evenly spread)
#   KEYS="t:key .." xdotool keys at those seconds (key+key together; key*N held N seconds)
#   CLICKS="t:x,y .." mouse clicks at those seconds (screen coordinates)
#   MEASURE=a-b     seconds between which frames and thread CPU are counted (default: last third)
#   PROFILE=default|safe|fast   box64 settings (as tools/box64/launcher.sh.in: box64.env)
#   CPUS=0-3        container CPU set (4 cores, as the handhelds); BOX64_MAXCPU follows it
#   MEM=1500m       container memory limit (a 1 GB handheld's game plus slack; keeps a runaway run,
#                   e.g. GL=noop drawing a thousand frames a second, from starving other containers)
#   OVERLAY=<dir>   files replacing the same paths in the data folder (e.g. pmunity convert output)
#   ARGS="..."      more player arguments; BOX64_EXTRA="VAR=val ..." more box64 settings
set -u
here=$(cd "$(dirname "$0")" && pwd)
GAME=$(cd "${1:?game folder}" && pwd); OUTD=${2:?out dir}; SECS=${3:-90}
mkdir -p "$OUTD"; OUTD=$(cd "$OUTD" && pwd)
BIN=$(cd "${BIN:-$here/out}" && pwd)
[ -x "$BIN/box64/box64" ] || { echo "no $BIN/box64/box64: run tools/box64/build.sh first"; exit 1; }
if [ -z "${EXE:-}" ]; then
  for d in "$GAME"/*_Data; do
    n=$(basename "$d" _Data)
    for f in "$GAME/$n" "$GAME/$n".*; do
      [ -f "$f" ] && [ "$(head -c 4 "$f" | od -An -tx1 | tr -d ' ')" = 7f454c46 ] && \
        [ "$(od -An -tu2 -j18 -N2 "$f" | tr -d ' ')" = 62 ] && { EXE=$(basename "$f"); break 2; }
    done
  done
fi
[ -n "${EXE:-}" ] || { echo "no Linux x86_64 executable in $GAME (set EXE=)"; exit 1; }
docker image inspect pmu-box64-rig >/dev/null 2>&1 || \
  docker build -q --platform linux/arm64 -t pmu-box64-rig -f "$here/docker/Dockerfile.rig" "$here/docker" >/dev/null
rm -rf "$OUTD/home" "$OUTD"/shot_* "$OUTD/hud"
name=pmu-rig-$$
trap 'docker kill $name >/dev/null 2>&1' INT TERM
docker run --rm --name $name --platform linux/arm64 --network none --cpuset-cpus="${CPUS:-0-3}" --shm-size=512m --memory="${MEM:-1500m}" \
  -v "$GAME":/game:ro ${OVERLAY:+-v "$(cd "$OVERLAY" && pwd)":/overlay:ro} -v "$OUTD":/out -v "$BIN":/pm:ro -v "$here":/rig:ro \
  -e EXE="$EXE" -e SECS="$SECS" -e GL="${GL:-mesa}" -e RES="${RES:-640x480}" -e SHOTS="${SHOTS:-}" \
  -e KEYS="${KEYS:-}" -e CLICKS="${CLICKS:-}" -e MEASURE="${MEASURE:-}" -e PROFILE="${PROFILE:-default}" \
  -e ARGS="${ARGS:-}" -e BOX64_EXTRA="${BOX64_EXTRA:-}" -e CPUS="${CPUS:-0-3}" \
  pmu-box64-rig bash /rig/rig_inside.sh
python3 -I "$here/rigstats.py" "$OUTD"
