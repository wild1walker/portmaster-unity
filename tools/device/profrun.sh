#!/bin/bash
# Device side of tools/device/profbench.sh (also usable from a shell on the handheld):
#   profrun.sh <game folder> <name>
# Runs the game in <game folder> (lib/, the APK, OBBs, android_package.json: as test.sh left it)
# with the ua_host beside this script, silent, takes it to the point to measure (PLAN), then
# measures for MEASURE seconds and writes <this script's folder>/<name>/:
#   run.log           the game's log ([fps] every second, UA_FPS=1), with "[bench] ..." lines:
#                     each plan step, "measure start" / "measure end"
#   snap-0.txt, snap-1.txt   raw copies of /proc and /sys files at the start and end of the
#                     measurement: per-core and per-thread CPU time (stat, schedstat), memory and
#                     swap traffic (meminfo, vmstat), CPU and GPU clocks and their residency
#                     (cpufreq stats, devfreq trans_stat), temperatures, throttling, every
#                     process's CPU time; tools/analysis/benchsum.py turns them into deltas
#   freq.log          every second of the measurement: each core's clock, the GPU's, MemAvailable,
#                     SwapFree, each thermal zone's temperature (C) and each cooling device's state
#   mem.log           every 2 s of the run: MemAvailable, swap, the game's resident memory
#   prof-<n>.txt      with UA_PROF=<hz>: the profile of the measurement (runtime/host/prof.c)
#   shot-<n>.ppm      the screenshots the plan asked for
#   result            one line: ok, or why the run stopped early
# Nothing is changed on the handheld except that folder (and EmulationStation stopped and started
# again with UATEST_STOP_ES=1): clocks, governors and thermal settings are only read.
#
# PLAN: steps separated by ';', each waiting (up to STEP_TIMEOUT s, default 300) or acting:
#   log:<ERE>       a new log line (after what the last wait matched, or the last press) matches the
#                   extended regular expression (awk's)
#   seen:<ERE>      the game has logged a matching line, at any time since it started
#   steady:<n>      n new [fps] windows in a row without a hitch (a frame over 50 ms)
#   hitch:<ms>      a new frame of at least <ms> ms (a scene load)
#   sleep:<s>       wait
#   press:<presses> buttons, in UA_INPUT_SCRIPT's syntax ("0:b 3:down~0.5"; times from now)
#   shot            a screenshot (shot.req)
# e.g. a saved game continued from its title screen:
#   PLAN='log:pd.maxHealth; steady:3; press:0:b; sleep:6; press:0:b; hitch:2000; steady:5'
# Settings: MEASURE (s, default 30), UA_PROF (profile at that rate during the measurement),
# UATEST_STOP_ES=1, UA_MEM_MIN (the memory watchdog's line, as uatest.sh), and every UA_* (but
# UA_AUDIO: these runs are always silent).
set -u
R=${1:?usage: profrun.sh <game folder> <name>}; NAME=${2:?name}
HERE=$(cd "$(dirname "$0")" && pwd); BIN=$HERE/ua_host
cd "$R" || exit 1
ROOT=$PWD; OUT=$HERE/$NAME
rm -rf "$OUT"; mkdir -p "$OUT" || exit 1
rm -f prof.req prof-*.txt shot.req shot-*.ppm input.req "$HERE/bench.done"
LOG=$OUT/run.log
ES=/etc/init.d/S31emulationstation
if [ "${UATEST_STOP_ES:-0}" = 1 ] && [ -x $ES ]; then
  $ES stop >/dev/null 2>&1; sleep 2
  trap '$ES start >/dev/null 2>&1; touch "$HERE/bench.done"' EXIT
else
  trap 'touch "$HERE/bench.done"' EXIT
