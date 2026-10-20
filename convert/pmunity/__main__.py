"""pmunity: converts a Unity game's desktop (macOS / Linux, or Windows: --from d3d11) data for Unity's
Android player on GLES handhelds.

    python -m pmunity convert <data> [<out>] --config game.json [--steps a,b] [--plugin steps.py] [--only REGEX]
    python -m pmunity shaders <data> [<out>] [--precision highp|default-samplers] [--replace] [--bundles REGEX]
                              [--from glcore|d3d11|glcore,d3d11] [--hlslcc PATH]
    python -m pmunity shaders-export <data> <dir> [--bundles REGEX]
    python -m pmunity android-data --src <converted data> --pkg <Unity Android player> [...]   (--help)
    python -m pmunity macpkg <root> <installer.pkg>...       (unpack macOS installers, e.g. GOG's)

<data> is the Unity data folder (<Game>_Data, or <Game>.app/Contents/Resources/Data). With <out>
the converted files go there and <data> is only read (an overlay: later steps read a file from
<out> once an earlier step wrote it); without it the conversion runs in place and can be resumed.

The config file (JSON) holds one section per step (shaders, textures, settings, audio) with the
step's settings; see each module's DEFAULTS. A plugin is a Python file with STEPS = {name:
function(job)} for a game's own steps, run in the order --steps names them.
"""
import argparse
import importlib.util
import json
import sys

from .job import Job

BUILTIN_STEPS = ('shaders', 'textures', 'settings', 'audio')


def _builtin(name):
    def run(job, a):
        if name == 'shaders':
            from .shaders import step
            c = job.cfg.get('shaders', {})
            src = c.get('from', ['glcore'])
            return step.run(job, c.get('precision', 'highp'), c.get('replace', False), c.get('bundles', '.'),
                            optimize=tuple(c.get('optimize', ('const-arrays', 'unroll'))),
                            sources=tuple(src.split(',') if isinstance(src, str) else src), hlslcc=c.get('hlslcc'))
        if name == 'textures':
            from . import textures
            return textures.run(job, a.only)
        if name == 'settings':
            from . import settings
            return settings.run(job)
        if name == 'audio':
            from . import audio
            return audio.run(job)
    return run


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ['android-data']:
        from . import android_data
        return android_data.main(argv[1:])
    if argv[:1] == ['macpkg']:
        from . import macpkg
        return macpkg.main(argv[1:])
    ap = argparse.ArgumentParser(prog='pmunity')
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('convert', help='run conversion steps with a game config')
    c.add_argument('data'); c.add_argument('out', nargs='?')
    c.add_argument('--config', required=True)
    c.add_argument('--steps', default=None, help='comma-separated (default: the config\'s "steps", else ' + ','.join(BUILTIN_STEPS) + ')')
    c.add_argument('--plugin', action='append', default=[], help="a Python file with STEPS = {name: function(job)}")
    c.add_argument('--only', help='regex: only convert matching files (tests)')
    c.add_argument('--set', action='append', default=[], metavar='SECTION.KEY=VALUE',
                   help='override a config value (VALUE as JSON, else a string), e.g. --set shaders.precision=highp')
    s = sub.add_parser('shaders', help='GLES3 programs from the OpenGL Core (--from d3d11: Direct3D 11) ones')
    s.add_argument('data'); s.add_argument('out', nargs='?')
    s.add_argument('--precision', default='highp', choices=['highp', 'default-samplers'])
    s.add_argument('--replace', action='store_true', help='rebuild GLES programs a shader already has')
    s.add_argument('--bundles', default='.', help='regex: asset bundles to search for shaders')
    s.add_argument('--log-each', action='store_true')
    s.add_argument('--optimize', default='const-arrays,unroll',
                   help="exact rewrites, comma-separated ('' = none): const-arrays, unroll")
    s.add_argument('--from', dest='sources', default='glcore',
                   help='desktop programs to translate, in order of preference: glcore (macOS/Linux builds), '
                        'd3d11 (Windows builds, needs tools/hlslcc), e.g. glcore,d3d11')
    s.add_argument('--hlslcc', help='the pmhlslcc binary (default: $PMHLSLCC, tools/hlslcc/out/pmhlslcc, PATH)')
    e = sub.add_parser('shaders-export', help='GLES3 programs as .vert/.frag files (compile checks)')
    e.add_argument('data'); e.add_argument('dir')
    e.add_argument('--bundles', default='.')
    sub.add_parser('android-data', help='package converted data for the Android player (see android-data --help)')
    a = ap.parse_args(argv)
    if a.cmd == 'convert':
        cfg = json.load(open(a.config))
        cfg.setdefault('version', 1)
        for kv in a.set:
            k, _, v = kv.partition('=')
            sec, _, key = k.partition('.')
            try:
                v = json.loads(v)
            except ValueError:
                pass
            cfg.setdefault(sec, {})[key] = v
        steps = {n: _builtin(n) for n in BUILTIN_STEPS}
        for i, p in enumerate(a.plugin):
            spec = importlib.util.spec_from_file_location(f'pmunity_plugin_{i}', p)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            for n, f in mod.STEPS.items():
                steps[n] = (lambda f: lambda job, a: f(job))(f)
        order = a.steps.split(',') if a.steps else cfg.get('steps', list(BUILTIN_STEPS))
        job = Job(a.data, a.out or a.data, cfg)
        for n in order:
            if n not in steps:
                sys.exit(f'unknown step {n}; steps: {", ".join(steps)}')
            job.log(f'== {n}')
            steps[n](job, a)
        job.log('done')
        return
    cfg = {'version': 1}
    if a.cmd == 'shaders':
        from .shaders import step
        job = Job(a.data, a.out or a.data, cfg)
        step.run(job, a.precision, a.replace, a.bundles, a.log_each, tuple(x for x in a.optimize.split(',') if x),
                 sources=tuple(x for x in a.sources.split(',') if x), hlslcc=a.hlslcc)
    elif a.cmd == 'shaders-export':
        from .shaders import export
        export.export(Job.reader(a.data, cfg), a.dir, a.bundles)


if __name__ == '__main__':
    main()
