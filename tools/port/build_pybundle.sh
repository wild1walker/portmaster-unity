#!/bin/bash
# build_pybundle.sh <out.tar.gz> [cache dir]: the Python a port's on-device setup runs (aarch64 Linux),
# for `pmunity port package` (ports made with `pmunity port new`; the same bundle portmaster-cuphead ships):
# python-build-standalone's CPython 3.12 with portmaster-unity's converter dependencies, trimmed,
# without symbolic links (exFAT cards cannot hold them).
#
# Downloads into the cache dir what is not there yet (default tools/port/out/cache/):
#   cpython-3.12.15-aarch64-linux-gnu.tar.gz   python-build-standalone 20261013, install_only_stripped
#   wheels/*.whl                               manylinux aarch64 wheels from PyPI (pip download)
#   wheels/unitypy-1.25.4-*.whl                built from source in a manylinux_2_28_aarch64 container
#                                              (PyPI has no aarch64 wheel of UnityPy 1.25.4)
# Needs: curl, a Python 3 with pip, Docker (for the UnityPy wheel only).
set -euo pipefail
OUT=${1:?usage: build_pybundle.sh <out.tar.gz> [cache dir]}
HERE=$(cd "$(dirname "$0")" && pwd)
C=${2:-$HERE/out/cache}; mkdir -p "$C/wheels"; C=$(cd "$C" && pwd)
PBS="$C/cpython-3.12.15-aarch64-linux-gnu.tar.gz"
PBS_URL="https://github.com/astral-sh/python-build-standalone/releases/download/20261013/cpython-3.12.15%2B20261013-aarch64-unknown-linux-gnu-install_only_stripped.tar.gz"
PBS_SHA256=9467a99bacc7d90b64b02de7fa9ecfc99b72905e64e74f7ff6b460ad95cfed5a
# UnityPy and what it imports, at the versions the converter is tested with
WHEELS="lz4==4.4.5 astc_encoder_py==0.1.12 pillow==12.3.0 texture2ddecoder==1.0.6 etcpak==0.9.15 brotli==1.2.0
        archspec==0.2.6 attrs==26.1.0 fsspec==2026.9.0 tpk_ar==0.2.4 pyfmodex==0.7.2"
sha() { [ -f "$1" ] || return 0; if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }

[ "$(sha "$PBS")" = "$PBS_SHA256" ] || { curl -fL -o "$PBS" "$PBS_URL"; [ "$(sha "$PBS")" = "$PBS_SHA256" ] || { echo "python-build-standalone: checksum mismatch"; exit 1; }; }
for w in $WHEELS; do
  n=${w%%==*}; v=${w##*==}
  ls "$C/wheels/" | grep -qi "^${n//-/_}-$v-" || \
    python3 -m pip download --no-deps --only-binary=:all: --platform manylinux_2_28_aarch64 --python-version 3.12 \
      --implementation cp -d "$C/wheels" "$w"
done
if ! ls "$C/wheels/" | grep -q '^unitypy-1.25.4-'; then
  docker run --rm --platform linux/arm64 -v "$C/wheels":/out quay.io/pypa/manylinux_2_28_aarch64 bash -c '
    /opt/python/cp312-cp312/bin/pip wheel --no-deps -w /tmp/w UnityPy==1.25.4 && auditwheel repair -w /out /tmp/w/unitypy-*.whl'
fi

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
tar -xzf "$PBS" -C "$T"
SP="$T/python/lib/python3.12/site-packages"
# wheels are zip files: unpacked as pip would (pure files, no scripts needed)
for w in "$C"/wheels/*.whl; do
  case "$(basename "$w")" in numpy-*) continue;; esac           # (not needed by the converter)
  python3 -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$w" "$SP"
done
# UnityPy imports fmod_toolkit for audio export, which needs FMOD (not redistributable): a stand-in
rm -rf "$SP/fmod_toolkit"*; mkdir -p "$SP/fmod_toolkit"
cat > "$SP/fmod_toolkit/__init__.py" <<'PY'
# Stand-in for fmod_toolkit (FMOD audio export), which UnityPy imports at start-up. The converter
# never exports audio, and FMOD's library may not be redistributed, so it is left out.
def raw_to_wav(*args, **kwargs):
    raise RuntimeError("audio export is not available in this build")
PY
# trimmed: tests, IDLE, tkinter, pip, the shared libpython (the executable is static), headers
L="$T/python/lib/python3.12"
rm -rf "$L/test" "$L/idlelib" "$L/tkinter" "$L/turtledemo" "$L/ensurepip" "$SP/pip" "$SP/pip-"* "$T/python/include" \
       "$T/python/share" "$T/python/lib/libpython3.12.so"* "$T/python/lib/pkgconfig" "$T/python/lib/tcl"* "$T/python/lib/tk"* \
       "$T/python/lib/itcl"* "$T/python/lib/thread"*
find "$T/python" -name __pycache__ -type d -prune -exec rm -rf {} +
# no symbolic links: python3 / python -> python3.12 and the like are removed (the setup calls python3.12)
find "$T/python" -type l -delete
# written with Python's tarfile: no platform extras (macOS tar adds extended attributes)
python3 -c "
import sys, tarfile
with tarfile.open(sys.argv[1], 'w:gz', format=tarfile.USTAR_FORMAT) as t:
    t.add(sys.argv[2], 'python', filter=lambda i: (setattr(i, 'uid', 0), setattr(i, 'gid', 0), setattr(i, 'uname', ''), setattr(i, 'gname', ''), i)[-1])
" "$OUT.tmp" "$T/python" && mv "$OUT.tmp" "$OUT"
echo "built $OUT ($(du -h "$OUT" | cut -f1))"
