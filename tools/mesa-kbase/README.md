# Mesa Panfrost on the Mali kernel driver (kbase)

A test path, off by default: Unity draws with Mesa's Panfrost (Gallium GLES) instead of the Mali blob, on
the kernel driver the handhelds already have (Arm's kbase, job manager). The handhelds' GPU is a Mali-G31
(Bifrost): Allwinner H700 (RG34XX-SP: Knulli and muOS, kbase r20p0, UK 11.17, kernel 4.9.170) and
Rockchip RK3326 (G350 dArkOS: kbase r12p0, UK 11.7, kernel 4.4.189). Upstream Panfrost talks to the
`panfrost` DRM driver, which these kernels do not have.

Why: the 64-bit blob is the slower half of the 64-bit graphics gap (its heavy calls cost 17-28 % more than
the 32-bit blob's on the same GPU work, docs/arm64-routes.md "The graphics side, counted"), and its licence
forbids looking inside it. Mesa can be built, measured and changed.

Status: **built, checked off the device, not yet run on a GPU.** No run of this driver has opened `/dev/mali0`
yet (first light waits for the go). Nothing here is in a release.

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

What Mesa tells Unity there (`glreplay --info`, `test/mesa-info-fake.txt`):

    Mesa | Mali-G31 MC2 (Panfrost) | OpenGL ES 3.1 Mesa 27.0.0-devel | OpenGL ES GLSL ES 3.10
    EGL 1.5, 130 configs, 143 GL extensions, 51 compressed formats

The fake reports every texture feature bit, so the compressed formats there (ASTC LDR and HDR, ETC) are not
the G31's: the real list comes at first light.

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
