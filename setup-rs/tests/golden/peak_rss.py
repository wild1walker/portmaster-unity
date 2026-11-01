"""peak_rss.py <out file> <command...>: runs the command and writes to <out file> the peak resident set
(kB) of the command's own process (its VmHWM, read until it exits) and of the largest of all its
processes (getrusage's RUSAGE_CHILDREN: a tool it runs, e.g. the code patch, included); exits as
the command did."""
import resource
import subprocess
import sys
import time

p = subprocess.Popen(sys.argv[2:])
own = 0
while p.poll() is None:
    try:
        with open(f'/proc/{p.pid}/status') as f:
            for line in f:
                if line.startswith('VmHWM:'):
                    own = max(own, int(line.split()[1]))
    except OSError:
        pass
    time.sleep(0.1)
with open(sys.argv[1], 'w') as f:
    f.write(f'{own} {resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss}\n')
sys.exit(p.returncode)
