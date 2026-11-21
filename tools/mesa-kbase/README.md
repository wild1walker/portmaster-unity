# Mesa Panfrost on the Mali kernel driver (kbase)

A test path, off by default: Unity draws with Mesa's Panfrost (Gallium GLES) instead of the Mali blob, on
the kernel driver the handhelds already have (Arm's kbase, job manager). The handhelds' GPU is a Mali-G31
(Bifrost): Allwinner H700 (RG34XX-SP: Knulli and muOS, kbase r20p0, UK 11.17, kernel 4.9.170) and
Rockchip RK3326 (G350 dArkOS: kbase r12p0, UK 11.7, kernel 4.4.189). Upstream Panfrost talks to the
`panfrost` DRM driver, which these kernels do not have.

Why: the 64-bit blob is the slower half of the 64-bit graphics gap (its heavy calls cost 17-28 % more than
the 32-bit blob's on the same GPU work, docs/arm64-routes.md "The graphics side, counted"), and its licence
forbids looking inside it. Mesa can be built, measured and changed.

Status: **parked (2026-11-21).** It runs on muOS (RG34XX-SP, kbase r20p0, UK 11.17) without a kernel fault and
draws within a few levels of the blob, but it costs more CPU and more GPU than the 64-bit blob, and no change
within days closes that (below, "Why it is parked"). Not on the G350, not in Unity, not in a release.

## What is in it

- `kbase.patch`: the Mesa changes, on the tree mesa0's Step 0 builds (bmdhacks/armsx2-turnip `malisx2`
  b5f8787 + Knifethrower's #6 #7 #4):
  - `src/panfrost/lib/kmod/kbase_kmod.c`, `kbase_kmod.h`, `kbase_kmod_sync.c.inc`: a `pan_kmod` backend
    ("kbase") beside panfrost and panthor. Structure from JimVulkan's mesa-unified-drivers (per-BO
    submission numbers, the event thread, the sync provider), ported to this tree's `pan_kmod` API
    (Mesa 27.0-devel). Every ioctl and struct goes through bmdhacks' kbase library (below), which checks
    each struct's size per UK version at compile time.
  - `pan_jm.c`: `jm_submit_batch_kbase`: the vertex/tiler chain and the fragment job as two atoms (the
    fragment depends on the chain), ordered after the previous submission; every BO the batch touches
    remembers the submission's number (kbase has no per-BO wait).
  - `pan_fence.c`, `pan_context.c`: fences are submission numbers; a sync file only where the kernel
    makes one (an optional fence-trigger soft atom: the RK3326's kernel has no sync files, so a refused
    soft atom turns them off for the run and the submission is made again without it;
    `PAN_KBASE_NO_FENCES=1` forces that).
  - `loader.c`, `platform_surfaceless.c`: a `/dev/mali*` fd is Panfrost's; surfaceless EGL opens
    `MESA_KBASE_NODE` (default `/dev/mali0`) before looking for DRM devices.
  - AFBC is never used on kbase (the backend reports no AFBC: never on the H700).
  - The window (the surfaceless pbuffer the host shows) is linear and CPU-cached on kbase: `glReadPixels`
    of it is a cache invalidate and a copy from cached memory instead of uncached reads.
    `PAN_KBASE_CACHED_WINDOW=0` turns that off (an A/B).
  - No DRM ioctl reaches the kbase node: the loader, the DRM backends' version probe and the screen's
    `DRM_CAP_PRIME` query skip it (found with the fake below; kbase answers unknown ioctls with a kernel
    warning).
- `build.sh`: fetches bmdhacks/malisx2 at a fixed commit, copies its `src/kbase` into
  `src/panfrost/lib/kmod/kbase/`, applies `kbase.patch` and builds aarch64 libraries in `ua-mesa-build24`
  (Ubuntu 24.04, glibc 2.39).

## Checked off the device

`test/run.sh <mesa tree> <out dir> [runtime/host/out/arm64]` runs in the build image with `test/fake_kbase.c`
as `/dev/mali0` (LD_PRELOAD): the version handshake (UK 11.17 as on the H700, 11.7 as on the RK3326), the
G31's properties, memory (SAME_VA through cookies, the executable zone from UK 11.13, every mapping of a
region the same bytes), JOB_SUBMIT (each atom completes at once, its event read back from the fd), no sync
files (fence-trigger atoms and STREAM_CREATE refused, as on the RK3326), and any DRM ioctl on the node counted.
`test/mesa_smoke.c` makes a surfaceless display and a GLES 3 context on a 720x480 pbuffer, compiles and links
a program, then 30 frames of clear, draw, `glReadPixels` and swap, a GL and an EGL fence waited for, and tears
everything down. 2026-11-21: both UK versions and `PAN_KBASE_CACHED_WINDOW=0` pass, 30 submissions of 2 atoms,
0 DRM ioctls on the node; the window's BO is the one allocation with `CACHED_CPU`. Nothing is drawn (the fake
runs no jobs): this checks the paths, not the picture.

