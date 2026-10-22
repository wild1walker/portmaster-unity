"""pmunity il2cpp for Unity 2023.1+ (il2cpp_mix.py): the comparison of two libil2cpp source trees
(metadata version, global-metadata.dat tables, the structures the generated C++ fills in), the
converter choice and the mixed folder, on hand-made headers (no Unity file is needed)."""
import json
import os
import tempfile
import unittest

from pmunity import il2cpp as I
from pmunity import il2cpp_bee as BEE
from pmunity import il2cpp_mix as M
from pmunity import unity_version as UV

METADATA_CPP = '''
bool il2cpp::vm::GlobalMetadata::Initialize(int32_t* imagesCount, int32_t* assembliesCount)
{
    IL2CPP_ASSERT(s_GlobalMetadataHeader->sanity == 0xFAB11BAF);
    IL2CPP_ASSERT(s_GlobalMetadataHeader->version == %d);
}
'''
METHOD_DEF = '''
typedef struct Il2CppMethodDefinition
{
    StringIndex nameIndex;   // the name
    TypeIndex returnType;
%s    uint16_t slot;
} Il2CppMethodDefinition;
'''
CLASS_INTERNALS = '''
typedef struct Il2CppCodeGenModule
{
    const char* moduleName;
    const uint32_t methodPointerCount;
%s} Il2CppCodeGenModule;

struct MethodInfo
{
    Il2CppMethodPointer methodPointer;
%s};

typedef enum Il2CppRGCTXDataType
{
    IL2CPP_RGCTX_DATA_INVALID,
    IL2CPP_RGCTX_DATA_TYPE,
} Il2CppRGCTXDataType;
'''


def write_lib(root, version=31, method_extra='', module_extra='', method_info_extra=''):
    """A libil2cpp tree with just what compat reads."""
    lib = os.path.join(root, 'il2cpp', 'libil2cpp')
    os.makedirs(os.path.join(lib, 'vm'), exist_ok=True)
    with open(os.path.join(lib, 'vm', 'GlobalMetadata.cpp'), 'w') as f:
        f.write(METADATA_CPP % version)
    with open(os.path.join(lib, 'vm', 'GlobalMetadataFileInternals.h'), 'w') as f:
        f.write(METHOD_DEF % method_extra)
    with open(os.path.join(lib, 'il2cpp-class-internals.h'), 'w') as f:
        f.write(CLASS_INTERNALS % (module_extra, method_info_extra))
    return lib


