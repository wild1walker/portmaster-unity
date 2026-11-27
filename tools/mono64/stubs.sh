#!/bin/sh
# stubs.sh <mono source folder>: empty stand-ins for the test folders configure.ac and the m4 files
# name, which the runtime-only source download leaves out (build-mono.sh, build-classlibs.sh): a
# Makefile.am for each Makefile, a <file>.in for each file configure writes from one. POSIX sh, grep, sed, awk.
set -eu
s=$1
names=$(cat "$s/configure.ac" "$s/acinclude.m4" "$s"/m4/*.m4 2>/dev/null |
  grep -oE '(^|[^A-Za-z0-9_])(mono/tests|acceptance-tests)[A-Za-z0-9_/.+-]*' | sed -E 's#^[^am]##' || true)
{ printf '%s\n' "$names"; for d in acceptance-tests mono/tests mono/tests/gc-descriptors mono/tests/testing_gac; do echo "$d/Makefile"; done; } |
  grep -v '^$' | LC_ALL=C sort -u | awk -v s="$s" '
  { all[NR] = $0; n = NR }
  END {
    for (i = 1; i <= n; i++) {
      x = all[i]; if (x ~ /\.in$/) x = substr(x, 1, length(x) - 3)
      p = s "/" x; base = x; sub(/.*\//, "", base); dir = p; sub(/\/[^\/]*$/, "", dir)
      if (base == "Makefile") {
        system("mkdir -p \"" dir "\" && { [ -e \"" p ".am\" ] || echo \"# left out of the runtime-only source download\" > \"" p ".am\"; }")
      } else {
        parent = 0
        for (j = 1; j <= n; j++) if (index(all[j], x "/") == 1) { parent = 1; break }
        if (!parent) system("mkdir -p \"" dir "\" && { [ -e \"" p ".in\" ] || echo > \"" p ".in\"; }")
      }
    }
  }'
