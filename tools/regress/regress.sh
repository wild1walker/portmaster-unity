#!/bin/bash
# regress.sh <level> [options]: the regression run (`pmu-dev regress`, crates/devtools/src/regress) on
# this computer, through setup-rs/tools/pmu-dev.sh: in setup-rs's arm64 container, which sees this
# checkout and every absolute path of the local config (local.toml, or $REGRESS_CONFIG), and runs
# what needs the computer (the emulated rig's Docker, asmpatch's .NET) outside it. README.md.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PM=$(cd "$HERE/../.." && pwd)
CFG=${REGRESS_CONFIG:-$HERE/local.toml}
mounts="$HERE"
if [ -f "$CFG" ]; then
  mounts="$mounts $(dirname "$CFG") $(sed -e 's/#.*//' "$CFG" | grep -oE '"/[^"]+"' | tr -d '"' | tr '\n' ' ')"
fi
for v in $(env | sed -n 's/^\(REGRESS_[A-Z0-9_]*\)=.*/\1/p'); do
  case ${!v} in /*) mounts="$mounts ${!v//:/ }" ;; esac
done
# the setup's pmu, built when missing or older than setup-rs's sources (pmu.sh's rule)
BIN=$PM/tools/port/out/pmu/pmu
if [ ! -x "$BIN" ] || [ -n "$(cd "$PM/setup-rs" && find Cargo.toml Cargo.lock pmu crates vendor templates -newer "$BIN" -type f \
     -not -path '*/target/*' -print -quit 2>/dev/null)" ]; then
  echo "== building pmu (setup-rs/tools/build_pmu.sh, Docker)" >&2
  "$PM/setup-rs/tools/build_pmu.sh" "$(dirname "$BIN")" >&2
fi
names=$(env | sed -n 's/^\(REGRESS_[A-Z0-9_]*\)=.*/\1/p' | tr '\n' ' ')
REGRESS_HERE=$HERE PMU_ENV="REGRESS_HERE ${names}PMHLSLCC PMSPIRVCROSS" PMU_MOUNTS="${PMU_MOUNTS:-} $mounts" \
  exec "$PM/setup-rs/tools/pmu-dev.sh" regress "$@"
