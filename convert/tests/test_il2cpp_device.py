"""pmunity il2cpp --on-device (il2cpp_device.py): where the Debian packages' files go, the ELF
reader, .deb unpacking, the toolchain folder's finishing (sonames, loader copies, wrappers, the
library check), the compiler job slots, the memory-based job count and the pins. On hand-made
files; nothing is downloaded."""
import io
import json
import lzma
import os
import shutil
import subprocess
import tarfile
import tempfile
import time
import struct
import unittest

from pmunity import il2cpp_device as D
from pmunity import il2cpp_device_lock as LOCK


def elf(soname=None, needed=()):
    """A minimal 64-bit ELF file with a PT_DYNAMIC: DT_NEEDED entries and a DT_SONAME."""
    strs = b'\0'
    idx = {}
    for n in ([soname] if soname else []) + list(needed):
        idx[n] = len(strs)
        strs += n.encode() + b'\0'
    tags = [(1, idx[n]) for n in needed] + ([(14, idx[soname])] if soname else [])
    phoff, phnum = 64, 2
    dyn_off = phoff + 56 * phnum
    tags += [(5, 0), (10, len(strs)), (0, 0)]           # DT_STRTAB (patched below), DT_STRSZ, DT_NULL
    str_off = dyn_off + 16 * len(tags)
    tags[-3] = (5, str_off)                              # vaddr == file offset (one PT_LOAD at 0)
    dyn = b''.join(struct.pack('<qQ', t, v) for t, v in tags)
    size = str_off + len(strs)
    head = b'\x7fELF' + bytes([2, 1, 1]) + bytes(9) + struct.pack('<HHIQQQIHHHHHH', 3, 183, 1, 0, phoff, 0, 0, 64, 56,
                                                                  phnum, 64, 0, 0)
    ph = struct.pack('<IIQQQQQQ', 1, 5, 0, 0, 0, size, size, 0x1000) + \
        struct.pack('<IIQQQQQQ', 2, 6, dyn_off, dyn_off, dyn_off, len(dyn), len(dyn), 8)
    return head + ph + dyn + strs


