# Third-party code

Everything in this repository is MIT-licensed (LICENSE) except:

| what | where | licence |
|---|---|---|
| [libhybris](https://github.com/libhybris/libhybris) (git submodule, pinned commit) | `runtime/thirdparty/libhybris` | Apache 2.0 (some files carry other permissive licences; see the files and the `LICENSE.*` files in the submodule). `runtime/loader/build.sh` compiles a few of its `hybris/common` sources, and `runtime/loader/hybris_compat.c` includes its `hooks.c`, into the loader. |
| `jni.h` from the Android Open Source Project (libnativehelper), with every function-table slot marked `JNI_ABI` | `runtime/jni/include/jni.h` | Apache 2.0 (header in the file) |

A binary built from this repository (`ua_host`) contains libhybris code: distribute it with the
Apache 2.0 licence text and libhybris's notices.

## Linked into `ua_host`, not vendored (video, `runtime/media`)

The media core links these decoders statically, from the build images' Debian / Ubuntu packages
(`runtime/media/docker/`); none of their source is in this repository. `runtime/media/build.sh`
copies each package's copyright file into `runtime/media/out/[arm64/]licenses/`: distribute those
with a `ua_host` built this way.

| what | version (armhf: Debian 12; arm64: Ubuntu 20.04) | licence | used for |
|---|---|---|---|
| [libvpx](https://chromium.googlesource.com/webm/libvpx) (`libvpx-dev`) | 1.12.0 / 1.8.2 | BSD-3-Clause (+ WebM project patent grant) | VP8 / VP9 decoding |
| [libvorbis](https://xiph.org/vorbis/) + [libogg](https://xiph.org/ogg/) (`libvorbis-dev`, `libogg-dev`) | 1.3.7 + 1.3.5 / 1.3.6 + 1.3.4 | BSD-3-Clause | Vorbis decoding |
| [libopus](https://opus-codec.org/) (`libopus-dev`) | 1.3.1 | BSD-3-Clause | Opus decoding |
| [OpenH264](https://github.com/cisco/openh264) (armhf: `libopenh264-dev`; arm64: Debian 12's source package built in the image) | 2.3.1 | BSD-2-Clause | H.264 decoding. Built from source, it carries no MPEG LA patent licence (Cisco's own binaries do); whether that matters is the distributor's call |

The tests also use the encoders of the same packages (libvpx, libvorbisenc, libopus, OpenH264) to
make their clips, and `runtime/media/build.sh test-ffmpeg` runs Debian's FFmpeg in a test image
(test patterns only; nothing of it is kept).

## Used at run time when the system has it (not shipped)

The system's [FFmpeg](https://ffmpeg.org/) `libavcodec` (LGPL 2.1+, or GPL depending on the
firmware's build), opened with `dlopen` by `runtime/media/um_ffmpeg.c` when present: AAC, MP3,
HEVC and some H.264. Nothing of FFmpeg is linked, compiled in or distributed.

The system's [libcurl](https://curl.se/) (curl licence, MIT-style), opened with `dlopen` by
`runtime/jni/up_http.c` only when `UA_HTTP=1` (Unity 2017.4 - 2018.4 web requests). The system's CA
bundle is read as data (runtime/jni/up_tls.c). Native plugins a port supplies (e.g. FMOD's or
Wwise's libraries, docs/plugins.md) come under their own licences and are never part of this
repository. The TLS tests (`runtime/jni/test/tls_test.sh`) run Debian's mbedTLS (Apache 2.0),
OpenSSL (Apache 2.0), Python and libcurl in a test image; nothing of them is kept.

## Not included

- Unity's Android player (`libunity.so`, `libmain.so`, `libmono.so`, the managed `UnityEngine*.dll`,
  `unity default resources`): proprietary, from Unity's editor installers, under Unity's terms. You
  unpack it yourself; this repository only reads it.
- Any game's files. The converter works on your own copy.

## Used at build or conversion time (not vendored)

Debian's armhf cross toolchains and libraries (Dockerfiles), SDL2, Mesa (test rig), Python
packages in `convert/requirements.txt` (UnityPy MIT, lz4 BSD, astc-encoder Apache 2.0, Pillow
MIT-CMU, texture2ddecoder MIT, pyelftools public domain).

[Mono.Cecil](https://github.com/jbevain/cecil) 0.11.6 (MIT, Copyright (c) 2008 - 2015 Jb Evain,
Copyright (c) 2008 - 2011 Novell, Inc.), a NuGet package `tools/asmpatch` is built with:
`out/linux-arm64/asmpatch` contains it and the .NET runtime (MIT; distribute that binary with
Mono.Cecil's licence and the runtime pack's `LICENSE.TXT` and `THIRD-PARTY-NOTICES.TXT`).

[HLSLcc](https://github.com/Unity-Technologies/HLSLcc) (MIT; it includes bstrlib, BSD), fetched by
`tools/hlslcc/build.sh` at a pinned commit and compiled into `pmhlslcc` (Direct3D 11 shaders,
`pmunity shaders --from d3d11`): distribute that binary with `pmhlslcc.LICENSE.txt`, which the
build writes next to it.
