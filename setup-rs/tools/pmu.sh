#!/bin/bash
# pmu.sh <pmu command> [args...]: pmu on the porter's computer (a Mac, or a PC with Docker), e.g.
#   setup-rs/tools/pmu.sh port package <port folder> --donors <dir> [--out <dir>]
#   setup-rs/tools/pmu.sh port new <name> --data <game folder> --out <port folder>
#   setup-rs/tools/pmu.sh port check <game folder>      (and port routes, route, arch-check, ...)
# The setup's pmu (static aarch64 Linux: tools/port/out/pmu/pmu, setup-rs/tools/build_pmu.sh, built
# again when setup-rs's sources are newer) runs in an arm64 container that sees this checkout, the
# current folder, every path in the arguments and the folders the variables below name, each at its
# own path. What pmu runs for a port's builds runs here, outside the container, as the Python
# packager ran it (crates/port host.rs: $PMU_HOST_EXEC): the runtime's and tools' build scripts
# (Docker), the port's build.commands (port.toml [build], e.g. a .NET or Docker build of the port's
# own code), asmpatch's build and survey (.NET 8 SDK). pmu's own output and theirs go to this terminal.
#   PMU_BIN      a pmu for linux-arm64 to run (default: tools/port/out/pmu/pmu, built when missing or older
#                than setup-rs's sources)
#   PMU_IMAGE    the container image (default ua-rust, setup-rs/docker; any linux/arm64 image runs the
#                static pmu)
#   PMU_MOUNTS   more folders the container sees (space-separated), e.g. a build.outputs path outside
#                the port folder
#   DONORS, PMU_DIR, PYBUNDLE, PMUNITY_MONO64_WORK, BOX64_OUT, NUGET_PACKAGES, PMUNITY_ASMPATCH, DOTNET,
#   OUT          passed to pmu (paths mounted); DOTNET defaults to this computer's dotnet when there is one
#   PMU_ENV      more variables passed (names, space-separated: pmu-dev.sh's)
# (pmu.sh --serve <dir> runs the request loop alone, until <dir>/stop: crates/port's tests.)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PM=$(cd "$HERE/../.." && pwd)

# the request loop: each <id>.run / <id>.cap run in the folder pmu started in, its status in <id>.rc
serve_once() {
  local x=$1 f b rc
  for f in "$x"/*.run "$x"/*.cap; do
    [ -e "$f" ] || continue
    b=${f%.*}
    mv "$f" "$b.sh" 2>/dev/null || continue
    rc=0
    case $f in
      *.run) /bin/sh "$b.sh" || rc=$? ;;
      *.cap) /bin/sh "$b.sh" >"$b.out" 2>"$b.err" || rc=$? ;;
    esac
    echo "$rc" > "$b.rc.tmp"; mv "$b.rc.tmp" "$b.rc"
  done
}
if [ "${1:-}" = --serve ]; then
  X=${2:?usage: pmu.sh --serve <dir>}
  until [ -e "$X/stop" ]; do serve_once "$X"; sleep 0.1; done
  exit 0
fi
[ $# -gt 0 ] || { echo "usage: pmu.sh <pmu command> [args...]" >&2; exit 2; }

IMG=${PMU_IMAGE:-ua-rust}
if [ -n "${PMU_BIN:-}" ]; then
  BIN=$(cd "$(dirname "$PMU_BIN")" && pwd)/$(basename "$PMU_BIN")
else
  BIN=$PM/tools/port/out/pmu/pmu
  if [ ! -x "$BIN" ] || [ -n "$(cd "$PM/setup-rs" && find Cargo.toml Cargo.lock pmu crates vendor templates -newer "$BIN" -type f \
       -not -path '*/target/*' -print -quit 2>/dev/null)" ]; then
    echo "== building pmu (setup-rs/tools/build_pmu.sh, Docker)" >&2
    "$HERE/build_pmu.sh" "$(dirname "$BIN")" >&2
  fi
fi
[ -x "$BIN" ] || { echo "pmu.sh: no pmu at $BIN" >&2; exit 1; }

# the folders the container sees: this checkout, the current folder, pmu, each path in the arguments
# (a file: its folder; a path not made yet: its folder when that is there) and in the variables
if [ -z "${DOTNET:-}" ]; then
  DOTNET=$(command -v dotnet || true)
  [ -n "$DOTNET" ] || { [ -x "$HOME/.dotnet/dotnet" ] && DOTNET=$HOME/.dotnet/dotnet; } || true
fi
[ -n "${NUGET_PACKAGES:-}" ] || { [ -d "$HOME/.nuget/packages" ] && NUGET_PACKAGES=$HOME/.nuget/packages; } || true
VARS="DONORS PMU_DIR PYBUNDLE PMUNITY_MONO64_WORK BOX64_OUT NUGET_PACKAGES OUT"
want=("$PM" "$PWD" "$(dirname "$BIN")")
addpath() {
  local p=$1
  case $p in /*) ;; *) p=$PWD/$p ;; esac
  if [ -d "$p" ]; then want+=("$(cd "$p" && pwd)")
  elif [ -e "$p" ] || [ -d "$(dirname "$p")" ]; then want+=("$(cd "$(dirname "$p")" && pwd)")
  fi
}
for a in "$@"; do case $a in -*|'') ;; *) addpath "$a" ;; esac; done
for v in $VARS; do [ -z "${!v:-}" ] || addpath "${!v}"; done
for m in ${PMU_MOUNTS:-}; do addpath "$m"; done
X=$(mktemp -d "${TMPDIR:-/tmp}/pmu-host.XXXXXX"); X=$(cd "$X" && pwd)
want+=("$X")
mounts=()
while IFS= read -r m; do
  [ -n "$m" ] && [ "$m" != / ] || continue
  for k in "${mounts[@]+"${mounts[@]}"}"; do case $m/ in "$k"/*) continue 2 ;; esac; done   # (inside one already seen)
  mounts+=("$m")
done < <(printf '%s\n' "${want[@]}" | sort -u)
# (TMPDIR in the shared folder: the files a command run here writes into pmu's temporary folders)
mkdir -p "$X/tmp"
args=(--rm --pull=never --platform linux/arm64 -w "$PWD" -e PMU_HOST_EXEC="$X" -e TMPDIR="$X/tmp" -e PMUNITY_REPO="$PM")
for m in "${mounts[@]}"; do args+=(-v "$m:$m"); done
for v in $VARS PMUNITY_ASMPATCH DOTNET ${PMU_ENV:-}; do [ -z "${!v:-}" ] || args+=(-e "$v=${!v}"); done
case $(uname -s) in Linux) args+=(--user "$(id -u):$(id -g)") ;; esac

docker run "${args[@]}" "$IMG" "$BIN" "$@" &
pid=$!
trap 'kill $pid 2>/dev/null || true; rm -rf "$X"' EXIT
while kill -0 $pid 2>/dev/null; do serve_once "$X"; sleep 0.1; done
rc=0; wait $pid || rc=$?
exit $rc
