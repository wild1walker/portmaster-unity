#!/bin/bash
# Device side of tools/device/test.sh (also usable on its own, from a shell on the handheld):
#   uatest.sh <game folder> <seconds> [screenshot seconds...]
# Runs <game folder>/ua_host on the game in that folder (lib/ + the APK, OBBs, android_package.json)
# for <seconds>, silent, and writes into the folder:
#   run.log   the runtime's and Unity's log ([fps] lines every UA_FPS s), then how the run ended
#             ("[uatest] ..." and "exit <code>")
#   mem.log   every 2 s: the system's free memory (MemAvailable), swap, the game's resident memory
#   shot-<n>.ppm  a screenshot at each [screenshot seconds]
# A memory watchdog stops the game (its own PID only) when MemAvailable falls below UA_MEM_MIN MB
# (default 100): a game that runs out of memory can otherwise freeze a 1 GB handheld until it
# reboots. UA_AUDIO=1 plays sound; other UA_* settings pass through to the runtime.
R=$1; SECS=${2:-60}; shift 2
cd "$R" || exit 1
rm -f shot-*.ppm shot.req mem.log
MIN=${UA_MEM_MIN:-100}
export UA_FPS=${UA_FPS:-5}
if [ "$UA_AUDIO" = 1 ]; then export SDL_AUDIODRIVER=alsa AUDIODEV=${AUDIODEV:-plughw:0,0}; else export SDL_AUDIODRIVER=dummy; fi
# the pad's SDL GameController mapping: PortMaster's database for this firmware, when there is one
PM=${XDG_DATA_HOME:-$HOME/.local/share}/PortMaster
if [ -z "$SDL_GAMECONTROLLERCONFIG" ] && [ -z "$SDL_GAMECONTROLLERCONFIG_FILE" ]; then
  for f in "$PM"/*/gamecontrollerdb.txt; do [ -f "$f" ] && export SDL_GAMECONTROLLERCONFIG_FILE="$f" && break; done
fi
# a 32-bit ua_host (ELF class 1) on a 64-bit firmware finds its libraries in lib32
[ "$(od -An -tx1 -j4 -N1 ua_host | tr -d ' ')" = 01 ] && export LD_LIBRARY_PATH=/lib32:/usr/lib32${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

# append mode for both writers, so this script's own [uatest] lines are not overwritten
: > run.log; ./ua_host "$PWD" >> run.log 2>&1 &
pid=$!
mem() { awk -v k="$1:" '$1 == k {print int($2 / 1024)}' "${2:-/proc/meminfo}"; }
t=0; low=
while kill -0 $pid 2>/dev/null && [ $t -lt "$SECS" ]; do
  for s in "$@"; do [ "$s" = $t ] && touch shot.req; done
  a=$(mem MemAvailable)
  if [ $((t % 2)) = 0 ]; then
    echo "t=$t avail=${a}M swapfree=$(mem SwapFree)M rss=$(mem VmRSS /proc/$pid/status)M load=$(cut -d' ' -f1 /proc/loadavg)" >> mem.log
  fi
  if [ "${a:-$MIN}" -lt "$MIN" ]; then
    echo "[uatest] LOW MEMORY at $t s (${a} MB available, under UA_MEM_MIN=$MIN): stopping the game" | tee -a mem.log >> run.log
    kill -KILL $pid; low=1; break
  fi
  sleep 1; t=$((t + 1))
done
# TERM (a background job of a script ignores INT); ua_host handles both
if kill -0 $pid 2>/dev/null; then
  echo "[uatest] ran the full $SECS s: stopping the game" >> run.log
  kill -TERM $pid; sleep 4; kill -0 $pid 2>/dev/null && kill -KILL $pid
elif [ -z "$low" ]; then
  echo "[uatest] the game exited on its own after $t s" >> run.log
fi
{ wait $pid; } 2>/dev/null; echo "exit $?" >> run.log
