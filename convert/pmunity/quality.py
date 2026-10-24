"""Conversion step `quality`: the handheld quality profile (cfg['quality']; docs/quality.md).

A desktop build keeps the quality its PC players chose (most often the Ultra or High level of its
QualitySettings, a URP asset made for a desktop GPU). On a 1 GB handheld (the RG34XX-SP's Mali-G31)
the picture may be made cheaper by default, never the game play: this step lowers what costs GPU
time and only changes how things look.

What it never touches: timing, physics, scripts, LOD selection and culling distances (lodBias,
maximumLODLevel, layer cull distances: things would vanish earlier), shadow distance (shadows keep
showing where things are), lights (they stay lights: point lights may be lit per vertex, spot
lights and lights with shadows or a cookie stay per pixel), anything the game's own code or its
animations drive (post effects then stay as they are), and shader variants: a setting is changed
only where the variant it needs is in the data, so Unity never falls back to another one.

  profile          'auto' (the default): 'handheld' where the machine running the conversion has at
                   most `ram_mb` MB of RAM (a port's setup runs on the handheld), else 'full';
                   'handheld'; 'full' (nothing changed)
  ram_mb           1536: MemTotal of a 1 GB handheld is 900 - 1000 MB, of a 2 GB one about 1900

Built-in render pipeline (QualitySettings, every level; the scenes' lights; shaders):
  pixel_lights     1: per-pixel lights (forward rendering) at most; the others are lit per vertex
                   (up to four) or by spherical harmonics. Auto lights that are spot lights, or have
                   shadows or a cookie, become Important (always per pixel). Needs VERTEXLIGHT_ON
                   variants in the ForwardBase passes. None: keep
  shadow_resolution 1 (Medium): cap on QualitySettings.shadowResolution and lights' own (0 - 3)
  hard_shadows     true: soft shadows drawn hard, where every soft variant has a hard twin
  shadow_cascades  2: cap on the directional light's cascades (at least 2: no variant change)
  msaa             0: cap on QualitySettings.antiAliasing (URP: m_MSAA 1)
  anisotropic      1: cap on anisotropic filtering (2 = forced on, 1 = per texture)
  soft_particles   false: soft particles off
  standard_lod150  true: the Standard shaders' (Standard, Standard (Specular setup), Autodesk
                   Interactive) LOD-150 SubShader first, which Unity then takes: Unity's own mobile
                   class (BRDF3, no box projection or probe blending, no parallax or detail normal
                   maps, hard shadows), compiled into every desktop build. Forward rendering only
  hdr_format       false (opt-in): the graphics tiers' HDR target R11G11B10 instead of FP16 (no alpha
                   channel: image effects that keep data in it break)

URP (every UniversalRenderPipelineAsset the build uses, its renderers' features, volume profiles):
  urp_shadow_resolution 2048: cap on the main and additional lights' shadow atlas
  urp_soft_shadows  'low': soft shadow quality Low (URP 14+), where no variant needs to change
  ssao             true: Downsample on, the cheapest blur, the fewest samples a variant in the data has
  post             ['ChromaticAberration', 'LensDistortion', 'FilmGrain', 'MotionBlur']: volume
                   components switched off (not when the game's code or animations drive volumes)
  render_scale     None: cap on m_RenderScale (e.g. 0.75; ua_host's display scaling is the general way)

  dry_run          true: log what would change, write nothing (`--set quality.dry_run=true`)

Every change is logged; `quality: <what>` lines name what was found and left alone, and why.
"""
import os
import re

from . import clrmeta as CM
from . import monotree as MT
from . import typetrees
from .unity_io import Loaded, collect

DEFAULTS = {'profile': 'auto', 'ram_mb': 1536,
            'pixel_lights': 1, 'shadow_resolution': 1, 'hard_shadows': True, 'shadow_cascades': 2, 'msaa': 0,
            'anisotropic': 1, 'soft_particles': False, 'standard_lod150': True, 'hdr_format': False,
            'urp_shadow_resolution': 2048, 'urp_soft_shadows': 'low', 'ssao': True,
            'post': ['ChromaticAberration', 'LensDistortion', 'FilmGrain', 'MotionBlur'], 'render_scale': None,
            'dry_run': False}

