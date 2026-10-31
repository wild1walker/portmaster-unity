#!/bin/bash
# pmunity's `player donor` (the release's setup/pmunity, PC venv) on a donor APK, into <out>/py; then
# on Windows: node tests/player_test.js <apk> <version> <out>/js, and compare the md5 lists.
#   tests/refplayer.sh <port dir with setup/ and donor/> <out> [version] [apk name]
set -e
G=${1:?port dir}; R=${2:?out}; V=${3:-2017.4.9f1}; A=${4:-donor-$V-mono-armv7.apk}
VENV=${PMU_PY:?a Python with the setup's package versions (setup/pylibs, for the PC)}
rm -rf "$R/py"; mkdir -p "$R"
(cd "$G/setup" && PYTHONPATH="$G/setup" "$VENV" -m pmunity player donor "$V" "$R/py" --apk "$G/donor/$A")
cp "$G/donor/$A" "$R/"
