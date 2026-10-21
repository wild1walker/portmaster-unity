"""Type trees UnityPy's bundled ones get wrong for some Unity versions.

A desktop build's main data has no type trees: UnityPy reads its objects with the trees it ships
(UnityPy 1.25.4: Unity 3.4 - 6000.7 alphas), taking the newest one not newer than the data's
version. A release that added a field after UnityPy's snapshot (6000.6.3f1's PlayerSettings has
`captureStartupLogsBaked`, which UnityPy's 6000.6 alpha tree lacks) then fails to read
("Expected to read 892 bytes, but only read 888 bytes"). The fields such releases added are known
from the players' own type tree code (libunity's `Transfer<GenerateTypeTreeTransfer>` functions,
read with tools/re/strrefs.py and the player's symbols); FIELDS lists them. A fixed tree is only
used when UnityPy's own one does not read the object and the fixed one reads it exactly (every
byte), so the table never changes data that UnityPy reads correctly.

    tree, node = read(obj)       # node: the tree used (None: UnityPy's own)
    save(obj, tree, node)
"""
# class name -> [(field it follows, type, name, aligned)]: fields Unity releases added after the
# type trees UnityPy ships
FIELDS = {
    # 6000.6.3f1 (PlayerSettingsState::Transfer: after insecureHttpOption). 6000.0.58f2 and
    # 6000.3.11f1 data are 4 bytes longer than UnityPy's trees too, and a 4-byte field fits there
    # only among the last fields (after organizationId; androidUseSwappy and the other fields
    # android_data edits come long before), so the same tree reads and writes them exactly.
    'PlayerSettings': [('insecureHttpOption', 'bool', 'captureStartupLogsBaked', True)],
}
ALIGN = 0x4000                      # kAlignBytesFlag
SIZES = {'bool': 1, 'UInt8': 1, 'SInt8': 1, 'char': 1, 'SInt16': 2, 'UInt16': 2, 'int': 4,
         'unsigned int': 4, 'float': 4, 'SInt64': 8, 'UInt64': 8, 'double': 8}


def _with_fields(node, fields):
    """A copy of a class tree with the given top-level fields inserted (None if a field it follows
    is missing, or every field is already there)."""
    nodes = node.to_dict_list()                 # pre-order, m_Level 0 = the class
    added = 0
    for after, typ, name, aligned in fields:
        top = [i for i, d in enumerate(nodes) if d['m_Level'] == 1]
        names = [nodes[i]['m_Name'] for i in top]
        if name in names:
            continue
        if after not in names:
            return None
        i = top[names.index(after)] + 1
        while i < len(nodes) and nodes[i]['m_Level'] > 1:      # past the field's own children
            i += 1
        d = dict(nodes[top[0]])
        d.update(m_Type=typ, m_Name=name, m_ByteSize=SIZES[typ], m_Version=1, m_Children=[],
                 m_TypeFlags=0, m_MetaFlag=ALIGN if aligned else 0)
        nodes.insert(i, d)
        added += 1
    if not added:
        return None
    for k, d in enumerate(nodes):
        d['m_Index'] = k
        d.pop('m_Children', None)
    return type(node).from_list(nodes)


def candidates(obj):
    """Fixed trees for obj's class, most likely first."""
    fields = FIELDS.get(obj.type.name)
    if not fields:
        return []
    base = obj._get_typetree_node()
    out = []
    for n in range(len(fields), 0, -1):          # all the fields, then fewer (older releases)
        for k in range(len(fields) - n + 1):
            t = _with_fields(base, fields[k:k + n])
            if t is not None:
                out.append(t)
    return out


def read(obj):
    """(tree, node): the object read with UnityPy's tree, or else with a fixed one that reads all
    of its bytes (node is then that tree, to save with)."""
    try:
        return obj.read_typetree(), None
    except ValueError as ex:
        first = ex
    for node in candidates(obj):
        try:
            return obj.read_typetree(nodes=node), node
        except (ValueError, IndexError, KeyError):
            continue
    raise first


def save(obj, tree, node=None):
    if node is None:
        obj.save_typetree(tree)
    else:
        obj.save_typetree(tree, nodes=node)