STANDARD = ('Standard', 'Standard (Specular setup)', 'Standard (Roughness setup)', 'Autodesk Interactive')
SSAO_SHADER = 'Hidden/Universal Render Pipeline/ScreenSpaceAmbientOcclusion'
URP_ASSETS = ('Unity.RenderPipelines.Universal.Runtime', 'Unity.RenderPipelines.Core.Runtime')
POST_CLASSES = ('Bloom', 'ChromaticAberration', 'LensDistortion', 'FilmGrain', 'MotionBlur', 'DepthOfField',
                'Vignette', 'PaniniProjection', 'Tonemapping', 'ColorAdjustments', 'ColorLookup', 'ChannelMixer',
                'LiftGammaGain', 'ShadowsMidtonesHighlights', 'SplitToning', 'WhiteBalance', 'ColorCurves')
# Light: m_Type (0 spot, 1 directional, 2 point, 3 area), m_RenderMode (0 auto, 1 important, 2 not
# important), m_Lightmapping (4 realtime, 1 mixed, 2 baked), m_Shadows.m_Type (0 none, 1 hard, 2 soft)
SPOT, DIRECTIONAL, POINT = 0, 1, 2
AUTO, IMPORTANT = 0, 1
BAKED = 2


def profile_of(c, mem_mb=None):
    """('handheld' | 'full', why)."""
    p = c.get('profile', 'auto')
    if p in (None, False, 'off', 'full'):
        return 'full', 'quality.profile = full'
    if p == 'handheld':
        return 'handheld', 'quality.profile = handheld'
    if p != 'auto':
        raise ValueError(f'quality.profile: {p!r} (auto, handheld or full)')
    if mem_mb is None:
        from .textures import mem_total_mb
        mem_mb = mem_total_mb()
    if mem_mb <= c.get('ram_mb', 1536):
        return 'handheld', f'{mem_mb} MB of RAM (at most {c.get("ram_mb", 1536)})'
    return 'full', f'{mem_mb} MB of RAM (more than {c.get("ram_mb", 1536)})'


def _bundles(job):
    from .shaders.step import _is_bundle
    n = 0
    for d, _, names in os.walk(os.path.join(job.src, 'StreamingAssets')):
        n += sum(1 for x in names if _is_bundle(os.path.join(d, x)))
    return n


def data_files(job):
    files = job.list('', lambda n: n.endswith('.assets') or n == 'globalgamemanagers' or re.match(r'level\d+$', n))
    files += job.list('Resources', lambda n: n == 'unity_builtin_extra')
    return files


# ---------------------------------------------------------------------------- shader variants

class Variants:
    """The GLES3 variants of every shader in the data: {shader name: [(subshader index, LOD, light
    mode (upper case, '' without the tag), pass name, {frozenset of keywords})]}. A pass's set holds
    every stage's subprograms (Unity 2017 - 2018 list the fragment ones without the keywords only
    the vertex stage uses)."""

    def __init__(self):
        self.shaders = {}

    def add(self, tree):
        from .shaders import blob as B
        from .shaders import convert as SC
        pf = tree['m_ParsedForm']
        if B.PLATFORM_GLES3PLUS not in tree['platforms']:
            return
        try:
            entries = SC.platform_entries(tree, B.PLATFORM_GLES3PLUS)
        except Exception:  # noqa: BLE001 - an unreadable blob: the shader counts as having nothing
            return
        types = SC.PLATFORM_TYPES[B.PLATFORM_GLES3PLUS]
        out = []
        for si, ss in enumerate(pf['m_SubShaders']):
            for p in ss['m_Passes']:
                state = p.get('m_State') or {}
                tags = {k.upper(): v for k, v in (state.get('m_Tags') or {}).get('tags', [])}
                kws = set()
                for st in SC.STAGES:
                    prog = p.get(st) or {}
                    lists = [prog['m_SubPrograms']] if prog.get('m_SubPrograms') is not None else []
                    lists += list(prog.get('m_PlayerSubPrograms') or [])
                    for lst in lists:
                        for sp in lst:
                            if sp['m_GpuProgramType'] in types:
                                e = entries[sp['m_BlobIndex']]
                                kws.add(frozenset(list(getattr(e, 'keywords', ()) or ()) +
                                                  list(getattr(e, 'local_keywords', None) or ())))
                out.append((si, ss.get('m_LOD', 0), str(tags.get('LIGHTMODE', '')).upper(), state.get('m_Name', ''), kws))
        self.shaders[pf['m_Name']] = out

    def passes(self, mode=None):
        for name, ps in self.shaders.items():
            for si, lod, m, pn, kws in ps:
                if mode is None or m == mode:
                    yield name, si, lod, m, pn, kws

    def without_twin(self, kw):
        """Shaders with a variant of keyword `kw` whose twin without it (the same other keywords) is
        not in the data: turning `kw` off would ask them for a variant the build stripped."""
        bad = set()
        for name, si, lod, m, pn, kws in self.passes():
            for k in kws:
                if kw in k and (k - {kw}) not in kws:
                    bad.add(name)
        return sorted(bad)

    def soft_without_hard(self):
        return self.without_twin('SHADOWS_SOFT')

    def vertex_lights(self):
        """(ForwardBase passes, of them with VERTEXLIGHT_ON variants)."""
        n = v = 0
        for name, si, lod, m, pn, kws in self.passes('FORWARDBASE'):
            n += 1
            v += any('VERTEXLIGHT_ON' in k for k in kws)
        return n, v

    def keyword_present(self, kw):
        return any(kw in k for _, _, _, _, _, kws in self.passes() for k in kws)

    def of(self, name):
        return self.shaders.get(name)


