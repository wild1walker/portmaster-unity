#!/bin/bash
# rehearse.sh <port zip> <game folder or archive> <work dir> [emu seconds] [input script]
#
# A port's first launch rehearsed on a PC, as on a handheld: the release zip (`pmu port package`)
# is unpacked into <work>/roms/ports, the game copied into its gamedata/, and the port's own
# launcher started in an aarch64 container with a handheld's tools (busybox first on PATH, bash,
# curl, PortMaster's 7-Zip, 1 GB of RAM, the network on) and a stand-in for PortMaster
# (control.txt, utils/patcher.txt). The launcher runs the setup through the "patcher"; afterwards
# ports/<name>/ is what the handheld would have. With [emu seconds] the installed game then runs
# in the emulated arm64 rig (tools/emu: Mesa llvmpipe) with the port's port.env (and, as the
# launcher does, UA_WWISE_DATA / UA_FMOD_DATA when the setup converted Wwise / FMOD banks), four screenshots go to
# <work>/shots.png and the sound to <work>/sound.wav (never played aloud: what the game's OpenSL ES
# output played, the Wwise player's mix included, with its levels printed at the end). The results of
# an earlier emulated run in <work> move to <work>/earlier/<n>/ first.
#
#   PM_7ZZ          PortMaster's 7zzs.aarch64 (from PortMaster's release zip; required)
#   ANSWERS         answers to the port's questions, e.g. "TEXTURE_QUALITY=fast" (default: none,
#                   as a PortMaster without questions)
#   REHEARSE_IMAGE  the container image (default ua-port-rehearse, built from rehearse/Dockerfile)
#   REHEARSE_DONE   the file a finished setup leaves in the port folder (default .setup_done)
#   PM_PYTHON       PortMaster's python_3.11.squashfs, for a launcher that mounts it (libs/): unpacked
#                   into $HOME/python_3.11 in the container instead (mount and umount do nothing there)
#   EMU_ENV         more variable prefixes for the emulated run (tools/emu/emu.sh), e.g. "UM_"
#   EMU_ONLY=1      skip the setup: run what an earlier rehearsal in <work> installed
#   REHEARSE_SOUND=0  no sound.wav (default 1: ANDROID_SLES_CAPTURE in the rig; about 11 MB a minute).
#                   Other variables reach the rig as tools/emu/emu.sh says (UA_*, ANDROID_*, ..., EMU_ENV),
#                   e.g. UA_WWISE_CAPTURE=/emu/wwise.wav (the Wwise player's mix alone, for the whole run
#                   unless UA_WWISE_CAPTURE_SECS) lands in <work>/emuroot/
#   REHEARSE_AARCH64_ONLY=1  before the game goes in, the release's own pmu checks it in the container
#                   (`pmu arch-check`: no armhf ELF file, the donors' entries included, and the 64-bit route's
#                   pieces there; docs/arm64.md, "Shipping aarch64 only") and the rehearsal stops if it fails
#   REHEARSE_NAME   the setup container's name (to docker kill it)
#   REHEARSE_OVER   an update rehearsed: the work dir of an earlier rehearsal (of an older release), whose
#                   installed roms/ is copied and the zip unpacked over it as PortMaster updates a port
#                   (nothing removed first; <game> is not used: pass -). The launcher then runs what it
#                   runs on an updated install (a setup/update step, or the game at once)
#   REHEARSE_NET=none  no network in the setup container: a port whose setup never downloads anything
#                   (its donors carry Unity's player) shows that it needs no Wi-Fi
#   PMU_EXACT_STATS set: the textures step's exact_shaders walk prints its memory and tables (exactindex.rs)
#   PMU_PROF        set: pmu's per-step profile in the setup log (prof: <step>: <stage> <s> (<calls> calls):
#                   the file opens, writes, journal writes...), counts that machine noise does not change
#   REHEARSE_MEM    the setup container's memory with no swap (e.g. 800m: the BattleXP G350 with
#                   EmulationStation stopped; 700m with it running); default 1g with 1 GB of swap
#                   (REHEARSE_MEMSWAP: memory + swap), where a step that needs more than a handheld has
#                   only gets slower: Blasphemous's textures step was killed on the G350 (2026-11-18)
#   REHEARSE_NOFILE  the setup's open-file limit, soft:hard (default 1024:524288: dArkOS's, which a port's
#                   processes inherit from EmulationStation; a step that holds more files open at once
#                   fails there with "os error 24")
#   REHEARSE_MEMINFO  a /proc/meminfo for the setup container (Docker shows the VM's RAM): e.g. one with
#                   "MemTotal: 996352 kB", for the setup steps that decide by the handheld's RAM (video,
#                   textures lowmem_scale)
#   REHEARSE_CARD_MB  the card's free space the setup is told (default: the work folder's): more than the drive
#                   has where the game is an APFS clone (cp -c) that the setup's space checks count whole
#   REHEARSE_MEMLOG=1  the setup's memory sampled twice a second into <work>/mem.log (the container's: all,
#                   anonymous, page cache; each process over 2 MB: RSS/anonymous/peak RSS in MB; the setup's
#                   step and its last line), then each program's peak and the step it was in (`pmu-dev memlog`):
#                   which step and program need the most of a handheld's memory
#   EMU_ARCH        the emulated rig's architecture (tools/emu/emu.sh): arm64 (armhf was removed); the
#                   port's ua_host64 runs where it has one (the apk route)
# Game files stay in <work>; nothing of them goes anywhere else.
set -euo pipefail
ZIP=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); GAME=$2; WORK=$3; SECS=${4:-0}; INP=${5:-}
HERE=$(cd "$(dirname "$0")" && pwd); PU=$(cd "$HERE/../.." && pwd)
PM7Z=${PM_7ZZ:?set PM_7ZZ to PortMaster\'s 7zzs.aarch64}
IMG=${REHEARSE_IMAGE:-ua-port-rehearse}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
docker image inspect "$IMG" >/dev/null 2>&1 || docker build --platform linux/arm64 -t "$IMG" "$HERE/rehearse"

R="$WORK/roms"
if [ "${EMU_ONLY:-}" = 1 ]; then
  pj=$(ls "$R"/ports/*/port.json | head -1)
  name=$(basename "$(dirname "$pj")"); P="$R/ports/$name"
else
if [ -n "${REHEARSE_OVER:-}" ]; then
  [ -d "$REHEARSE_OVER/roms/ports" ] || { echo "REHEARSE_OVER: no $REHEARSE_OVER/roms/ports"; exit 1; }
  [ "$(cd "$REHEARSE_OVER" && pwd)" != "$WORK" ] || { echo "REHEARSE_OVER: the earlier rehearsal's work dir, not this one's"; exit 1; }
fi
rm -rf "$R"; mkdir -p "$R/ports"
[ -z "${REHEARSE_OVER:-}" ] || cp -cR "$REHEARSE_OVER/roms/." "$R/" 2>/dev/null || cp -R "$REHEARSE_OVER/roms/." "$R/"
# the release zip, unpacked as PortMaster does (file modes kept; over an earlier install with REHEARSE_OVER)
# (the zip's own Unix modes kept: unzip restores them)
unzip -qo "$ZIP" -d "$R/ports"
pj=$(ls "$R"/ports/*/port.json | head -1)
read -r name launcher < <(jq -r '"\(.items[1]) \(.items[0])"' "$pj")
P="$R/ports/$name"
echo "port $name, launcher $launcher"
if [ "${REHEARSE_AARCH64_ONLY:-}" = 1 ]; then
  [ -z "${REHEARSE_OVER:-}" ] || { echo "REHEARSE_AARCH64_ONLY: a fresh install only (not with REHEARSE_OVER)"; exit 1; }
  [ -x "$P/setup/pmu" ] || { echo "REHEARSE_AARCH64_ONLY: the release has no setup/pmu to check it with"; exit 1; }
  docker run --rm --platform linux/arm64 -v "$R/ports:/rel:ro" "$IMG" "/rel/$name/setup/pmu" arch-check /rel \
    || { echo "the release is not aarch64 only (pmu arch-check above)"; exit 1; }
fi
[ -n "${REHEARSE_OVER:-}" ] && { m=$(head -1 "$P/${REHEARSE_DONE:-.setup_done}" 2>/dev/null | cut -c1-120 || true); echo "over the install of $REHEARSE_OVER (its marker: ${m:-empty})"; GAME=; }
case "$GAME" in
  "") ;;
  *.zip|*.7z|*.rar|*.tar|*.tar.gz|*.tgz|*.tar.xz|*.pkg|*.exe|*.sh) cp "$GAME" "$P/gamedata/";;   # (.sh: GOG's Linux installer)
  *) cp -cR "$GAME" "$P/gamedata/" 2>/dev/null || cp -R "$GAME" "$P/gamedata/";;   # (-c: an APFS clone on a Mac, at once)
