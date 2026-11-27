#!/bin/bash
# package.sh [out dir]: the PortMaster release zip of this port (@@ZIP@@), built by portmaster-unity's
# `pmu port package` (setup-rs/tools/pmu.sh: pmu in Docker, the builds it needs run on this computer):
# the launcher and port folder, ua_host, the setup's tools (pmu), asmpatch when the port uses it,
# the donors, and their licences. No game files go in, and from Unity only the donors: small games
# of the port's own built with the game's Unity version (a free licence), whose APKs carry Unity's
# player, so the handheld never downloads it.
#   DONORS    the folder with the donor APKs the port needs (port.toml's player.donors; `port new` and
#             `port check` name them), built by portmaster-unity's tools/donor/build.sh
#   PMUNITY   a portmaster-unity checkout (default ../portmaster-unity), its runtime built (or Docker)
#   PMU_DIR   the setup's tools: pmu and licenses/ (default: built by setup-rs/tools/build_pmu.sh, Docker)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PM=$(cd "${PMUNITY:-$HERE/../portmaster-unity}" && pwd)
OUT=${1:-$HERE/out}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
exec "$PM/setup-rs/tools/pmu.sh" port package "$HERE" --out "$OUT" ${DONORS:+--donors "$DONORS"} \
  ${PMU_DIR:+--pmu "$PMU_DIR"}