def standard_reorder(tree, info=None):
    """Moves a Standard shader's first SubShader below LOD 300 with GLES3 programs for its forward
    and shadow passes to the front. Returns the (from, to) LODs, or None (not such a shader).
    info: the shader's Variants entry (default: read from the tree)."""
    pf = tree['m_ParsedForm']
    if pf['m_Name'] not in STANDARD:
        return None
    subs = pf['m_SubShaders']
    if len(subs) < 2 or subs[0].get('m_LOD', 0) < 300:
        return None
    if info is None:
        v = Variants()
        v.add(tree)
        info = v.of(pf['m_Name']) or []
    for i in range(1, len(subs)):
        if subs[i].get('m_LOD', 0) >= 300:
            continue
        modes = {m for si, lod, m, pn, kws in info if si == i and kws}
        if {'FORWARDBASE', 'FORWARDADD', 'SHADOWCASTER'} <= modes:
            lods = (subs[0].get('m_LOD'), subs[i].get('m_LOD'))
            subs.insert(0, subs.pop(i))
            return lods
        return None
    return None


# ---------------------------------------------------------------------------- the game's own code

# packages whose volume code runs only with a component of theirs in a scene: {assembly: those classes}
# (Cinemachine's volume extensions blend profiles per virtual camera, e.g. depth of field focus)
VOLUME_PACKAGES = {'Cinemachine.dll': ('CinemachineVolumeSettings', 'CinemachinePostProcessing'),
                   'Unity.Cinemachine.dll': ('CinemachineVolumeSettings', 'CinemachinePostProcessing')}


def drives_volumes(managed, classes=()):
    """The game's assemblies (not Unity's or its render pipelines') that reference URP's volumes or
    post-processing components: {assembly: [type names]}. `classes`: the scripts' class names the
    data's MonoBehaviours have (a package of VOLUME_PACKAGES counts only with one of its volume
    components among them)."""
    out = {}
    if not managed or not os.path.isdir(managed):
        return out
    classes = {c.rsplit('.', 1)[-1] for c in classes}
    watch = {'UnityEngine.Rendering.Volume', 'UnityEngine.Rendering.VolumeProfile', 'UnityEngine.Rendering.VolumeComponent',
             'UnityEngine.Rendering.VolumeManager', 'UnityEngine.Rendering.VolumeStack'}
    watch |= {f'UnityEngine.Rendering.Universal.{c}' for c in POST_CLASSES}
    for n in sorted(os.listdir(managed)):
        if not n.endswith('.dll') or n.startswith(('UnityEngine', 'Unity.RenderPipelines', 'System', 'mscorlib',
                                                   'netstandard', 'Mono.', 'Microsoft.')):
            continue
        try:
            a = CM.Assembly(os.path.join(managed, n))
            names = {a.typeref_name(r) for r in range(1, a.count(CM.TYPEREF) + 1)}
        except (CM.NotAssembly, ValueError, IndexError):
            continue
        hit = sorted(names & watch)
        if hit and (n not in VOLUME_PACKAGES or classes & set(VOLUME_PACKAGES[n])):
            out[n] = hit
    return out


# ---------------------------------------------------------------------------- the step

class Plan:
    def __init__(self):
        self.done, self.kept = [], []

    def did(self, what):
        self.done.append(what)

    def keep(self, what):
        self.kept.append(what)


