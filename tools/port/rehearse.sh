#!/bin/bash
# rehearse.sh <port zip> <game folder or archive> <work dir> [emu seconds] [input script]
#
# A port's first launch rehearsed on a PC, as on a handheld: the release zip (`pmunity port package`)
# is unpacked into <work>/roms/ports, the game copied into its gamedata/, and the port's own
# launcher started in an aarch64 container with a handheld's tools (busybox first on PATH, bash,
# curl, PortMaster's 7-Zip, 1 GB of RAM, the network on) and a stand-in for PortMaster
# (control.txt, utils/patcher.txt). The launcher runs the setup through the "patcher"; afterwards
# ports/<name>/ is what the handheld would have. With [emu seconds] the installed game then runs
# in the emulated armv7 rig (tools/emu: QEMU + Mesa llvmpipe) with the port's port.env, and four
# screenshots go to <work>/shots.png.
#
#   PM_7ZZ          PortMaster's 7zzs.aarch64 (from PortMaster's release zip; required)
#   ANSWERS         answers to the port's questions, e.g. "TEXTURE_QUALITY=fast" (default: none,
#                   as a PortMaster without questions)
#   REHEARSE_IMAGE  the container image (default ua-port-rehearse, built from rehearse/Dockerfile)
#   PYTHON          a Python with Pillow, for shots.png (default python3)
#   EMU_ENV         more variable prefixes for the emulated run (tools/emu/emu.sh), e.g. "UM_"
#   EMU_ONLY=1      skip the setup: run what an earlier rehearsal in <work> installed
#   REHEARSE_64ONLY=1  a 64-bit-only system: the image's 32-bit loader (/lib/ld-linux-armhf.so.3)
#                   is removed in the container first, so a launcher takes its 64-bit path
# Game files stay in <work>; nothing of them goes anywhere else.
set -euo pipefail
ZIP=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); GAME=$2; WORK=$3; SECS=${4:-0}; INP=${5:-}
HERE=$(cd "$(dirname "$0")" && pwd); PU=$(cd "$HERE/../.." && pwd)
PM7Z=${PM_7ZZ:?set PM_7ZZ to PortMaster\'s 7zzs.aarch64}
IMG=${REHEARSE_IMAGE:-ua-port-rehearse}
PY=${PYTHON:-python3}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
docker image inspect "$IMG" >/dev/null 2>&1 || docker build --platform linux/arm64 -t "$IMG" "$HERE/rehearse"

R="$WORK/roms"
if [ "${EMU_ONLY:-}" = 1 ]; then
  pj=$(ls "$R"/ports/*/port.json | head -1)
  name=$(basename "$(dirname "$pj")"); P="$R/ports/$name"
else
rm -rf "$R"; mkdir -p "$R/ports"
# the release zip, unpacked as PortMaster does (file modes kept)
"$PY" -I - "$ZIP" "$R/ports" <<'PY'
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    for i in z.infolist():
        p = z.extract(i, sys.argv[2])
        m = i.external_attr >> 16 & 0o777
        if m:
            os.chmod(p, m)
PY
pj=$(ls "$R"/ports/*/port.json | head -1)
read -r name launcher < <("$PY" -I -c "import json,sys; d=json.load(open(sys.argv[1])); print(d['items'][1], d['items'][0])" "$pj")
P="$R/ports/$name"
echo "port $name, launcher $launcher"
case "$GAME" in
  *.zip|*.7z|*.rar|*.tar|*.tar.gz|*.tgz|*.tar.xz|*.pkg|*.exe) cp "$GAME" "$P/gamedata/";;
  *) cp -R "$GAME" "$P/gamedata/";;
esac

# a stand-in for PortMaster: control.txt, the patcher, 7-Zip
PMD="$R/ports/PortMaster"; mkdir -p "$PMD/utils"
cp "$PM7Z" "$PMD/7zzs.aarch64"; chmod +x "$PMD/7zzs.aarch64"
cat > "$PMD/control.txt" <<'TXT'
# rehearsal stand-in for PortMaster's control.txt
directory=roms
CFW_NAME=Rehearsal; DEVICE_NAME=container; DEVICE_ARCH=aarch64; DISPLAY_WIDTH=720; DISPLAY_HEIGHT=480
ESUDO=""; GPTOKEYB="true"; GPTOKEYB2="true"; sdl_controllerconfig=""
get_controls() { :; }
pm_message() { echo "[pm_message] $*"; }
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

echo "== setup in the container ($(date +%T))"
t0=$(date +%s)
docker run --rm --platform linux/arm64 --memory 1g --memory-swap 2g -v "$R":/roms -e ANSWERS="${ANSWERS:-}" \
  -e HOME=/roms/home "$IMG" bash -c "mkdir -p /roms/home; ${REHEARSE_64ONLY:+rm -f /lib/ld-linux-armhf.so.3; }export PATH=/bb:\$PATH; cd /roms/ports && bash '/roms/ports/$launcher'" \
  > "$WORK/launch1.log" 2>&1 || true
echo "setup: $(( $(date +%s) - t0 )) s; launcher log: $WORK/launch1.log; setup log: $P/patchlog.txt"
grep -h "PATCH_FAIL_MSG" "$P/patchlog.txt" 2>/dev/null && exit 1
[ -f "$P/.setup_done" ] || { echo "setup did not finish"; tail -20 "$WORK/launch1.log"; exit 1; }
echo "installed: $(du -sm "$P" | cut -f1) MB; lib: $(ls "$P/lib" | tr '\n' ' ')"
fi
[ "$SECS" -gt 0 ] || exit 0

echo "== the installed game in the emulated rig, $SECS s ($(date +%T))"
E="$WORK/emuroot"; rm -rf "$E"; mkdir -p "$E"; cp "$P/ua_host" "$E/"
(
  set +u
  . "$P/port.env"; [ -f "$P/answers.env" ] && . "$P/answers.env"
  cd "$PU" && UA_INPUT_SCRIPT="$INP" EMU_ROOT="$E" EMU_ZIP="$P" EMU_HOSTBIN="$P" tools/emu/emu.sh run.log "$SECS" release
) >/dev/null 2>&1 &
prev=0
for f in 0.2 0.4 0.65 0.9; do
  t=$(awk "BEGIN{print int($SECS*$f)}"); sleep $((t - prev)); prev=$t; touch "$E/shot.req"
done
wait
"$PY" -I - "$E" "$WORK/shots.png" <<'PY'
import glob, sys
from PIL import Image
fs = sorted(glob.glob(sys.argv[1] + '/shot-*.ppm'), key=lambda f: int(f.rsplit('-', 1)[1].split('.')[0]))
ims = [Image.open(f) for f in fs]
if ims:
    w, h = ims[0].size
    s = Image.new('RGB', (w * 2, h * 2))
    for i, im in enumerate(ims[:4]):
        s.paste(im, ((i % 2) * w, (i // 2) * h))
    s.save(sys.argv[2])
print(len(ims), 'shots', [im.getextrema()[0] for im in ims], '->', sys.argv[2])
PY
grep -E 'UnityMain thread exits|\[crash\]|FORTIFY|abort message|^exit' "$E/run.log" | head -5 || true
