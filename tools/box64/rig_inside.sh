#!/bin/bash
# Inside the pmu-box64-rig container (tools/box64/rig.sh): Xvfb, a PulseAudio null sink played in
# real time (so the game's mixer runs at a handheld's pace instead of spinning on a null ALSA
# device), the game under box64, screenshots, key presses, frame and thread-CPU counts.
set -u
W=${RES%x*}; H=${RES#*x}
mkdir -p /out/home /out/hud /tmp/xdg
: > /out/rig.txt; rm -f /out/cpu[01].txt /out/t[01].txt
export HOME=/out/home XDG_CONFIG_HOME=/out/home/.config XDG_RUNTIME_DIR=/tmp/xdg
pulseaudio -n --daemonize=yes --exit-idle-time=-1 --log-target=file:/out/pulse.log \
  --load="module-null-sink sink_name=null rate=48000" --load="module-native-protocol-unix" \
  --load="module-always-sink" >/dev/null 2>&1
sleep 1; pactl set-default-sink null 2>/dev/null
Xvfb :99 -screen 0 "${W}x${H}x24" -nolisten tcp >/dev/null 2>&1 &
for i in $(seq 40); do DISPLAY=:99 xdpyinfo >/dev/null 2>&1 && break; sleep 0.25; done
export DISPLAY=:99

# box64: the launcher's profiles (box64.env), the x86_64 libraries box64 brings, 4 CPUs
. /rig/box64.env
export $(box64_profile "$PROFILE")
ncpu=$(python3 -c "import os; print(len(os.sched_getaffinity(0)))")
export BOX64_LOG=1 BOX64_NOBANNER=1 BOX64_LD_LIBRARY_PATH=/pm/box64/x64lib BOX64_MAXCPU=$ncpu
[ -n "${BOX64_EXTRA:-}" ] && export ${BOX64_EXTRA}

# GL: frames counted by Mesa's HUD (written to /out/hud, not drawn)
export GALLIUM_HUD=fps GALLIUM_HUD_VISIBLE=false GALLIUM_HUD_DUMP_DIR=/out/hud GALLIUM_HUD_PERIOD=1
case "$GL" in
  noop)  export GALLIUM_NOOP=1 ;;
  # a GLES-only driver, as Mali's: no desktop GL libraries at all, EGL and GLES only
  gles|gles-noop)
         rm -f /usr/lib/aarch64-linux-gnu/libGL.so* /usr/lib/aarch64-linux-gnu/libGLX* /usr/lib/aarch64-linux-gnu/libOpenGL.so*
         [ "$GL" = gles-noop ] && export GALLIUM_NOOP=1 ;;
  gl4es) export BOX64_LIBGL=/pm/gl4es/libGL.so.1 LIBGL_ES=2 LIBGL_GL=21 LIBGL_NOBANNER=1 ;;
esac
glxinfo -B > /out/glxinfo.txt 2>&1

