"""build.py [<out dir>]: builds the guided_astc extension (guided_astc.c) for the Python running
this script with the system's C compiler (cc, or $CC), into <out dir> (default: this folder's
`out/`), for tests and Mac runs. The handheld gets it as a wheel (setup.py, built in the
manylinux aarch64 container with the port's other Python packages). Prints the folder to put on
sys.path."""
import os
import subprocess
import sys
import sysconfig

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'guided_astc.c')


def build(out=None, quiet=False):
    """Compiles the module into out (if it is older than the source); returns out."""
    out = out or os.path.join(HERE, 'out')
    os.makedirs(out, exist_ok=True)
    so = os.path.join(out, 'guided_astc' + ('.pyd' if os.name == 'nt' else '.abi3.so'))
    if os.path.exists(so) and os.path.getmtime(so) >= os.path.getmtime(SRC):
        return out
    cc = os.environ.get('CC', 'cc').split()
    flags = ['-O2', '-shared', '-fPIC', '-pthread', '-I' + sysconfig.get_paths()['include']]
    if sys.platform == 'darwin':
        flags += ['-undefined', 'dynamic_lookup']
    subprocess.run(cc + flags + [SRC, '-o', so + '.tmp', '-lm'], check=True,
                   stdout=subprocess.DEVNULL if quiet else None)
    os.replace(so + '.tmp', so)
    return out


if __name__ == '__main__':
    print(build(sys.argv[1] if len(sys.argv) > 1 else None))
