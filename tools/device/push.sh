#!/bin/bash
# push.sh <local file> [remote name]: copies a file into /userdata/unity-android on the RGSP via the
# ControlMaster connection (.new + mv, md5-checked). DEVICE=root@<the handheld's address>
set -e
: "${DEVICE:?set DEVICE=root@HANDHELD_ADDRESS}"
SSH=(ssh -n -o "ControlPath=$HOME/.ssh/rgsp-%r@%h" "$DEVICE")
f=$1; n=${2:-$(basename "$1")}; d=/userdata/unity-android
scp -q -o "ControlPath=$HOME/.ssh/rgsp-%r@%h" "$f" "$DEVICE:$d/$n.new"
r=$("${SSH[@]}" "cd $d && mv $n.new $n && chmod +x $n 2>/dev/null; md5sum $n" | awk '{print $1}'); l=$(md5 -q "$f")
[ "$r" = "$l" ] && echo "pushed $n ($l)" || { echo "MD5 MISMATCH $n: $r vs $l"; exit 1; }
