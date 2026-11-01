#!/bin/bash
# rs.sh <worktree> <command...>: runs a command in the arm64 Rust image (ua-rust: Rust 1.88, the
# aarch64 musl target, PortMaster's python_3.11 at /opt/py with the setup's packages at /opt/pylibs)
# in <worktree>/setup-rs. Builds go to /target/<worktree's name> in a Docker volume (RS_TARGET_VOLUME,
# default pmu-target) and the crate cache to another (RS_CARGO_VOLUME, default pmu-cargo): Docker's
# bind mounts of the host's disks are slow, and broke incremental builds (which stay off).
# RS_ROOT (required) is the folder of the shared setup (env.sh); RS_MOUNTS adds -v mounts (test data,
# scratch: volumes or read-only host folders).
set -euo pipefail
wt=$(cd "$1" && pwd); shift
: "${RS_ROOT:?set RS_ROOT (source the shared env.sh)}"
tgt=/target/$(basename "$wt")
mounts=(-v "$wt:$wt" -v "${RS_TARGET_VOLUME:-pmu-target}:/target" -v "${RS_CARGO_VOLUME:-pmu-cargo}:/opt/cargo/registry")
for m in ${RS_MOUNTS:-}; do mounts+=(-v "$m"); done
exec docker run --rm --pull=never --platform linux/arm64 --memory "${RS_MEM:-1g}" "${mounts[@]}" \
  -e CARGO_TARGET_DIR="$tgt" -e CARGO_INCREMENTAL=0 -e CARGO_BUILD_JOBS="${RS_JOBS:-2}" -e PMU_TEST_DATA="${PMU_TEST_DATA:-}" \
  -e PMU_SCRATCH="${PMU_SCRATCH:-}" -w "$wt/setup-rs" ua-rust "$@"
