#!/bin/bash
# rehearse.sh <port zip> <game folder or archive> <work dir> [emu seconds] [input script]
#
# A port's first launch rehearsed on a PC, as on a handheld: the release zip (`pmunity port package`)
# is unpacked into <work>/roms/ports, the game copied into its gamedata/, and the port's own
# launcher started in an aarch64 container with a handheld's tools (busybox first on PATH, bash,
# curl, PortMaster's 7-Zip, 1 GB of RAM, the network on) and a stand-in for PortMaster
# (control.txt, utils/patcher.txt). The launcher runs the setup through the "patcher"; afterwards
# ports/<name>/ is what the handheld would have. With [emu seconds] the installed game then runs
# in the emulated armv7 rig (tools/emu: QEMU + Mesa llvmpipe) with the port's port.env (and, as the
# launcher does, UA_WWISE_DATA when the setup converted Wwise banks), four screenshots go to
# <work>/shots.png and the sound to <work>/sound.wav (never played aloud: what the game's OpenSL ES
# output played, the Wwise player's mix included, with its levels printed at the end). The results of
# an earlier emulated run in <work> move to <work>/earlier/<n>/ first.
#
#   PM_7ZZ          PortMaster's 7zzs.aarch64 (from PortMaster's release zip; required)
#   ANSWERS         answers to the port's questions, e.g. "TEXTURE_QUALITY=fast" (default: none,
#                   as a PortMaster without questions)
#   REHEARSE_IMAGE  the container image (default ua-port-rehearse, built from rehearse/Dockerfile)
#   PYTHON          a Python with Pillow, for shots.png (default python3)
#   REHEARSE_DONE   the file a finished setup leaves in the port folder (default .setup_done)
#   PM_PYTHON       PortMaster's python_3.11.squashfs, for a launcher that mounts it (libs/): unpacked
#                   into $HOME/python_3.11 in the container instead (mount and umount do nothing there)
#   EMU_ENV         more variable prefixes for the emulated run (tools/emu/emu.sh), e.g. "UM_"
#   EMU_ONLY=1      skip the setup: run what an earlier rehearsal in <work> installed
#   REHEARSE_SOUND=0  no sound.wav (default 1: ANDROID_SLES_CAPTURE in the rig; about 11 MB a minute).
#                   Other variables reach the rig as tools/emu/emu.sh says (UA_*, ANDROID_*, ..., EMU_ENV),
#                   e.g. UA_WWISE_CAPTURE=/emu/wwise.wav (the Wwise player's mix alone, for the whole run
#                   unless UA_WWISE_CAPTURE_SECS) lands in <work>/emuroot/
#   REHEARSE_64ONLY=1  a 64-bit-only system: the image's 32-bit loader (/lib/ld-linux-armhf.so.3)
#                   is removed in the container first, so a launcher takes its 64-bit path
#   REHEARSE_NAME   the setup container's name (to docker kill it)
#   EMU_ARCH        the emulated rig's architecture (tools/emu/emu.sh: armhf or arm64); default:
#                   arm64 when the installed game is an arm64-v8a build (android_package.json "abi"),
#                   which then runs with the port's ua_host64 when it has one (the apk route)
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
FREE_MB=$(df -Pm "$WORK" | awk 'NR==2{print $4}')
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
for c in mount umount; do
  if [ -n "${PM_PYTHON:-}" ]; then printf '#!/bin/sh\nexit 0\n' > "$WORK/rbin/$c"; chmod +x "$WORK/rbin/$c"; else rm -f "$WORK/rbin/$c"; fi
done
echo "== setup in the container ($(date +%T); the card: $FREE_MB MB free)"
t0=$(date +%s)
docker run --rm --platform linux/arm64 ${REHEARSE_NAME:+--name "$REHEARSE_NAME"} --memory 1g --memory-swap 2g -v "$R":/roms -v "$WORK/rbin":/rbin:ro \
  -e REHEARSE_FREE_MB="$FREE_MB" -e PMUNITY_DISK_FREE_MB="$FREE_MB" -e ANSWERS="${ANSWERS:-}" \
  -e HOME=/roms/home "$IMG" bash -c "mkdir -p /roms/home; ${PM_PYTHON:+/roms/ports/PortMaster/7zzs.aarch64 x -snl -y -o/roms/home/python_3.11 /roms/ports/PortMaster/libs/python_3.11.squashfs >/dev/null; }${REHEARSE_64ONLY:+rm -f /lib/ld-linux-armhf.so.3; }export PATH=/rbin:/bb:\$PATH; cd /roms/ports && bash '/roms/ports/$launcher'" \
  > "$WORK/launch1.log" 2>&1 || true
echo "setup: $(( $(date +%s) - t0 )) s; launcher log: $WORK/launch1.log; setup log: $P/patchlog.txt"
grep -h "PATCH_FAIL_MSG" "$P/patchlog.txt" 2>/dev/null && exit 1
[ -f "$P/${REHEARSE_DONE:-.setup_done}" ] || { echo "setup did not finish"; tail -20 "$WORK/launch1.log"; exit 1; }
echo "installed: $(du -sm "$P" | cut -f1) MB; lib: $(ls "$P/lib" | tr '\n' ' ')"
fi
[ "$SECS" -gt 0 ] || exit 0

