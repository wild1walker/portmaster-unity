#!/bin/bash
# A stand-in for Westonpack's westonwrap.sh: <backend> <renderer> <shell> <gl> [VAR=value ...] <program> [args]
# runs the program on the test rig's X server with the VAR=value settings; "cleanup" does nothing.
[ "$1" = cleanup ] && exit 0
echo "westonwrap: $1 $2 $3 $4" >&2
shift 4
while [[ "${1:-}" == *=* ]]; do export "$1"; shift; done
exec "$@"
