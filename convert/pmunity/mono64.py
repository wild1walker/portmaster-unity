"""The mono-arm64 route's `pmunity port` hook (docs/mono-arm64.md): a Mono game's own assemblies on
Unity's arm64 IL2CPP Android player, run by Unity's Mono built for aarch64 Linux, which the release
ships (setup/mono64/<line>/: the bridge as libil2cpp.so, libmonobdwgc-2.0.so and its helpers; port.py's
mono64 package part builds them with runtime/mono64/build.sh and tools/mono64/build-mono.sh)."""
import json
import os


def port_new(ctx):
    """`pmunity port new` hook: port.json for the mono-arm64 route (a 64-bit system, no 32-bit
    userland; nothing is compiled on the handheld)."""
    p = ctx.cfg['port']
    if p['route'] != 'mono-arm64':
        return
    pj_path = os.path.join(ctx.out, p['name'], 'port.json')
    if not os.path.isfile(pj_path):
        return
    with open(pj_path) as f:
        pj = json.load(f)
    a = pj['attr']
    a['reqs'] = [r for r in a.get('reqs') or [] if r != 'armhf']
    if not p.get('desc'):
        a['desc'] = f"{p['title']} on Unity's 64-bit Android player (its own code on Unity's Mono, shipped with the port)."
    ctx.write(f"{p['name']}/port.json", json.dumps(pj, indent=4) + '\n')
