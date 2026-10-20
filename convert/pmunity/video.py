"""Hidden/VideoDecodeAndroid for the Android player: the shader Unity's Android VideoPlayer draws
video frames with (libunity: ScriptMapper::FindShader("Hidden/VideoDecodeAndroid"), passes
RGBAExternal_To_RGBA and RGBASplitExternal_To_RGBA, found by name ignoring case; the same in
the 2017.4 - 2022.3 players). An Android build's unity_builtin_extra carries Unity's copy; a desktop
build's has only the desktop Hidden/VideoDecode, so without this the player logs "Could not find
material Hidden/VideoDecodeAndroid" and never shows a frame.

The programs are our own (video_decode_android.shader: GLSL ES 3.00, samplerExternalOES), written
for the runtime's SurfaceTexture (runtime/media/um_surface.c), whose external texture holds the
decoded YUV 4:2:0 picture: the conversion to RGB happens in the shader, with the coefficients
Unity's desktop players use for decoded video, so a clip looks as it does there.

The Shader object is built on the data's own Hidden/VideoDecode (its layout is that Unity
version's): name and passes replaced, the two passes made from its FLIP_RGBA_TO_RGBA and
FLIP_RGBASPLIT_TO_RGBA (vertex inputs, parameters _MainTex / _MainTex_ST / _AlphaParam and render
state as Unity's video passes have them), one GLES3 program each, no other platform. It goes into
unity_builtin_extra under a free path id, and globalgamemanagers' ScriptMapper gets its name.
"""
import copy
import os
import re

from .shaders import blob as B
from .shaders import convert as C

NAME = 'Hidden/VideoDecodeAndroid'
TEMPLATE = 'Hidden/VideoDecode'
# (pass of the new shader, desktop pass it is made from)
PASSES = (('RGBAExternal_To_RGBA', 'FLIP_RGBA_TO_RGBA'), ('RGBASplitExternal_To_RGBA', 'FLIP_RGBASPLIT_TO_RGBA'))
GLSL = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'video_decode_android.shader')
PREFERRED_PATH_ID = 16001       # next to Hidden/VideoDecode's 16000 (any free id works)


def programs():
    """{pass name: GLSL program source (both stages, Unity's #ifdef VERTEX / FRAGMENT form)}"""
    text = open(GLSL, encoding='utf-8').read()
    out = {}
    for m in re.finditer(r'^//// pass (\S+)\n(.*?)(?=^//// pass |\Z)', text, re.S | re.M):
        out[m.group(1)] = m.group(2).strip() + '\n'
    missing = [p for p, _ in PASSES if p not in out]
    if missing:
        raise ValueError(f'{GLSL}: no program for {missing}')
    return out


def _pass_name(p):
    st = p.get('m_State') or {}
    return st.get('m_Name') or p.get('m_Name') or ''


def build(template_tree):
    """The Hidden/VideoDecodeAndroid Shader typetree made from a Hidden/VideoDecode one."""
    t = copy.deepcopy(template_tree)
    pf = t['m_ParsedForm']
    if pf['m_Name'] != TEMPLATE:
        raise ValueError(f'template is {pf["m_Name"]}, not {TEMPLATE}')
    if B.PLATFORM_GLCORE not in t['platforms']:
        raise ValueError(f'{TEMPLATE} has no OpenGL Core programs to take the layout from')
    progs = programs()
    blobs = B.ShaderBlobs(t)
    src = C.platform_entries(t, B.PLATFORM_GLCORE, blobs)
    ss = pf['m_SubShaders'][0]
    by_name = {_pass_name(p).upper(): p for p in ss['m_Passes']}
    new_passes, entries, remap = [], [], {}

    def take(i):
        """the new blob index of GLCore entry i (copied once)"""
        if i not in remap:
            e = src[i]
            e = e.copy() if e is not None else None
            if isinstance(e, B.Entry):
                e.gpu_type = B.GPU_GLES3
            remap[i] = len(entries)
            entries.append(e)
        return remap[i]

    for new, old in PASSES:
        p = by_name.get(old)
        if p is None:
            raise ValueError(f'{TEMPLATE} has no pass {old}')
        p = copy.deepcopy(p)
        p['m_State']['m_Name'] = new.upper()
        if 'm_Name' in p and isinstance(p['m_Name'], str) and p['m_Name']:
            p['m_Name'] = new.upper()
        code_entries = set()
        for st in C.STAGES:
            prog = p.get(st)
            if not prog:
                continue
            lists = []
            if prog.get('m_SubPrograms') is not None:
                lists.append((prog['m_SubPrograms'], None))
            players = prog.get('m_PlayerSubPrograms')
            if players:
                for tier, lst in enumerate(players):
                    lists.append((lst, prog['m_ParameterBlobIndices'][tier]))
            for lst, pbi in lists:
                keep = [k for k, sp in enumerate(lst) if sp['m_GpuProgramType'] == B.GPU_GLCORE32]
                subs = []
                for k in keep:
                    sp = copy.deepcopy(lst[k])
                    sp['m_GpuProgramType'] = B.GPU_GLES3
                    sp['m_BlobIndex'] = take(lst[k]['m_BlobIndex'])
                    if st == 'progVertex':
                        code_entries.add(sp['m_BlobIndex'])
                    subs.append(sp)
                if pbi is not None:
                    pbi[:] = [take(pbi[k]) if pbi[k] >= 0 else pbi[k] for k in keep]
                lst[:] = subs
        if not code_entries:
            raise ValueError(f'{TEMPLATE} pass {old}: no OpenGL Core 3.2 program')
        for i in code_entries:
            e = entries[i]
            if not isinstance(e, B.Entry):
                raise ValueError(f'{TEMPLATE} pass {old}: program entry {i} does not parse')
            e.code = progs[new].encode('utf-8')
            e.keywords = []
            if e.local_keywords is not None:
                e.local_keywords = []
        if isinstance(p.get('m_Platforms'), list):
            p['m_Platforms'] = [B.PLATFORM_GLES3PLUS]
        new_passes.append(p)
    ss['m_Passes'] = new_passes
    pf['m_SubShaders'] = [ss]
    pf['m_Name'] = NAME
    if isinstance(t.get('m_Name'), str) and t['m_Name']:
        t['m_Name'] = NAME
    for plat in list(blobs.platforms):
        blobs.remove(plat)
    blobs.set_entries(B.PLATFORM_GLES3PLUS, entries, [0] * len(entries), 1)
    blobs.store()
    return t


