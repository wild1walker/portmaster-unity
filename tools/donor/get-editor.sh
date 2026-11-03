#!/bin/bash
# get-editor.sh <version> <dir>: Unity's macOS editor of exactly that version with its Android Build
# Support, from Unity's download archive, unpacked into <dir> without installing anything (pkgutil
# --expand-full: no administrator password, no Unity Hub, no sign-in), for build.sh:
#   <dir>/Unity.app                       build.sh's <editor>
#   <dir>/PlaybackEngines/AndroidPlayer   Android Build Support (where Unity.app looks for it)
# The two installers come from Unity's release API (the version's changeset and its macOS downloads,
# as `pmunity player url` finds the Android one), downloaded into <dir>/.dl (a stopped download
# continues) and removed once unpacked unless KEEP_PKGS=1. PKGS=<folder> takes them from that folder
# instead (Unity-<version>.pkg and the Android one, any names), nothing downloaded.
#   ARCH       x86_64 (default: the Intel editor, which Apple silicon runs under Rosetta) or arm64
#              (Apple silicon editors exist from 2021.2)
#   CHANGESET  the version's changeset (the hash in Unity's download links) when the release API
#              does not list the version
# The editor still needs a licence to build (Unity Personal is enough): Unity_lic.ulf in
# /Library/Application Support/Unity, made once by the Unity Hub or Unity's manual activation. This
# script never signs in, and the donor build only reads the licence that is there.
# The porter's Mac downloads editors; the handheld never downloads anything (docs/porting.md, "Donors").
set -euo pipefail
V=${1:?usage: get-editor.sh <version> <dir>}; OUT=${2:?usage: get-editor.sh <version> <dir>}
[[ $V =~ ^[0-9]{4}\.[0-9]+\.[0-9]+[abfp][0-9]+$ ]] || { echo "not a Unity version: $V" >&2; exit 2; }
[ "$(uname)" = Darwin ] || { echo "get-editor.sh unpacks macOS installers: run it on a Mac" >&2; exit 2; }
case "${ARCH:-x86_64}" in x86_64|X86_64) A=X86_64;; arm64|ARM64) A=ARM64;; *) echo "ARCH: x86_64 or arm64" >&2; exit 2;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
[ -e "$OUT/Unity.app" ] && { echo "$OUT/Unity.app is there already (remove it to unpack again)" >&2; exit 1; }
log() { echo "== $*"; }

# ---- the two installers
if [ -n "${PKGS:-}" ]; then
  E=; AN=
  for f in "$PKGS"/*.pkg; do
    [ -f "$f" ] || continue
    if xar -tf "$f" | grep -q '^TargetSupport\.pkg'; then AN=$f; else E=$f; fi
  done
  [ -n "$E" ] && [ -n "$AN" ] || { echo "$PKGS: no editor and Android Build Support .pkg in it" >&2; exit 1; }
else
  DL="$OUT/.dl"; mkdir -p "$DL"
  api="https://services.api.unity.com/unity/editor/release/v1/releases?version=$V&limit=1"
  urls=
  if curl -fsSL --retry 3 -o "$DL/release.json" "$api"; then
    # (JSON read with macOS's own JavaScript: the editor's and its Android module's .pkg for ARCH)
    urls=$(osascript -l JavaScript -e '
      function run(a) {
        var d = JSON.parse($.NSString.stringWithContentsOfFileEncodingError(a[0], $.NSUTF8StringEncoding, null).js);
        var r = (d.results || []).filter(function (x) { return x.version === a[1]; })[0];
        if (!r) return "";
        var dl = (r.downloads || []).filter(function (x) {
          return x.platform === "MAC_OS" && (x.architecture || "X86_64") === a[2] && /\.pkg$/.test(x.url || ""); })[0];
        if (!dl) return "";
        var m = (dl.modules || []).filter(function (x) { return x.id === "android" && /\.pkg$/.test(x.url || ""); })[0];
        return dl.url + "\n" + (m ? m.url : "");
      }' "$DL/release.json" "$V" "$A" 2>/dev/null || true)
  fi
  E_URL=$(echo "$urls" | sed -n 1p); A_URL=$(echo "$urls" | sed -n 2p)
  if [ -z "$E_URL" ] || [ -z "$A_URL" ]; then
    [ -n "${CHANGESET:-}" ] || { echo "Unity's release API does not list $V for macOS $A: set CHANGESET (the hash in its download links)" >&2; exit 1; }
    [ "$A" = X86_64 ] || { echo "with CHANGESET only the x86_64 editor's links are known" >&2; exit 1; }
    base="https://download.unity3d.com/download_unity/$CHANGESET"
    E_URL="$base/MacEditorInstaller/Unity-$V.pkg"
    A_URL="$base/MacEditorTargetInstaller/UnitySetup-Android-Support-for-Editor-$V.pkg"
  fi
  E="$DL/$(basename "$E_URL")"; AN="$DL/$(basename "$A_URL")"
  for u in "$E_URL" "$A_URL"; do
    log "downloading $u"
    curl -fL --retry 3 -C - -o "$DL/$(basename "$u")" "$u"
  done
fi

# ---- unpacked, not installed: each component's payload where its install location puts it, with
# /Applications/Unity (the editor's folder) as <dir>
X="$OUT/.x"; rm -rf "$X"; mkdir -p "$X"
n=0
for pkg in "$E" "$AN"; do
  n=$((n + 1))
  log "unpacking $(basename "$pkg")"
  pkgutil --expand-full "$pkg" "$X/$n"
  for pi in "$X/$n"/*/PackageInfo; do
    c=$(dirname "$pi")
    [ -d "$c/Payload" ] || continue
    loc=$(sed -n 's/.*install-location="\([^"]*\)".*/\1/p' "$pi" | head -1)
    case "$loc" in
      /Applications) src="$c/Payload/Unity"; dst="$OUT";;                     # (Payload/Unity/Unity.app)
      /Applications/Unity) src="$c/Payload"; dst="$OUT";;
      /Applications/Unity/*) src="$c/Payload"; dst="$OUT/${loc#/Applications/Unity/}";;
      *) echo "$(basename "$pkg"): unexpected install location $loc" >&2; exit 1;;
    esac
    [ -d "$src" ] || { echo "$(basename "$pkg"): no $src" >&2; exit 1; }
    mkdir -p "$dst"
    ditto "$src" "$dst"
  done
  rm -rf "${X:?}/$n"
done
rm -rf "$X"
[ -x "$OUT/Unity.app/Contents/MacOS/Unity" ] || { echo "no Unity.app/Contents/MacOS/Unity in $OUT" >&2; exit 1; }
[ -d "$OUT/PlaybackEngines/AndroidPlayer" ] || { echo "no PlaybackEngines/AndroidPlayer in $OUT" >&2; exit 1; }
got=$(defaults read "$OUT/Unity.app/Contents/Info" CFBundleVersion 2>/dev/null || true)
case "$got" in "$V"*) ;; *) echo "warning: Unity.app says version '$got', not $V" >&2;; esac

# ---- before the first launch: macOS refuses the unpacked bundle as it is (a symlink outside it)
rm -f "$OUT/Unity.app/Contents/Documentation"
codesign --force --sign - "$OUT/Unity.app"
codesign --verify --deep --strict "$OUT/Unity.app"

[ "${KEEP_PKGS:-0}" = 1 ] || [ -n "${PKGS:-}" ] || rm -rf "$OUT/.dl"
log "Unity $V with Android Build Support: $OUT/Unity.app (build.sh's <editor>)"
