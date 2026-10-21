#!/bin/bash
# A box64 port's release zip (`pmunity port package`) run as on a handheld, in the test rig's
# container: unzipped into a ports folder, the game copied into its gamedata/, a stand-in
# PortMaster (mockpm/: control.txt, the patcher, Westonpack's westonwrap.sh), the launcher started
# twice: the first launch runs the setup (the port's own Python, `pmunity route --env`, the GLES
# shader conversion in place), then the game.
#
#   tools/box64/test/port_test.sh <port.zip> <game folder> <work dir> [seconds]
#
# The work dir gets the ports folder (the zip's contents and a copy of the game: put it on a drive
# with room) and the test's logs and screenshots (in <work dir>/out). Env: GL=gles (default: no
# desktop GL library in the container) or mesa, RES=WxH, MEM, CPUS. No network.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
ZIP=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); GAME=$(cd "${2:?game folder}" && pwd)
WORK=${3:?work dir}; SECS=${4:-120}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
P="$WORK/ports"; OUTD="$WORK/out"
rm -rf "$P" "$OUTD"; mkdir -p "$P" "$OUTD"
( cd "$P" && unzip -q "$ZIP" ) || exit 1
port=$(cd "$P" && ls -d */ | head -1); port=${port%/}
cp -cR "$GAME/." "$P/$port/gamedata/" 2>/dev/null || cp -R "$GAME/." "$P/$port/gamedata/"
mkdir -p "$P/PortMaster/libs" "$P/PortMaster/utils"
cp "$here/test/mockpm/control.txt" "$P/PortMaster/"; cp "$here/test/mockpm/utils/patcher.txt" "$P/PortMaster/utils/"
: > "$P/PortMaster/libs/weston_pkg_0.2.squashfs"
launcher=$(cd "$P" && ls *.sh | head -1)
docker run --rm --platform linux/arm64 --network none --cpuset-cpus="${CPUS:-0-3}" --shm-size=512m --memory="${MEM:-1500m}" \
  -v "$P":/roms/ports -v "$OUTD":/out -v "$here/test/mockpm":/mockpm:ro \
  -e SECS="$SECS" -e GL="${GL:-gles}" -e RES="${RES:-640x480}" -e LAUNCHER="$launcher" -e PORT="$port" \
  pmu-box64-rig bash -c '
set -u
W=${RES%x*}; H=${RES#*x}
mkdir -p /tmp/xdg; export XDG_RUNTIME_DIR=/tmp/xdg
Xvfb :99 -screen 0 "${W}x${H}x24" -nolisten tcp >/dev/null 2>&1 &
for i in $(seq 40); do DISPLAY=:99 xdotool getdisplaygeometry >/dev/null 2>&1 && break; sleep 0.25; done
export DISPLAY=:99 DISPLAY_WIDTH=$W DISPLAY_HEIGHT=$H
[ "$GL" = gles ] && rm -f /usr/lib/aarch64-linux-gnu/libGL.so* /usr/lib/aarch64-linux-gnu/libGLX* /usr/lib/aarch64-linux-gnu/libOpenGL.so*
export PATH=/mockpm/cmd:$PATH
# first launch: the setup only (the launcher stops when the setup has not finished; here it has)
( sleep 3; while true; do w=$(xdotool search --onlyvisible --name . 2>/dev/null | tail -1)
    [ -n "$w" ] && xdotool windowfocus "$w" 2>/dev/null; sleep 2; done ) &
bash "/roms/ports/$LAUNCHER" > /out/launch.out 2>&1 &
L=$!
for t in $(seq 1 "$SECS"); do
  sleep 1
  kill -0 $L 2>/dev/null || { echo "launcher ended at ${t}s" >> /out/launch.out; break; }
  case " $((SECS / 2)) $((SECS * 3 / 4)) $((SECS - 2)) " in *" $t "*) xwd -root -silent -out "/out/shot_${t}.xwd" 2>/dev/null ;; esac
done
pkill -f "box64/box64" 2>/dev/null; sleep 3
G=/roms/ports/$PORT
cp "$G/patchlog.txt" "$G/log.txt" "$G/player.log" "$G/game.env" /out/ 2>/dev/null
ls -a "$G" > /out/portdir.txt; ls "$G/gamedata" > /out/gamedata.txt
tail -3 /out/launch.out'
python3 -I "$here/rigstats.py" "$OUTD" --png
