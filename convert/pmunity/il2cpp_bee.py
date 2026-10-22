"""Unity 2021.2+'s il2cpp builds the generated C++ with Bee, Unity's build system: il2cpp's build
program (in il2cpp.dll, run in-process) writes the build's graph, `<cachedirectory>/buildstate/
bee.dag.json`, and Unity's `bee_backend`, a native program, runs the graph's commands. The
editor installers carry the backend for their own system only (the Windows one has
`il2cpp/build/deploy/bee_backend/win-x64/bee_backend.exe`); il2cpp on Linux looks for
`bee_backend/linux-x64/bee_backend`. So, as for the NDK's x86-64 compilers (ndk-clang.sh), pmunity
puts a stand-in there (install):

  - called without --dagfilejson (no graph yet), it exits 4: Bee's "run the build program", which
    then writes the graph and calls the backend again with --dagfilejson;
  - called with it, it notes its arguments (the target) in `pmunity-backend.args` and exits 0, so
    il2cpp ends with "Build succeeded with 0 successful nodes";
  - pmunity then runs the graph itself (run_graph): the nodes the target needs, in dependency
    order, the compiles in parallel in the same container (or, with --on-device, on the
    handheld), the response and lump files and the copies written directly.

The graph's node kinds (Bee.TundraBackend): RunShellCommand (`Action`, run with /bin/sh -c in the
graph's folder with its `Env`), WriteTextFile (`WriteTextFilePayload` -> the output) and CopyFiles
(`Inputs[i]` -> `Outputs[i]`). `ToBuildDependencies` must be built before a node; a dependency's
`ToUseDependencies` are needed by whoever uses it, so they are built first too. A node done in an
earlier run (`pmunity-done.jsonl`: its command, environment and inputs' sizes and times) is not
run again, so an interrupted build continues where it stopped, as with il2cpp's own object cache
in 2017.4 - 2020.3.
"""
import concurrent.futures
import hashlib
import json
import os
import shutil
import threading
import time

TARGET = 'FinalProgram'          # what il2cpp's BeeDriverRunner builds
ARGS_FILE = 'pmunity-backend.args'
DONE_FILE = 'pmunity-done.jsonl'

# The stand-in for bee_backend (POSIX sh: the container's, a handheld's /bin/sh). Bee starts it as
#   bee_backend --profile=<f> --stdin-canary --dagfile=<state>/bee.dag [--continue-on-failure]
#               [--dagfilejson=<state>/bee.dag.json] <target>
# with the build state folder as its working directory. Exit 4 asks Bee for the build program.
STUB = r'''#!/bin/sh
# pmunity (portmaster-unity, MIT): stands in for Unity's bee_backend, which the Windows editor
# installer has only as a Windows program. It lets il2cpp's build program write the build graph
# (bee.dag.json); pmunity then runs the graph's commands itself (convert/pmunity/il2cpp_bee.py).
for a do
  case $a in
    --dagfilejson=*)
      printf '%s\n' "$@" > pmunity-backend.args
      exit 0 ;;
  esac
done
exit 4
'''
# il2cpp passes the build program how to start il2cpp itself: deploy/il2cpp next to il2cpp.dll
# (the editor's apphost) if that exists, else the .NET of il2cpp-deps (a folder the editor
# installers do not have: "Run build.pl to populate il2cpp-deps")
LAUNCHER = r'''#!/bin/sh
# pmunity: il2cpp.dll on the .NET in PATH (what il2cpp hands its build program as the way to run it)
exec dotnet exec "$(dirname "$0")/il2cpp.dll" "$@"
'''


def deploy_dir(editor):
    return os.path.join(editor, 'il2cpp', 'build', 'deploy')


def uses_bee(editor):
    """Whether this il2cpp builds with Bee and its native backend (Unity 2021.2+)."""
    return os.path.isdir(os.path.join(deploy_dir(editor), 'bee_backend'))


def _put(path, text):
    try:
        with open(path) as f:
            if f.read() == text and os.access(path, os.X_OK):
                return
    except OSError:
        pass
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + '.part', 'w') as f:
        f.write(text)
    os.chmod(path + '.part', 0o755)
    os.replace(path + '.part', path)


def install(editor):
    """The backend stand-in and the il2cpp launcher in a fetched Editor/Data (idempotent);
    False when this il2cpp does not use Bee."""
    if not uses_bee(editor):
        return False
    d = deploy_dir(editor)
    for arch in ('linux-x64', 'linux-arm64'):     # (Bee.Core: Linux is always "x64"; arm64 in case)
        _put(os.path.join(d, 'bee_backend', arch, 'bee_backend'), STUB)
    if not os.path.exists(os.path.join(d, 'il2cpp')):
        _put(os.path.join(d, 'il2cpp'), LAUNCHER)
    return True