def run(job, mem_mb=None):
    c = dict(DEFAULTS, **(job.cfg.get('quality') or {}))
    prof, why = profile_of(c, mem_mb)
    if prof == 'full':
        job.log(f'quality: full picture ({why}): nothing changed')
        return
    if c['shadow_cascades'] is not None and c['shadow_cascades'] < 2:
        raise ValueError('quality.shadow_cascades: at least 2 (1 cascade is another shader variant)')
    if job.is_done('quality', 'all'):
        return
    job.log(f'quality: handheld profile ({why})')
    files = data_files(job)
    plan = Plan()
    scan = _scan(job, files, c)
    if scan.shaders_seen and not scan.variants.shaders:
        # (the variant checks read the GLES3 programs the shaders step adds)
        job.log('quality: no shader has GLES3 programs yet: run this step after `shaders`; nothing changed')
        return
    edits = _decide(job, scan, c, plan)
    for rel in files:
        if rel in edits:
            _edit(job, rel, edits[rel], scan, c, plan)
    bundles = _bundles(job)
    if bundles:
        plan.keep(f'{bundles} asset bundles under StreamingAssets: not looked at (scenes, lights and URP data '
                  f'there keep the game\'s quality)')
    for k in plan.kept:
        job.log(f'quality: kept: {k}')
    job.log('quality: ' + ('would change: ' if c.get('dry_run') else '') + ('; '.join(plan.done) or 'nothing to change'))
    if c.get('dry_run'):
        return
    caps = host_caps(scan.qcaps)
    if caps:
        job.state['quality_caps'] = caps            # android_package.json's "quality" (android_data.py)
        job.log(f'quality: run-time caps for ua_host: {caps}')
    job.mark_done('quality', 'all')


# QualitySettings fields -> the caps' names ua_host knows (runtime/host/quality.c)
HOST_CAPS = (('pixelLightCount', 'pixel_lights'), ('shadows', 'shadows'), ('shadowResolution', 'shadow_resolution'),
             ('shadowCascades', 'shadow_cascades'), ('antiAliasing', 'msaa'), ('anisotropicTextures', 'anisotropic'),
             ('softParticles', 'soft_particles'))


def host_caps(qcaps):
    """The caps as ua_host reads them: "pixel_lights=1,msaa=0,..."."""
    return ','.join(f'{h}={int(qcaps[k])}' for k, h in HOST_CAPS if k in qcaps)


class Scan:
    """What the data holds that the profile looks at."""

    def __init__(self):
        self.variants = Variants()
        self.quality = self.graphics = None     # QualitySettings / GraphicsSettings trees
        self.srp = False
        self.lights = []                        # (rel, path id, tree)
        self.cameras = []                       # their rendering paths
        self.std_files = set()                  # files with a Standard shader
        self.mbs = {}                           # (rel, path id) -> (class name, assembly, full name): URP's
        self.volume_scripts = set()             # (file name, path id) of the MonoScripts of Volume and the post components
        self.clip_scripts = []                  # (clip name, file name, path id) of the scripts animation clips bind
        self.script_classes = set()             # every MonoBehaviour's script class (full name)
        self.shaders_seen = 0
        self.unity = None
        self.qcaps = {}
        self.post_ok = False


def _external(sf, file_id, rel):
    """The file name a PPtr's file id names (0: the file itself)."""
    if not file_id:
        return os.path.basename(rel)
    try:
        return os.path.basename(sf.externals[file_id - 1].path.replace('\\', '/'))
    except (IndexError, AttributeError):
        return None


