#!/bin/bash
# Reference for the ready-to-run zip : the Cuphead release's own setup/patchscript
# (steps 2-5) on the page's prepatched zip, as a handheld runs it, but in WSL: pmunity on the PC venv
# (the same package versions as the release's setup/pylibs) and the release's aarch64 codepatch under
# qemu (binfmt, libraries from the bookworm arm64 chroot). Output: $REF/cuphead, the
# finished ports/cuphead tree.
#   tests/refcup_full.sh <prepatched zip> [aspect resolution buttons]
set -e
ZIP=${1:?prepatched zip}
R=${REF:-$PWD/ref-cupfull}
VENV=${PMU_PY:?a Python with the setup's package versions (setup/pylibs, for the PC)}
rm -rf "$R"; mkdir -p "$R/py/bin"
"$VENV" - "$ZIP" "$R" <<'EOF'
import sys, zipfile, os
z = zipfile.ZipFile(sys.argv[1])
for i in z.infolist():
    if i.filename.startswith('cuphead/'):
        p = z.extract(i, sys.argv[2])
        m = i.external_attr >> 16
        if m and not i.is_dir(): os.chmod(p, m & 0o777)
EOF
G="$R/cuphead"; S="$G/setup"
# python_dir as patchscript expects it: bin/python3 = the venv's Python on the setup's own modules
# (setup/pylibs is aarch64: the venv has the same versions)
cat > "$R/py/bin/python3" <<EOF
#!/bin/sh
unset PYTHONHOME
export PYTHONPATH="$S"
exec "$VENV" "\$@"
EOF
chmod +x "$R/py/bin/python3"
# patchscript ends with a plain sync, which blocks forever on WSL's hung portal mount: sync -f instead
printf '#!/bin/sh\nexec /usr/bin/sync -f "%s"\n' "$R" > "$R/py/bin/sync"; chmod +x "$R/py/bin/sync"
export PATH="$R/py/bin:$PATH" python_dir="$R/py" GAMEDIR="$G" ESUDO= CPU_SYS=/nonexistent
export QEMU_LD_PREFIX=${QEMU_LD_PREFIX:?an arm64 root file system for qemu}
export ASPECT=${2:-fill} RESOLUTION=${3:-device} BUTTONS=${4:-nintendo} TEXTURE_QUALITY=fast
chmod +x "$S/patchscript" "$S/codepatch"
cd "$G"
/usr/bin/time -v bash "$S/patchscript" > "$R/run.log" 2>&1 || echo "patchscript failed: see $R/run.log"
grep -E "^== |PATCH_FAIL|Elapsed|Maximum resident" "$R/run.log"
ls -la "$G"
