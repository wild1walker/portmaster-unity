# The guided_astc extension as a wheel (CPython's limited API: one build for 3.11 and later):
#   pip wheel --no-deps -w <out> convert/native/guided_astc
from setuptools import Extension, setup

setup(
    name='guided_astc',
    version='1.0',
    description='The cheap part of ASTC LDR encoding for pmunity\'s textures step',
    license='MIT',
    ext_modules=[Extension('guided_astc', ['guided_astc.c'], py_limited_api=True,
                           extra_compile_args=['-O2', '-pthread'], extra_link_args=['-pthread'])],
    options={'bdist_wheel': {'py_limited_api': 'cp311'}},
)
