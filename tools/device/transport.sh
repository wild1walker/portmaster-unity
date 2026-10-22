# Sourced by the device tools (test.sh, drive.sh, push.sh): how this machine reaches the handheld.
#   DEVICE=user@host   over SSH (SSH_OPTS: more ssh options, e.g. a ControlMaster's ControlPath)
#   DEVICE=adb         over adb (USB; no network needed), the one device adb lists
#   DEVICE=adb:<serial> over adb, that device (ADB=<adb binary>, default adb)
# Knulli's adbd (and other Linux adbds) speak adb's first shell protocol: no stdin for a command,
# no exit status, and a pseudo-terminal (CR LF line ends, so binary output would be mangled). So
# over adb a command's exit status comes back as a marker line, and files go through adb push/pull
# (the sync protocol, binary-safe) instead of tar streams.
# Functions:
#   dev_sh <command>          runs a shell command there; its output; returns its exit status
#   dev_tar_in <dir>          unpacks the tar archive on stdin into <dir> there
#   dev_tar_out <dir> <files> writes a tar archive of <files> (globs, in <dir>) to stdout
#   dev_get <remote> <local>  copies one file back
#   dev_put <local> <remote>  copies one file there
#   dev_name                  the device's name for messages
# shellcheck disable=SC2206
case "${DEVICE:-}" in
  adb) ADBS=(${ADB:-adb}) ;;
  adb:*) ADBS=(${ADB:-adb} -s "${DEVICE#adb:}") ;;
  *) ADBS=(); OPTS=(${SSH_OPTS:-}) ;;
esac
dev_name() { if [ ${#ADBS[@]} -gt 0 ]; then echo "adb${DEVICE#adb}"; else echo "$DEVICE"; fi; }
# a path there as adb push/pull want it: relative ones are in the login's home, as over SSH
_dev_home=
_dev_abs() {
  case "$1" in /*) echo "$1"; return;; esac
  [ -n "$_dev_home" ] || _dev_home=$("${ADBS[@]}" shell 'echo "$HOME"' 2>/dev/null | tr -d '\r')
  echo "${_dev_home:-/}/$1"
}

dev_sh() {
  if [ ${#ADBS[@]} -eq 0 ]; then ssh -n ${OPTS[@]+"${OPTS[@]}"} "$DEVICE" "$@"; return; fi
  local out rc
  # (cd ~: an adb shell starts in /, an SSH login in its home; a subshell, so the command's own
  # exit still leaves the marker line)
  out=$("${ADBS[@]}" shell 'cd ~ 2>/dev/null; ( '"$*"'
); echo "__uarc=$?"' 2>/dev/null | tr -d '\r') || return 255
  rc=$(printf '%s\n' "$out" | sed -n 's/^__uarc=\([0-9]*\)$/\1/p' | tail -1)
  printf '%s\n' "$out" | sed '/^__uarc=[0-9]*$/d'
  [ -n "$rc" ] || return 255                              # (no marker: the connection broke)
  return "$rc"
}

dev_tar_in() {
  if [ ${#ADBS[@]} -eq 0 ]; then ssh ${OPTS[@]+"${OPTS[@]}"} "$DEVICE" "cd $(printf '%q' "$1") && tar -xof -"; return; fi
  local local_tar remote_tar rc
  local_tar=$(mktemp "${TMPDIR:-/tmp}/uatransfer.XXXXXX")
  cat > "$local_tar"
  remote_tar=$(_dev_abs "$1/.uatransfer.$$.tar")           # (beside the files: the card, not RAM)
  "${ADBS[@]}" push "$local_tar" "$remote_tar" >/dev/null 2>&1 || { rm -f "$local_tar"; return 1; }
  rm -f "$local_tar"
  dev_sh "cd $(printf '%q' "$1") && tar -xof $(printf '%q' "$remote_tar"); rc=\$?; rm -f $(printf '%q' "$remote_tar"); exit \$rc" >/dev/null
}

dev_tar_out() {
  local dir=$1; shift
  if [ ${#ADBS[@]} -eq 0 ]; then ssh -n ${OPTS[@]+"${OPTS[@]}"} "$DEVICE" "cd $(printf '%q' "$dir") && tar -cf - \$(ls $* 2>/dev/null)"; return; fi
  local remote_tar local_tar rc
  remote_tar=$(_dev_abs "$dir/.uatransfer.$$.tar")
  dev_sh "cd $(printf '%q' "$dir") && tar -cf $(printf '%q' "$remote_tar") \$(ls $* 2>/dev/null)" >/dev/null || return 1
  local_tar=$(mktemp "${TMPDIR:-/tmp}/uatransfer.XXXXXX")
  "${ADBS[@]}" pull "$remote_tar" "$local_tar" >/dev/null 2>&1; rc=$?
  dev_sh "rm -f $(printf '%q' "$remote_tar")" >/dev/null
  [ $rc = 0 ] && cat "$local_tar"
  rm -f "$local_tar"
  return $rc
}

dev_get() {
  if [ ${#ADBS[@]} -eq 0 ]; then ssh -n ${OPTS[@]+"${OPTS[@]}"} "$DEVICE" "cat $(printf '%q' "$1")" > "$2"; return; fi
  "${ADBS[@]}" pull "$(_dev_abs "$1")" "$2" >/dev/null 2>&1
}

dev_put() {
  if [ ${#ADBS[@]} -eq 0 ]; then ssh ${OPTS[@]+"${OPTS[@]}"} "$DEVICE" "cat > $(printf '%q' "$2")" < "$1"; return; fi
  "${ADBS[@]}" push "$1" "$(_dev_abs "$2")" >/dev/null 2>&1
}
