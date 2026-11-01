#!/bin/bash
# package.sh [out dir]: the PortMaster release zip of this port (@@ZIP@@), built by portmaster-unity's
# `pmunity port package`: the launcher and port folder, ua_host, the setup's tools (pmu, or the
# converter and the setup's Python), asmpatch when the port uses it, and their licences. No game files
# and nothing of Unity go in.
#   PMUNITY   a portmaster-unity checkout (default ../portmaster-unity), its runtime built
#   PYTHON    a Python 3.11+ (default python3)
#   PYBUNDLE  the setup's Python (default: built by portmaster-unity's tools/port/build_pybundle.sh)
#   PMU_DIR   the setup's tools: pmu and licenses/ (default: built by setup-rs/tools/build_pmu.sh, Docker)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PM=$(cd "${PMUNITY:-$HERE/../portmaster-unity}" && pwd)
OUT=${1:-$HERE/out}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
cd "$PM/convert"
exec "${PYTHON:-python3}" -m pmunity port package "$HERE" --out "$OUT" ${PYBUNDLE:+--pybundle "$PYBUNDLE"}
