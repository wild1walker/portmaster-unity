"""Converter plugins without code: a JSON file of named steps, each a list of field edits, which
the setup's Python and pmu (setup-rs, crates/convert/src/steps/edits.rs) run the same way
(docs/plugins.md, "Converter plugins"):

    {"about": "anything (not read)",
     "steps": {"<step>": [{"file": "<glob>", "class": "<class>", "name": "<glob>",
                           "set": {"<field path>": <value>, ...}}, ...]}}

An edit applies to every object of the class (UnityPy's class name, e.g. QualitySettings,
Material, Light) in the data files whose path (relative to the data folder, '/' separated)
matches `file` (fnmatch's case-sensitive glob: * ? [seq] [!seq]; * also matches '/'), and with
`name` only to objects whose m_Name matches that glob. Each `set` replaces one value: the path's
parts are field names, and a decimal number indexes a list (or 0 / 1 a pair). The value must be
of the kind the field has: true / false for a bool, an integer for an integer field (its width
is the type tree's), a number for a float, a string for a string. MonoBehaviour is refused: its
script's fields need the game's assemblies.

Files a glob matches that UnityPy does not take for a serialized file or a bundle (streams,
configs), and bundles of several serialized files (scenes), are left out. A step's files are journaled as the built-in steps' are (an interrupted
setup goes on); a file whose objects were edited is written again, the others are left as they
are."""
import fnmatch
import json
import os

FIELDS = ('file', 'class', 'name', 'set')


class _Bad(Exception):
    pass


def _check(plugin, cfg):
    if not isinstance(cfg, dict):
        raise _Bad('not a JSON object')
    for k in cfg:
        if k not in ('about', 'steps'):
            raise _Bad(f'unknown key {k!r} (about, steps)')
    steps = cfg.get('steps')
    if not isinstance(steps, dict) or not steps:
        raise _Bad('"steps" must be an object of named steps')
    for name, edits in steps.items():
        if not name:
            raise _Bad('a step without a name')
        if not isinstance(edits, list) or not edits:
            raise _Bad(f'step {name}: not a list of edits')
        for i, e in enumerate(edits):
            where = f'step {name}, edit {i}'
            if not isinstance(e, dict):
                raise _Bad(f'{where}: not an object')
            for k in e:
                if k not in FIELDS:
                    raise _Bad(f'{where}: unknown key {k!r} (file, class, name, set)')
            for k in ('file', 'class', 'name'):
                if k in e and (not isinstance(e[k], str) or not e[k]):
                    raise _Bad(f'{where}: "{k}" must be a string')
            for k in ('file', 'class', 'set'):
                if k not in e:
                    raise _Bad(f'{where}: no "{k}"')
            if e['class'] == 'MonoBehaviour':
                raise _Bad(f'{where}: MonoBehaviour fields need the game\'s assemblies (a Python plugin)')
            if not isinstance(e['set'], dict) or not e['set']:
                raise _Bad(f'{where}: "set" must be an object of field paths and values')
            for path, v in e['set'].items():
                if not path or '' in path.split('.'):
                    raise _Bad(f'{where}: field path {path!r}')
                if v is None or isinstance(v, (list, dict)):
                    raise _Bad(f'{where}: {path}: a value must be a bool, a number or a string')


def load(path):
    """STEPS of a JSON plugin, {name: function(job)}; SystemExit with the reason for a bad file."""
    try:
        with open(path, 'rb') as f:
            cfg = json.loads(f.read().decode('utf-8'))
        _check(path, cfg)
    except (OSError, ValueError, _Bad) as ex:
        raise SystemExit(f'plugin {path}: {ex}')
    return {name: (lambda job, name=name, edits=edits: run(job, path, name, edits))
            for name, edits in cfg['steps'].items()}


def data_files(root):
    """The files under the data folder, relative and '/'-separated, sorted; dot files and folders
    (the journal, .pmunity-work) left out."""
    out = []
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if not x.startswith('.')]
        for n in files:
            if not n.startswith('.'):
                out.append(os.path.relpath(os.path.join(d, n), root).replace(os.sep, '/'))
    return sorted(out)


def _unity_file(path):
    """Whether UnityPy takes the file for a serialized file or a bundle (check_file_type): a glob
    may match the data folder's other files (streams, configs), which are left out."""
    from UnityPy.enums import FileType
    from UnityPy.helpers.ImportHelper import check_file_type
    with open(path, 'rb') as f:
        ft, _ = check_file_type(f)
    return ft in (FileType.AssetsFile, FileType.BundleFile)


def _kind(v):
    if isinstance(v, bool):
        return 'a bool'
    if isinstance(v, int):
        return 'an integer'
    if isinstance(v, float):
        return 'a number'
    if isinstance(v, str):
        return 'a string'
    return None


def _set(t, path, v):
    """t with the value at path replaced by v (a pair, a tuple, is rebuilt); _Bad when the path or
    the kind does not fit."""
    parts = path.split('.')

    def put(c, i):
        k = parts[i]
        if isinstance(c, dict):
            if k not in c:
                raise _Bad(f'no field {k}')
            key = k
        elif isinstance(c, (list, tuple)):
            if not k.isdigit() or not k.isascii() or int(k) >= len(c):
                raise _Bad(f'{k}: not an index of {len(c)} items')
            key = int(k)
        else:
            raise _Bad(f'{k}: {".".join(parts[:i]) or "the object"} is not a struct or a list')
        if i + 1 < len(parts):
            new = put(c[key], i + 1)
        else:
            old = c[key]
            have = _kind(old)
            if have is None:
                raise _Bad('not a single value (a struct, list or byte array)')
            want = _kind(v)
            ok = want == have or (have == 'a number' and want == 'an integer')
            if not ok:
                raise _Bad(f'{have}, not {want}')
            new = float(v) if have == 'a number' else v
        if isinstance(c, tuple):
            c = list(c)
            c[key] = new
            return tuple(c)
        c[key] = new
        return c
    return put(t, 0)


def run(job, plugin, step, edits):
    from .unity_io import Loaded, NotOneSerializedFile
    files = [rel for rel in data_files(job.src) if any(fnmatch.fnmatchcase(rel, e['file']) for e in edits)]
    total = 0
    for rel in files:
        if job.is_done(step, rel):
            continue
        if not _unity_file(job.path_in(rel)):
            continue
        mine = [e for e in edits if fnmatch.fnmatchcase(rel, e['file'])]
        try:
            lf = Loaded(job.path_in(rel))
        except NotOneSerializedFile:
            job.log(f'{step}: {rel}: a bundle of several serialized files (scenes): left out')
            continue
        n = 0
        for pid, o in lf.objects():
            es = [e for e in mine if e['class'] == o.type.name]
            if not es:
                continue
            t = o.read_typetree()
            hit = False
            for e in es:
                if 'name' in e:
                    nm = t.get('m_Name')
                    if not isinstance(nm, str) or not fnmatch.fnmatchcase(nm, e['name']):
                        continue
                for path, v in e['set'].items():
                    try:
                        t = _set(t, path, v)
                    except _Bad as ex:
                        lf.close()
                        raise SystemExit(f'plugin {plugin}: step {step}: {rel}: {o.type.name} (path id {pid}): '
                                         f'{path}: {ex}')
                hit = True
            if hit:
                o.save_typetree(t)
                n += 1
        if n:
            lf.write_to(job, rel)
        lf.close()
        job.mark_done(step, rel)
        total += n
        job.log(f'{step}: {rel}: {n} object(s) edited')
    job.log(f'{step}: {len(files)} file(s), {total} object(s) edited')
