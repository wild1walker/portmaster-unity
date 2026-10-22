#!/bin/bash
# profbench.sh + profrun.sh + benchsum.py end to end without a handheld: "ssh" runs the commands
# here (a fake in PATH), the "game" is a script that logs like ua_host (a title line, [fps] every
# half second, a long frame after a press, a profile when prof.req appears). Checks that the plan's
# steps wait and act, the measurement and the profile come back, and the summary reads them.
# Runs on Linux or macOS (bash 3.2+); /proc counters are summed only where there is a /proc.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); PU=$(cd "$HERE/../../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/profbench.XXXXXX"); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/dev/game/lib" "$T/out"
touch "$T/dev/game/Game.apk"
# ssh [-n] [-o x]... host command: the command, here
cat > "$T/bin/ssh" <<'S'
#!/bin/bash
while [ $# -gt 0 ]; do case $1 in -n) shift ;; -o) shift 2 ;; -*) shift ;; *) break ;; esac; done
shift; exec bash -c "$*"
S
chmod +x "$T/bin/ssh"
command -v setsid >/dev/null || { printf '#!/bin/sh\nexec "$@"\n' > "$T/bin/setsid"; chmod +x "$T/bin/setsid"; }   # (macOS has none)
# a profile in prof.c's format (one UnityMain stack in a JIT method)
cat > "$T/prof.txt" <<'P'
ua_prof 1
arch armhf
hz 100
secs 2.000
depth 32 scan 16384
signal 39
mono yes
jit 200000 100 M:Fake.Player:Update
thread 0 100 UnityMain 2 0
stack 0 2 0 R ffff 200010 0
seq 0 0:0 10000:0
end
P
cat > "$T/ua_host" <<'G'
#!/bin/bash
root=$1; i=0
trap 'echo "[fake] stopped"; exit 0' TERM INT
echo "[fake] started with UA_PROF=${UA_PROF:-} UA_FPS=${UA_FPS:-} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-} UA_AUDIO=${UA_AUDIO:-unset} X_SETTING=${X_SETTING:-}"
sleep 2; echo "title ready"           # (after the run's own lines, which quote the plan)
while :; do
  sleep 0.5; i=$((i + 1))
  if [ -f "$root/input.req" ]; then echo "[fake] input $(cat "$root/input.req")"; rm -f "$root/input.req"; echo "[hitch] 1500.0 ms at $i s"; fi
  echo "[fps] 33.0 fps, frame 30.3 avg / 35.0 p99 / 36.0 worst ms, late 10 hitch 0, gc 0 heap 64 MB (16 frames, $((i * 16)) total, $i s) rss 700 MB"
  if [ -f "$root/prof.req" ]; then
    s=$(cat "$root/prof.req"); rm -f "$root/prof.req"
    ( sleep "$s"; cp "$PROF_SAMPLE" "$root/prof-1.txt.tmp" && mv "$root/prof-1.txt.tmp" "$root/prof-1.txt"; echo "[prof] prof-1.txt: ${s} s" ) &
  fi
done
G
chmod +x "$T/ua_host"
cat > "$T/list.txt" <<'L'
# two runs, the second with its own setting and a sound request that must be dropped
one
two X_SETTING='a b' UA_AUDIO=1
L
export PATH="$T/bin:$PATH" DEVICE=fake@handheld OUT="$T/out" HOST_BIN="$T/ua_host" PROF_SAMPLE="$T/prof.txt"
export PLAN='log:title ready; steady:2; press:0:b; hitch:1000; seen:title ready; steady:2; shot' MEASURE=3 UA_PROF=100 STEP_TIMEOUT=30
log=$("$PU/tools/device/profbench.sh" "$T/dev/game" --list "$T/list.txt" 2>&1)
fail() { echo "profbench_test: FAILED: $*"; printf '%s\n' "$log" | tail -40; exit 1; }
for n in one two; do
  [ "$(cat "$T/out/$n/result" 2>/dev/null)" = ok ] || fail "$n: result $(cat "$T/out/$n/result" 2>/dev/null)"
  grep -q '^\[fake\] input 0:b' "$T/out/$n/run.log" || fail "$n: the press did not arrive"
  [ "$(grep -n '^title ready' "$T/out/$n/run.log" | cut -d: -f1)" -lt "$(grep -n '^\[fake\] input' "$T/out/$n/run.log" | cut -d: -f1)" ] ||
    fail "$n: the press came before the line the plan waits for"
  grep -q '^\[bench\] measure start' "$T/out/$n/run.log" && grep -q '^\[bench\] measure end' "$T/out/$n/run.log" || fail "$n: no measurement"
  [ -f "$T/out/$n/prof-1.txt" ] || fail "$n: no profile"
  [ -f "$T/out/$n/snap-0.txt" ] && [ -f "$T/out/$n/freq.log" ] || fail "$n: no snapshots"
  grep -q 'SDL_AUDIODRIVER=dummy' "$T/out/$n/run.log" && grep -q 'UA_AUDIO=unset' "$T/out/$n/run.log" || fail "$n: not silent"
done
grep -q 'X_SETTING=a b' "$T/out/two/run.log" || fail "the run's own setting did not arrive"
printf '%s\n' "$log" | grep -q 'frames: 33.0 fps' || fail "benchsum did not read the frame rate"
printf '%s\n' "$log" | grep -q 'M:Fake.Player:Update' || fail "benchsum did not sum the profile"
printf '%s\n' "$log" | grep -q 'runs are silent' || fail "UA_AUDIO=1 was not refused"
printf '%s\n' "$log" | grep -q '^  two .*+0.0%' || fail "no comparison table"
echo "profbench_test: ok"
