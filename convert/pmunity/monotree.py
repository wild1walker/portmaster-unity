"""Type trees of MonoBehaviour data from the game's own assemblies (pure Python).

A desktop build's serialized files carry no type trees, and UnityPy reads a MonoBehaviour's script
fields only with a tree for its script's class. This module builds one from the class's metadata
in the game's Managed/ assemblies (clrmeta.py), by Unity's serialization rules:

  - the fields of the class and of its base classes (base first, up to MonoBehaviour /
    ScriptableObject), in declaration order, that are instance fields, not const, not readonly,
    not [NonSerialized], and public or [SerializeField];
  - of a serializable type: the primitives (bool, the integers, float, double, char), string, enums
    (as their underlying type), references to UnityEngine.Object classes (PPtr: file and path id),
    Unity's own value types (Vector2/3/4, Quaternion, Color, Color32, Rect, Matrix4x4, Bounds,
    LayerMask, the Int vectors, RectOffset, Hash128, ...), [Serializable] classes and structs of
    the game or its packages (inline, their fields by the same rules, at most 10 levels deep),
    arrays and List<T> of those (an array of arrays is not serialized); generic classes as field
    types from Unity 2020.1 (List<T> always);
  - 1- and 2-byte fields, strings and arrays are followed by padding to 4 bytes.

What it does not build (the object is then left alone, with the reason): [SerializeReference]
fields, AnimationCurve, Gradient, GUIStyle and the other native classes with their own layouts.
A tree is used only when it reads the object's bytes exactly, every one of them (read()), so a rule
this gets wrong for some class shows up as an object that is not read, never as a wrong edit: the
caller saves only objects read that way, with that same tree.

    gen = Generator('<Game>_Data/Managed', unity_version)
    tree, node = read(obj, gen)          # (None, why) when no exact tree
"""
import os
import struct

from . import clrmeta as CM

ALIGN = 0x4000                      # kAlignBytesFlag

# field attributes (ECMA-335 II.23.1.5)
F_ACCESS, F_PUBLIC, F_STATIC, F_INITONLY, F_LITERAL, F_NOTSERIALIZED = 0x7, 0x6, 0x10, 0x20, 0x40, 0x80
# type attributes (II.23.1.15)
T_INTERFACE, T_ABSTRACT, T_SERIALIZABLE = 0x20, 0x80, 0x2000
GENERICPARAM, TYPESPEC, MEMBERREF, CUSTOMATTRIBUTE, EXPORTEDTYPE = 0x2A, 0x1B, 0x0A, 0x0C, 0x27
MAX_DEPTH = 10

PRIM_TYPES = {CM.E_BOOL: ('bool', 1), CM.E_CHAR: ('UInt16', 2), CM.E_I1: ('SInt8', 1), CM.E_U1: ('UInt8', 1),
              CM.E_I2: ('SInt16', 2), CM.E_U2: ('UInt16', 2), CM.E_I4: ('int', 4), CM.E_U4: ('unsigned int', 4),
              CM.E_I8: ('SInt64', 8), CM.E_U8: ('UInt64', 8), CM.E_R4: ('float', 4), CM.E_R8: ('double', 8)}