What Mesa tells Unity there (`glreplay --info`):

    Mesa | Mali-G31 MC2 (Panfrost) | OpenGL ES 3.1 Mesa 27.0.0-devel | OpenGL ES GLSL ES 3.10
    EGL 1.5, 130 configs, 143 GL extensions, 51 compressed formats

The fake reports every texture feature bit, so the compressed formats there (ASTC LDR and HDR, ETC) are not
the G31's: the real list comes at first light.

## First light (muOS, 2026-11-21)

Each step in its own short hold, dmesg read before and after: no new kernel line in any of them, no faulted
atom, no hang.

- `mesa_smoke` 30 frames: `Mali-G31 MC1 (Panfrost)` (the H700's G31 has one core), ok.
- `glreplay --info`, blob against Mesa (`test/info-mali-r20p0.txt`, `test/info-mesa-muos.txt`): the blob is
  GLES 3.2 / GLSL 3.20, EGL 1.4, 25 configs, 94 extensions, 69 compressed formats, 1 program binary format;
  Mesa GLES 3.1 / GLSL 3.10, EGL 1.5, 130 configs, 143 extensions, 51 compressed formats, none. Only the blob:
  geometry and tessellation shaders, cube map arrays, `GL_ARM_shader_framebuffer_fetch(_depth_stencil)`,
  multiview, paletted and 3D ASTC formats. Only Mesa: S3TC/RGTC/BPTC, `GL_EXT_shader_framebuffer_fetch`,
  anisotropic filtering, base instance, and more. Limits differ (blob: 8192 textures, 16 samples, 4 colour
  attachments, 1024 uniform vectors; Mesa: 16384, 4, 8, 4088).
- Hollow Knight's King's Pass trace from Knulli (64-bit, 3,457 frames) replayed through Mesa: Unity made its
  shaders for the blob's 3.2, so 64 shaders (`#version 320 es`) do not compile on 3.1 and 32 programs do not
  link. Unity on Mesa would choose 3.1 shaders itself; for the replay, `MESA_GLES_VERSION_OVERRIDE=3.2
  MESA_GLSL_VERSION_OVERRIDE=320`: every program links; 23 GL errors left (one cube-map-array texture).
- The pictures, last 135 frames (`--hash`, glpixdiff): 674 of 2,295 readbacks differ, all by rounding: frame
  3322 dumped (PPM) shows at most 2 levels of one channel, in 0.3 - 0.9 % of the pixels of three 576x360
  passes and 0.06 % of the final frame; the first differing call is a draw into a 576x360 pass. Not
  bit-identical to the blob (the vanilla-look rule: to be traced to the draw before any default).
- The times: the same trace, last 300 frames, glFinish after each (untimed), Mesa with the 3.2 override, two
  holds of Mesa then the blob:

  | per frame | blob r20p0 | Mesa |
  |---|---|---|
  | GL calls on the render thread (CPU) | 11.17 / 11.38 ms | 14.84 / 16.10 ms |
  | draw (`glDrawElements`, 85 a frame) | 53.5 / 54.3 us | 110.6 / 120.7 us |
  | framebuffer bind (16 a frame) | 142 / 145 us | 8.5 / 9.4 us |
  | render thread CPU / whole process CPU | 12.1 / 17.4 ms | 16.1 / 17.8 ms |
  | wall, with the GPU (glFinish) | 26.0 / 27.1 ms | 35.9 / 39.5 ms |

  Mesa does its work on the render thread (the blob has its own thread, about 5.5 ms a frame), so the whole
  process costs about the same; the render thread pays 4 ms more, almost all in the draws (Mesa builds the
  jobs there), and the frame takes 10 - 12 ms longer with the GPU: where that goes (GPU time, the jobs, the
  waits) is the next question. (Without the override, the 3.20 programs fail and their draws cost nothing:
  5.7 ms, not a measurement.)

## Why it is parked (2026-11-21)

Hollow Knight's King's Pass trace, muOS, each driver in turn (GPU counters: `gpucnt exec` around a whole replay;
CPU: an ITIMER_PROF sampler, `test/prof_preload.c`; the GPU's own time per atom: `PAN_KBASE_STATS=1`):

- **GPU work is 15 - 20 % more.** GPU_ACTIVE +20 % over the same 3,457 frames, fragment job slot +16 %,
  vertex/tiler slot +64 % (tiler unit 2.4x), fragment warps +50 %, quads rasterized +37 %, external reads +58 %.
  Part of it is Panfrost loading (preloading) render targets the game had invalidated: `glInvalidateFramebuffer`
  only dropped the store, the next pass still loaded the old contents. Clearing the resource's valid bit there
  (in kbase.patch, `PAN_INVALIDATE_KEEP_VALID=1` for the old behaviour) leaves every readback identical and takes
  the quads from +37 % to +16 %, reads from +58 % to +40 %, GPU_ACTIVE from +20 % to +15 %. The rest (tiler
  2.2x, more warps) is Panfrost's own job layout and compiler output.
- **Throughput without the per-frame glFinish** (`--no-finish`, the GPU fed as in a game): the blob 19.0 ms a
  frame, Mesa 25.3, Mesa with the invalidate change 23.9. The GPU sets the pace there (Mesa's draws wait for
  it: 166 - 178 us each).
- **The +10 - 12 ms with glFinish** is about 4 ms more CPU, 4 ms more GPU and 3 ms of lost overlap: Mesa
  submits a frame's passes only at the flush (`PAN_KBASE_STATS`: ~14 submissions of 2 atoms a frame, the
  fragment atoms 93 % of the GPU time, the CPU waiting 55 % of the time), the blob as each pass ends, so its GPU
  starts while the CPU still records.
- **CPU:** the render thread's driver time is about twice the blob's (samples in the driver per sample in the
  replay's own work: 3.1 against 1.5; the blob has a second thread, Mesa does everything on the render
  thread). No hotspot: the largest functions are `panfrost_emit_const_buf` 5.6 %, `prepare_draw` 2.8 %,
  `panfrost_ptr_map` 2.0 %, then dozens at 0.5 - 2 % (state validation, descriptor emission, BO tracking;
  our per-BO submission numbers under 1 %) and libc's memcpy. mesa0's drm-shim runs agree: at parity with warm
  caches, 70 % slower with the game's cache pressure: the driver's code itself is cache-hungry.
- **Verdict:** below 10 ms of GL CPU a frame would need Panfrost's whole draw path made about twice as cheap in
  i-cache terms, and the GPU gap needs tiler and compiler work: months, not days.

The pictures: the first frame that differs is 21 (a title screen). Its first differing draw (call 13441) is
TextMeshPro's SDF text shader (`discard` on the distance), one glyph quad: the blob draws one more pixel row
(y 309, 220 pixels, value 60,0,60) at the quad's edge, Mesa none: an edge or threshold difference (vertex
snapping, varying interpolation at the edge, or the discard threshold), not blending or a format; fp16 is not it
(`PAN_MESA_DEBUG=nofp16` changes nothing). Later frames carry such differences on (max 2 levels in frame 3322).

