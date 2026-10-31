"""export_typetrees.py <out.json>: the type trees prepatch.html's conversion steps need, from UnityPy's
own database (UnityPy 1.25.4, its lzma.tpk through tpk_ar), for files built without type trees:
  classes   {class id: [[unity version (decimal string), node list index or null], ...]}  (ascending; the
            last entry at or below a file's version applies, as UnityPy's get_item_for_version)
  nodes     [[[level, type, name, byte size, meta flag], ...], ...]   (UnityPy's generate_node order)
  strings   {offset: common string}   (type tree blobs' shared strings, for files with type trees)
Run with the Python pmunity uses (UnityPy installed)."""
import json
import sys

from UnityPy.helpers import Tpk

CLASSES = {28: 'Texture2D', 213: 'Sprite', 687078895: 'SpriteAtlas', 43: 'Mesh', 89: 'Cubemap',
           187: 'Texture2DArray', 117: 'Texture3D', 188: 'CubemapArray', 142: 'AssetBundle',
           # the settings, audio and animation-clip steps
           141: 'BuildSettings', 13: 'InputManager', 11: 'AudioManager', 83: 'AudioClip', 150: 'PreloadData',
           74: 'AnimationClip',
           # android-data's globalgamemanagers edits
           30: 'GraphicsSettings', 116: 'MonoManager', 129: 'PlayerSettings', 47: 'QualitySettings', 72: 'ComputeShader',
           # the shaders step
           48: 'Shader'}


def flat(node, out):
    out.append([node.m_Level, node.m_Type, node.m_Name, node.m_ByteSize, node.m_MetaFlag])
    for c in node.m_Children:
        flat(c, out)
    return out


def main(out_path):
    tree = Tpk.get_typetree()
    lists, index, classes = [], {}, {}
    for cid in CLASSES:
        entries = []
        for version, cls in tree.ClassInformation[cid]:
            li = None
            if cls is not None and cls.ReleaseRootNode is not None:
                nodes = flat(Tpk.generate_node(cls), [])
                key = json.dumps(nodes)
                if key not in index:
                    index[key] = len(lists)
                    lists.append(nodes)
                li = index[key]
            entries.append([str(int(version)), li])
        classes[str(cid)] = entries
    strings = {str(k): v for k, v in Tpk.get_common_strings().items()}
    with open(out_path, 'w') as f:
        json.dump({'source': 'UnityPy 1.25.4 lzma.tpk', 'classes': classes, 'nodes': lists, 'strings': strings}, f,
                  separators=(',', ':'))
    print(f'{out_path}: {len(lists)} node lists, {sum(len(e) for e in classes.values())} version entries, '
          f'{len(strings)} common strings')


if __name__ == '__main__':
    main(sys.argv[1])