_V = lambda *n: [('float', x) for x in n]                       # noqa: E731
_I = lambda *n: [('int', x) for x in n]                         # noqa: E731
# Unity's own value types, serialized natively: (type name, [(type, name) | (type name, name, fields)])
BUILTIN = {
    'UnityEngine.Vector2': ('Vector2f', _V('x', 'y')),
    'UnityEngine.Vector3': ('Vector3f', _V('x', 'y', 'z')),
    'UnityEngine.Vector4': ('Vector4f', _V('x', 'y', 'z', 'w')),
    'UnityEngine.Quaternion': ('Quaternionf', _V('x', 'y', 'z', 'w')),
    'UnityEngine.Color': ('ColorRGBA', _V('r', 'g', 'b', 'a')),
    'UnityEngine.Color32': ('ColorRGBA', [('unsigned int', 'rgba')]),
    'UnityEngine.Rect': ('Rectf', _V('x', 'y', 'width', 'height')),
    'UnityEngine.Matrix4x4': ('Matrix4x4f', _V(*[f'e{r}{c}' for r in range(4) for c in range(4)])),
    'UnityEngine.LayerMask': ('BitField', [('unsigned int', 'm_Bits')]),
    'UnityEngine.RenderingLayerMask': ('RenderingLayerMask', [('unsigned int', 'm_Bits')]),
    'UnityEngine.Bounds': ('AABB', [('Vector3f', 'm_Center', _V('x', 'y', 'z')), ('Vector3f', 'm_Extent', _V('x', 'y', 'z'))]),
    'UnityEngine.Vector2Int': ('int2_storage', _I('x', 'y')),
    'UnityEngine.Vector3Int': ('int3_storage', _I('x', 'y', 'z')),
    'UnityEngine.RectInt': ('RectInt', _I('x', 'y', 'width', 'height')),
    'UnityEngine.BoundsInt': ('BoundsInt', [('int3_storage', 'm_Position', _I('x', 'y', 'z')),
                                            ('int3_storage', 'm_Size', _I('x', 'y', 'z'))]),
    'UnityEngine.RectOffset': ('RectOffset', _I('m_Left', 'm_Right', 'm_Top', 'm_Bottom')),
    'UnityEngine.Hash128': ('Hash128', [('UInt8', f'bytes[{i}]') for i in range(16)]),
    'UnityEngine.Rendering.SphericalHarmonicsL2': ('SphericalHarmonicsL2', _V(*[f'sh[{i:2d}]' for i in range(27)])),
}
# Unity's classes with layouts of their own (not their managed fields): no tree is built
NATIVE = {'UnityEngine.AnimationCurve', 'UnityEngine.Gradient', 'UnityEngine.GUIStyle', 'UnityEngine.ExposedReference`1',
          'UnityEngine.LazyLoadReference`1', 'UnityEngine.PropertyName', 'UnityEngine.Keyframe',
          'UnityEngine.ParticleSystem/MinMaxCurve', 'UnityEngine.ParticleSystem/MinMaxGradient'}
STOP = {'UnityEngine.MonoBehaviour', 'UnityEngine.ScriptableObject', 'UnityEngine.Behaviour', 'UnityEngine.Component',
        'UnityEngine.Object', 'System.Object', 'System.ValueType'}
LISTS = {'System.Collections.Generic.List`1'}


class Unsupported(Exception):
    """No tree for this class (the reason)."""


class TypeDef:
    """One TypeDef row of an assembly: names, flags, base, generic parameters, fields."""

    def __init__(self, gen, asm, row):
        self.gen, self.asm, self.row = gen, asm, row
        t = asm.types()[row - 1]
        self.full, self.name, self.flags = t['full'], t['name'], t['flags']
        self.extends = t['extends']
        self.field_rows = range(*t['fields'])
        self.nparams = gen.generic_params(asm).get(row, 0)

    def __repr__(self):
        return f'<{self.full}>'

    def base(self):
        """The base type as a resolved signature ('type', TypeDef, args) or None."""
        tab, r = self.extends
        if not r:
            return None
        return self.gen.resolve_ref(self.asm, tab, r)

    def is_enum(self):
        b = self.base()
        return b is not None and _full(b) == 'System.Enum'


