#!/bin/bash
# Runs a box64-route launcher (made from tools/box64/launcher.sh.in) end to end in the test rig's
# container, with a stand-in PortMaster (mockpm/: control.txt, Westonpack's westonwrap.sh) and the
# port's folder laid out as on a handheld: box64, glxes/gl4es, box64.env, unityprefs.sh, gamedata.
#
#   tools/box64/test/launcher_test.sh <launcher.sh> <game folder> <out dir> [seconds]
#
# Env: BIN=<tools/box64/build.sh output>, OVERLAY=<converted data files> (GLES mode: pmunity
# convert --config tools/box64/gles.json output), GL=gles (the default: no desktop GL library in
# the container, as on a Mali driver) or mesa, RES=WxH, PMU_*=... passed to the launcher.
# The game folder is mounted read-only; no network.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
LAUNCHER=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); GAME=$(cd "${2:?game folder}" && pwd)
OUTD=${3:?out dir}; SECS=${4:-80}
mkdir -p "$OUTD"; OUTD=$(cd "$OUTD" && pwd); rm -rf "$OUTD"/shot_* "$OUTD/port"
BIN=$(cd "${BIN:-$here/out}" && pwd)
envs=(); for v in $(env | grep -oE '^PMU_[A-Z0-9_]*='); do envs+=(-e "${v%=}"); done
docker run --rm --platform linux/arm64 --network none --cpuset-cpus="${CPUS:-0-3}" --shm-size=512m --memory="${MEM:-1500m}" \
  -v "$GAME":/game:ro ${OVERLAY:+-v "$(cd "$OVERLAY" && pwd)":/overlay:ro} -v "$OUTD":/out -v "$BIN":/pm:ro \
  -v "$here":/rig:ro -v "$here/test/mockpm":/mockpm:ro -v "$LAUNCHER":/launcher.sh:ro \
  -e SECS="$SECS" -e GL="${GL:-gles}" -e RES="${RES:-640x480}" ${envs[@]+"${envs[@]}"} \
  pmu-box64-rig bash -c '
set -u
W=${RES%x*}; H=${RES#*x}
mkdir -p /tmp/xdg; export XDG_RUNTIME_DIR=/tmp/xdg
Xvfb :99 -screen 0 "${W}x${H}x24" -nolisten tcp >/dev/null 2>&1 &
for i in $(seq 40); do DISPLAY=:99 xdotool getdisplaygeometry >/dev/null 2>&1 && break; sleep 0.25; done
export DISPLAY=:99 DISPLAY_WIDTH=$W DISPLAY_HEIGHT=$H
# a GLES-only driver (after Xvfb, which links libGL itself)
[ "$GL" = gles ] && rm -f /usr/lib/aarch64-linux-gnu/libGL.so* /usr/lib/aarch64-linux-gnu/libGLX* /usr/lib/aarch64-linux-gnu/libOpenGL.so*
# PortMaster stand-in and the port folder, as the launcher expects them
PM=/roms/ports/PortMaster; mkdir -p $PM/libs; cp /mockpm/control.txt $PM/; : > $PM/libs/weston_pkg_0.2.squashfs
port=$(sed -n "s|^GAMEDIR=/\$directory/ports/||p" /launcher.sh); P=/roms/ports/$port
mkdir -p $P/box64 $P/glxes $P/gl4es $P/gamedata
cp /pm/box64/box64 $P/box64/; ln -s /pm/box64/x64lib $P/box64/x64lib
cp /rig/box64.env /rig/unityprefs.sh $P/box64/; cp /pm/glxes/libGL.so.1 $P/glxes/; cp /pm/gl4es/libGL.so.1 $P/gl4es/
: > "$P/$port.gptk"
for f in /game/*; do ln -s "$f" "$P/gamedata/$(basename "$f")"; done
exe=$(sed -n "s|^EXE=\"\(.*\)\"$|\1|p" /launcher.sh); rm "$P/gamedata/$exe"; cp "/game/$exe" "$P/gamedata/$exe"
D=$(cd /game && ls -d *_Data | head -1); rm "$P/gamedata/$D"; cp -as "/game/$D" "$P/gamedata/$D"
if [ -d /overlay ]; then
  (cd /overlay && find . -type f ! -name .pmunity.json) | while read -r f; do
    rm -f "$P/gamedata/$D/${f#./}"; mkdir -p "$(dirname "$P/gamedata/$D/${f#./}")"; ln -s "/overlay/${f#./}" "$P/gamedata/$D/${f#./}"
  done
fi
PATH=/mockpm/cmd:$PATH bash /launcher.sh > /out/launcher.out 2>&1 &
L=$!
# the window manager a handheld has (Westonpack kiosk) gives the game the input focus; Xvfb has none
( sleep 3; while kill -0 $L 2>/dev/null; do
    w=$(xdotool search --onlyvisible --name . 2>/dev/null | tail -1)
    [ -n "$w" ] && xdotool windowfocus "$w" 2>/dev/null; sleep 2; done ) &
for t in $(seq 1 "$SECS"); do
  sleep 1
  kill -0 $L 2>/dev/null || { echo "launcher ended at ${t}s" >> /out/launcher.out; break; }
  case " $((SECS / 3)) $((SECS * 2 / 3)) $((SECS - 2)) " in *" $t "*) xwd -root -silent -out "/out/shot_${t}.xwd" 2>/dev/null ;; esac
done
pkill -f "box64/box64" 2>/dev/null; sleep 3
mkdir -p /out/port; cp $P/log.txt $P/player.log /out/port/ 2>/dev/null
find $P/conf -name prefs -exec cp {} /out/port/prefs \; 2>/dev/null
ls $P/conf/dynacache 2>/dev/null | head -3 > /out/port/dynacache.txt
tail -5 /out/launcher.out'
python3 -I "$here/rigstats.py" "$OUTD" --png     # the screenshots as PNGs