def deb(path, files):
    """A .deb (ar archive with an xz data.tar) holding files {name: bytes, or ('link', target)}."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w') as t:
        for name, data in files.items():
            ti = tarfile.TarInfo('./' + name)
            if isinstance(data, tuple):
                ti.type, ti.linkname = tarfile.SYMTYPE, data[1]
                t.addfile(ti)
            else:
                ti.size, ti.mode = len(data), 0o755 if name.endswith(('clang', 'mono-sgen')) else 0o644
                t.addfile(ti, io.BytesIO(data))
    members = [('debian-binary', b'2.0\n'), ('control.tar.xz', lzma.compress(b'')), ('data.tar.xz', lzma.compress(buf.getvalue()))]
    with open(path, 'wb') as f:
        f.write(b'!<arch>\n')
        for n, d in members:
            f.write(f'{n + "/":<16}{0:<12}{0:<6}{0:<6}{"100644":<8}{len(d):<10}`\n'.encode())
            f.write(d + (b'\n' if len(d) & 1 else b''))


class PlaceTest(unittest.TestCase):
    def test_paths(self):
        p = D.place
        self.assertEqual(p('./lib/aarch64-linux-gnu/ld-linux-aarch64.so.1'), 'lib/ld-linux-aarch64.so.1')
        self.assertEqual(p('./usr/lib/aarch64-linux-gnu/libstdc++.so.6.0.30'), 'lib/libstdc++.so.6.0.30')
        self.assertEqual(p('./usr/lib/aarch64-linux-gnu/libLLVM-14.so.1'), 'lib/libLLVM-14.so.1')
        self.assertEqual(p('./usr/lib/llvm-14/lib/libclang-cpp.so.14'), 'lib/libclang-cpp.so.14')
        self.assertEqual(p('./usr/lib/llvm-14/bin/clang'), 'llvm/bin/clang')
        self.assertEqual(p('./usr/lib/llvm-14/bin/lld'), 'llvm/bin/lld')
        self.assertIsNone(p('./usr/lib/llvm-14/bin/llvm-exegesis'))
        self.assertEqual(p('./usr/lib/llvm-14/lib/clang/14.0.6/include/arm_neon.h'), 'llvm/lib/clang/14.0.6/include/arm_neon.h')
        self.assertIsNone(p('./usr/lib/llvm-14/lib/clang/14.0.6/include/riscv_vector.h'))
        self.assertIsNone(p('./usr/lib/llvm-14/lib/clang/14.0.6/lib/linux/libclang_rt.asan-aarch64.so'))
        self.assertEqual(p('./usr/bin/mono-sgen'), 'mono/bin/mono-sgen')
        self.assertEqual(p('./usr/lib/mono/4.5/mscorlib.dll'), 'mono/lib/mono/4.5/mscorlib.dll')
        self.assertEqual(p('./usr/lib/mono/gac/System/4.0.0.0__b77a5c561934e089/System.dll'),
                         'mono/lib/mono/gac/System/4.0.0.0__b77a5c561934e089/System.dll')
        self.assertEqual(p('./usr/lib/mono/4.5/Facades/netstandard.dll'), 'mono/lib/mono/4.5/Facades/netstandard.dll')
        self.assertEqual(p('./usr/lib/libmono-native.so.0.0.0'), 'mono/lib/libmono-native.so')
        self.assertEqual(p('./etc/mono/config'), 'mono/etc/mono/config')
        self.assertEqual(p('./etc/mono/4.5/machine.config'), 'mono/etc/mono/4.5/machine.config')
        self.assertIsNone(p('./etc/mono/4.5/web.config'))
        self.assertIsNone(p('./usr/lib/aarch64-linux-gnu/libicutest.so.72.1'))
        self.assertIsNone(p('./lib/aarch64-linux-gnu/libmemusage.so'))
        self.assertIsNone(p('./usr/lib/aarch64-linux-gnu/gconv/UTF-16.so'))
        self.assertIsNone(p('./usr/share/doc/libc6/copyright'))

    def test_groups(self):
        from pmunity import il2cpp as I
        real = I.assembly_refs
        I.assembly_refs = lambda p: open(p).read().split()          # (stand-in: the file lists its references)
        try:
            with tempfile.TemporaryDirectory() as d:
                def tools(**refs):
                    for n, r in refs.items():
                        with open(os.path.join(d, n.replace('_', '.')), 'w') as f:
                            f.write(r)
                    return [os.path.join(d, 'UnityLinker.exe'), os.path.join(d, 'il2cpp.exe')]
                t = tools(UnityLinker_exe='mscorlib.dll System.dll', il2cpp_exe='mscorlib.dll')
                self.assertEqual(D.groups_for(t), ['base', 'llvm', 'mono'])               # 2017.4 / 2018.4
                t = tools(UnityLinker_exe='mscorlib.dll System.Web.Extensions.dll', Mono_Cecil_dll='netstandard.dll')
                self.assertEqual(D.groups_for(t), ['base', 'llvm', 'mono', 'mono-facades', 'mono-web'])   # 2019.x
                self.assertEqual(D.groups_for([os.path.join(d, 'UnityLinker.dll'), os.path.join(d, 'il2cpp.dll')]),
                                 ['base', 'llvm', 'dotnet'])                                # 2020.1+
        finally:
            I.assembly_refs = real
        self.assertEqual(D.place('./usr/lib/mono/4.5/Facades/netstandard.dll'), 'mono/lib/mono/4.5/Facades/netstandard.dll')
        self.assertTrue(D.ONLY['mono-devel'].match('./usr/lib/mono/4.5/Facades/netstandard.dll'))
        self.assertFalse(D.ONLY['mono-devel'].match('./usr/lib/mono/gac/Mono.Cecil/0.11.1.0__0738eb9f132ed756/Mono.Cecil.dll'))

    def test_lock(self):
        names = set()
        for p in LOCK.DEBIAN['packages']:
            self.assertIn(p['group'], ('base', 'llvm', 'mono', 'mono-facades', 'mono-web', 'dotnet'))
            self.assertRegex(p['sha256'], r'^[0-9a-f]{64}$')
            self.assertTrue(p['filename'].startswith('pool/main/') and p['filename'].endswith('.deb'), p)
            self.assertGreater(p['size'], 0)
            names.add(p['name'])
        for n in ('libc6', 'libllvm14', 'clang-14', 'lld-14', 'mono-runtime-sgen', 'libmono-corlib4.5-dll'):
            self.assertIn(n, names)
        self.assertRegex(LOCK.DEBIAN['snapshot'], r'^\d{8}T\d{6}Z$')
        self.assertRegex(LOCK.DOTNET['sha256'], r'^[0-9a-f]{64}$')
        self.assertTrue(LOCK.DOTNET['url'].startswith('https://builds.dotnet.microsoft.com/'))
        # the ids change with the pins and the groups
        self.assertNotEqual(D.lock_id(['base', 'llvm']), D.lock_id(['base', 'llvm', 'mono']))


class ElfTest(unittest.TestCase):
    def test_dynamic(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'x.so')
            with open(p, 'wb') as f:
                f.write(elf('libx.so.1', ['libc.so.6', 'libm.so.6']))
            self.assertEqual(D.elf_dynamic(p), ('libx.so.1', ['libc.so.6', 'libm.so.6']))
            with open(p, 'wb') as f:
                f.write(elf(None, ['libc.so.6']))
            self.assertEqual(D.elf_dynamic(p), (None, ['libc.so.6']))
            with open(p, 'wb') as f:
                f.write(b'#!/bin/sh\n')
            self.assertIsNone(D.elf_dynamic(p))


class FolderTest(unittest.TestCase):
    """A toolchain folder from hand-made packages: placed files, sonames, the loader copies, the
    wrappers and the check that names a missing library."""

    def make(self, d, extra_needed=()):
        debs = os.path.join(d, 'debs')
        os.makedirs(debs)
        deb(os.path.join(debs, 'libc6.deb'), {
            'lib/aarch64-linux-gnu/ld-linux-aarch64.so.1': elf('ld-linux-aarch64.so.1'),
            'lib/aarch64-linux-gnu/libc.so.6': elf('libc.so.6'),
            'lib/aarch64-linux-gnu/libnss_files.so.2': elf('libnss_files.so.2', ['libc.so.6']),
            'lib/aarch64-linux-gnu/libnss_dns.so.2': elf('libnss_dns.so.2', ['libc.so.6']),
            'lib/ld-linux-aarch64.so.1': ('link', 'aarch64-linux-gnu/ld-linux-aarch64.so.1'),
            'usr/share/doc/libc6/copyright': b'text'})
        deb(os.path.join(debs, 'libstdc++6.deb'), {
            'usr/lib/aarch64-linux-gnu/libstdc++.so.6.0.30': elf('libstdc++.so.6', ['libc.so.6']),
            'usr/lib/aarch64-linux-gnu/libstdc++.so.6': ('link', 'libstdc++.so.6.0.30')})
        deb(os.path.join(debs, 'llvm.deb'), {
            'usr/lib/llvm-14/bin/clang': elf(None, ['libstdc++.so.6', 'libc.so.6'] + list(extra_needed)),
            'usr/lib/llvm-14/bin/lld': elf(None, ['libc.so.6']),
            'usr/lib/llvm-14/bin/llvm-objcopy': elf(None, ['libc.so.6']),
            'usr/lib/llvm-14/bin/llvm-ar': elf(None, ['libc.so.6']),
            'usr/lib/llvm-14/bin/llvm-nm': elf(None, ['libc.so.6']),
            'usr/lib/llvm-14/bin/clang++': ('link', 'clang'),
            'usr/lib/llvm-14/lib/clang/14.0.6/include/stddef.h': b'/* */'})
        deb(os.path.join(debs, 'mono.deb'), {
            'usr/bin/mono-sgen': elf(None, ['libc.so.6']),
            'usr/lib/libmono-native.so.0.0.0': elf('libmono-native.so.0', ['libc.so.6']),
            'usr/lib/libMonoPosixHelper.so': elf(None, ['libc.so.6']),
            'usr/lib/mono/4.5/mscorlib.dll': b'MZ',
            'usr/lib/mono/gac/System/4.0.0.0__b77a5c561934e089/System.dll': b'MZ',
            'usr/lib/mono/4.5/System.dll': ('link', '../gac/System/4.0.0.0__b77a5c561934e089/System.dll'),
            'etc/mono/config': b'<configuration/>'})
        tc = os.path.join(d, 'tc')
        for n in sorted(os.listdir(debs)):
            D.unpack_deb(os.path.join(debs, n), tc)
        return tc

    def test_folder(self):
        with tempfile.TemporaryDirectory() as d:
            tc = self.make(d)
            D.finish(tc, ['base', 'llvm', 'mono'])
            lib = sorted(os.listdir(os.path.join(tc, 'lib')))
            self.assertEqual(lib, ['ld-linux-aarch64.so.1', 'libc.so.6', 'libnss_dns.so.2', 'libnss_files.so.2',
                                   'libstdc++.so.6'])                                         # sonames, no links
            for r, ds, fs in os.walk(tc):
                for n in fs + ds:
                    self.assertFalse(os.path.islink(os.path.join(r, n)), n)
            self.assertTrue(os.path.isfile(os.path.join(tc, 'mono', 'lib', 'mono', 'gac', 'System', '4.0.0.0__b77a5c561934e089', 'System.dll')))
            self.assertFalse(os.path.exists(os.path.join(tc, 'mono', 'lib', 'mono', '4.5', 'System.dll')))   # (a link)
            self.assertTrue(os.path.isfile(os.path.join(tc, 'mono', 'lib', 'libmono-native.so')))
            with open(os.path.join(tc, 'mono', 'bin', 'mono'), 'rb') as a, open(os.path.join(tc, 'lib', D.LOADER), 'rb') as b:
                self.assertEqual(a.read(), b.read())             # Mono's prefix comes from the loader's path
            for w in ('clang', 'clang++', 'ld.lld', 'llvm-objcopy', 'llvm-strip', 'llvm-ar', 'llvm-nm', 'mono'):
                text = open(os.path.join(tc, 'bin', w)).read()
                self.assertIn(f'"{tc}/lib/{D.LOADER}" --library-path "{tc}/lib"' if w != 'mono' else f'"{tc}/mono/bin/mono"', text)
            self.assertIn('--driver-mode=g++', open(os.path.join(tc, 'bin', 'clang++')).read())
            self.assertIn('--argv0 llvm-strip', open(os.path.join(tc, 'bin', 'llvm-strip')).read())
            self.assertTrue(os.path.isfile(os.path.join(tc, 'llvm', 'bin', 'ld.lld')))   # (clang: next to itself)
            self.assertFalse(os.path.exists(os.path.join(tc, 'dotnet')))
            D.finish(tc, ['base', 'llvm', 'mono'])                                  # (again: no change)
            self.assertEqual(sorted(os.listdir(os.path.join(tc, 'lib'))), lib)

    def test_missing_library(self):
        with tempfile.TemporaryDirectory() as d:
            tc = self.make(d, extra_needed=['libz3.so.4'])
            with self.assertRaises(SystemExit) as cm:
                D.finish(tc, ['base', 'llvm'])
            self.assertIn('libz3.so.4 (needed by llvm/bin/clang)', str(cm.exception))


class DownloadTest(unittest.TestCase):
    def test_file_urls(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, 'a.deb')
            with open(src, 'wb') as f:
                f.write(b'x' * 1000)
            sha = D.sha_file(src)
            dst = os.path.join(d, 'out.deb')
            D.download(['file://' + src], dst, sha, tries=1)
            self.assertEqual(open(dst, 'rb').read(), b'x' * 1000)
            os.unlink(dst)
            with self.assertRaises(SystemExit):
                D.download(['file://' + src], dst, '0' * 64, tries=1)
            self.assertFalse(os.path.exists(dst) or os.path.exists(dst + '.part'))


@unittest.skipUnless(shutil.which('sh'), 'needs sh')
class JobSlotsTest(unittest.TestCase):
    """The compiler wrappers' slots: never more compilers at once than PMUNITY_JOBS, a big file
    alone, stale slots of killed runs taken over."""

    def run_many(self, slots, files, extra_env=None):
        with tempfile.TemporaryDirectory() as d:
            jd = os.path.join(d, 'jobs')
            os.makedirs(jd)
            script = os.path.join(d, 'cc')
            with open(script, 'w') as f:
                f.write('#!/bin/sh\n' + D.JOBS_SH + f'echo start >> "{d}/events"; sleep 0.3; echo end >> "{d}/events"\n'
                        'release\n')
            env = dict(os.environ, PMUNITY_JOBS=str(slots), PMUNITY_JOBS_DIR=jd, **(extra_env or {}))
            ps = [subprocess.Popen(['sh', script, '-c', f], env=env) for f in files]
            for p in ps:
                self.assertEqual(p.wait(), 0)
            running = peak = 0
            with open(os.path.join(d, 'events')) as f:
                events = f.read().split()
            for e in events:
                running += 1 if e == 'start' else -1
                peak = max(peak, running)
            self.assertEqual(os.listdir(jd), [])                   # every slot freed
            return peak

    def test_slots(self):
        self.assertEqual(self.run_many(1, ['a.cpp'] * 4), 1)
        self.assertLessEqual(self.run_many(2, ['a.cpp'] * 6), 2)

    def test_big_alone(self):
        with tempfile.TemporaryDirectory() as d:
            big = os.path.join(d, 'big.cpp')
            with open(big, 'wb') as f:
                f.write(b'x' * 3000)
            peak = self.run_many(3, [big, big, big], {'PMUNITY_IL2CPP_BIG_KB': '2'})
            self.assertEqual(peak, 1)

    def test_killed_noted(self):
        with tempfile.TemporaryDirectory() as d:
            jd, killed = os.path.join(d, 'jobs'), os.path.join(d, 'killed.log')
            os.makedirs(jd)
            script = os.path.join(d, 'cc')
            with open(script, 'w') as f:
                f.write('#!/bin/sh\n' + D.JOBS_SH + 'sh -c \'kill -9 $$\'\nst=$?\nrelease\n' + D.KILLED_SH + 'exit $st\n')
            r = subprocess.run(['sh', script, '-c', 'Bulk_0.cpp'], timeout=20,
                               env=dict(os.environ, PMUNITY_JOBS='1', PMUNITY_JOBS_DIR=jd, PMUNITY_IL2CPP_KILLED=killed))
            self.assertEqual(r.returncode, 137)
            with open(killed) as f:
                self.assertTrue(f.read().startswith('137 Bulk_0.cpp '))
            self.assertEqual(os.listdir(jd), [])

    def test_stale_slot(self):
        with tempfile.TemporaryDirectory() as d:
            jd = os.path.join(d, 'jobs')
            os.makedirs(os.path.join(jd, '0'))
            p = subprocess.Popen(['sh', '-c', 'exit 0'])
            p.wait()
            with open(os.path.join(jd, '0', 'pid'), 'w') as f:
                f.write(str(p.pid))                                 # (a process that is gone)
            script = os.path.join(d, 'cc')
            with open(script, 'w') as f:
                f.write('#!/bin/sh\n' + D.JOBS_SH + 'echo ran\nrelease\n')
            out = subprocess.run(['sh', script], env=dict(os.environ, PMUNITY_JOBS='1', PMUNITY_JOBS_DIR=jd),
                                 capture_output=True, text=True, timeout=20)
            self.assertEqual(out.stdout.strip(), 'ran')


class JobsTest(unittest.TestCase):
    def test_auto(self):
        mb = lambda total, avail: {'MemTotal': total, 'MemAvailable': avail}
        self.assertEqual(D.auto_jobs(4, mb(973, 600)), 1)               # a 1 GB handheld
        self.assertEqual(D.auto_jobs(4, mb(973, 300)), 1)               # (never 0)
        self.assertEqual(D.auto_jobs(8, mb(8000, 7000)), 8)
        self.assertEqual(D.auto_jobs(4, mb(4000, D.RESERVE_MB + 2 * D.JOB_MB + 1)), 2)

    def test_progress(self):
        with tempfile.TemporaryDirectory() as d:
            cpp, ed = os.path.join(d, 'cpp'), os.path.join(d, 'editor')
            lib, gc = os.path.join(ed, 'il2cpp', 'libil2cpp', 'vm'), os.path.join(ed, 'il2cpp', 'external', 'boehmgc', 'extra')
            for p, n in ((cpp, 'Bulk_0.cpp'), (cpp, 'Bulk_1.cpp'), (lib, 'Class.cpp'), (gc, 'gc.c')):
                os.makedirs(p, exist_ok=True)
                with open(os.path.join(p, n), 'wb') as f:
                    f.write(b'x' * 1000)
            with open(os.path.join(lib, 'Class.h'), 'w') as f:
                f.write('')
            n, cost = D.sources_estimate(cpp, ed)
            self.assertEqual((n, cost), (4, 4 * (1000 + D.FILE_COST)))
            log = os.path.join(d, 'cc.log')
            with open(log, 'w') as f:
                f.write(f'clang++ -c -Os {cpp}/Bulk_0.cpp -o x.o\nclang++ -shared a.o b.o -o libil2cpp.so\nclang++ -c {cpp}/Bul')
            c = D.CompileLog(log).read()
            self.assertEqual((c.n, c.cost), (1, 1000 + D.FILE_COST))       # (a partial last line waits)
            with open(log, 'a') as f:
                f.write(f'k_1.cpp -o y.o\n')
            self.assertEqual(c.read().n, 2)

    def test_markers(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, 'in')
            os.makedirs(src)
            with open(os.path.join(src, 'a.dll'), 'wb') as f:
                f.write(b'1')
            h = D._digest(['tool', '--x'], src)
            self.assertFalse(D._done(d, 'step', h))
            D._mark(d, 'step', h)
            self.assertTrue(D._done(d, 'step', h))
            with open(os.path.join(src, 'a.dll'), 'wb') as f:
                f.write(b'2')
            self.assertNotEqual(D._digest(['tool', '--x'], src), h)     # changed inputs: run again
            self.assertNotEqual(D._digest(['tool', '--y'], src), D._digest(['tool', '--x'], src))


if __name__ == '__main__':
    unittest.main()