def _scan(job, files, c):
    s = Scan()
    from . import unity_version as UV
    try:
        s.unity = UV.of_data(job.src).line
    except Exception:  # noqa: BLE001
        s.unity = None
    scripts = {}
    for rel in files:
        lf = Loaded(job.path_in(rel))
        try:
            for pid, o in lf.objects():
                n = o.type.name
                if n == 'Shader':
                    t = o.read_typetree()
                    s.shaders_seen += 1
                    s.variants.add(t)
                    if t['m_ParsedForm']['m_Name'] in STANDARD:
                        s.std_files.add(rel)
                elif n == 'QualitySettings':
                    s.quality = typetrees.read(o)[0]
                    s.srp |= any((lv.get('customRenderPipeline') or {}).get('m_PathID') for lv in s.quality.get('m_QualitySettings', []))
                elif n == 'GraphicsSettings':
                    s.graphics = typetrees.read(o)[0]
                    s.srp |= bool((s.graphics.get('m_CustomRenderPipeline') or {}).get('m_PathID'))
                elif n == 'Light':
                    try:
                        s.lights.append((rel, pid, o.read_typetree()))
                    except Exception:  # noqa: BLE001 - a light UnityPy cannot read: left alone
                        pass
                elif n == 'Camera':
                    try:
                        s.cameras.append(o.read_typetree().get('m_RenderingPath', -1))
                    except Exception:  # noqa: BLE001
                        pass
                elif not s.srp:
                    continue
                elif n == 'MonoScript':
                    try:
                        m = o.read_typetree()
                    except Exception:  # noqa: BLE001
                        continue
                    if m.get('m_AssemblyName', '').replace('.dll', '') in URP_ASSETS and \
                            m.get('m_ClassName') in POST_CLASSES + ('Volume', 'VolumeProfile'):
                        s.volume_scripts.add((os.path.basename(rel), pid))
                elif n == 'MonoBehaviour':
                    try:
                        sp = o.read(check_read=False).m_Script
                        key = (_external(lf.sf, sp.m_FileID, rel), sp.m_PathID)
                    except Exception:  # noqa: BLE001
                        continue
                    if key not in scripts:
                        scripts[key] = MT.script_of(o)
                        if scripts[key]:
                            s.script_classes.add(scripts[key][1])
                    sc = scripts[key]
                    if sc and sc[0].replace('.dll', '') in URP_ASSETS:
                        s.mbs[(rel, pid)] = (sc[1].rsplit('.', 1)[-1], sc[0], sc[1])
                elif n == 'AnimationClip' and c.get('post'):
                    try:
                        t = o.read_typetree()
                    except Exception:  # noqa: BLE001
                        continue
                    for b in (t.get('m_ClipBindingConstant') or {}).get('genericBindings') or []:
                        sp = b.get('script') or {}
                        if sp.get('m_PathID'):
                            s.clip_scripts.append((t.get('m_Name'), _external(lf.sf, sp.get('m_FileID'), rel), sp['m_PathID']))
        finally:
            lf.close()
            collect()
    return s


def _decide(job, s, c, plan):
    """{rel: {what to edit there}} from the scan; what is left alone goes to plan.keep."""
    edits = {}
    levels = s.quality.get('m_QualitySettings', []) if s.quality else []
    # ---- QualitySettings caps (every level; pixel lights and soft shadows: the built-in pipeline's)
    gq = {}
    if levels:
        if c['pixel_lights'] is not None and not s.srp and any(lv.get('pixelLightCount', 0) > c['pixel_lights'] for lv in levels):
            n, v = s.variants.vertex_lights()
            if n and not v:
                plan.keep(f'pixel lights: none of the {n} ForwardBase passes has VERTEXLIGHT_ON variants (the build '
                          f'strips vertex lights; the other lights would be spherical harmonics only)')
            else:
                gq['pixelLightCount'] = c['pixel_lights']
        if c['hard_shadows'] and not s.srp and any(lv.get('shadows', 0) == 2 for lv in levels):
            bad = s.variants.soft_without_hard()
            if bad:
                plan.keep(f'soft shadows: no hard variant in {", ".join(bad[:5])}{" ..." if len(bad) > 5 else ""}')
            else:
                gq['shadows'] = 1
        for k, ck in (('shadowResolution', 'shadow_resolution'), ('shadowCascades', 'shadow_cascades'),
                      ('antiAliasing', 'msaa'), ('anisotropicTextures', 'anisotropic')):
            if c[ck] is not None:
                gq[k] = c[ck]
        if c['soft_particles'] is False and any(lv.get('softParticles') for lv in levels):
            bad = s.variants.without_twin('SOFTPARTICLES_ON')
            if bad:
                plan.keep(f'soft particles: no variant without SOFTPARTICLES_ON in {", ".join(bad[:5])}'
                          f'{" ..." if len(bad) > 5 else ""}')
            else:
                gq['softParticles'] = 0
    s.qcaps = gq
    if gq or c['hdr_format']:
        edits.setdefault('globalgamemanagers', set()).add('settings')
    # ---- lights that stay per pixel; lights' own shadow resolution
    for rel, pid, l in s.lights:
        if _light_edit(l, gq, c):
            edits.setdefault(rel, set()).add('lights')
    # ---- the Standard shaders' LOD-150 SubShader
    if c['standard_lod150'] and not s.srp and s.std_files:
        paths = [t.get('renderingPath') for k, t in (s.graphics or {}).items() if k.startswith('m_TierSettings') and isinstance(t, dict)]
        if any(p in (2, 3) for p in paths + s.cameras):
            plan.keep('Standard shaders: deferred rendering (their LOD-150 SubShader has no deferred pass)')
        else:
            for rel in s.std_files:
                edits.setdefault(rel, set()).add('shaders')
    # ---- URP: its assets, SSAO, post components
    if s.srp and s.mbs:
        s.post_ok = bool(c.get('post'))
        if s.post_ok:
            drive = drives_volumes(os.path.join(job.src, 'Managed'), s.script_classes)
            if drive:
                s.post_ok = False
                plan.keep('post effects: the game\'s code drives volumes (' + '; '.join(
                    f'{a}: {", ".join(x.rsplit(".", 1)[-1] for x in t)}' for a, t in drive.items()) + ')')
        if s.post_ok:
            clips = sorted({name for name, f, pid in s.clip_scripts if (f, pid) in s.volume_scripts})
            if clips:
                s.post_ok = False
                plan.keep(f'post effects: animations drive volumes ({", ".join(clips[:4])})')
        for (rel, pid), (cls, asm, full) in s.mbs.items():
            if cls in ('UniversalRenderPipelineAsset', 'ScreenSpaceAmbientOcclusion') or \
                    (s.post_ok and cls in c.get('post', ())):
                edits.setdefault(rel, set()).add('urp')
    return edits