# the game folder is read-only: when the executable lost its x bit (zip extraction), run a copy of it
# from a folder of links to the game's files (Unity finds <name>_Data next to the executable)
# OVERLAY (/overlay): files that replace the same paths in the data folder (a converted
# globalgamemanagers, assets with GLES programs, ...); the data folder then becomes links too
G=/game
if [ $((0$(stat -c %a "/game/$EXE") & 0111)) = 0 ] || [ -d /overlay ]; then
  G=/tmp/game; mkdir -p $G
  for f in /game/*; do ln -s "$f" "$G/$(basename "$f")"; done
  rm "$G/$EXE"; cp "/game/$EXE" "$G/$EXE"; chmod +x "$G/$EXE"
  if [ -d /overlay ]; then
    D=$(cd /game && ls -d *_Data | head -1)
    rm "$G/$D"; cp -as "/game/$D" "$G/$D"
    (cd /overlay && find . -type f ! -name '.pmunity.json') | while read -r f; do
      rm -f "$G/$D/${f#./}"; mkdir -p "$(dirname "$G/$D/${f#./}")"; ln -s "/overlay/${f#./}" "$G/$D/${f#./}"
    done
  fi
fi
cd $G
/pm/box64/box64 "./$EXE" -screen-fullscreen 0 -screen-width "$W" -screen-height "$H" \
  -logFile /out/player.log $ARGS > /out/box64.log 2>&1 &
GPID=$!
# Unity reads keys only while its window has the X input focus
( sleep 3; while kill -0 $GPID 2>/dev/null; do
    w=$(xdotool search --onlyvisible --pid $GPID 2>/dev/null | head -1)
    [ -z "$w" ] && w=$(xdotool search --onlyvisible --name . 2>/dev/null | tail -1)
    [ -n "$w" ] && xdotool windowfocus "$w" 2>/dev/null; sleep 2; done ) &

SHOTS=${SHOTS:-$(python3 -c "s=$SECS; print(' '.join(str(int(s*f)) for f in (0.25, 0.5, 0.75, 0.97)))")}
MEASURE=${MEASURE:-$((SECS * 2 / 3))-$((SECS - 2))}
m0=${MEASURE%-*}; m1=${MEASURE#*-}
threads() {   # tid name utime+stime (clock ticks) for every thread of the game
  local task
  for task in /proc/$GPID/task/*; do
    awk -v tid="${task##*/}" '{c=$0; sub(/^[^)]*\) /, "", c); split(c, a, " "); n=$0; sub(/^[^(]*\(/, "", n); sub(/\).*/, "", n);
         gsub(/ /, "_", n); print tid, n, a[12] + a[13]}' "$task/stat" 2>/dev/null
  done
}
for t in $(seq 1 "$SECS"); do
  sleep 1
  kill -0 $GPID 2>/dev/null || { echo "game exited at ${t}s" >> /out/rig.txt; break; }
  for s in $SHOTS; do [ "$s" = "$t" ] && import -window root "/out/shot_${t}.png" 2>/dev/null; done
  [ "$t" = "$m0" ] && { threads > /out/cpu0.txt; echo "$t" > /out/t0.txt; }
  [ "$t" = "$m1" ] && { threads > /out/cpu1.txt; echo "$t" > /out/t1.txt; }
  for k in $KEYS; do
    [ "${k%%:*}" = "$t" ] || continue
    spec=${k#*:}; hold=0.15; case "$spec" in *\**) hold=${spec#*\*}; spec=${spec%%\**} ;; esac
    keys=$(echo "$spec" | tr '+' ' ')
    ( xdotool keydown $keys; sleep "$hold"; xdotool keyup $keys ) &
    echo "${t}s key $spec" >> /out/rig.txt
  done
  for c in $CLICKS; do
    [ "${c%%:*}" = "$t" ] || continue
    xy=${c#*:}; xdotool mousemove "${xy%,*}" "${xy#*,}" click 1 2>/dev/null
    echo "${t}s click $xy" >> /out/rig.txt
  done
done
awk '/VmHWM/{print "peak rss " int($2/1024) " MB"}' /proc/$GPID/status >> /out/rig.txt 2>/dev/null
kill $GPID 2>/dev/null; sleep 2; kill -9 $GPID 2>/dev/null
# frames and CPU between m0 and m1: fps from the HUD's per-second values, CPU ms per frame per thread
python3 - "$m0" "$m1" >> /out/rig.txt 2>&1 <<'PY'
import os, sys
m0, m1 = int(sys.argv[1]), int(sys.argv[2])
f = '/out/hud/fps'
vals = [float(l.split()[0]) for l in open(f)] if os.path.exists(f) else []
win = vals[m0:m1] if len(vals) >= m1 else vals[-max(1, m1 - m0):]
fps = sum(win) / len(win) if win else 0
print(f'fps {fps:.1f} over {len(win)} s (HUD seconds {len(vals)}; whole run min/max {min(vals or [0]):.0f}/{max(vals or [0]):.0f})')
try:
    c0 = {l.split()[0]: (l.split()[1], int(l.split()[2])) for l in open('/out/cpu0.txt')}
    c1 = {l.split()[0]: (l.split()[1], int(l.split()[2])) for l in open('/out/cpu1.txt')}
    secs = int(open('/out/t1.txt').read()) - int(open('/out/t0.txt').read())
except (OSError, ValueError, IndexError) as ex:
    sys.exit(print(f'no thread CPU counts ({ex})'))
hz = os.sysconf('SC_CLK_TCK')
rows = sorted(((c1[t][1] - c0.get(t, (0, 0))[1]) / hz, t, c1[t][0]) for t in c1)[::-1]
tot = sum(r[0] for r in rows)
print(f'cpu total {100 * tot / secs:.0f}% of a core over {secs} s')
pid = min(c1, key=int)
for cpu, tid, n in rows[:8]:
    if cpu <= 0:
        continue
    per_frame = 1000 * cpu / (fps * secs) if fps else 0
    print(f'  {"main" if tid == pid else "tid " + tid} {n}: {100 * cpu / secs:.0f}%, {per_frame:.1f} ms/frame')
PY
cat /out/rig.txt
