#!/bin/bash
# survey_player.sh PKG OUT
#   PKG: an unpacked Unity Android Build Support package (the directory holding Variations/, Data/)
#   OUT: where the notes go (text and JSON, a few MB at most)
# Runs the analysis helpers of tools/re on the Release players of that package:
#   facts.json          ELF facts of every Release library (elfimage.py)
#   imports/<var>/      undefined symbols per library + summary.json (elfimports.py)
#   jnitables.txt       RegisterNatives tables of the Mono armeabi-v7a libunity / libmain
#   strrefs_jni.txt     functions of that libunity referencing JNI names (strrefs.py, with symbols)
#   bindings.tsv        Java members libunity binds (jnibindings.py), direct.txt other JNI users
#   strings.txt         libunity strings about paths, libraries, Java classes, files
#   dis_monomanager.txt MonoManager::GetAvailableDllAssemblyMask (engine-module slot count)
#   il2cpp_api.txt      il2cpp_* functions the IL2CPP arm64-v8a libunity looks up in libil2cpp.so
#   javap.txt           every class and member of the player's classes.jar (javap -p; javap_s.txt
#                       with JNI descriptors)
# Docker images: $UA_PY_IMAGE (python3, pyelftools, capstone, c++filt; default ua-spec) and
# $UA_JAVA_IMAGE (javap; default ua-jni-build).
set -euo pipefail
PKG=$(cd "$1" && pwd); mkdir -p "$2"; OUT=$(cd "$2" && pwd)
RE=$(cd "$(dirname "$0")" && pwd)
PY=${UA_PY_IMAGE:-ua-spec}; JAVA=${UA_JAVA_IMAGE:-ua-jni-build}
run() { docker run --rm -i -v "$RE":/t:ro -v "$PKG":/p:ro -v "$OUT":/o "$PY" "$@"; }

V=Variations
libs=$(cd "$PKG" && ls $V/*/Release/Libs/{armeabi-v7a,arm64-v8a}/*.so $V/*/Release/MonoLibs/armeabi-v7a/*.so 2>/dev/null || true)
run sh -c "cd /p && python3 -I /t/elfimage.py $(echo $libs | tr '\n' ' ') > /o/facts.json"

for d in mono/Release/Libs/armeabi-v7a il2cpp/Release/Libs/armeabi-v7a il2cpp/Release/Libs/arm64-v8a; do
  [ -d "$PKG/$V/$d" ] || continue
  tag=$(echo "$d" | cut -d/ -f1)-$(basename "$d")
  extra=""
  [ "$tag" = mono-armeabi-v7a ] && extra=$(cd "$PKG" && ls $V/mono/Release/MonoLibs/armeabi-v7a/*.so 2>/dev/null | sed 's#^#/p/#' | tr '\n' ' ')
  run sh -c "mkdir -p /o/imports/$tag && python3 -I /t/elfimports.py /o/imports/$tag /p/$V/$d/*.so $extra > /o/imports/$tag/summary.json"
done

U=$V/mono/Release/Libs/armeabi-v7a/libunity.so
S=$V/mono/Release/Symbols/armeabi-v7a/libunity.sym.so
if [ -f "$PKG/$S" ]; then
  run sh -c "python3 -I /t/jnitables.py /p/$U /p/$S > /o/jnitables.txt; \
             python3 -I /t/jnitables.py /p/$V/mono/Release/Libs/armeabi-v7a/libmain.so >> /o/jnitables.txt || true"
  run sh -c "python3 -I /t/strrefs.py /p/$U /p/$S '^\(|^\[?L?(android|java|javax|com|org|bitter|dalvik)/' > /o/strrefs_jni.txt; \
             python3 -I /t/jnibindings.py /o/strrefs_jni.txt /o/bindings.tsv /o/direct.txt"
  run sh -c "python3 -I /t/dis.py /p/$U /p/$S 'GetAvailableDllAssemblyMask' 1 > /o/dis_monomanager.txt"
fi
run sh -c "python3 -I - /p/$U > /o/strings.txt" <<'PYEOF'
import sys, re
data = open(sys.argv[1], 'rb').read()
pat = re.compile(rb'[\x20-\x7e]{5,300}')
keep = re.compile(r'(\.so\b|\.obb|\.apk|\.dat\b|\.unity3d|\.resS?\b|assets/|bin/Data|global-metadata|il2cpp|'
                  r'^(android|java|com|org|dalvik|bitter)/|unity\.|/proc/|/sys/|/system/|/data/|AAudio|'
                  r'Choreographer|HardwareBuffer|Vulkan|vk[A-Z]|Swappy|GameActivity|GameSDK|'
                  r'build-id|splash|libc\+\+|AsyncUpload|UNITY_|^__)')
seen = set()
for m in pat.finditer(data):
    s = m.group().decode()
    if keep.search(s) and s not in seen:
        seen.add(s)
        print(s)
PYEOF

A=$V/il2cpp/Release/Libs/arm64-v8a/libunity.so
[ -f "$PKG/$A" ] && run sh -c "python3 -I - /p/$A > /o/il2cpp_api.txt" <<'PYEOF'
import sys, re
data = open(sys.argv[1], 'rb').read()
print('\n'.join(sorted(set(m.group().decode() for m in re.finditer(rb'(?<=\0)il2cpp_[a-z0-9_]+(?=\0)', data)))))
PYEOF

jar=$(cd "$PKG" && ls $V/mono/Release/Classes/classes.jar $V/il2cpp/Release/Classes/classes.jar 2>/dev/null | head -1 || true)
if [ -n "$jar" ]; then
  docker run --rm -v "$PKG":/p:ro -v "$OUT":/o "$JAVA" sh -c \
    "cd /tmp && classes=\$(jar tf /p/$jar | grep '\.class\$' | sed 's/\.class\$//; s#/#.#g' | sort) && \
     javap -p -cp /p/$jar \$classes > /o/javap.txt 2>&1; javap -p -s -cp /p/$jar \$classes > /o/javap_s.txt 2>&1"
fi
echo "notes in $OUT: $(du -sh "$OUT" | cut -f1)"
