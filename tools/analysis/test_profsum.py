"""profsum.py and benchsum.py on synthetic recordings (no game or Unity files):
python3 -m unittest discover -s tools/analysis"""
import contextlib, io, os, sys, tempfile, unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profsum  # noqa: E402
import benchsum  # noqa: E402
import elfsyms  # noqa: E402

# libunity at 0x10000 (bias 0x10000), libc at 0x80000, JIT code at 0x200000; UnityMain 100 samples
# at 100 Hz over 1 s: 50 in libunity's "Update" called from a managed method, 20 in the managed
# method itself, 10 in libc memcpy (dladdr name), 10 blocked in futex, 10 Q
PROFILE = """ua_prof 2
arch armhf
hz 100
secs 1.000
depth 32 scan 16384
signal 39
mono yes
module 10000 20000 10000 libunity.so
module 80000 90000 80000 libc.so.6
sym 80010 memcpy 10
sym 80100 __futex_wait 4
jit 200000 100 M:HeroController:Update
jit 200100 80 W:UnityEngine.Transform:get_position_Injected
thread 0 100 UnityMain 100 0
thread 1 101 UnityGfxDeviceW 10 0
cpu 100 UnityMain 800000000 50000000 400 70 10
cpu 101 UnityGfxDeviceW 200000000 1000000 900 15 5
cpu 102 Job.Worker_0 10000000 0 20 1 0
stat cpu 100 0 20 280 0 0 0
stat cpu0 80 0 10 10 0 0 0
stat cpu1 20 0 10 70 0 0 0
stack 0 50 0 R ffff 11010 200041 1 200045 30001
stack 1 20 0 R ffff 200020 0 0 30001
stack 2 10 0 R ffff 80010 11011 1 11011 200041
stack 3 10 0 S f0 80100 0 0 11801
stack 4 10 0 Q ffff 0 0 0
stack 5 10 1 S f0 80100 0 0 12001
seq 0 0:0 10000:0 20000:0 30000:0 40000:0 50000:1 60000:2 70000:3 80000:4 90000:0
swap 0 16000 33000 50000
main 0 10000 100000 200000
end
"""


class ProfsumTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.path = os.path.join(self.d, 'prof-1.txt')
        with open(self.path, 'w') as f:
            f.write(PROFILE)
        # libunity's names without a symbol file: a table injected as ElfNames would load it
        self.orig = elfsyms.ElfNames._table

        def table(this, mod):
            if mod == 'libunity.so':
                rows = [(0x1000, 0x100, 'Behaviour::Update()'), (0x1800, 0x100, 'PlayerLoop()'), (0x2000, 0x10, 'GfxDevice::Present()')]
                return rows, [r[0] for r in rows]
            return [], []
        elfsyms.ElfNames._table = table

    def tearDown(self):
        elfsyms.ElfNames._table = self.orig

    def run_sum(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            profsum.main([self.path, *args])
        return out.getvalue()

    def test_parse(self):
        p = profsum.Profile(self.path)
        self.assertEqual(p.hz, 100)
        self.assertEqual(len(p.stacks), 6)
        self.assertEqual(len(p.seq[0]), 10)
        self.assertEqual(p.module(0x11010)[3], 'libunity.so')
        self.assertEqual(p.jit(0x200041)[2], 'M:HeroController:Update')
        self.assertIsNone(p.jit(0x300000))

    def test_views(self):
        out = self.run_sum('--top', '5')
        self.assertIn('== UnityMain (tid 100): 100 samples', out)
        self.assertIn('R 80.0%', out)
        self.assertIn('S futex 10.0%', out)
        self.assertIn('Q 10.0%', out)
        self.assertIn('62.5%  Unity engine (libunity)', out)            # 50 of 80 running samples
        self.assertIn('25.0%  managed', out)
        self.assertIn('Behaviour::Update', out)
        self.assertIn('libc.so.6:memcpy', out)
        # the managed method gets its own time and the engine time it calls
        self.assertIn('100.0%  M:HeroController:Update', out)
        self.assertIn('futex from PlayerLoop', out)
        self.assertIn('80.0% of the time', out)                         # schedstat 800 ms of 1 s
        self.assertIn('cpu0:  90.0% busy', out)

    def test_frames_and_slow(self):
        out = self.run_sum('--frames', '--thread', 'UnityMain')
        self.assertIn('UnityMain per frame (3 frames)', out)
        out = self.run_sum('--slow', '50', '--thread', 'UnityMain')
        # frames of 90 and 100 ms are slow (10..100 ms and 100..200 ms): samples 10000..90000
        self.assertIn('2 of 3 UnityMain frames are longer', out)
        self.assertIn('9 samples', out)

    def test_callers(self):
        out = self.run_sum('--callers', 'Update', '--thread', 'UnityMain')
        self.assertIn('callers of *Update*', out)

    def test_version1(self):
        # the first format: no <exact> field
        v1 = PROFILE.replace('ua_prof 2', 'ua_prof 1').replace('11010 200041 1 200045', '11010 200041 200045')
        with open(self.path, 'w') as f:
            f.write(v1)
        p = profsum.Profile(self.path)
        self.assertEqual(p.stacks[0]['rets'], [0x200045, 0x30001])
        self.assertEqual(p.stacks[0]['exact'], 0)

    def test_exact(self):
        out = self.run_sum('--exact', '--thread', 'UnityMain')
        self.assertIn('75.0% unwound', out)                              # stacks 0 and 2: 60 of 80
        self.assertNotIn('anon', out)                                    # 0x30001 was scanned, not unwound

    def test_offsets(self):
        out = self.run_sum('--offsets', '--thread', 'UnityMain')
        self.assertIn('M:HeroController:Update+0x20', out)


def snap(uptime, cpu0, main_run, main_wait, swpin, tis, gpu_ms, temp, es_ticks):
    return f"""=== /proc/uptime
{uptime} 100.00
=== /proc/stat
cpu  {sum(cpu0)} 0 0 0 0 0 0
cpu0 {' '.join(map(str, cpu0))}
=== /proc/meminfo
MemTotal:         973000 kB
MemAvailable:      61440 kB
SwapTotal:        498688 kB
SwapFree:         400000 kB
=== /proc/vmstat
pswpin {swpin}
pswpout 10
pgmajfault {swpin * 2}
=== /proc/4242/status
Name:	ua_host
VmRSS:	  800000 kB
=== /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
schedutil
=== /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
1512000
=== /sys/devices/system/cpu/cpu0/cpufreq/stats/time_in_state
1512000 {tis[0]}
1008000 {tis[1]}
=== /sys/class/devfreq/gpu/governor
simple_ondemand
=== /sys/class/devfreq/gpu/trans_stat
     From  :   To
           : 252000000 600000000   time(ms)
  252000000:         0         5   {gpu_ms[0]}
* 600000000:         4         0   {gpu_ms[1]}
Total transition : 9
=== /sys/class/thermal/thermal_zone0/type
cpu_thermal_zone
=== /sys/class/thermal/thermal_zone0/temp
{temp}
=== task 4243 UnityMain
{main_run} {main_wait} 1000
4243 (UnityMain) R 1 2 3 4 5 6 7 8 9 10 {main_run // 10000000} 5 0 0
=== task 4244 UnityGfxDeviceW
1000000000 0 500
4244 (UnityGfxDeviceW) S 1 2 3 4 5 6 7 8 9 10 50 50 0 0
=== procs
4242 (ua_host) S 1 2 3 4 5 6 7 8 9 10 1000 10 0 0
77 (emulationstation) S 1 2 3 4 5 6 7 8 9 10 {es_ticks} 0 0 0
"""


class BenchsumTest(unittest.TestCase):
    def make(self, name, fps):
        d = os.path.join(tempfile.mkdtemp(), name)
        os.makedirs(d)
        with open(os.path.join(d, 'result'), 'w') as f:
            f.write('ok\n')
        lines = ['[bench] name x', '[bench] env UA_MONO_OPTIONS=-O=abcrem',
                 '[fps] 59.0 fps, frame 16.9 avg / 18.0 p99 / 20.0 worst ms, late 0 hitch 0, gc 0 heap 64 MB (59 frames, 100 total, 10 s) rss 700 MB',
                 '[bench] measure start (2 s)']
        for _ in range(2):
            lines.append(f'[fps] {fps:.1f} fps, frame {1000 / fps:.1f} avg / 35.0 p99 / 36.0 worst ms, late 30 hitch 0, gc 0 heap 64 MB ({int(fps)} frames, 200 total, 12 s) rss 700 MB')
        lines.append('[bench] measure end')
        with open(os.path.join(d, 'run.log'), 'w') as f:
            f.write('\n'.join(lines) + '\n')
        with open(os.path.join(d, 'snap-0.txt'), 'w') as f:
            f.write(snap(100.0, [100, 0, 50, 850, 0, 0, 0], 10_000_000_000, 1_000_000_000, 5, (1000, 500), (100, 200), 60000, 100))
        with open(os.path.join(d, 'snap-1.txt'), 'w') as f:
            f.write(snap(102.0, [250, 0, 100, 850, 0, 0, 0], 11_900_000_000, 1_100_000_000, 25, (1150, 550), (300, 2000), 63000, 104))
        with open(os.path.join(d, 'freq.log'), 'w') as f:
            f.write('t=1 cpu=1512,1512,1512,1512 gpu=600 avail=60M swapfree=390M\nt=2 cpu=1008,1512,1512,1512 gpu=252 avail=60M swapfree=390M\n')
        return d

    def test_report_and_compare(self):
        a, b = self.make('base', 32.0), self.make('mono-abcrem', 34.0)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            benchsum.main([a, b, '--brief'])
        o = out.getvalue()
        self.assertIn('######## base: ok', o)
        self.assertIn('frames: 32.0 fps over 2 s', o)
        self.assertIn('UA_MONO_OPTIONS=-O=abcrem', o)
        self.assertIn('CPU clock: 1386 MHz avg (1512: 75%, 1008: 25%)', o)   # 150 + 50 ticks
        self.assertIn('gpu 565 MHz avg (600: 90%, 252: 10%)', o)            # 200 ms at 252, 1800 at 600
        self.assertIn('governors: cpu schedutil (max 1512 MHz); gpu=simple_ondemand', o)
        self.assertIn('cores busy: cpu0 100%', o)
        self.assertIn('UnityMain 95/5%', o)                                  # 1.9 s of 2 s on a CPU, 0.1 s waiting
        self.assertIn('swap-ins 20', o)
        self.assertIn('cpu_thermal_zone 60->63 C', o)
        self.assertIn('emulationstation 2%', o)
        self.assertIn('mono-abcrem            34.0   +6.2%', o)


if __name__ == '__main__':
    unittest.main()
