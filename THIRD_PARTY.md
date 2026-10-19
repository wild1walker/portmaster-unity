# Third-party code

Everything in this repository is MIT-licensed (LICENSE) except:

| what | where | licence |
|---|---|---|
| [libhybris](https://github.com/libhybris/libhybris) (git submodule, pinned commit) | `runtime/thirdparty/libhybris` | Apache 2.0 (some files carry other permissive licences; see the files and the `LICENSE.*` files in the submodule). `runtime/loader/build.sh` compiles a few of its `hybris/common` sources, and `runtime/loader/hybris_compat.c` includes its `hooks.c`, into the loader. |
| `jni.h` from the Android Open Source Project (libnativehelper), with every function-table slot marked `JNI_ABI` | `runtime/jni/include/jni.h` | Apache 2.0 (header in the file) |

A binary built from this repository (`ua_host`) contains libhybris code: distribute it with the
Apache 2.0 licence text and libhybris's notices.

## Not included

- Unity's Android player (`libunity.so`, `libmain.so`, `libmono.so`, the managed `UnityEngine*.dll`,
  `unity default resources`): proprietary, from Unity's editor installers, under Unity's terms. You
  unpack it yourself; this repository only reads it.
- Any game's files. The converter works on your own copy.

## Used at build or conversion time (not vendored)

Debian's armhf cross toolchains and libraries (Dockerfiles), SDL2, Mesa (test rig), Python
packages in `convert/requirements.txt` (UnityPy MIT, lz4 BSD, astc-encoder Apache 2.0, Pillow
MIT-CMU, texture2ddecoder MIT, pyelftools public domain).