esac

# a stand-in for PortMaster: control.txt, the patcher, 7-Zip
PMD="$R/ports/PortMaster"; mkdir -p "$PMD/utils"
cp "$PM7Z" "$PMD/7zzs.aarch64"; chmod +x "$PMD/7zzs.aarch64"
[ -n "${PM_PYTHON:-}" ] && { mkdir -p "$PMD/libs"; cp "$PM_PYTHON" "$PMD/libs/python_3.11.squashfs"; }
cat > "$PMD/control.txt" <<'TXT'
# rehearsal stand-in for PortMaster's control.txt
directory=roms
CFW_NAME=Rehearsal; DEVICE_NAME=container; DEVICE_ARCH=aarch64; DISPLAY_WIDTH=720; DISPLAY_HEIGHT=480
ESUDO=""; GPTOKEYB="true"; GPTOKEYB2="true"; sdl_controllerconfig=""
get_controls() { :; }
pm_message() { echo "[pm_message] $*"; }
pm_show_error() { echo "[pm_show_error] $1"; }   # (PortMaster's error card: the text; --title and --type are its options)
pm_message_end() { :; }
pm_finish() { :; }
pm_platform_helper() { :; }
TXT
cat > "$PMD/utils/patcher.txt" <<'TXT'
# rehearsal stand-in for PortMaster's utils/patcher.txt (the real one shows the script's output
# on screen and asks PATCHER_QUESTIONS first; answers reach the script as <id>=<value>)
echo "[patcher] $PATCHER_GAME ($PATCHER_TIME): $PATCHER_FILE${PATCHER_QUESTIONS:+, questions $PATCHER_QUESTIONS}"
for kv in $ANSWERS; do export "$kv"; done
bash "$PATCHER_FILE"
echo "[patcher] exit $?"
TXT

