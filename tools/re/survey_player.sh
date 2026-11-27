#!/bin/bash
# survey_player.sh PKG OUT
#   PKG: an unpacked Unity Android Build Support package (the directory holding Variations/, Data/)
#   OUT: where the notes go (text and JSON, a few MB at most)
# Runs the player survey's tools (pmu-dev, setup-rs/crates/devtools/src/re) on the Release players of
# that package:
#   facts.json          ELF facts of every Release library (pmu-dev elf-facts)
#   imports/<var>/      undefined symbols per library + summary.json (pmu-dev elf-imports)
#   jnitables.txt       RegisterNatives tables of the Mono armeabi-v7a libunity / libmain (jni-tables)
#   strrefs_jni.txt     functions of that libunity referencing JNI names (strrefs, with symbols)
#   bindings.tsv        Java members libunity binds (jni-bindings), direct.txt other JNI users
#   strings.txt         libunity strings about paths, libraries, Java classes, files (strings)
#   dis_monomanager.txt MonoManager::GetAvailableDllAssemblyMask (engine-module slot count; dis)
#   il2cpp_api.txt      il2cpp_* functions the IL2CPP arm64-v8a libunity looks up in libil2cpp.so
#   javap.txt           every class and member of the player's classes.jar (javap -p; javap_s.txt
#                       with JNI descriptors)
# Docker: setup-rs/tools/pmu-dev.sh (the ua-rust image: setup-rs/docker, with armhf glibc for
# elf-imports) and $UA_JAVA_IMAGE (javap; default ua-jni-build).
set -euo pipefail
PKG=$(cd "$1" && pwd); mkdir -p "$2"; OUT=$(cd "$2" && pwd)
PM=$(cd "$(dirname "$0")/../.." && pwd)
JAVA=${UA_JAVA_IMAGE:-ua-jni-build}
dev() { (cd "$PKG" && PMU_MOUNTS="$OUT" "$PM/setup-rs/tools/pmu-dev.sh" "$@"); }

V=Variations
libs=$(cd "$PKG" && ls $V/*/Release/Libs/{armeabi-v7a,arm64-v8a}/*.so $V/*/Release/MonoLibs/armeabi-v7a/*.so 2>/dev/null || true)
# shellcheck disable=SC2086
dev elf-facts $libs > "$OUT/facts.json"

for d in mono/Release/Libs/armeabi-v7a il2cpp/Release/Libs/armeabi-v7a il2cpp/Release/Libs/arm64-v8a; do
  [ -d "$PKG/$V/$d" ] || continue
  tag=$(echo "$d" | cut -d/ -f1)-$(basename "$d")
  extra=""
  [ "$tag" = mono-armeabi-v7a ] && extra=$(cd "$PKG" && ls $V/mono/Release/MonoLibs/armeabi-v7a/*.so 2>/dev/null | sed "s#^#$PKG/#" | tr '\n' ' ' || true)
  mkdir -p "$OUT/imports/$tag"
  # shellcheck disable=SC2086
  dev elf-imports "$OUT/imports/$tag" "$PKG/$V/$d"/*.so $extra > "$OUT/imports/$tag/summary.json"
done

U=$V/mono/Release/Libs/armeabi-v7a/libunity.so
S=$V/mono/Release/Symbols/armeabi-v7a/libunity.sym.so
if [ -f "$PKG/$S" ]; then
  dev jni-tables "$U" "$S" > "$OUT/jnitables.txt"
  dev jni-tables "$V/mono/Release/Libs/armeabi-v7a/libmain.so" >> "$OUT/jnitables.txt" || true
  dev strrefs "$U" "$S" '^\(|^\[?L?(android|java|javax|com|org|bitter|dalvik)/' > "$OUT/strrefs_jni.txt"
  dev jni-bindings "$OUT/strrefs_jni.txt" "$OUT/bindings.tsv" "$OUT/direct.txt"
  dev dis "$U" "$S" 'GetAvailableDllAssemblyMask' 1 > "$OUT/dis_monomanager.txt"
fi
dev strings "$U" '(\.so\b|\.obb|\.apk|\.dat\b|\.unity3d|\.resS?\b|assets/|bin/Data|global-metadata|il2cpp|^(android|java|com|org|dalvik|bitter)/|unity\.|/proc/|/sys/|/system/|/data/|AAudio|Choreographer|HardwareBuffer|Vulkan|vk[A-Z]|Swappy|GameActivity|GameSDK|build-id|splash|libc\+\+|AsyncUpload|UNITY_|^__)' > "$OUT/strings.txt"

A=$V/il2cpp/Release/Libs/arm64-v8a/libunity.so
[ -f "$PKG/$A" ] && dev strings "$A" '^il2cpp_[a-z0-9_]+$' --nul --sort > "$OUT/il2cpp_api.txt"

jar=$(cd "$PKG" && ls $V/mono/Release/Classes/classes.jar $V/il2cpp/Release/Classes/classes.jar 2>/dev/null | head -1 || true)
if [ -n "$jar" ]; then
  docker run --rm -v "$PKG":/p:ro -v "$OUT":/o "$JAVA" sh -c \
    "cd /tmp && classes=\$(jar tf /p/$jar | grep '\.class\$' | sed 's/\.class\$//; s#/#.#g' | sort) && \
     javap -p -cp /p/$jar \$classes > /o/javap.txt 2>&1; javap -p -s -cp /p/$jar \$classes > /o/javap_s.txt 2>&1"
fi
echo "notes in $OUT: $(du -sh "$OUT" | cut -f1)"