## In the runtime

`UA_GL_DRIVER=mesa` (runtime/host/gldriver.c; unset or `mali`: the blob):

- ua_host starts itself again with `UA_MESA_DIR` (default `mesa/` beside ua_host) first in
  `LD_LIBRARY_PATH`, `EGL_PLATFORM=surfaceless` and `UA_EGL_PBUFFER=1`. Without `libEGL.so.1` there it says
  so and keeps the blob.
- Display: the fbdev path as with the blob (buffers, host flips, render size, rotation). Mesa's surfaceless
  EGL has no window surfaces, so Unity's window is a pbuffer (the panel's size under the GPU scaler) and each
  swap reads it back, copies it top-down into the framebuffer's next buffer and pans there through fbdev.c's
  flips. The blob draws into the framebuffer itself; the read and the copy are this path's extra cost,
  logged every 10 s: `[gldriver] present: N frames, read X ms, copy Y ms a frame` (`UA_GLDRIVER_STATS=0`: off).
  Modelled on bmdhacks' fbdev WSI for malisx2 (PR #61).

## Unity's path: the blob's strings

Unity picks its GLES features from the driver's strings. Known so far (glreplay on Knulli, 64-bit blob):

    ARM | Mali-G31 | OpenGL ES 3.2 v1.r20p0-01rel0.8ec337de4ddab3365a32cff58063a9da
    EGL 1.4

`glreplay --info` prints every EGL and GL string, all EGL configs and the limits Unity reads, one item per
line, so the blob's and Mesa's outputs compare with `diff`. The full lists (both blobs: r20p0 and the G350's
r12p0) and Mesa's are taken at first light (a run that opens `/dev/mali0`).

Differences already known, for Unity's path:

- GLES 3.1 against the blob's 3.2: Unity's 3.2-only features (tessellation, geometry shaders) are ones these
  games do not use; checked with `--info`, not hidden (no version override).
- No program binary formats (`GL_NUM_PROGRAM_BINARY_FORMATS` 0: the build has Mesa's shader cache off), so
  Unity's shader cache and `UA_GL_PROGCACHE` cannot keep programs: every launch compiles them. To look at
  after first light (Mesa's disk cache, or binary formats without it).
- Mesa lists `EGL_ANDROID_native_fence_sync` on both handhelds; on the RK3326 (no sync files) a native fence
  fd cannot be made. Unity's Swappy stays off (its EGL table only needs the entry points).

## Credits

- JimVulkan (XclipseTools / mesa-unified-drivers, MIT): the kbase `pan_kmod` backend design, the JM batch
  submission and the surfaceless kbase probe this is taken from.
- bmdhacks and ARMSX2 (malisx2, MIT): the kbase library (ioctls, structs per UK version, atom layouts,
  cache maintenance) and the fbdev WSI the present path follows.
- Knifethrower: malisx2 PR #60 (job-manager kernels from UK 11.7: the RK3326 and H700 G31 handhelds) and
  the Mesa tree's fixes (#4 #6 #7).
- Icecream95: the first Panfrost on kbase, which showed it could be done.
