# media/: video playback (Unity's VideoPlayer) on the runtime

Unity's Android VideoPlayer decodes with Android's media framework and draws the frames from a
`SurfaceTexture`. This folder is what stands behind that framework here: `libuamedia.a`, a
demuxer + decoder + surface core that the Java classes (`../jni/up_media.c`) and the NDK's
`libmediandk` (`../android/media_ndk.c`) both drive, plus the decoders it links. The shader Unity
draws the frames with, `Hidden/VideoDecodeAndroid`, is not in a desktop build's data: the
converter adds our own (`convert/pmunity/video.py`, `video_decode_android.shader`).

```
./build.sh                 # -> out/libuamedia.a (+ out/licenses/, out/umbench); ARCH=arm64 -> out/arm64/
./build.sh test            # + unit tests (armhf in an arm/v7 container, arm64 natively; GL test with Mesa)
./build.sh test-ffmpeg     # the system-FFmpeg decoders on clips made by Debian's FFmpeg tools
```
Link with `libuamedia.a -lstdc++ -ldl -lpthread -lm` (`../host/build.sh` does). `../build.sh`
builds it after `jni/` (the Java-side test links `libfakejni.a`).

## Files

| file | what |
|---|---|
| `um.h` | the interface: `um_format` (MediaFormat), `um_extractor` (MediaExtractor), `um_codec` (MediaCodec), `um_surface` (SurfaceTexture), the packed picture layout |
| `um_format.c` | typed key/value formats (Android's key names), the picture pool, `[media]` logging |
| `um_extractor.c` | MediaExtractor: sources (an fd range, a path, a read callback), track selection, Android's sample order (earliest time among the selected tracks), seeks (previous / next / closest sync), the frame rate from the samples when the container has none, Annex B for H.264 / HEVC samples |
| `um_webm.c` | WebM / Matroska demuxer: EBML, Info, Tracks, every Cluster block (SimpleBlock, BlockGroup, Xiph / EBML / fixed lacing); Android's track formats (Vorbis csd-0/1, Opus csd-0/1/2, AVC csd-0/1) |
| `um_mp4.c` | MP4 / QuickTime demuxer: stts/ctts/stsc/stsz/stz2/stco/co64/stss, edit lists (as Android's MPEG4Extractor applies them), avcC/hvcC/esds |
| `um_codec.c` | MediaCodec: Android's buffer model (indices, `INFO_OUTPUT_FORMAT_CHANGED`, `INFO_OUTPUT_BUFFERS_CHANGED`, end of stream), a decoding thread per codec, the decoder table and the fallback to the next decoder when one refuses a stream |
| `um_vpx.c` | VP8 / VP9: libvpx |
| `um_audio.c` | Vorbis (libvorbis), Opus (libopus), silence for AAC / MP3 without a decoder |
| `um_h264.c` | H.264: OpenH264 |
| `um_ffmpeg.c` | the system's libavcodec 58-61 through `dlopen` (AAC, MP3, HEVC, H.264 with B-frames) |
| `um_surface.c` | SurfaceTexture: the frame queue, `updateTexImage` (upload into an EGLImage behind the external texture), `getTransformMatrix` |
| `um_stubs.c`, `um_compat.c` | builds without a backend; libvorbis' finite-math names on Ubuntu 20.04 |
| `umbench.c` | device check: what decoding a clip costs (`umbench <file> [offset size]`) |
| `test/` | unit tests on clips the tests make (below) |
| `docker/` | build images (Debian bookworm armhf cross + Debian's static decoder libraries; arm64 on `ua-build-arm64`), the FFmpeg test image |

## What Unity does (read from libunity, 2017.4 with symbols; checked against later players' bindings)

**2017.4 - 2021.3: Java** (`AndroidVideoMedia<AndroidMediaJNI>`; libmediandk's dlopen is refused
by the host, which is what makes these players take the Java path):
1. The clip is opened by libunity itself: for a clip inside the APK/OBB (a VideoClip's
   `.resource` file) an fd of the archive, then `ParcelFileDescriptor.adoptFd(fd)
   .getFileDescriptor()` → `MediaExtractor.setDataSource(FileDescriptor, offset, length)` →
   `detachFd()`, and libunity closes its fd (the extractor keeps a dup, as Android's). Paths and
   `file://` URLs go to `setDataSource(String)`; http(s) is refused (no network).
2. `getTrackFormat`: mime, width/height, durationUs, `frame-rate` (`getFloat`, then `getInteger`
   after an exception), `rotation-degrees`, `channel-count`, `sample-rate`, language; video tracks
   are refused when width or height are missing.
3. `GetFormatInfo` probes first: decoders without a surface, the first frames' times and the
   audio output format; then the playback decoders.
4. Video: `MediaCodec.createDecoderByType`, `configure(format, Surface(SurfaceTexture(texName)),
   null, 0)` (the external texture is made by `ApiGLES::GenTexture(GL_TEXTURE_EXTERNAL_OES)` on
   the render thread; the SurfaceTexture on a `VideoDecoderLooper` HandlerThread),
   `getInputBuffers` / `getOutputBuffers` (every buffer must have an address and a capacity, or
   `AndroidMediaJNI::Buffers::Init` fails), `dequeueInputBuffer` / `readSampleData` /
   `queueInputBuffer`, `dequeueOutputBuffer` (`INFO_OUTPUT_FORMAT_CHANGED` → `getOutputFormat`:
   width, height, crop-*), `releaseOutputBuffer(i, true)` for the frame to show.
5. `onFrameAvailable` (listener proxy) marks the frame available; Unity's render thread (it waits up
   to 16 ms for it) calls `updateTexImage` once and `getTransformMatrix` (it uses m[0], m[5] as
   the uv scale and m[12], m[13] as the offset), then blits the texture with
   `Hidden/VideoDecodeAndroid` pass `RGBAExternal_To_RGBA` (`RGBASplitExternal_To_RGBA` for split
   alpha), `_AlphaParam` 1, point filtering and clamp on the texture. The blit is skipped when the
   latched frame is more than 5 frames behind the clock; a decoder that falls far behind gets a
   seek (`SetFramePosition`: seekTo + flush + decode to the frame, about half a second allowed).
6. Audio: decoders without a surface, 16-bit PCM from the output buffers (`WriteValuesToRingBuffer
   <short>`), rate and channels from the output format; to an AudioSource or Unity's own output.

**2022.3: NDK** (`AndroidVideoMedia<AndroidMediaNDK>`; libunity links libmediandk): the same steps
through `AMediaExtractor_setDataSourceFd` / `setDataSourceCustom`, `AMediaFormat_get*`,
`AMediaCodec_*` with the `ANativeWindow` of a Java `Surface(SurfaceTexture)`
(`ANativeWindow_fromSurface`), and the same SurfaceTexture / shader. `AImageReader` (the
Vulkan path) stays a stub.

## Frames: decoder → SurfaceTexture → Unity's texture

A decoded picture is copied (on the codec's thread) into the **packed 4:2:0 layout**: a W x 1.5 H
array of bytes (W, H: the size rounded up to even; odd sizes repeat the last column / row), the Y
plane, then each chroma row as U (W/2) followed by V (W/2). `releaseOutputBuffer(i, true)` moves it
into the SurfaceTexture's queue (up to 4; the oldest is dropped beyond) and the listener is told on
the SurfaceTexture's looper. `updateTexImage`, on Unity's render thread, uploads the oldest queued
picture with one `glTexSubImage2D` into a one-channel texture (`GL_R8`, else `GL_LUMINANCE`) of
that size; that texture is the EGLImage (`EGL_KHR_gl_texture_2D_image`) behind Unity's
`GL_TEXTURE_EXTERNAL_OES` texture (`glEGLImageTargetTexture2DOES`, once per size). The bindings
and unpack state the upload touches are put back. `getTransformMatrix` gives Android's y flip with
the crop of the padding: scale (w/W, -h/H), offset (0, h/H).

The shader (converter) samples Y, U and V nearest from that texture and converts them as Unity's
**desktop** players convert the same clips, so a clip looks as it does on the PC:
VP8 / VP9 (Linux builds) with Unity's `YCbCr_TO_RGB1` (BT.601, limited range, coefficients in
1/64 steps), H.264 / HEVC (macOS / Windows builds) with Unity's NV12 pass (BT.709, limited range).
The converter picks one per game from its clips' headers. `test/test_surface_gl.c` draws decoded
pictures through EGLImage + shader with Mesa: every pixel equals the desktop formula (0/255).

## Decoders

| codec | container | decoder | licence | linked |
|---|---|---|---|---|
| VP8, VP9 | WebM, Matroska | libvpx (`OMX.google.vp8.decoder`, `...vp9...`) | BSD-3-Clause | static (Debian's `libvpx-dev`) |
| Vorbis | WebM, Matroska | libvorbis + libogg | BSD-3-Clause | static |
| Opus | WebM, Matroska | libopus | BSD-3-Clause | static |
| H.264 | MP4, MOV, Matroska | OpenH264 (`OMX.google.h264.decoder`) | BSD-2-Clause | static (Debian's `libopenh264-dev` on armhf; built from Debian's source package for arm64) |
| H.264 with B-frames, interlaced, > 8 bit; HEVC; AAC; MP3 | MP4, MOV, Matroska | the system's libavcodec (`ffmpeg.*`) | LGPL (the firmware's) | `dlopen` when present, never shipped |
| AAC, MP3 without libavcodec | | silence of the right length | ours | |

What each covers:
- **Linux builds** (Unity transcodes clips to VP8 + Vorbis WebM): everything with the bundled
  decoders, on every firmware.
- **macOS / Windows builds** (H.264 + AAC MP4): the picture with OpenH264, exactly for Baseline,
  Main and High profile without B-frames (pictures equal libavcodec's); with B-frames OpenH264's
  pictures differ slightly (38-79 dB against libavcodec's on x264 clips), so such streams go to the
  system's libavcodec when the firmware has one. The sound needs the system's libavcodec (no
  permissive AAC decoder is bundled); without it the clip plays silently rather than not at all.
  A 32-bit runtime finds a system FFmpeg only in a 32-bit userland that has one (rare); arm64
  firmwares usually have libavcodec.
- Clips a game ships untranscoded play when their codec is in the table (VP9 / Opus WebM, HEVC with
  libavcodec, ...).

Licences: `build.sh` copies the libraries' copyright files into `out/[arm64/]licenses/`; a
`ua_host` that links the core ships them (`THIRD_PARTY.md`). OpenH264 built from source carries
no MPEG LA patent licence (Cisco's own binaries do); FFmpeg is the system's. `OPENH264=0
./build.sh` (or `OPENH264=0 runtime/build.sh`) builds the core without OpenH264, for a release that
must not carry an H.264 decoder: H.264 clips then play only where the handheld has libavcodec, and
`out/licenses/` lists only what was linked.

## Environment

| variable | effect |
|---|---|
| `UM_TRACE=1` | one line per decoded input (with its decode time), per frame and per latched picture |
| `UM_VIDEO_BUFFERS=<n>` | a video decoder's output buffers (default 4, Android's usual; each holds a picture). More let a slow consumer still find the frame it asks for (the emulated rig needs ~24 at 2 fps) |
| `UM_VPX_THREADS=<n>` | libvpx threads (default: CPUs, up to 4; VP8 uses them only for clips with several token partitions) |
| `UM_H264=openh264` | OpenH264 for every H.264 stream, B-frames included |
| `UM_FFMPEG=0` / `=<path>` | no system libavcodec / that libavcodec |
| `ANDROID_SHIM_LIBEGL`, `UM_LIBGLES` | the libEGL / libGLESv2 the surface code opens (the ones Unity uses; default `libEGL.so.1`, `libGLESv2.so.2`) |

The host's log names the decoders at start: `[media] decoders: vp8 (libvpx-vp8), ...`.

## Tests (`./build.sh test`, both architectures)

- `test_media`: WebM clips made by the test (libvpx VP8/VP9 and libvorbisenc/libopus encoders,
  `test/webm_writer.c`; one inside a bigger file at an offset, one with Xiph and EBML laced audio,
  odd sizes) through extractor and codecs as Unity drives them: formats, sample order, sync samples,
  seeks in the three modes, flush, end of stream, PSNR of the pictures, tone frequencies, the
  surface queue and listener, the transform matrix; H.264 in MP4 (OpenH264's encoder,
  `test/mp4_writer.c`, edit list and composition offsets); AAC without a decoder; errors.
- `test_media_jni`: the Java classes through JNI in libunity's call order (ParcelFileDescriptor,
  setDataSource with a range, MediaFormat getters and their exceptions, a codec with a
  Surface(SurfaceTexture) and its listener on the looper, the ByteBuffer arrays, BufferInfo, PCM).
- `test_media_ndk`: the NDK calls of 2022.3 (fd range and AMediaDataSource, a codec into the window
  of a Surface(SurfaceTexture)).
- `test_surface_gl`: EGLImage upload and the converter's shader with Mesa (orientation, odd sizes,
  the alpha split pass, BT.601 and BT.709, GL state left as found).
- `test_media_ffmpeg` (`./build.sh test-ffmpeg`): AAC in MP4, x264 High profile with B-frames
  (OpenH264 against libavcodec), interlaced H.264 (refused by OpenH264, taken by libavcodec),
  WebM and Matroska written by FFmpeg's muxer.

## Cost

Measured with `umbench` on a 1920x1080 VP8 clip of 5.6 Mbit/s (a game's intro): 1.4-1.8 ms per
frame natively on an Apple M-series core (arm64 build), 27-40 ms under QEMU (armhf, the emulated
rig); decode threads do not help (one token partition). Vorbis: 37x real time under QEMU.
For a Cortex-A53 at 1.5 GHz (H700) that is an estimated 15-30 ms per 1080p frame on one core
(the M-series core is roughly 8-12x faster on this code; libvpx's NEON paths are used on both):
real time at 30 fps on the decoding thread, which is not Unity's main thread. Per shown frame the
render thread uploads 3.1 MB (1080p) and the GPU runs the blit (3 texture reads per pixel of the
video's RenderTexture): on a Mali-G31, from the per-layer costs measured for Cuphead (about 0.3 ms
per texture read per 720x480 layer), roughly 5-8 ms per 1080p frame, i.e. 15-25% of the GPU at
30 fps while a video plays; the upload is a CPU copy of 3.1 MB in the driver. `umbench` on the
handheld gives the decode number for real.

## Not done

- `AImageReader` / `AHardwareBuffer` (2022.3's Vulkan video path), HDR and 10-bit pictures (VP9
  profile 2, 10-bit H.264 / HEVC go to libavcodec and are refused there: 8-bit 4:2:0 only).
- Streams from the network; Matroska content encodings (compressed / encrypted tracks).
- Measured on the handheld: decode cost, the upload, and that Mali's driver takes the GL_R8
  EGLImage as an external texture (`GL_LUMINANCE` is tried next; both work on Mesa).
