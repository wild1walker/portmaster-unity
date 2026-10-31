#!/bin/bash
# rs.sh <worktree> <command...>: runs a command in the arm64 Rust image (ua-rust: Rust 1.88, the
# aarch64 musl target, PortMaster's python_3.11 at /opt/py with the setup's packages at /opt/pylibs)
# in <worktree>/setup-rs (incremental builds off: their cache breaks on Docker's bind mounts), with a build folder of the worktree's own (RS_TARGET, default
# $RS_ROOT/target/<worktree's name>) and the shared crate cache ($RS_ROOT/cargo-registry).
# RS_ROOT (required) is a folder for both; RS_MOUNTS adds -v mounts (test data, read-only).
set -euo pipefail
wt=$(cd "$1" && pwd); shift
: "${RS_ROOT:?set RS_ROOT to a folder for the build output and the crate cache}"
tgt=${RS_TARGET:-$RS_ROOT/target/$(basename "$wt")}
mkdir -p "$tgt" "$RS_ROOT/cargo-registry"
mounts=(-v "$wt:$wt" -v "$tgt:$tgt" -v "$RS_ROOT/cargo-registry:/opt/cargo/registry")
for m in ${RS_MOUNTS:-}; do mounts+=(-v "$m"); done
exec docker run --rm --pull=never --platform linux/arm64 --memory "${RS_MEM:-1g}" "${mounts[@]}" \
  -e CARGO_TARGET_DIR="$tgt" -e CARGO_INCREMENTAL=0 -e CARGO_BUILD_JOBS="${RS_JOBS:-2}" -e PMU_TEST_DATA="${PMU_TEST_DATA:-}" \
  -w "$wt/setup-rs" ua-rust "$@"