class DefinitionsTest(unittest.TestCase):
    def test_parse(self):
        d = M.definitions(CLASS_INTERNALS % ('    int x; /* a { brace */\n', ''))
        self.assertIn('Il2CppCodeGenModule', d)
        self.assertIn('MethodInfo', d)
        self.assertIn('Il2CppRGCTXDataType', d)
        self.assertNotIn('a { brace', d['Il2CppCodeGenModule'])     # (comments go)
        self.assertTrue(d['Il2CppCodeGenModule'].endswith('} Il2CppCodeGenModule;'))

    def test_nested_and_anonymous(self):
        d = M.definitions('typedef struct Il2CppType { union { void* dummy; int i; } data; unsigned int attrs : 16; } '
                          'Il2CppType;\ntypedef enum { A, B } Il2CppNoTag;\n')
        self.assertIn('union { void* dummy; int i; } data;', d['Il2CppType'])
        self.assertIn('Il2CppNoTag', d)

    def test_whitespace_and_comments_do_not_count(self):
        a = M.definitions('struct X {\n  int a; // one\n};')
        b = M.definitions('struct X { int a; };')
        self.assertEqual(a, b)

    def test_metadata_version(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertEqual(M.metadata_version(write_lib(t, 29)), 29)
            self.assertIsNone(M.metadata_version(os.path.join(t, 'nothing')))


class CompatTest(unittest.TestCase):
    def test_same(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            self.assertEqual(M.compat(write_lib(a), write_lib(b)), [])

    def test_runtime_only_structures_may_differ(self):
        # (MethodInfo is the runtime's own: Unity 6 added a field the generated code never fills in)
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            self.assertEqual(M.compat(write_lib(a), write_lib(b, method_info_extra='    uint8_t flag : 1;\n')), [])

    def test_metadata_version_differs(self):
        # 2022.3.23f1's il2cpp (metadata 29) against Unity 6000.0's libil2cpp (31, a field more in
        # every method definition): both are reported
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            why = M.compat(write_lib(a, 29), write_lib(b, 31, method_extra='    uint32_t returnParameterToken;\n'))
            self.assertEqual(len(why), 2)
            self.assertIn('writes 29, this libil2cpp reads 31', why[0])
            self.assertIn('Il2CppMethodDefinition', why[1])

    def test_emitted_structure_differs(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            why = M.compat(write_lib(a), write_lib(b, module_extra='    const void* more;\n'))
            self.assertEqual(len(why), 1)
            self.assertIn('Il2CppCodeGenModule', why[0])

    def test_converter_by_metadata(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertEqual(M.converter_for(write_lib(os.path.join(t, 'a'), 31)), '2022.3.62f3')
            self.assertEqual(M.converter_for(write_lib(os.path.join(t, 'b'), 29)), '2022.3.23f1')
            self.assertIsNone(M.converter_for(write_lib(os.path.join(t, 'c'), 35)))

    def test_needed(self):
        self.assertFalse(M.needed(UV.Version('2022.3.62f3')))
        self.assertTrue(M.needed(UV.Version('2023.1.8f1')))
        self.assertTrue(M.needed(UV.Version('6000.0.61f1')))

    def test_refusal(self):
        for v in ('2022.3.62f3', '2023.1.8f1', '2023.2.20f1', '6000.0.61f1', '6000.1.4f1', '6000.2.6f2'):
            self.assertIsNone(M.refusal(UV.Version(v)), v)
        self.assertIn('version 39', M.refusal(UV.Version('6000.3.16f1')))
        self.assertIn('version 108', M.refusal(UV.Version('6000.6.3f1')))
        self.assertIn('not been compared', M.refusal(UV.Version('6000.9.1f1')))

    def test_converter_files(self):
        self.assertTrue(M.converter_wanted('il2cpp/build/deploy/il2cpp.dll'))
        self.assertTrue(M.converter_wanted('il2cpp/libil2cpp/il2cpp-class-internals.h'))
        self.assertTrue(M.converter_wanted('il2cpp/libil2cpp/vm/GlobalMetadata.cpp'))
        self.assertFalse(M.converter_wanted('il2cpp/libil2cpp/vm/Class.cpp'))
        self.assertFalse(M.converter_wanted('il2cpp/external/bdwgc/alloc.c'))
        self.assertFalse(M.converter_wanted('MonoBleedingEdge/lib/mono/unityaot-linux/mscorlib.dll'))


class PrepareTest(unittest.TestCase):
    def home(self, t, data_version=31, conv_version=31, **kw):
        """<home>/<ver>/editor (the data version's il2cpp, with a native il2cpp.exe) and a full
        editor of the converter in the home."""
        ver, conv = '6000.0.58f2', '2022.3.62f3'
        editor = os.path.join(t, ver, 'editor')
        write_lib(editor, data_version, **kw)
        dep = os.path.join(editor, 'il2cpp', 'build', 'deploy')
        os.makedirs(os.path.join(dep, 'bee_backend', 'win-x64'))
        open(os.path.join(dep, 'il2cpp.exe'), 'w').close()
        os.makedirs(os.path.join(editor, 'il2cpp', 'external', 'bdwgc'))
        with open(os.path.join(editor, 'il2cpp', 'external', 'bdwgc', 'alloc.c'), 'w') as f:
            f.write('/* gc */')
        open(os.path.join(editor, 'il2cpp', 'il2cpp_root'), 'w').close()
        ce = os.path.join(t, conv, 'editor')
        write_lib(ce, conv_version)
        cdep = os.path.join(ce, 'il2cpp', 'build', 'deploy')
        os.makedirs(os.path.join(cdep, 'bee_backend', 'win-x64'))
        with open(os.path.join(cdep, 'il2cpp.dll'), 'w') as f:
            f.write('converter')
        with open(os.path.join(ce, '.complete'), 'w') as f:
            json.dump({'version': conv}, f)
        return ver, editor

    def test_mixed_folder(self):
        with tempfile.TemporaryDirectory() as t:
            ver, editor = self.home(t)
            mix, conv = M.prepare(UV.Version(ver), editor, t, log=lambda m: None)
            self.assertEqual(conv, '2022.3.62f3')
            self.assertEqual(mix, os.path.join(t, ver, 'mix-2022.3.62f3'))
            il = os.path.join(mix, 'il2cpp')
            # the data version's sources and root marker, the converter's .NET il2cpp
            self.assertTrue(os.path.isfile(os.path.join(il, 'il2cpp_root')))
            self.assertTrue(os.path.isfile(os.path.join(il, 'external', 'bdwgc', 'alloc.c')))
            self.assertEqual(M.metadata_version(os.path.join(il, 'libil2cpp')), 31)
            self.assertEqual(I.deploy_tool(mix, 'il2cpp'), os.path.join(il, 'build', 'deploy', 'il2cpp.dll'))
            self.assertFalse(os.path.exists(os.path.join(il, 'build', 'deploy', 'il2cpp.exe')))
            # the Bee stand-ins are pmunity's own files in the mixed folder, the converter's untouched
            self.assertTrue(BEE.uses_bee(mix))
            stub = os.path.join(il, 'build', 'deploy', 'bee_backend', 'linux-x64', 'bee_backend')
            with open(stub) as f:
                self.assertEqual(f.read(), BEE.STUB)
            self.assertFalse(os.path.exists(os.path.join(t, '2022.3.62f3', 'editor', 'il2cpp', 'build', 'deploy',
                                                         'bee_backend', 'linux-x64')))
            # made once: a second call keeps it
            mark = os.path.join(mix, '.complete')
            m0 = os.stat(mark).st_mtime_ns
            self.assertEqual(M.prepare(UV.Version(ver), editor, t, log=lambda m: None)[0], mix)
            self.assertEqual(os.stat(mark).st_mtime_ns, m0)
            # the native il2cpp of the data's own editor stays refused
            with self.assertRaises(SystemExit):
                I.deploy_tool(editor, 'il2cpp')

    def test_refused_with_the_differences(self):
        with tempfile.TemporaryDirectory() as t:
            ver, editor = self.home(t, module_extra='    const void* more;\n')
            with self.assertRaises(UV.Unsupported) as cm:
                M.prepare(UV.Version(ver), editor, t, log=lambda m: None)
            self.assertIn('Il2CppCodeGenModule', str(cm.exception))
            self.assertFalse(os.path.exists(os.path.join(t, ver, 'mix-2022.3.62f3')))

    def test_unknown_metadata_version(self):
        with tempfile.TemporaryDirectory() as t:
            ver, editor = self.home(t, data_version=35)
            with self.assertRaises(UV.Unsupported) as cm:
                M.prepare(UV.Version(ver), editor, t, log=lambda m: None)
            self.assertIn('version 35', str(cm.exception))


if __name__ == '__main__':
    unittest.main()