def graph_path(cache):
    return os.path.join(cache, 'buildstate', 'bee.dag.json')


def target_of(dag):
    """The target the backend was asked to build (its last argument), else TARGET."""
    try:
        with open(os.path.join(os.path.dirname(dag), ARGS_FILE)) as f:
            args = [a for a in f.read().splitlines() if a]
        if args and not args[-1].startswith('-'):
            return args[-1]
    except OSError:
        pass
    return TARGET


class Graph:
    def __init__(self, path):
        with open(path, encoding='utf-8') as f:
            d = json.load(f)
        self.path = path
        self.nodes = d['Nodes']
        self.named = d.get('NamedNodes') or {}
        self.root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(path)), d.get('RelativePathToRoot') or '.'))

    def index(self, target):
        if target in self.named:
            return self.named[target]
        for i, n in enumerate(self.nodes):
            if n.get('Annotation') == target:
                return i
        raise SystemExit(f'{self.path}: no node "{target}" in the build graph')

    def deps(self, i):
        return self.nodes[i].get('ToBuildDependencies') or []

    def use_deps(self, i):
        return self.nodes[i].get('ToUseDependencies') or []

    def needed(self, target):
        """The nodes building `target` takes: its dependencies of both kinds, transitively."""
        seen, todo = set(), [self.index(target)]
        while todo:
            i = todo.pop()
            if i in seen:
                continue
            seen.add(i)
            todo += self.deps(i) + self.use_deps(i)
        return seen

    def prerequisites(self, i):
        """What must be done before node i runs: its ToBuild dependencies and what those need
        when used (their ToUse dependencies, transitively)."""
        out, todo = set(), list(self.deps(i))
        while todo:
            j = todo.pop()
            if j in out:
                continue
            out.add(j)
            todo += self.use_deps(j)
        out.discard(i)
        return out

    def path_of(self, p):
        return p if os.path.isabs(p) else os.path.join(self.root, p)


def kind(node):
    return node.get('ActionType') or 'RunShellCommand'


def env_of(node):
    return {e['Key']: e['Value'] for e in node.get('Env') or [] if 'Key' in e}


def signature(g, node):
    h = hashlib.sha256(json.dumps([kind(node), node.get('Action'), node.get('WriteTextFilePayload'),
                                   node.get('Inputs'), node.get('Outputs'), env_of(node)]).encode())
    for p in node.get('Inputs') or []:
        try:
            st = os.stat(g.path_of(p))
            h.update(f'{p}\0{st.st_size}\0{st.st_mtime_ns}\0'.encode())
        except OSError:
            h.update(f'{p}\0-\0'.encode())
    return h.hexdigest()


def _load_done(path):
    done = {}
    try:
        with open(path) as f:
            for line in f:
                try:
                    k, v = json.loads(line)
                    done[k] = v
                except ValueError:
                    pass                    # (a line cut short by a kill)
    except OSError:
        pass
    return done


def _key(node):
    outs = node.get('Outputs') or []
    return outs[0] if outs else node.get('Annotation', '')


