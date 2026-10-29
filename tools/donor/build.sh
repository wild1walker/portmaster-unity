#!/bin/bash
# build.sh <editor> <MonoArmv7|Il2cppArm64> <out dir>: a donor APK from Unity's editor (Unity.app on
# macOS) in batch mode: a one-scene game of pong whose APK carries that Unity version's Android player
# (`pmunity player donor` reads it; docs/porting.md). Needs the editor's Android Build Support, a
# signed-in licence (Personal is enough) and the Android tools 2017.4 builds with:
#   SDK   an Android SDK (tools r25.2.5, build-tools 28.0.3, platforms;android-28)
#   JDK   a JDK 8 home
#   NDK   for Il2cppArm64: the NDK the editor asks for (2017.4: r13b)
set -euo pipefail
ED=${1:?usage: build.sh <editor> <MonoArmv7|Il2cppArm64> <out dir>}; WHAT=${2:?}; OUT=${3:?}
HERE=$(cd "$(dirname "$0")" && pwd)
P=$(mktemp -d)/donor; mkdir -p "$P/Assets"; cp -R "$HERE/Assets/Editor" "$HERE/Assets/Scripts" "$P/Assets/"
BIN="$ED/Contents/MacOS/Unity"; [ -x "$BIN" ] || BIN="$ED"
if [ -n "${NDK:-}" ]; then defaults write com.unity3d.UnityEditor5.x AndroidNdkRoot -string "$NDK"; fi
DONOR_SDK=${SDK:?set SDK} DONOR_JDK=${JDK:?set JDK} DONOR_NDK=${NDK:-} \
  "$BIN" -batchmode -nographics -quit -projectPath "$P" -executeMethod "DonorBuild.$WHAT" -logFile "$P/build.log" \
  || { tail -40 "$P/build.log"; exit 1; }
mkdir -p "$OUT"; cp "$P"/out/*.apk "$OUT/"; ls -la "$OUT"
