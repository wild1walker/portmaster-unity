"""peak_rss.py <out file> <command...>: runs the command and writes the peak resident set of its
processes (the largest one, in kB: getrusage's RUSAGE_CHILDREN) to <out file>; exits as it did."""
import resource
import subprocess
import sys

r = subprocess.call(sys.argv[2:])
with open(sys.argv[1], 'w') as f:
    f.write(f'{resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss}\n')
sys.exit(r)