def _light_edit(l, gq, c, apply=False):
    """Whether a Light changes (and with apply, changes it)."""
    changed = False
    sh = l.get('m_Shadows') or {}
    realtime = l.get('m_Lightmapping', 4) != BAKED
    if 'pixelLightCount' in gq and realtime and l.get('m_RenderMode') == AUTO and \
            (l.get('m_Type') == SPOT or sh.get('m_Type', 0) or (l.get('m_Cookie') or {}).get('m_PathID')):
        changed = True
        if apply:
            l['m_RenderMode'] = IMPORTANT
    r = sh.get('m_Resolution', -1)
    if c['shadow_resolution'] is not None and r is not None and r > c['shadow_resolution']:
        changed = True
        if apply:
            sh['m_Resolution'] = c['shadow_resolution']
    return changed


def _edit(job, rel, what, s, c, plan):
    lf = Loaded(job.path_in(rel))
    n = 0
    gen = None
    lights = {}
    try:
        for pid, o in lf.objects():
            t = o.type.name
            if 'settings' in what and t == 'QualitySettings':
                tree, node = typetrees.read(o, edits=('m_QualitySettings',))
                ch = []
                for lv in tree['m_QualitySettings']:
                    for k, cap in s.qcaps.items():
                        if k in lv and lv[k] > cap:
                            old, lv[k] = lv[k], type(lv[k])(cap)
                            ch.append(f'{lv.get("name")}.{k} {old} -> {lv[k]}')
                if ch:
                    typetrees.save(o, tree, node)
                    n += 1
                    plan.did('QualitySettings: ' + ', '.join(ch))
            elif 'settings' in what and t == 'GraphicsSettings' and c['hdr_format']:
                tree, node = typetrees.read(o)
                ch = [k for k, v in tree.items() if k.startswith('m_TierSettings') and isinstance(v, dict) and v.get('hdrMode') == 1]
                for k in ch:
                    tree[k]['hdrMode'] = 2
                if ch:
                    typetrees.save(o, tree, node)
                    n += 1
                    plan.did(f'GraphicsSettings: HDR targets R11G11B10 ({len(ch)} tiers)')
            elif 'lights' in what and t == 'Light':
                try:
                    tree = o.read_typetree()
                except Exception:  # noqa: BLE001
                    continue
                before = (tree.get('m_RenderMode'), (tree.get('m_Shadows') or {}).get('m_Resolution'))
                if _light_edit(tree, s.qcaps, c, apply=True):
                    o.save_typetree(tree)
                    n += 1
                    kind = {SPOT: 'spot', DIRECTIONAL: 'directional', POINT: 'point'}.get(tree.get('m_Type'), 'area')
                    if before[0] != tree.get('m_RenderMode'):
                        k = f'{kind} lights Important'
                        lights[k] = lights.get(k, 0) + 1
                    if before[1] != (tree.get('m_Shadows') or {}).get('m_Resolution'):
                        k = 'lights with their shadow resolution capped'
                        lights[k] = lights.get(k, 0) + 1
            elif 'shaders' in what and t == 'Shader':
                tree = o.read_typetree()
                lods = standard_reorder(tree)
                if lods:
                    o.save_typetree(tree)
                    n += 1
                    plan.did(f'{rel}: {tree["m_ParsedForm"]["m_Name"]}: SubShader LOD {lods[1]} first (was LOD {lods[0]})')
            elif 'urp' in what and t == 'MonoBehaviour' and (rel, pid) in s.mbs:
                cls, asm, full = s.mbs[(rel, pid)]
                if cls not in ('UniversalRenderPipelineAsset', 'ScreenSpaceAmbientOcclusion') and \
                        not (s.post_ok and cls in (c.get('post') or ())):
                    continue
                if gen is None:
                    gen = MT.Generator(os.path.join(job.src, 'Managed'), s.unity)
                tree, node = MT.read(o, gen, (asm, full))
                if tree is None:
                    plan.keep(f'{rel}: {cls} {pid}: not read ({node})')
                    continue
                ch = _urp_edit(cls, tree, s, c)
                if ch is None:
                    plan.keep(f'{rel}: {cls} {pid}: left alone (its fields read with values they cannot hold)')
                elif ch:
                    o.save_typetree(tree, nodes=node)
                    n += 1
                    plan.did(f'{rel}: {cls} {tree.get("m_Name") or pid}: ' + ', '.join(ch))
        if lights:
            plan.did(f'{rel}: ' + ', '.join(f'{v} {k}' for k, v in sorted(lights.items())))
        if n and not c.get('dry_run'):
            lf.write_to(job, rel)
    finally:
        lf.close()
        collect()