# the card's free space as the PC sees it: Docker Desktop's file sharing reports another disk's for a
# folder on an external drive, which a setup's space checks would trust (rbin/df, first on PATH,
# answers `df -Pm` with it, and PMUNITY_DISK_FREE_MB tells pmunity's own checks; the figure does not
# shrink as the setup writes)
FREE_MB=${REHEARSE_CARD_MB:-$(df -Pm "$WORK" | awk 'NR==2{print $4}')}
mkdir -p "$WORK/rbin"
cat > "$WORK/rbin/df" <<'SH'
#!/bin/sh
case " $* " in
  *" -Pm "*) echo "Filesystem 1048576-blocks Used Available Capacity Mounted on"
             echo "rehearsal 0 0 ${REHEARSE_FREE_MB:-0} 0% /roms"; exit 0 ;;
esac
exec /bb/df "$@"
SH
chmod +x "$WORK/rbin/df"
# REHEARSE_MEMLOG: a sampler beside the launcher in the container (busybox sh and awk, ~1 MB), until it ends
cat > "$WORK/rbin/memsamp" <<'SH'
#!/bin/sh
# memsamp <log> <port folder>: the container's memory (cgroup v2) and each process's, twice a second: a line
# <time> <MB> <anonymous MB> <page cache MB> <name/pid RSS/anonymous/peak RSS ...> <step> <the setup's last line>
# (tab-separated; MB)
# (the step: the setup log's last "== " line, and pmu convert's own last "] == <step>" after it, read as the log grows)
cg=/sys/fs/cgroup; off=0; s1=; s2=
while :; do
  sz=$(wc -c < "$2/patchlog.txt" 2>/dev/null || echo 0)
  if [ "$sz" -gt "$off" ]; then
    r=$(tail -c +$((off + 1)) "$2/patchlog.txt" | head -c $((sz - off)) | tr -d '\r\t' | awk -v s1="$s1" -v s2="$s2" '
      /^== / { s1 = substr($0, 4, 64); s2 = "" } /\] == / { sub(/.*\] == /, ""); s2 = substr($0, 1, 24) }
      END { print s1 "|" s2 }')
    s1=${r%|*}; s2=${r##*|}; off=$sz
  fi
  st="$s1${s2:+: $s2}"
  last=$(tail -c 512 "$2/patchlog.txt" 2>/dev/null | tail -n 1 | tr -d '\r\t' | cut -c1-80)
  awk -v t="$(date +%s)" -v cur="$(cat $cg/memory.current 2>/dev/null)" -v st="$st" -v last="$last" -v cgs="$cg/memory.stat" '
    FILENAME == cgs { if ($1 == "anon" || $1 == "file") m[$1] = int($2 / 1048576); next }
    FNR == 1 && n != "" { if (r > 2048) out = out sprintf(" %s/%s %d/%d/%d", n, p, r / 1024, a / 1024, h / 1024); n = ""; r = a = h = 0 }
    /^Name:/ { n = $2 } /^Pid:/ { p = $2 } /^VmRSS:/ { r = $2 } /^RssAnon:/ { a = $2 } /^VmHWM:/ { h = $2 }
    END { if (n != "" && r > 2048) out = out sprintf(" %s/%s %d/%d/%d", n, p, r / 1024, a / 1024, h / 1024)
          printf "%s\t%d\t%d\t%d\t%s\t%s\t%s\n", t, cur / 1048576, m["anon"], m["file"], substr(out, 2), st, last }
  ' $cg/memory.stat /proc/[0-9]*/status 2>/dev/null >> "$1"
  sleep 0.5
done
SH
chmod +x "$WORK/rbin/memsamp"
rm -f "$R/mem.log"
for c in mount umount; do
  if [ -n "${PM_PYTHON:-}" ]; then printf '#!/bin/sh\nexit 0\n' > "$WORK/rbin/$c"; chmod +x "$WORK/rbin/$c"; else rm -f "$WORK/rbin/$c"; fi
done
echo "== setup in the container ($(date +%T); the card: $FREE_MB MB free)"
t0=$(date +%s)
# the setup container's memory: 1 GB with 1 GB of swap beside it (the default), or a handheld's: REHEARSE_MEM alone
# is that much and no swap (a G350: 800m, what its setup has with EmulationStation stopped)
RMEM=${REHEARSE_MEM:-1g}; RSWAP=${REHEARSE_MEMSWAP:-${REHEARSE_MEM:-2g}}
docker run --rm --platform linux/arm64 ${REHEARSE_NAME:+--name "$REHEARSE_NAME"} ${REHEARSE_NET:+--network "$REHEARSE_NET"} --memory "$RMEM" --memory-swap "$RSWAP" --ulimit "nofile=${REHEARSE_NOFILE:-1024:524288}" -v "$R":/roms -v "$WORK/rbin":/rbin:ro \
  ${REHEARSE_MEMINFO:+-v "$REHEARSE_MEMINFO":/proc/meminfo:ro} ${PMU_PROF:+-e PMU_PROF="$PMU_PROF"} ${PMU_EXACT_STATS:+-e PMU_EXACT_STATS=1} \
  -e REHEARSE_FREE_MB="$FREE_MB" -e PMUNITY_DISK_FREE_MB="$FREE_MB" -e ANSWERS="${ANSWERS:-}" \
  -e HOME=/roms/home "$IMG" bash -c "mkdir -p /roms/home; ${REHEARSE_MEMLOG:+(/rbin/memsamp /roms/mem.log '/roms/ports/$name' &); }${PM_PYTHON:+/roms/ports/PortMaster/7zzs.aarch64 x -snl -y -o/roms/home/python_3.11 /roms/ports/PortMaster/libs/python_3.11.squashfs >/dev/null; }export PATH=/rbin:/bb:\$PATH; cd /roms/ports && bash '/roms/ports/$launcher'" \
  > "$WORK/launch1.log" 2>&1 || true
echo "setup: $(( $(date +%s) - t0 )) s; launcher log: $WORK/launch1.log; setup log: $P/patchlog.txt"
if [ -f "$R/mem.log" ]; then mv "$R/mem.log" "$WORK/mem.log"; "$PU/setup-rs/tools/pmu-dev.sh" memlog "$WORK/mem.log" || true; fi
grep -h "PATCH_FAIL_MSG" "$P/patchlog.txt" 2>/dev/null && exit 1
[ -f "$P/${REHEARSE_DONE:-.setup_done}" ] || { echo "setup did not finish"; tail -20 "$WORK/launch1.log"; exit 1; }
[ -z "${REHEARSE_OVER:-}" ] || echo "after the update: $(head -1 "$P/${REHEARSE_DONE:-.setup_done}" | cut -c1-120); setup folder: $(ls "$P/setup" 2>/dev/null | wc -l | tr -d ' ') entries"
# (the player's libraries: in the port's folder, or one level down where a port keeps a second build there)
lib=$(ls -d "$P/lib" "$P"/*/lib 2>/dev/null | head -1 || true)
echo "installed: $(du -sm "$P" | cut -f1) MB; ${lib#$P/}: $(ls "${lib:-$P/lib}" 2>/dev/null | tr '\n' ' ')"
fi
[ "$SECS" -gt 0 ] || exit 0

# the ua_host that runs the installed build: the port's ua_host, or its ua_host64 (the apk route)
abi=$(sed -n 's/^ *"abi": *"\([^"]*\)".*/\1/p' "$P/android_package.json" 2>/dev/null | head -1)
# an earlier emulated run's results (EMU_ONLY reruns, a second rehearsal) are kept: <work>/earlier/<n>/
E="$WORK/emuroot"
if [ -f "$E/run.log" ]; then
  n=1; while [ -e "$WORK/earlier/$n" ]; do n=$((n + 1)); done; mkdir -p "$WORK/earlier/$n"
  for f in "$E"/run.log "$E"/shot-*.ppm "$E"/*.wav "$WORK/shots.png" "$WORK/sound.wav"; do if [ -f "$f" ]; then mv "$f" "$WORK/earlier/$n/"; fi; done
  echo "the earlier run's log, shots and sound: $WORK/earlier/$n"
fi
H="$WORK/hostbin"; rm -rf "$H"; mkdir -p "$H"
if [ -f "$P/ua_host64" ]; then cp "$P/ua_host64" "$H/ua_host"; else cp "$P/ua_host" "$H/ua_host"; fi
export EMU_ARCH=arm64
echo "== the installed game in the emulated rig (arm64${abi:+, $abi}), $SECS s ($(date +%T))"
rm -rf "$E"; mkdir -p "$E"; cp "$H/ua_host" "$E/"
(
  set +u
  . "$P/port.env"; [ -f "$P/answers.env" ] && . "$P/answers.env"
  # as the launcher: ua_host's own Wwise player on the banks the setup converted (the port folder is /zip in the rig)
  [ -d "$P/wwise" ] && export UA_WWISE_DATA="${UA_WWISE_DATA-/zip/wwise}"
  [ -d "$P/fmod" ] && export UA_FMOD_DATA="${UA_FMOD_DATA-/zip/fmod}"
  [ -n "${UA_WWISE_CAPTURE:-}" ] && export UA_WWISE_CAPTURE_SECS="${UA_WWISE_CAPTURE_SECS:-$SECS}"
  [ "${REHEARSE_SOUND:-1}" = 1 ] && export ANDROID_SLES_CAPTURE=/emu/sound.pcm
  cd "$PU" && UA_INPUT_SCRIPT="$INP" EMU_ROOT="$E" EMU_ZIP="$P" EMU_HOSTBIN="$H" tools/emu/emu.sh run.log "$SECS" release
) >/dev/null 2>&1 &
prev=0
for f in 0.2 0.4 0.65 0.9; do
  t=$(awk "BEGIN{print int($SECS*$f)}"); sleep $((t - prev)); prev=$t; touch "$E/shot.req"
done
wait
"$PU/setup-rs/tools/pmu-dev.sh" montage "$E" "$WORK/shots.png"
# the sound: the OpenSL ES output's raw PCM (its format in run.log) as WAV files, and how loud they are
[ "${REHEARSE_SOUND:-1}" = 1 ] && "$PU/setup-rs/tools/pmu-dev.sh" pcmwav "$E" "$WORK"
grep -E 'UnityMain thread exits|\[crash\]|FORTIFY|abort message|^exit' "$E/run.log" | head -5 || true