def add_to_builtin_extra(sf, log=print):
    """Adds Hidden/VideoDecodeAndroid to a loaded unity_builtin_extra (UnityPy SerializedFile) that
    has Hidden/VideoDecode. Returns the new path id, or None (no template, or already there)."""
    template = None
    for pid, o in sf.objects.items():
        if o.type.name != 'Shader':
            continue
        name = o.read_typetree()['m_ParsedForm']['m_Name']
        if name == NAME:
            return None
        if name == TEMPLATE:
            template = o
    if template is None:
        return None
    tree = build(template.read_typetree())
    pid = PREFERRED_PATH_ID
    while pid in sf.objects:
        pid += 1
    new = copy.copy(template)
    new.path_id = pid
    new.data = None
    new.save_typetree(tree)
    sf.objects[pid] = new
    log(f'video: {NAME} added to unity_builtin_extra (path id {pid}, passes '
        f'{", ".join(p for p, _ in PASSES)})')
    return pid


def map_shader(ggm_sf, path_id, notes):
    """globalgamemanagers: ScriptMapper name -> the shader in unity_builtin_extra."""
    fid = next((i + 1 for i, e in enumerate(ggm_sf.externals) if e.path.lower().endswith('unity_builtin_extra')), None)
    if fid is None:
        notes.append(f'video: globalgamemanagers does not reference unity_builtin_extra; {NAME} not mapped')
        return False
    for o in ggm_sf.objects.values():
        if o.type.name != 'ScriptMapper':
            continue
        t = o.read_typetree()
        m = t['m_Shaders']
        lst = m['m_ObjectToName'] if isinstance(m, dict) else m
        ptr = {'m_FileID': fid, 'm_PathID': path_id}
        have = [i for i, e in enumerate(lst) if e[1] == NAME]
        # a desktop build lists the name with a null pointer (the editor maps every built-in
        # shader name; the other platforms' ones point nowhere)
        if have and (lst[have[0]][0].get('m_FileID') or lst[have[0]][0].get('m_PathID')):
            return False
        if have:
            lst[have[0]] = type(lst[have[0]])((ptr, NAME))
        else:
            lst.append((ptr, NAME))
        o.save_typetree(t)
        notes.append(f'ScriptMapper: {NAME} -> unity_builtin_extra:{path_id}')
        return True
    notes.append(f'video: no ScriptMapper in globalgamemanagers; {NAME} not mapped')
    return False


def path_id_for(builtin_extra_path):
    """The path id add_to_builtin_extra gives the shader in that file (None: it adds none)."""
    import UnityPy
    sf = UnityPy.load(builtin_extra_path).file
    names = {}
    for pid, o in sf.objects.items():
        if o.type.name == 'Shader':
            names[o.read_typetree()['m_ParsedForm']['m_Name']] = pid
    if NAME in names or TEMPLATE not in names:
        return None
    pid = PREFERRED_PATH_ID
    while pid in sf.objects:
        pid += 1
    return pid


def has_template(path):
    """True if a unity_builtin_extra file has Hidden/VideoDecode (byte search: cheap)."""
    try:
        with open(path, 'rb') as f:
            return TEMPLATE.encode() in f.read()
    except OSError:
        return False