def _cap(tree, key, cap, ch, name=None):
    if cap is None or key not in tree or tree[key] <= cap:
        return
    ch.append(f'{name or key} {tree[key]} -> {cap}')
    tree[key] = type(tree[key])(cap)


# the values the fields the step edits can hold: an object whose fields read outside them is left
# alone (a second guard, after the exact read, against a tree with the right size and the fields
# in the wrong places)
PLAUSIBLE = {
    'UniversalRenderPipelineAsset': {'m_MSAA': {1, 2, 4, 8},
                                     'm_MainLightShadowmapResolution': {256, 512, 1024, 2048, 4096, 8192},
                                     'm_AdditionalLightsShadowmapResolution': {256, 512, 1024, 2048, 4096, 8192},
                                     'm_SoftShadowQuality': {0, 1, 2, 3}, 'm_SupportsHDR': {0, 1}},
    'ScreenSpaceAmbientOcclusion': {'Downsample': {0, 1}, 'BlurQuality': {0, 1, 2}, 'Samples': {0, 1, 2},
                                    'Source': {0, 1}, 'NormalSamples': {0, 1, 2}, 'AfterOpaque': {0, 1}},
}


def plausible(cls, t):
    """Whether the fields the step may edit hold values they can hold (None: which one does not)."""
    if cls == 'ScreenSpaceAmbientOcclusion':
        t = t.get('m_Settings')
        if not isinstance(t, dict):
            return False
        if 'SampleCount' in t and not -1 <= t['SampleCount'] <= 64:
            return False
    elif cls == 'UniversalRenderPipelineAsset':
        if 'm_RenderScale' in t and not 0.05 <= t['m_RenderScale'] <= 2.0:
            return False
    elif 'active' in t and t['active'] not in (0, 1):
        return False
    return all(k not in t or t[k] in vals for k, vals in PLAUSIBLE.get(cls, {}).items())


def _urp_edit(cls, t, s, c):
    ch = []
    if not plausible(cls, t):
        return None
    if cls == 'UniversalRenderPipelineAsset':
        if c['msaa'] is not None:
            _cap(t, 'm_MSAA', max(1, c['msaa']), ch)
        _cap(t, 'm_MainLightShadowmapResolution', c['urp_shadow_resolution'], ch)
        _cap(t, 'm_AdditionalLightsShadowmapResolution', c['urp_shadow_resolution'], ch)
        if c['urp_soft_shadows'] == 'low' and t.get('m_SoftShadowQuality', 0) > 1:
            if s.variants.keyword_present('_SHADOWS_SOFT_HIGH') or s.variants.keyword_present('_SHADOWS_SOFT_MEDIUM'):
                if s.variants.keyword_present('_SHADOWS_SOFT_LOW'):
                    _cap(t, 'm_SoftShadowQuality', 1, ch)
            else:
                _cap(t, 'm_SoftShadowQuality', 1, ch)
        if c['render_scale']:
            _cap(t, 'm_RenderScale', float(c['render_scale']), ch)
    elif cls == 'ScreenSpaceAmbientOcclusion' and c['ssao']:
        st = t.get('m_Settings') or {}
        ch += ssao_settings(st, s.variants.of(SSAO_SHADER))
    elif cls in (c.get('post') or ()) and getattr(s, 'post_ok', False):
        if t.get('active'):
            t['active'] = False if isinstance(t['active'], bool) else 0
            ch.append('off')
    return ch


