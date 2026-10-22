"""pmunity il2cpp, Unity 2021.2+: the stand-in for Bee's native backend (il2cpp_bee) and the run of
the build graph il2cpp's build program writes, on a hand-made graph in Bee.TundraBackend's
format (the node kinds, both kinds of dependencies, relative paths, resuming)."""
import json
import os
import subprocess
import tempfile
import threading
import unittest

from pmunity import il2cpp as I
from pmunity import il2cpp_bee as B


def local_shell(calls=None, lock=threading.Lock()):
    def sh(command, cwd, env):
        if calls is not None:
            with lock:
                calls.append(command)
        p = subprocess.run(['/bin/sh', '-c', command], cwd=cwd, env=dict(os.environ, **env),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        return p.returncode, p.stdout
    return sh


def graph(nodes, named):
    for i, n in enumerate(nodes):
        n.setdefault('Inputs', [])
        n.setdefault('Outputs', [])
        n['DebugActionIndex'] = i
    return {'Nodes': nodes, 'NamedNodes': named, 'DefaultNodes': [0], 'RelativePathToRoot': '.'}


class StubTest(unittest.TestCase):
    def test_install(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertFalse(B.install(t))                       # (2020.3 and older: no bee_backend folder)
            d = os.path.join(t, 'il2cpp', 'build', 'deploy')
            os.makedirs(os.path.join(d, 'bee_backend', 'win-x64'))
            self.assertTrue(B.install(t))
            stub = os.path.join(d, 'bee_backend', 'linux-x64', 'bee_backend')
            self.assertTrue(os.access(stub, os.X_OK))
            self.assertTrue(os.access(os.path.join(d, 'bee_backend', 'linux-arm64', 'bee_backend'), os.X_OK))
            self.assertTrue(os.access(os.path.join(d, 'il2cpp'), os.X_OK))
            m = os.stat(stub).st_mtime_ns
            B.install(t)
            self.assertEqual(os.stat(stub).st_mtime_ns, m)      # (unchanged: not rewritten)
            # no graph yet: 4 (Bee runs its build program); with the graph: 0 and the arguments noted
            st = os.path.join(t, 'state')
            os.makedirs(st)
            r = subprocess.run([stub, '--profile=p', '--stdin-canary', '--dagfile=x/bee.dag', 'FinalProgram'], cwd=st)
            self.assertEqual(r.returncode, 4)
            self.assertFalse(os.path.exists(os.path.join(st, B.ARGS_FILE)))
            r = subprocess.run([stub, '--stdin-canary', '--dagfile=x/bee.dag', '--continue-on-failure',
                                '--dagfilejson=x/bee.dag.json', 'FinalProgram'], cwd=st)
            self.assertEqual(r.returncode, 0)
            self.assertEqual(B.target_of(os.path.join(st, 'bee.dag.json')), 'FinalProgram')

    def test_target_default(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertEqual(B.target_of(os.path.join(t, 'bee.dag.json')), B.TARGET)


class GraphTest(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.mkdtemp()
        self.state = os.path.join(self.t, 'cache', 'buildstate')
        os.makedirs(self.state)
        self.out = os.path.join(self.t, 'out')
        src = os.path.join(self.t, 'a.c')
        with open(src, 'w') as f:
            f.write('int a;\n')
        self.src = src
        nodes = [
            {'Annotation': 'all_tundra_nodes', 'Action': '', 'ToBuildDependencies': [1, 2, 3, 4, 5, 6, 7, 8]},
            {'Annotation': 'WriteText dq/_dummy', 'ActionType': 'WriteTextFile', 'WriteTextFilePayload': '//dummy',
             'Outputs': ['dq/_dummy']},
            {'Annotation': 'C_Android_arm64 dq/a.o', 'Action': f'cp "{src}" dq/a.o && echo compiled a',
             'Inputs': [src, 'dq/_dummy'], 'Outputs': ['dq/a.o'], 'ToBuildDependencies': [1], 'Env': []},
            {'Annotation': 'MakeLump lu/1.lump.cpp', 'ActionType': 'WriteTextFile',
             'WriteTextFilePayload': '#include "x.cpp"\n', 'Outputs': ['lu/1.lump.cpp']},
            {'Annotation': 'C_Android_arm64 lu/b.o', 'Action': 'cat lu/1.lump.cpp > lu/b.o; echo "$GREETING" >> lu/b.o',
             'Inputs': ['lu/1.lump.cpp'], 'Outputs': ['lu/b.o'], 'ToBuildDependencies': [3],
             'Env': [{'Key': 'GREETING', 'Value': 'hello'}]},
            {'Annotation': 'WriteResponseFile rsp/1.rsp', 'ActionType': 'WriteTextFile',
             'WriteTextFilePayload': 'dq/a.o lu/b.o', 'Outputs': [os.path.join(self.state, 'rsp', '1.rsp')]},
            {'Annotation': 'Link_Android_arm64 ln/libx.so',
             'Action': f'cat $(cat "{self.state}/rsp/1.rsp") > ln/libx.so',
             'Inputs': ['dq/a.o', 'lu/b.o', os.path.join(self.state, 'rsp', '1.rsp')], 'Outputs': ['ln/libx.so'],
             'ToBuildDependencies': [2, 4, 5], 'Env': []},
            # the copy the target uses needs the symbols copy and the data next to it (ToUse)
            {'Annotation': 'CopyFiles out/libx.so', 'ActionType': 'CopyFiles', 'Inputs': ['ln/libx.so'],
             'Outputs': [os.path.join(self.out, 'libx.so')], 'ToBuildDependencies': [6], 'ToUseDependencies': [8]},
            {'Annotation': 'CopyFiles out/Data/m.dat', 'ActionType': 'CopyFiles', 'Inputs': [src],
             'Outputs': [os.path.join(self.out, 'Data', 'm.dat')]},
            {'Annotation': 'C_Android_arm64 unused.o', 'Action': 'exit 1', 'Outputs': ['unused.o']},
            {'Annotation': 'FinalProgram', 'Action': '', 'ToBuildDependencies': [7]},
        ]
        self.dag = os.path.join(self.state, 'bee.dag.json')
        with open(self.dag, 'w') as f:
            json.dump(graph(nodes, {'all_tundra_nodes': 0, 'FinalProgram': 10}), f)
        self.logs = []

    def tearDown(self):
        import shutil
        shutil.rmtree(self.t)

    def run_graph(self, calls=None, jobs=3):
        return B.run_graph(self.dag, local_shell(calls), jobs, self.logs.append, os.path.join(self.t, 'build.log'))

    def test_needed_and_order(self):
        g = B.Graph(self.dag)
        self.assertEqual(g.needed('FinalProgram'), {1, 2, 3, 4, 5, 6, 7, 8, 10})   # (not the unused compile)
        self.assertEqual(g.prerequisites(10), {7, 8})                             # (7's ToUse dependency)
        self.assertEqual(g.prerequisites(6), {2, 4, 5})

    def test_run(self):
        calls = []
        self.assertEqual(self.run_graph(calls), (3, 0))
        with open(os.path.join(self.out, 'libx.so')) as f:
            self.assertEqual(f.read(), 'int a;\n#include "x.cpp"\nhello\n')
        self.assertTrue(os.path.isfile(os.path.join(self.out, 'Data', 'm.dat')))
        self.assertEqual(len(calls), 3)
        self.assertTrue(calls[-1].startswith('cat $('))                    # (the link last)
        with open(os.path.join(self.t, 'build.log')) as f:
            self.assertIn('compiled a', f.read())
        # again: nothing to do; a changed source compiles and links again, the rest stays
        self.assertEqual(self.run_graph(), (0, 3))
        with open(self.src, 'w') as f:          # (written again, the same: as il2cpp converting again)
            f.write('int a;\n')
        os.utime(self.src, (2, 2))
        self.assertEqual(self.run_graph(), (0, 3))
        with open(self.src, 'w') as f:
            f.write('int a2;\n')
        os.utime(self.src, (1, 1))
        calls = []
        self.assertEqual(self.run_graph(calls), (2, 1))
        with open(os.path.join(self.out, 'libx.so')) as f:
            self.assertTrue(f.read().startswith('int a2;\n'))

    def test_failure(self):
        with open(self.dag) as f:
            d = json.load(f)
        d['Nodes'][4]['Action'] = 'echo "lu/1.lump.cpp:1: error: no x.cpp" >&2; exit 1'
        with open(self.dag, 'w') as f:
            json.dump(d, f)
        with self.assertRaises(SystemExit) as e:
            self.run_graph()
        self.assertIn('C_Android_arm64 lu/b.o failed (exit 1)', str(e.exception))
        self.assertIn('no x.cpp', str(e.exception))
        self.assertFalse(os.path.exists(os.path.join(self.state, 'ln', 'libx.so')))   # (never linked)
        # the compile that worked is not run again
        d['Nodes'][4]['Action'] = 'cat lu/1.lump.cpp > lu/b.o'
        with open(self.dag, 'w') as f:
            json.dump(d, f)
        self.assertEqual(self.run_graph(), (2, 1))

    def test_unwritten_output(self):
        with open(self.dag) as f:
            d = json.load(f)
        d['Nodes'][2]['Action'] = 'true'
        with open(self.dag, 'w') as f:
            json.dump(d, f)
        with self.assertRaises(SystemExit) as e:
            self.run_graph(jobs=1)
        self.assertIn('did not write dq/a.o', str(e.exception))


class PayloadTest(unittest.TestCase):
    def test_write_file(self):
        """2022.x: WriteFile nodes' bytes in the graph's payloads file; an input written by another
        node is a dependency even without ToBuildDependencies"""
        with tempfile.TemporaryDirectory() as t:
            pay = os.path.join(t, 'bee.dag.payloads')
            body = b'//Generated lump file\n#include "a.cpp"\n'
            head = b'Payload for "MakeLump lu/1.lump.cpp"\n\n'
            with open(pay, 'wb') as f:
                f.write(head + body + bytes([0, 0, 0, 0, 10, 10]))
            nodes = [
                {'Annotation': 'C lu/1.o', 'Action': 'cp lu/1.lump.cpp lu/1.o', 'Inputs': ['lu/1.lump.cpp'],
                 'Outputs': ['lu/1.o'], 'OverwriteOutputs': False},
                {'Annotation': 'MakeLump lu/1.lump.cpp', 'ActionType': 'WriteFile', 'PayloadOffset': len(head),
                 'PayloadLength': len(body), 'PayloadDebugContentSnippet': '//Generated', 'Outputs': ['lu/1.lump.cpp']},
                {'Annotation': 'FinalProgram', 'Action': '', 'ToBuildDependencies': [0]},
            ]
            d = graph(nodes, {'FinalProgram': 2})
            d['PayloadsFile'] = pay
            dag = os.path.join(t, 'bee.dag.json')
            with open(dag, 'w') as f:
                json.dump(d, f)
            self.assertEqual(B.Graph(dag).prerequisites(0), {1})
            self.assertEqual(B.run_graph(dag, local_shell(), 2, lambda m: None, os.path.join(t, 'build.log')), (1, 0))
            with open(os.path.join(t, 'lu', '1.o'), 'rb') as f:
                self.assertEqual(f.read(), body)

    def test_rpc_refused(self):
        with tempfile.TemporaryDirectory() as t:
            dag = os.path.join(t, 'bee.dag.json')
            with open(dag, 'w') as f:
                json.dump(graph([{'Annotation': 'CSharp x', 'ActionType': 'RunRPCCommand'},
                                 {'Annotation': 'FinalProgram', 'Action': '', 'ToBuildDependencies': [0]}],
                                {'FinalProgram': 1}), f)
            with self.assertRaises(SystemExit) as e:
                B.run_graph(dag, local_shell(), 1, lambda m: None, os.path.join(t, 'build.log'))
            self.assertIn('RunRPCCommand', str(e.exception))


class NdkApiTest(unittest.TestCase):
    def test_api_22(self):
        """Unity 2021.x / 2022.x's Bee builds link for API 22: its libraries come with r21d / r23b"""
        self.assertEqual(I.ndk_apis('r21d', 'arm64-v8a'), [21, 22])
        self.assertEqual(I.ndk_apis('r23b', 'armeabi-v7a'), [19, 22])
        self.assertEqual(I.ndk_apis('r19', 'arm64-v8a'), [21])
        llvm = 'android-ndk-r21d/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/'
        self.assertTrue(I.ndk_wanted(llvm + '22/libc.so', 'r21d', 'arm64-v8a'))
        self.assertTrue(I.ndk_wanted(llvm + '21/libc.so', 'r21d', 'arm64-v8a'))
        self.assertIsNone(I.ndk_wanted(llvm + '23/libc.so', 'r21d', 'arm64-v8a'))
        self.assertIsNone(I.ndk_wanted(llvm.replace('r21d', 'r19') + '22/libc.so', 'r19', 'arm64-v8a'))
        self.assertIsNone(I.ndk_wanted(llvm + '21/libc.so', 'r21d', 'arm64-v8a', [22]))

    def test_shims(self):
        with tempfile.TemporaryDirectory() as t:
            I.install_ndk_shims(t, 'arm64-v8a')
            llvm = os.path.join(t, 'toolchains', 'llvm', 'prebuilt', 'linux-x86_64')
            for rel in ('bin/clang++', 'aarch64-linux-android/bin/objcopy', 'aarch64-linux-android/bin/strip',
                        'bin/llvm-objcopy', 'bin/llvm-strip', 'bin/llvm-ar', 'bin/aarch64-linux-android-objcopy'):
                self.assertTrue(os.access(os.path.join(llvm, rel), os.X_OK), rel)
            p = os.path.join(llvm, 'bin', 'clang++')
            m = os.stat(p).st_mtime_ns
            I.install_ndk_shims(t, 'arm64-v8a')
            self.assertEqual(os.stat(p).st_mtime_ns, m)


if __name__ == '__main__':
    unittest.main()
