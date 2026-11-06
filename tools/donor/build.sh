#!/bin/bash
# build.sh <editor> <MonoArmv7|Il2cppArm64|Il2cppArmv7> <out dir>: a donor APK from Unity's editor (Unity.app on
# macOS) in batch mode: a one-scene game of pong whose APK carries that Unity version's Android player
# (`pmunity player donor` reads it), written as donor-<version>-mono-armv7.apk or
# donor-<version>-il2cpp-arm64.apk (Il2cppArmv7: -il2cpp-armv7.apk), the names a port's setup reads. Every port of the android and
# mono-arm64 routes ships these (the handheld never downloads Unity's player): `pmunity port check` and
# `port new` name the ones a port needs, `port package --donors <out dir>` ships them (docs/porting.md,
# section 5, "Donors"). The editor: get-editor.sh <version> <dir> (unpacked from Unity's installers, no
# Hub), or one the Unity Hub installed.
# Needs the editor's Android Build Support, a licence (Unity Personal is enough: Unity_lic.ulf in
# /Library/Application Support/Unity; nothing here signs in) and the Android tools the editor builds with:
#   SDK   an Android SDK (2017.4: tools r25.2.5, build-tools 28.0.3, platforms;android-28; Unity 6:
#         the sub-modules Unity's release API lists for the version: build-tools, platform-tools,
#         platforms, cmdline-tools, and cmake/3.22.1, which Unity 6 checks for every Android build)
#   JDK   a JDK home (2017.4: JDK 8; Unity 6: the OpenJDK 17 its release API lists)
#   NDK   for Il2cppArm64 and Il2cppArmv7: the NDK the editor asks for (2017.4: r13b); Unity 6 asks for
#         one for every build, Mono too (6000.0: r27c, in <editor>/PlaybackEngines/AndroidPlayer/NDK)
# 6000.0.61f1 (2026-11-06): its MonoArmv7 donor's player (`pmu player donor`) is the one Unity's
# installer gives, file for file (75 files, same SHA-256).
set -euo pipefail
export TZ=UTC     # (the APK's file times: UTC, not the porter's local time)
ED=${1:?usage: build.sh <editor> <MonoArmv7|Il2cppArm64|Il2cppArmv7> <out dir>}; WHAT=${2:?}; OUT=${3:?}
HERE=$(cd "$(dirname "$0")" && pwd)
P=$(mktemp -d)/donor; mkdir -p "$P/Assets"; cp -R "$HERE/Assets/Editor" "$HERE/Assets/Scripts" "$P/Assets/"
BIN="$ED/Contents/MacOS/Unity"; [ -x "$BIN" ] || BIN="$ED"
if [ -n "${NDK:-}" ]; then defaults write com.unity3d.UnityEditor5.x AndroidNdkRoot -string "$NDK"; fi
DONOR_SDK=${SDK:?set SDK} DONOR_JDK=${JDK:?set JDK} DONOR_NDK=${NDK:-} \
  "$BIN" -batchmode -nographics -quit -projectPath "$P" -executeMethod "DonorBuild.$WHAT" -logFile "$P/build.log" \
  || { tail -40 "$P/build.log"; exit 1; }
mkdir -p "$OUT"; cp "$P"/out/*.apk "$OUT/"; ls -la "$OUT"