# URP's SSAO (renderer feature) settings. URP 14+: Samples High 0, Medium 1, Low 2; BlurQuality High 0
# (bilateral, 3 passes), Medium 1 (gaussian, 2), Low 2 (kawase, 1); Source Depth 0, DepthNormals 1;
# NormalSamples (the normals rebuilt from depth) Low 0, Medium 1, High 2. Their keywords on the
# occlusion pass: _SAMPLE_COUNT_<samples>, _SOURCE_DEPTH_<normal samples> or _SOURCE_DEPTH_NORMALS
# (URP strips the combinations no asset uses). URP 10 - 12: SampleCount (a number, a uniform), one
# bilateral blur. Downsample (the occlusion and its blur at half the width and height) and the blur
# passes have no keywords.
SAMPLES = ('high', 'medium', 'low')
SAMPLE_KW = {0: '_SAMPLE_COUNT_HIGH', 1: '_SAMPLE_COUNT_MEDIUM', 2: '_SAMPLE_COUNT_LOW'}
DEPTH_KW = {0: '_SOURCE_DEPTH_LOW', 1: '_SOURCE_DEPTH_MEDIUM', 2: '_SOURCE_DEPTH_HIGH'}
OCCLUSION_KW = set(SAMPLE_KW.values()) | set(DEPTH_KW.values()) | {'_SOURCE_DEPTH_NORMALS'}
KAWASE = 'SSAO_Kawase'


def _source_kw(source, normals):
    return '_SOURCE_DEPTH_NORMALS' if source == 1 else DEPTH_KW.get(normals)


def ssao_settings(st, variants):
    """URP's SSAO settings (`m_Settings`) made cheaper in place, as far as the SSAO shader's variants in
    the data allow; returns what changed."""
    ch = []
    if not variants:
        return ch
    if 'Downsample' in st and not st['Downsample']:
        st['Downsample'] = True if isinstance(st['Downsample'], bool) else 1
        ch.append('SSAO downsampled')
    names = {pn for si, lod, m, pn, kws in variants if kws}
    if st.get('BlurQuality', 2) < 2 and KAWASE in names:
        ch.append(f'SSAO blur {("bilateral", "gaussian")[st["BlurQuality"]]} -> kawase')
        st['BlurQuality'] = 2
    if st.get('Samples') in SAMPLE_KW:
        occ = set().union(*[kws for si, lod, m, pn, kws in variants if pn == 'SSAO_Occlusion'] or [set()])
        rest = lambda k: frozenset(x for x in k if x not in OCCLUSION_KW)                 # noqa: E731
        src, nrm = st.get('Source', 1), st.get('NormalSamples', 1)
        need = {rest(k) for k in occ if SAMPLE_KW[st['Samples']] in k and _source_kw(src, nrm) in k}
        # sources to try, the current one first: switching to depth (normals rebuilt from it) only
        # where a cheaper sample count is compiled with it alone
        sources = [(src, nrm)] + [(0, n) for n in (0, 1, 2) if (0, n) != (src, nrm)]
        for samples in range(2, st['Samples'], -1):
            got = next(((s_, n_) for s_, n_ in sources
                        if need and need <= {rest(k) for k in occ if SAMPLE_KW[samples] in k and _source_kw(s_, n_) in k}), None)
            if got:
                ch.append(f'SSAO samples {SAMPLES[st["Samples"]]} -> {SAMPLES[samples]}' +
                          (', normals rebuilt from depth' if got[0] == 0 and src == 1 else ''))
                st['Samples'] = samples
                if got != (src, nrm):
                    st['Source'], st['NormalSamples'] = got
                break
    elif st.get('SampleCount', 0) > 4:
        ch.append(f'SSAO samples {st["SampleCount"]} -> 4')
        st['SampleCount'] = 4
    return ch