def write_text(g, node):
    dst = g.path_of(node['Outputs'][0])
    text = node.get('WriteTextFilePayload') or ''
    try:
        with open(dst, encoding='utf-8', newline='') as f:
            if f.read() == text:
                return                      # (unchanged: its time stays, nothing using it reruns)
    except OSError:
        pass
    os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
    with open(dst, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def copy_files(g, node):
    for s, d in zip(node.get('Inputs') or [], node.get('Outputs') or []):
        s, d = g.path_of(s), g.path_of(d)
        try:
            a, b = os.stat(s), os.stat(d)
            if a.st_size == b.st_size and int(a.st_mtime) == int(b.st_mtime):
                continue
        except OSError:
            pass
        os.makedirs(os.path.dirname(d) or '.', exist_ok=True)
        if os.path.lexists(d):
            os.unlink(d)
        shutil.copy2(s, d)


def prepare(g, node):
    """What the backend does before a command: output folders made, outputs removed when the node
    asks for that (OverwriteOutputs false), its target directories made."""
    for p in node.get('Outputs') or []:
        p = g.path_of(p)
        os.makedirs(os.path.dirname(p) or '.', exist_ok=True)
        if node.get('OverwriteOutputs') is False and os.path.lexists(p):
            os.unlink(p)
    for p in node.get('TargetDirectories') or []:
        os.makedirs(g.path_of(p), exist_ok=True)


def run_graph(dag, shell, jobs, log, logfile, target=None, progress=None, every=30):
    """Runs what `target` (default: what the backend was asked for) needs from the graph at `dag`.
    shell(action, cwd, env) -> (exit code, output) runs one command (it is called from up to
    `jobs` threads at once). progress(done, total, seconds) -> a line to log every `every` s.
    Returns (commands run, commands done before)."""
    g = Graph(dag)
    target = target or target_of(dag)
    need = g.needed(target)
    pre = {i: g.prerequisites(i) & need for i in need}
    users = {i: [] for i in need}
    for i, ps in pre.items():
        for p in ps:
            users[p].append(i)
    left = {i: len(ps) for i, ps in pre.items()}
    donefile = os.path.join(os.path.dirname(dag), DONE_FILE)
    done = _load_done(donefile)
    shells = sum(1 for i in need if kind(g.nodes[i]) == 'RunShellCommand' and g.nodes[i].get('Action'))
    lock = threading.Lock()
    counts = {'ran': 0, 'before': 0, 'finished': 0}
    failed = []
    t0 = time.time()
    last = [t0]
    lf = open(logfile, 'a', encoding='utf-8')
    df = open(donefile, 'a')
    lf.write(f'graph {dag}: target {target}, {len(need)} nodes, {shells} commands\n')

    def finished_shell(i, sig, rc, out):
        n = g.nodes[i]
        with lock:
            counts['finished'] += 1
            if out.strip() or rc:
                lf.write(f'[{n.get("Annotation")}] exit {rc}\n{out}' + ('' if out.endswith('\n') else '\n'))
                lf.flush()
            if rc == 0:
                missing = [p for p in n.get('Outputs') or [] if not os.path.exists(g.path_of(p))]
                if missing and not n.get('AllowUnwrittenOutputFiles'):
                    failed.append((n, rc, out + f'\n(did not write {", ".join(missing)})'))
                    return False
                counts['ran'] += 1
                df.write(json.dumps([_key(n), sig]) + '\n')
                df.flush()
                return True
            failed.append((n, rc, out))
            return False

    def run_shell(i, sig):
        n = g.nodes[i]
        rc, out = shell(n['Action'], g.root, env_of(n))
        return i, finished_shell(i, sig, rc, out)

    ready = [i for i, c in left.items() if c == 0]
    running = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        def complete(i):
            for u in users[i]:
                left[u] -= 1
                if left[u] == 0:
                    ready.append(u)

        while ready or running:
            while ready and not failed and len(running) < max(1, jobs):
                i = ready.pop()
                n = g.nodes[i]
                k = kind(n)
                if k == 'WriteTextFile':
                    write_text(g, n)
                    complete(i)
                elif k == 'CopyFiles':
                    copy_files(g, n)
                    complete(i)
                elif not n.get('Action'):
                    complete(i)                          # (a named group: all_tundra_nodes, FinalProgram)
                else:
                    sig = signature(g, n)
                    if done.get(_key(n)) == sig and all(os.path.exists(g.path_of(p)) for p in n.get('Outputs') or []):
                        counts['before'] += 1
                        complete(i)
                        continue
                    prepare(g, n)
                    running[pool.submit(run_shell, i, sig)] = i
            if not running:
                break
            fin, _ = concurrent.futures.wait(list(running), timeout=2, return_when=concurrent.futures.FIRST_COMPLETED)
            for f in fin:
                i = running.pop(f)
                _, ok = f.result()
                if ok:
                    complete(i)
            if progress and time.time() - last[0] >= every:
                last[0] = time.time()
                msg = progress(counts['ran'] + counts['before'], shells, time.time() - t0)
                if msg:
                    log(msg)
    lf.write(f'graph done: {counts["ran"]} commands run, {counts["before"]} done before, '
             f'{len(failed)} failed, {time.time() - t0:.0f} s\n')
    lf.close()
    df.close()
    if failed:
        n, rc, out = failed[0]
        tail = '\n'.join(out.strip().splitlines()[-30:])
        raise SystemExit(f'{n.get("Annotation")} failed (exit {rc}):\n{tail}\ncommand: {n.get("Action")}\n'
                         f'full log: {logfile}')
    pending = [g.nodes[i].get('Annotation') for i, c in left.items() if c > 0]
    if pending:
        raise SystemExit(f'{dag}: {len(pending)} nodes never became ready (a dependency cycle?): {pending[:4]}')
    return counts['ran'], counts['before']