class Generator:
    """Trees of MonoBehaviour classes from the assemblies in `managed` (a folder of DLLs)."""

    def __init__(self, managed, unity=None):
        self.managed = managed
        self.unity = tuple(unity) if unity is not None else (2022, 3)
        self._asm, self._index, self._attrs, self._gparams = {}, {}, {}, {}
        self._cache = {}

    # ---- assemblies
    def assembly(self, name):
        """The clrmeta.Assembly named `name` (without .dll), or None."""
        if name in self._asm:
            return self._asm[name]
        a = None
        p = os.path.join(self.managed, name + '.dll')
        if os.path.isfile(p):
            try:
                a = CM.Assembly(p)
            except (CM.NotAssembly, ValueError, IndexError, struct.error):
                a = None
        self._asm[name] = a
        if a is not None:
            self._index[id(a)] = {t['full']: i + 1 for i, t in enumerate(a.types())}
        return a

    def find(self, asm_name, full):
        """TypeDef of `full` (namespace.Name, nested Outer/Inner) in that assembly, following type
        forwarders (netstandard and other facades), or None."""
        seen = set()
        while asm_name and asm_name not in seen:
            seen.add(asm_name)
            a = self.assembly(asm_name)
            if a is None:
                return None
            row = self._index[id(a)].get(full)
            if row:
                return self._typedef(a, row)
            asm_name = self._forwarded(a, full)
        return None

    def _typedef(self, asm, row):
        k = (id(asm), row)
        if k not in self._cache:
            self._cache[k] = TypeDef(self, asm, row)
        return self._cache[k]

    def _forwarded(self, a, full):
        for flags, tdid, name, ns, impl in a.rows(EXPORTEDTYPE):
            n = (a.string(ns) + '.' if a.string(ns) else '') + a.string(name)
            if n == full:
                tab, r = a.coded('Implementation', impl)
                if tab == CM.ASSEMBLYREF:
                    return a.string(a.rows(CM.ASSEMBLYREF)[r - 1][6])
        return None

    def generic_params(self, a):
        k = id(a)
        if k not in self._gparams:
            n = {}
            for num, flags, owner, name in a.rows(GENERICPARAM):
                tab, r = a.coded('TypeOrMethodDef', owner)
                if tab == CM.TYPEDEF:
                    n[r] = max(n.get(r, 0), num + 1)
            self._gparams[k] = n
        return self._gparams[k]

    def field_attributes(self, a):
        """{field row: {attribute type full names}} of an assembly."""
        k = id(a)
        if k in self._attrs:
            return self._attrs[k]
        out = {}
        mrefs = a.rows(MEMBERREF)
        for parent, typ, _ in a.rows(CUSTOMATTRIBUTE):
            ptab, prow = a.coded('HasCustomAttribute', parent)
            if ptab != CM.FIELD:
                continue
            ttab, trow = a.coded('CustomAttributeType', typ)
            name = None
            if ttab == MEMBERREF and 1 <= trow <= len(mrefs):
                ctab, crow = a.coded('MemberRefParent', mrefs[trow - 1][0])
                if ctab in (CM.TYPEDEF, CM.TYPEREF):
                    name = a.type_name(ctab, crow)
            elif ttab == CM.METHODDEF:
                t = a.type_of_method(trow)
                name = t['full'] if t else None
            if name:
                out.setdefault(prow, set()).add(name)
        self._attrs[k] = out
        return out

    # ---- signatures
    def resolve_ref(self, a, tab, row, args=()):
        """A TypeDefOrRef of assembly `a` as ('type', TypeDef, args), ('ext', full name) for a type
        outside the game's assemblies (System), or a parsed TypeSpec."""
        if tab == CM.TYPEDEF:
            return ('type', self._typedef(a, row), ())
        if tab == CM.TYPEREF:
            full = a.typeref_name(row)
            scope, name, ns = a.rows(CM.TYPEREF)[row - 1]
            stab, srow = a.coded('ResolutionScope', scope)
            if stab == CM.TYPEREF:                              # nested in another TypeRef
                outer = self.resolve_ref(a, CM.TYPEREF, srow)
                full = (outer[1].full if outer[0] == 'type' else outer[1]) + '/' + a.string(name)
                if outer[0] != 'type':
                    return ('ext', full)
                t = self.find(self._asm_name(outer[1].asm), full)
                return ('type', t, ()) if t else ('ext', full)
            if stab == CM.ASSEMBLYREF:
                asm = a.string(a.rows(CM.ASSEMBLYREF)[srow - 1][6])
                t = self.find(asm, full)
                return ('type', t, ()) if t else ('ext', full)
            t = self._index[id(a)].get(full)
            return ('type', self._typedef(a, t), ()) if t else ('ext', full)
        if tab == TYPESPEC:
            blob = a.blob(a.rows(TYPESPEC)[row - 1][0])
            return self.sig(a, blob, 0, args)[0]
        return ('ext', '?')

    def _asm_name(self, a):
        return next(n for n, x in self._asm.items() if x is a)

    def sig(self, a, b, o, args=()):
        """(type, offset): a signature type, with generic parameters replaced by `args`."""
        e = b[o]; o += 1
        while e in (CM.E_CMOD_REQD, CM.E_CMOD_OPT):
            _, o = a.cuint(o, b)
            e = b[o]; o += 1
        if e in PRIM_TYPES:
            return ('prim', e), o
        if e == CM.E_STRING:
            return ('string',), o
        if e in (CM.E_VALUETYPE, CM.E_CLASS):
            v, o = a.cuint(o, b)
            tab, row = a.coded('TypeDefOrRef', v)
            return self.resolve_ref(a, tab, row), o
        if e == CM.E_SZARRAY:
            t, o = self.sig(a, b, o, args)
            return ('array', t), o
        if e == CM.E_GENERICINST:
            o += 1                                              # CLASS / VALUETYPE
            v, o = a.cuint(o, b)
            tab, row = a.coded('TypeDefOrRef', v)
            n, o = a.cuint(o, b)
            targs = []
            for _ in range(n):
                t, o = self.sig(a, b, o, args)
                targs.append(t)
            base = self.resolve_ref(a, tab, row)
            if base[0] == 'type':
                return ('type', base[1], tuple(targs)), o
            return ('ext', base[1], tuple(targs)), o
        if e == CM.E_VAR:
            n, o = a.cuint(o, b)
            return (args[n] if n < len(args) else ('unsupported', 'generic parameter')), o
        if e == CM.E_MVAR:
            _, o = a.cuint(o, b)
            return ('unsupported', 'method generic parameter'), o
        if e == CM.E_ARRAY:
            t, o = self.sig(a, b, o, args)
            _, o = a.cuint(o, b)
            n, o = a.cuint(o, b)
            for _ in range(n):
                _, o = a.cuint(o, b)
            n, o = a.cuint(o, b)
            for _ in range(n):
                _, o = a.cuint(o, b)
            return ('unsupported', 'multidimensional array'), o
        if e in (CM.E_PTR, CM.E_BYREF):
            _, o = self.sig(a, b, o, args)
            return ('unsupported', 'pointer'), o
        if e == CM.E_OBJECT:
            return ('ext', 'System.Object'), o
        if e in (CM.E_I, CM.E_U):
            return ('unsupported', 'IntPtr'), o
        raise Unsupported(f'element type {e:#x}')

    # ---- serialization rules
    def is_unity_object(self, t):
        seen = 0
        while t is not None and seen < 32:
            if t.full == 'UnityEngine.Object':
                return True
            b = t.base()
            if b is None or b[0] != 'type':
                return b is not None and _full(b) == 'UnityEngine.Object'
            t = b[1]
            seen += 1
        return False

    def fields(self, td, args=()):
        """[(name, type)] of the serialized fields of a class and its bases (base first)."""
        chain = []
        t, targs = td, tuple(args)
        while t is not None and t.full not in STOP:
            chain.append((t, targs))
            b = t.base()
            if b is None or b[0] != 'type':
                break
            if b[2]:
                b = self.resolve_ref(t.asm, *t.extends, targs)    # the base's arguments in terms of ours
            t, targs = b[1], b[2]
        out = []
        for t, targs in reversed(chain):
            attrs = self.field_attributes(t.asm)
            rows = t.asm.rows(CM.FIELD)
            for r in t.field_rows:
                flags, name, sig = rows[r - 1]
                if flags & (F_STATIC | F_LITERAL | F_INITONLY | F_NOTSERIALIZED):
                    continue
                at = attrs.get(r, ())
                if 'UnityEngine.SerializeReference' in at:
                    raise Unsupported(f'{t.full}.{t.asm.string(name)}: [SerializeReference]')
                if (flags & F_ACCESS) != F_PUBLIC and 'UnityEngine.SerializeField' not in at:
                    continue
                b = t.asm.blob(sig)
                if not b or b[0] != 0x06:
                    continue
                typ, _ = self.sig(t.asm, b, 1, targs)
                out.append((t.asm.string(name), typ))
        return out

    def nodes(self, typ, name, level, depth=0, element=False):
        """Type tree node dicts for one field, or None when Unity does not serialize it."""
        k = typ[0]
        if k == 'prim':
            tn, size = PRIM_TYPES[typ[1]]
            return [_node(level, tn, name, size, ALIGN if size < 4 and not element else 0)]
        if k == 'string':
            return _string(level, name)
        if k == 'array':
            inner = typ[1]
            if inner[0] == 'array' or (inner[0] in ('type', 'ext') and _full(inner) in LISTS):
                return None                                     # arrays of arrays are not serialized
            sub = self.nodes(inner, 'data', level + 2, depth, element=True)
            if sub is None:
                return None
            return _vector(level, name, sub)
        if k == 'ext':
            full = typ[1]
            if full in LISTS and len(typ) > 2 and typ[2]:
                return self.nodes(('array', typ[2][0]), name, level, depth)
            if full in BUILTIN:
                return _builtin(level, name, *BUILTIN[full])
            if full in NATIVE:
                raise Unsupported(f'{name}: {full} (a native layout)')
            if full == 'UnityEngine.Object':
                return _pptr(level, name, 'Object')
            return None                                         # System types: not serialized
        if k == 'unsupported':
            return None
        td, args = typ[1], typ[2]
        full = td.full
        if full in LISTS:
            return self.nodes(('array', args[0]), name, level, depth) if args else None
        if full == 'UnityEngine.Object':
            return _pptr(level, name, 'Object')
        if full.startswith('System.'):
            return None                                         # (mscorlib's types, found in Managed/)
        if full in BUILTIN:
            return _builtin(level, name, *BUILTIN[full])
        if full in NATIVE:
            raise Unsupported(f'{name}: {full} (a native layout)')
        if td.is_enum():
            under = next((t for n, t in self._raw_fields(td) if n == 'value__'), None)
            if under is None or under[0] != 'prim':
                return None
            tn, size = PRIM_TYPES[under[1]]
            return [_node(level, tn, name, size, ALIGN if size < 4 and not element else 0)]
        if self.is_unity_object(td):
            return _pptr(level, name, '$' + td.name)
        if td.flags & (T_INTERFACE | T_ABSTRACT) or not td.flags & T_SERIALIZABLE:
            return None
        if (td.nparams or args) and self.unity < (2020, 1):
            return None                                         # generic field types: 2020.1+
        if depth >= MAX_DEPTH:
            raise Unsupported(f'{name}: {full}: more than {MAX_DEPTH} levels of classes')
        out = [_node(level, td.name.split('`')[0], name, -1, 0)]
        for fn, ft in self.fields(td, args):
            sub = self.nodes(ft, fn, level + 1, depth + 1)
            if sub:
                out += sub
        return out

    def _raw_fields(self, td):
        rows = td.asm.rows(CM.FIELD)
        for r in td.field_rows:
            flags, name, sig = rows[r - 1]
            b = td.asm.blob(sig)
            if b and b[0] == 0x06 and not flags & F_STATIC:
                yield td.asm.string(name), self.sig(td.asm, b, 1)[0]

    def class_nodes(self, assembly, full):
        """Node dicts of a script class's own serialized fields at level 1 (the MonoBehaviour base
        fields not included). Unsupported when the class is missing or has fields this cannot lay
        out."""
        td = self.find(assembly[:-4] if assembly.endswith('.dll') else assembly, full)
        if td is None:
            raise Unsupported(f'{full}: not in {assembly}')
        out = []
        for fn, ft in self.fields(td):
            sub = self.nodes(ft, fn, 1)
            if sub:
                out += sub
        return out

    def tree(self, base, assembly, full):
        """A TypeTreeNode: the MonoBehaviour base tree `base` (UnityPy's for the object's version)
        with the script class's fields after its own."""
        from UnityPy.helpers.TypeTreeNode import TypeTreeNode
        nodes = [dict(d) for d in base.to_dict_list()]
        nodes += self.class_nodes(assembly, full)
        for i, d in enumerate(nodes):
            d.pop('m_Children', None)
            d['m_Index'] = i
            d.setdefault('m_Version', 1)
            d.setdefault('m_TypeFlags', 0)
        return TypeTreeNode.from_list(nodes)


