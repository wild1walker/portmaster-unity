"""android_data_py.py <pmunity command...>: python -m pmunity, with the free space the golden test
gives ($PMU_TEST_FREE_BYTES: os.statvfs reports that much available, as pmu's statvfs does with
it), for the NO_SPACE case."""
import os
import runpy
import sys

free = os.environ.get('PMU_TEST_FREE_BYTES')
if free:
    real = os.statvfs

    def statvfs(path):
        st = real(path)
        return os.statvfs_result((st.f_bsize, 1, st.f_blocks, st.f_bfree, int(free), st.f_files, st.f_ffree,
                                  st.f_favail, st.f_flag, st.f_namemax))
    os.statvfs = statvfs
sys.argv = ['pmunity'] + sys.argv[1:]
runpy.run_module('pmunity', run_name='__main__', alter_sys=True)