fi
# silent, always: SDL's dummy audio driver, OpenSL ES to SDL (never the pipe to aplay)
unset UA_AUDIO AUDIODEV
export SDL_AUDIODRIVER=dummy ANDROID_SLES_OUTPUT=sdl UA_FPS=${UA_FPS:-1}
PM=${XDG_DATA_HOME:-$HOME/.local/share}/PortMaster
if [ -z "${SDL_GAMECONTROLLERCONFIG:-}" ] && [ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ]; then
  for f in "$PM"/*/gamecontrollerdb.txt; do [ -f "$f" ] && export SDL_GAMECONTROLLERCONFIG_FILE="$f" && break; done
fi
[ "$(od -An -tx1 -j4 -N1 "$BIN" | tr -d ' ')" = 01 ] && export LD_LIBRARY_PATH=/lib32:/usr/lib32${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
PROF=${UA_PROF:-}; unset UA_PROF_START
log() { echo "[bench] $*" >> "$LOG"; }
lines() { wc -l < "$LOG" | tr -d ' '; }
mem() { awk -v k="$1:" '$1 == k {print int($2 / 1024)}' "${2:-/proc/meminfo}"; }
# The loops that run every second read with bash's builtins only (no cat or awk process a read):
# on the RG34XX-SP the thermal governor shares one power budget between the CPU and the GPU, so
# the bench's own CPU time would lower the GPU's clock (docs/profiling.md, "the handheld around the
# game"). memkv <file> <key>...: each key's kB / 1024 (MB) into M_<key>
memkv() {
  local f=$1 k v _ w; shift
  for w in "$@"; do printf -v "M_$w" '%s' ''; done
  [ -r "$f" ] || return 0
  while read -r k v _; do
    k=${k%:}
    for w in "$@"; do [ "$k" = "$w" ] && printf -v "M_$w" '%s' $(( v / 1024 )); done
  done < "$f"
}
# rdlist <var> <divisor> <file>...: each file's number / divisor, comma-separated, into <var>
rdlist() {
  local out=$1 div=$2 x v r=""; shift 2
  for x in "$@"; do [ -r "$x" ] && read -r v < "$x" && r="$r${r:+,}$(( v / div ))"; done
  printf -v "$out" '%s' "$r"
}

: > "$LOG"
log "name $NAME, plan: ${PLAN:-none}, measure ${MEASURE:-30} s${PROF:+, profile at $PROF Hz}"
for v in $(compgen -e); do case $v in UA_*|SDL_*|GC_*|MONO_*|ANDROID_*) log "env $v=${!v}" ;; esac; done
pos=$(lines); start=$pos
"$BIN" "$ROOT" >> "$LOG" 2>&1 &
pid=$!

# the memory watchdog (as uatest.sh): the game's own PID only
(
  t=0
  while kill -0 $pid 2>/dev/null; do
    memkv /proc/meminfo MemAvailable SwapFree; a=$M_MemAvailable
    if [ -n "${UA_MEM_MIN:-}" ]; then MIN=$UA_MEM_MIN; elif [ "${M_SwapFree:-0}" -ge 64 ]; then MIN=40; else MIN=100; fi
    if [ $((t % 2)) = 0 ]; then
      memkv /proc/$pid/status VmRSS; ld=""; [ -r /proc/loadavg ] && read -r ld _ < /proc/loadavg
      echo "t=$t avail=${a}M swapfree=${M_SwapFree}M rss=${M_VmRSS}M load=$ld" >> "$OUT/mem.log"
    fi
    if [ "${a:-$MIN}" -lt "$MIN" ]; then
      echo "[bench] LOW MEMORY at $t s (${a} MB available, under UA_MEM_MIN=$MIN): stopping the game" >> "$LOG"
      kill -KILL $pid; break
    fi
    sleep 1; t=$((t + 1))
  done
) &
wd=$!

stop_game() {
  if kill -0 $pid 2>/dev/null; then kill -TERM $pid; sleep 4; kill -0 $pid 2>/dev/null && kill -KILL $pid; fi
  { wait $pid; } 2>/dev/null; echo "exit $?" >> "$LOG"
  kill $wd 2>/dev/null
}
fail() { log "STOPPED: $*"; echo "$*" > "$OUT/result"; stop_game; mv -f "$ROOT"/shot-*.ppm "$OUT"/ 2>/dev/null; exit 1; }
# A wait looks at the log after $pos: from the game's start, then after what the last wait matched
# (or the last press: what came before a press does not answer a wait after it)
wait_for() {
  local test=$1 arg=$2 t=0 at
  while [ $t -lt "${STEP_TIMEOUT:-300}" ]; do
    kill -0 $pid 2>/dev/null || fail "the game exited during the plan (step '$step')"
    at=$(awk -v from="$pos" -v arg="$arg" "$test" "$LOG")
    [ -n "$at" ] && { pos=$at; return 0; }
    sleep 1; t=$((t + 1))
  done
  fail "step '$step' timed out after ${STEP_TIMEOUT:-300} s"
}
# each prints the line number where its condition is met
T_LOG='NR > from && !/^\[bench\]/ && $0 ~ arg { print NR; exit }'
T_STEADY='NR > from && /^\[fps\]/ { if ($0 ~ / hitch 0,/) k++; else k = 0; if (k >= arg + 0) { print NR; exit } }'
T_HITCH='NR > from && /^\[hitch\] / { if ($2 + 0 >= arg + 0) { print NR; exit } }'

IFS=';' read -r -a steps <<< "${PLAN:-}"
for step in "${steps[@]}"; do
  step=$(echo "$step" | sed 's/^ *//; s/ *$//'); [ -n "$step" ] || continue
  log "step $step"
  case $step in
    log:*) wait_for "$T_LOG" "${step#log:}" ;;
    seen:*) p0=$pos; pos=$start; wait_for "$T_LOG" "${step#seen:}"; [ "$pos" -lt "$p0" ] && pos=$p0 ;;
    steady:*) wait_for "$T_STEADY" "${step#steady:}" ;;
    hitch:*) wait_for "$T_HITCH" "${step#hitch:}" ;;
    sleep:*) sleep "${step#sleep:}" ;;
    press:*) printf '%s' "${step#press:}" > input.req.tmp && mv input.req.tmp input.req; pos=$(lines) ;;
    shot) touch shot.req ;;
    *) fail "unknown plan step '$step'" ;;
  esac