# the ua_host that runs the installed build: the port's ua_host, or its ua_host64 for an arm64-v8a
# build when it has both (the apk route)
abi=$(sed -n 's/^ *"abi": *"\([^"]*\)".*/\1/p' "$P/android_package.json" 2>/dev/null | head -1)
[ "$abi" = arm64-v8a ] && export EMU_ARCH=${EMU_ARCH:-arm64}
# an earlier emulated run's results (EMU_ONLY reruns, a second rehearsal) are kept: <work>/earlier/<n>/
E="$WORK/emuroot"
if [ -f "$E/run.log" ]; then
  n=1; while [ -e "$WORK/earlier/$n" ]; do n=$((n + 1)); done; mkdir -p "$WORK/earlier/$n"
  for f in "$E"/run.log "$E"/shot-*.ppm "$E"/*.wav "$WORK/shots.png" "$WORK/sound.wav"; do if [ -f "$f" ]; then mv "$f" "$WORK/earlier/$n/"; fi; done
  echo "the earlier run's log, shots and sound: $WORK/earlier/$n"
fi
H="$WORK/hostbin"; rm -rf "$H"; mkdir -p "$H"
if [ "${EMU_ARCH:-${ARCH:-armhf}}" = arm64 ] && [ -f "$P/ua_host64" ]; then cp "$P/ua_host64" "$H/ua_host"; else cp "$P/ua_host" "$H/ua_host"; fi
echo "== the installed game in the emulated rig (${EMU_ARCH:-${ARCH:-armhf}}${abi:+, $abi}), $SECS s ($(date +%T))"
rm -rf "$E"; mkdir -p "$E"; cp "$H/ua_host" "$E/"
(
  set +u
  . "$P/port.env"; [ -f "$P/answers.env" ] && . "$P/answers.env"
  # as the launcher: ua_host's own Wwise player on the banks the setup converted (the port folder is /zip in the rig)
  [ -d "$P/wwise" ] && export UA_WWISE_DATA="${UA_WWISE_DATA-/zip/wwise}"
  [ -n "${UA_WWISE_CAPTURE:-}" ] && export UA_WWISE_CAPTURE_SECS="${UA_WWISE_CAPTURE_SECS:-$SECS}"
  [ "${REHEARSE_SOUND:-1}" = 1 ] && export ANDROID_SLES_CAPTURE=/emu/sound.pcm
  cd "$PU" && UA_INPUT_SCRIPT="$INP" EMU_ROOT="$E" EMU_ZIP="$P" EMU_HOSTBIN="$H" tools/emu/emu.sh run.log "$SECS" release
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
# the sound: the OpenSL ES output's raw PCM (its format in run.log) as WAV files, and how loud they are
[ "${REHEARSE_SOUND:-1}" = 1 ] && "$PY" -I - "$E" "$WORK" <<'PY'
import array, glob, math, operator, os, re, sys, wave
e, work = sys.argv[1], sys.argv[2]
log = open(os.path.join(e, 'run.log'), errors='replace').read()
fmts = {m.group(1): (int(m.group(2)), int(m.group(3)), int(m.group(4), 16))
        for m in re.finditer(r'output to /emu/(\S+) \((\d+) Hz, (\d+) ch, fmt 0x([0-9a-f]+)\)', log)}
pcms = sorted(glob.glob(os.path.join(e, 'sound.pcm*')))
if not pcms:
    print('sound: none (the game opened no OpenSL ES output)')
for p in pcms:
    name = os.path.basename(p)
    rate, ch, fmt = fmts.get(name, (48000, 2, 0x8010))
    if fmt != 0x8010:                       # (SDL's AUDIO_S16LSB; others kept raw)
        print(f'sound: {p}: format 0x{fmt:x}, {rate} Hz, {ch} ch, left raw'); continue
    out = os.path.join(work, name.replace('sound.pcm', 'sound') + '.wav')
    peak, sq, n, loud, secs = 0, 0.0, 0, 0, 0
    with open(p, 'rb') as f, wave.open(out, 'wb') as w:
        w.setnchannels(ch); w.setsampwidth(2); w.setframerate(rate)
        while True:
            b = f.read(rate * ch * 2)           # a second at a time
            if len(b) < 2:
                break
            b = b[:len(b) // 2 * 2]
            w.writeframes(b)
            v = array.array('h', b)
            if sys.byteorder == 'big':
                v.byteswap()
            m = max(map(abs, v))
            peak = max(peak, m); sq += sum(map(operator.mul, v, v)); n += len(v); secs += 1
            loud += m > 328                     # (above -40 dBFS)
    os.remove(p)
    db = lambda x: f'{20 * math.log10(x / 32768):.1f} dBFS' if x > 0 else 'silent'
    print(f'sound: {out}: {n / ch / rate:.0f} s, peak {db(peak)}, RMS {db(math.sqrt(sq / n) if n else 0)}, '
          f'sound in {loud} of {secs} seconds')
PY
grep -E 'UnityMain thread exits|\[crash\]|FORTIFY|abort message|^exit' "$E/run.log" | head -5 || true