def _full(t):
    return t[1].full if t[0] == 'type' else t[1]


def _node(level, typ, name, size, flag=0, typeflags=0):
    return {'m_Level': level, 'm_Type': typ, 'm_Name': name, 'm_ByteSize': size, 'm_Version': 1,
            'm_TypeFlags': typeflags, 'm_MetaFlag': flag}


def _string(level, name):
    return [_node(level, 'string', name, -1, ALIGN), _node(level + 1, 'Array', 'Array', -1, ALIGN, 1),
            _node(level + 2, 'int', 'size', 4), _node(level + 2, 'char', 'data', 1)]


def _vector(level, name, sub):
    return [_node(level, 'vector', name, -1, 0), _node(level + 1, 'Array', 'Array', -1, ALIGN, 1),
            _node(level + 2, 'int', 'size', 4)] + sub


def _pptr(level, name, cls):
    return [_node(level, f'PPtr<{cls}>', name, 12), _node(level + 1, 'int', 'm_FileID', 4),
            _node(level + 1, 'SInt64', 'm_PathID', 8)]


def _builtin(level, name, typ, fields):
    out = [_node(level, typ, name, -1)]
    for f in fields:
        if len(f) == 3:
            out += _builtin(level + 1, f[1], f[0], f[2])
        else:
            size = {'float': 4, 'int': 4, 'unsigned int': 4, 'UInt8': 1}[f[0]]
            out.append(_node(level + 1, f[0], f[1], size))
    return out