done
kill -0 $pid 2>/dev/null || fail "the game exited before the measurement"

# raw counters: everything read, nothing written
snapshot() {
  {
    for f in /proc/uptime /proc/loadavg /proc/stat /proc/meminfo /proc/vmstat /proc/$pid/status /proc/$pid/stat \
             /sys/devices/system/cpu/online /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq \
             /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor /sys/devices/system/cpu/cpu*/cpufreq/scaling_min_freq \
             /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq \
             /sys/devices/system/cpu/cpu*/cpufreq/stats/time_in_state /sys/devices/system/cpu/cpu*/cpufreq/stats/total_trans \
             /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq /sys/devices/system/cpu/cpufreq/policy*/stats/time_in_state \
             /sys/class/devfreq/*/cur_freq /sys/class/devfreq/*/governor /sys/class/devfreq/*/min_freq /sys/class/devfreq/*/max_freq \
             /sys/class/devfreq/*/available_frequencies /sys/class/devfreq/*/trans_stat /sys/class/devfreq/*/load \
             /sys/class/thermal/thermal_zone*/type /sys/class/thermal/thermal_zone*/temp \
             /sys/class/thermal/cooling_device*/type /sys/class/thermal/cooling_device*/cur_state \
             /sys/class/misc/mali0/device/utilization /sys/class/misc/mali0/device/gpuinfo /sys/class/misc/mali0/device/dvfs_period \
             /sys/class/misc/mali0/device/power_policy /sys/class/misc/mali0/device/core_mask \
             /sys/kernel/debug/mali0/gpu_memory; do
      [ -r "$f" ] || continue
      echo "=== $f"; cat "$f" 2>/dev/null
    done
    for t in /proc/$pid/task/*; do
      echo "=== task ${t##*/} $(cat "$t/comm" 2>/dev/null)"
      cat "$t/schedstat" "$t/stat" 2>/dev/null
    done
    echo "=== procs"
    for p in /proc/[0-9]*; do cat "$p/stat" 2>/dev/null; echo; done | grep -v '^$'
  } > "$1" 2>/dev/null
}
M=${MEASURE:-30}
log "measure start ($M s)"
snapshot "$OUT/snap-0.txt"
[ -n "$PROF" ] && { echo "$M" > prof.req.tmp && mv prof.req.tmp prof.req; }
for t in $(seq 1 "$M"); do
  sleep 1
  kill -0 $pid 2>/dev/null || fail "the game exited during the measurement"
  rdlist f 1000 /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq
  rdlist g 1000000 /sys/class/devfreq/*/cur_freq
  rdlist z 1000 /sys/class/thermal/thermal_zone[0-9]*/temp
  rdlist k 1 /sys/class/thermal/cooling_device[0-9]*/cur_state
  memkv /proc/meminfo MemAvailable SwapFree
  echo "t=$t cpu=${f:-?} gpu=${g:-?} avail=${M_MemAvailable}M swapfree=${M_SwapFree}M temp=${z:-?} cool=${k:-?}" >> "$OUT/freq.log"
done
snapshot "$OUT/snap-1.txt"
log "measure end"
if [ -n "$PROF" ]; then
  # the profile is written when the recording ends (the sampler names it first: a few seconds)
  n0=$(ls prof-*.txt 2>/dev/null | wc -l)
  for _ in $(seq 1 90); do
    [ "$(ls prof-*.txt 2>/dev/null | wc -l)" -gt "$n0" ] && break
    grep -q '^\[prof\] prof-' "$LOG" && break
    sleep 1
  done
fi
stop_game
mv -f "$ROOT"/prof-*.txt "$ROOT"/shot-*.ppm "$OUT"/ 2>/dev/null
grep -q 'LOW MEMORY' "$LOG" && { echo "stopped by the memory watchdog" > "$OUT/result"; exit 1; }
echo ok > "$OUT/result"
