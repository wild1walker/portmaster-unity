#!/bin/bash
# `pmu player donor` on a donor APK, into <out>/py; then
# on Windows: node tests/player_test.js <apk> <version> <out>/js, and compare the md5 lists.
#   tests/refplayer.sh <port dir with setup/ and donor/> <out> [version] [apk name]
set -e
G=${1:?port dir}; R=${2:?out}; V=${3:-2017.4.9f1}; A=${4:-donor-$V-mono-armv7.apk}
PMU=${PMU:?a pmu for this machine: setup-rs/tools/build_pmu.sh <dir> builds the aarch64 Linux one}
rm -rf "$R/py"; mkdir -p "$R"
"$PMU" player donor "$V" "$R/py" --apk "$G/donor/$A"
cp "$G/donor/$A" "$R/"