def script_of(obj):
    """(assembly name, full class name) of a MonoBehaviour's script, or None."""
    try:
        m = obj.read(check_read=False)
        if not m.m_Script or not m.m_Script.m_PathID:
            return None
        s = m.m_Script.read()
    except Exception:  # noqa: BLE001 - an unreadable script reference: not ours to edit
        return None
    full = f'{s.m_Namespace}.{s.m_ClassName}' if s.m_Namespace else s.m_ClassName
    return s.m_AssemblyName, full


def read(obj, gen, script=None):
    """(tree dict, node) of a MonoBehaviour read with a tree from its script's class that reads
    every byte of it, else (None, the reason)."""
    script = script or script_of(obj)
    if script is None:
        return None, 'no script'
    try:
        node = gen.tree(obj._get_typetree_node(), *script)
    except Unsupported as ex:
        return None, str(ex)
    except (ValueError, IndexError, KeyError, struct.error, RecursionError) as ex:
        return None, f'{script[1]}: {type(ex).__name__}: {ex}'
    try:
        return obj.read_typetree(nodes=node), node
    except (ValueError, IndexError, KeyError, EOFError, OverflowError, MemoryError, UnicodeDecodeError,
            struct.error) as ex:
        return None, f'{script[1]}: the tree does not read the object exactly ({ex})'
