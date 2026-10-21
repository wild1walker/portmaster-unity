#!/bin/bash
# PC side: drive a game running on a handheld (tools/device/test.sh) by what it shows.
#   DEVICE=root@<handheld> tools/device/drive.sh "<presses>" [seconds] [shot.png]
# Sends the presses (UA_INPUT_SCRIPT's syntax, times from now: "0:a~2 5:down") through the game
# folder's input.req (ua_host reads it every 0.25 s), waits [seconds] (default 10), then takes a
# screenshot (shot.req) and copies it back as [shot.png] (default ./shot.png; a .ppm without
# Python's PIL). "" sends nothing: only the screenshot.
# Settings: DEVICE, SSH_OPTS and REMOTE as for test.sh (REMOTE: the folder test.sh used).
set -u
PRESSES=${1-}; WAIT=${2:-10}; OUT=${3:-shot.png}
: "${DEVICE:?set DEVICE=user@host (the handheld)}"; : "${REMOTE:?set REMOTE=<the game folder on the handheld, as test.sh used it>}"
# shellcheck disable=SC2206
OPTS=(${SSH_OPTS:-})
rsh() { ssh -n ${OPTS[@]+"${OPTS[@]}"} "$DEVICE" "$@"; }
q() { printf '%q' "$1"; }
if [ -n "$PRESSES" ]; then
  rsh "cd $(q "$REMOTE") && printf '%s' $(q "$PRESSES") > input.req.tmp && mv input.req.tmp input.req" ||
    { echo "cannot reach $DEVICE:$REMOTE"; exit 1; }
fi
sleep "$WAIT"
# the next screenshot ua_host writes (shot-<n>.ppm, n counting up) after the request
shot=$(rsh "cd $(q "$REMOTE") && n=\$(ls shot-*.ppm 2>/dev/null | wc -l) && touch shot.req &&
  for i in \$(seq 1 30); do [ \$(ls shot-*.ppm 2>/dev/null | wc -l) -gt \$n ] && break; sleep 1; done &&
  ls -t shot-*.ppm | head -1")
[ -n "$shot" ] || { echo "no screenshot (is the game running in $REMOTE?)"; exit 1; }
tmp=$(mktemp "${TMPDIR:-/tmp}/drive.XXXXXX")
ssh ${OPTS[@]+"${OPTS[@]}"} "$DEVICE" "cat $(q "$REMOTE/$shot")" > "$tmp" || { rm -f "$tmp"; exit 1; }
if python3 -c 'import PIL' 2>/dev/null && [ "${OUT##*.}" != ppm ]; then
  python3 -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' "$tmp" "$OUT"
  rm -f "$tmp"
else
  OUT=${OUT%.*}.ppm; mv "$tmp" "$OUT"
fi
echo "$OUT"
