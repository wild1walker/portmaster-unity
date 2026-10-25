# Shaders

Unity's Android player only runs GPU programs of the GLES types. A desktop build has OpenGL Core
programs (written by Unity's HLSLcc from the game's HLSL: GLSL 1.50 for most shaders, GLSL 4.x
for `#pragma target 4.5` and up), Metal on macOS, and maybe Vulkan. `python -m pmunity shaders`
(convert/pmunity/shaders/) gives every OpenGL Core program a GLSL ES 3.00 (or 3.10) twin in the
same Shader asset, so the Android player finds a GLES3 program for every variant the game uses. A
Windows build has only Direct3D 11 programs (DXBC bytecode): `--from d3d11` translates those with
Unity's HLSLcc ([below](#windows-builds-direct3d-11-programs)). A Linux build made without OpenGL
Core (HDRP's, or one that lists only Vulkan) has Vulkan programs: `--from vulkan` translates them
with SPIRV-Cross ([below](#vulkan-only-builds-vulkan-programs-spir-v)); a macOS build of Unity 2020.3
and later has Metal programs only: `--from metal` reads Unity's MSL
([below](#macos-builds-metal-programs-msl)). The default stays `glcore`; with several sources a shader
is translated from the first one it has, so a game with OpenGL Core programs converts as before.

## What the converter does

1. **Program blobs** (`blob.py`). A Shader keeps its compiled programs per shader compiler
   platform in LZ4 blocks. The GLCore platform's blob (15) is copied as a GLES3Plus blob
   (9); every entry a GLCore subprogram uses is translated and retyped GLES3 (gpu program type
   4) or GLES 3.1 (3, item 4). Entries are parsed with the layout of their serializer version,
   checked by writing every entry back byte for byte ([Unity versions](#unity-versions) below;
   another version's layout is guessed and reported).
2. **Subprograms** (`convert.py`). Every GLCore subprogram gets a GLES twin with the same blob
   index, keywords, channels, requirements and parameter bindings (the GL and GLES backends bind
   by name the same way; 2021.2+: the same parameter entry). A variant has one GLCore program,
   typed by its target: GLCore32 (GLSL 1.50 / 3.30), or GLCore41 / GLCore43 (GLSL 4.x) for
   `#pragma target 4.5` and up ([below](#glcore4143-programs-pragma-target-45-and-up)).
3. **GLSL** (`glsl.py`), per stage block (`#ifdef VERTEX` / `#ifdef FRAGMENT`, each compiled by
   Unity as one shader):
   - `#version 300 es` instead of `#version 150` / `330` (GLSL 4.x: `410` - `450`);
   - the desktop extensions ES 3.00 has built in are dropped (explicit attribute locations,
     float/int bit casts), also in the `#ifdef GL_ARB_... / #extension / #endif` form of Unity
     2019+; any other extension is kept, so a program that needs one fails on a driver without
     it, as it would in a real Android build;
   - default precisions: **highp** for floats, ints and every sampler type the stage declares.
     Desktop GL computes and samples in 32-bit floats; ES would otherwise sample `sampler2D` at
     lowp. `--precision default-samplers` keeps ES's sampler defaults (for A/B tests);
   - two rewrites that keep every operation and operand (`--optimize`, both on):
     **const-arrays** (HLSLcc fills constant tables, e.g. blur weights, element by element at the
     start of every invocation; they become `const` arrays) and **unroll** (loops with literal
     bounds and a plain body are written out, the counter becoming a literal: up to 16 trips and
     120 statements in all; a larger one written out costs the Mali several times its loop,
     [below](#exact-rewrites-for-the-mali-g31s-compiler-looked-for-2026-10-23)).
4. A program that needs what GLSL ES 3.10 has and 3.00 lacks (multisampled textures
   `sampler2DMS`, `textureGather`, bit-field functions; for GLSL 4.x also storage buffers, binding
   points, images) is written as `#version 310 es` and typed GLES31 (gpu program type 3, with all
   subprograms of its variant): Unity runs it where the device has OpenGL ES 3.1+, as it does the
   GLES 3.1 programs of an Android build. (Unity's own GLES3 output keeps such a program as
   `#version 300 es`, which no driver compiles.)
5. A shader with a GLCore32 program that uses GLSL with no ES equivalent (fixed-function
   built-ins, rectangle or 1D samplers, `noperspective`, images and atomics, storage buffers) gets
   no GLES3 programs at all, and the run says which. Unity then treats it as unsupported on the
   device, as it would a shader without GLES3 programs in an Android build. For Cuphead that is
   only `Hidden/VideoDecodeOSX` (the macOS video decoder; Android decodes video elsewhere).
   Variants only VR uses (`STEREO_INSTANCING_ON` and the other VR keywords, among them the
   Post-processing Stack v2's own `STEREO_INSTANCING_ENABLED` and `STEREO_DOUBLEWIDE_TARGET`) that
   do not translate (they write `gl_Layer`) are left out instead, with their subprograms; Unity
   never asks for them on a handheld. (Before PPv2's keywords were counted, its `Uber` and
   `FinalPass` shaders got no GLES3 programs at all, and a game with PPv2 on its camera drew a
   black picture: Nainai's Recipe.) A SubShader with a GLCore41/43 program that does not translate gets no twins
   for its GLCore41/43 variants (Unity uses the next SubShader or the Fallback, as for a
   SubShader an Android build has no programs for); the rest of the shader is converted.
6. A shader whose variants were all stripped from the build (a GLCore blob without entries, e.g.
   URP's `StencilDeferred` in a forward-rendered game) gets an empty GLES3 blob, as Unity's own
   builds have a blob for each of their platforms: the player then finds its platform there
   (without one it logs "Desired shader compiler platform 9 is not available in shader blob").
7. Opt-in, off by default (it changes the picture slightly): `grab_fetch`, blend-mode passes that
   read the framebuffer instead of a GrabPass copy ([below](#blend-modes-without-grabpass-copies-grab_fetch-opt-in)).

### GLCore41/43 programs (`#pragma target 4.5` and up)

Shaders written for compute-capable GPUs are compiled for OpenGL 4.1+ (GLCore41, GLCore43:
`#version 410` - `450`): URP's error shaders (`MaterialError`, `FallbackError`), its FSR upscaler
(`Edge Adaptive Spatial Upsampling`), the `#pragma target 4.5` SubShaders of its post-processing
shaders (`FinalPost`, `BokehDepthOfField`, which keep a GLCore32 fallback SubShader), the
procedural-instancing variants of the particle shaders (one pass, one target per keyword) and
`unity_builtin_extra`'s `Hidden/BlitToDepth` and `BlitToDepth_MSAA` (in every game; the Android
player looks them up). The same rules apply, plus what GLSL 4.20 has built in:

- `GL_ARB_shading_language_420pack`, `GL_ARB_shader_storage_buffer_object` and
  `GL_ARB_shader_image_load_store` are dropped (what HLSLcc uses of them is in ES 3.10);
- the unused `precise vec4 u_xlat_precise_vec4;` (and `ivec4`, `bvec4`, `uvec4`) declarations at
  the top of each stage are dropped; any other `precise` is GLSL ES 3.20's (left out);
- varyings' `layout(location = n)` (vertex outputs, fragment inputs) are removed in ES 3.00
  programs, which match varyings by name only (HLSLcc gives a varying the same name and location
  in both stages); ES 3.10 programs keep them;
- GLES 3.1 (`#version 310 es`, item 4) for what ES 3.10 has: storage buffers (`buffer`, `std430`;
  in a vertex stage they need a driver with vertex shader storage blocks), explicit binding
  points and uniform locations outside Unity's macro blocks, images with a format qualifier
  (with `precision highp` for each image type), image load/store and buffer atomics,
  `early_fragment_tests`;
- left out, with the reason: what only desktop GL 4.x or ES 3.20 has (cube map array, buffer and
  1D textures, an image without a format qualifier, image atomics, `textureQueryLevels`,
  interpolation functions, interface blocks, `component` / `index` layouts, geometry and
  tessellation stages).

Every variant has one program, so a shader with GLCore32 and GLCore41/43 programs gets twins for
all of them: where its SubShaders differ by target (URP's post-processing: SubShader 0 at target
4.5, SubShader 1 its fallback), both get programs, and Unity runs SubShader 0 where the device
has what its requirements ask (OpenGL ES 3.1+; the GLCore subprograms' `m_ShaderRequirements`
are kept) and the fallback elsewhere, as an Android build of the game would, and as the desktop
build does on OpenGL 4.3+ and on 3.2 (the fallback does with plain fetches what SubShader 0 does
with `textureGather`, in URP's depth of field).

**On by default, for every Unity version.** `"shaders": {"glcore4x": false}` in a port's config
(or `pmunity convert ... --set shaders.glcore4x=false`) keeps the converter's earlier output,
GLCore32 programs only. Cuphead's port sets it: its output was checked on the device that way and
stays byte for byte the same (`BlitToDepth`, `BlitToDepth_MSAA` and the `#pragma target 4.5+`
SubShaders of its post-processing keep no GLES programs; the 2017.4 player logs "Desired shader
compiler platform 9 is not available" for the two blits at startup and runs on). On, Cuphead's 14
GLCore43 programs translate as well (9 of them GLES 3.1: the eye adaptation histogram's storage
buffer, `textureGather` in depth of field) and link on Mesa.

### Compute shaders (ComputeShader assets)

A ComputeShader keeps one variant per renderer: a desktop build has OpenGL Core kernels (GLSL
4.20 / 4.30 compute shaders by HLSLcc), Vulkan's (SPIR-V), Direct3D's or Metal's. The Android
player on GLES3 looks for the OpenGL ES 3 variant; without one every `FindKernel` fails ("Kernel
'X' not found"), and a game that runs its own kernels (particle or swarm simulations, GPU culling)
draws nothing of them or stops in an exception every frame. The shaders step
(`shaders/compute.py`) gives each ComputeShader with OpenGL Core kernels the variant an Android
build has: a copy of the GLCore variant with the GLES3 renderer (11) and level (3; 2 from 2023.1),
every kernel translated to GLSL ES 3.10 (`glsl.kernel_to_es`). Everything else (kernel names,
keywords, constant buffers, textures, buffers and their bind points, thread group sizes) is the
GLCore variant's. Unity runs them where the device has OpenGL ES 3.1+ (`SystemInfo.
supportsComputeShaders`), as it does an Android build's.

- the GLSL 4.x rules above, plus what ES 3.10 has for compute shaders: `barrier()`,
  `memoryBarrierShared()`, `groupMemoryBarrier()`, `shared` variables, atomic counters;
  `GL_ARB_compute_shader` and `GL_ARB_shader_atomic_counters` are dropped; image atomics get
  `GL_OES_shader_image_atomic` (core in ES 3.2);
- ES needs a format qualifier on every image, which HLSLcc leaves out of write-only images for
  desktop GL: it comes from the Vulkan SPIR-V of the same kernel and keywords, where HLSLcc wrote
  the format it derives from the HLSL type (`float4` -> `rgba32f`, `float` -> `r32f`, as for
  GLES). A kernel whose image has no such format, or one ES lacks (`rg32f`, `rg16f`), or that
  reads and writes an image of a format other than `r32f`, `r32i`, `r32ui`, is left out of the
  GLES3 variant (the run names it); the shader's other kernels are kept;
- the layouts: up to 2019.4 each kernel has its code, 2020.1 - 2021.1 a `variantMap` of keyword
  sets, from 2021.2 `uniqueVariants`; the GLES3 variant goes after the GLCore one (up to 2021.x)
  or in renderer order (2022.1+), where Unity's own builds have it.

Checked on Unity's built-in resources of Linux players 2017.4 - 6000.5 (games' "unity default
resources"; Internal-Skinning,
Internal-BlendShape, Internal-VT-TranslationTableReplace: they hold GLCore and GLES3 kernels of
the same compute shaders): with the GLES3 variant taken out and made again, it has the same
place, level, kernels and metadata as Unity's; from 2019.4 every kernel declares the same uniform
blocks, buffers, images, samplers and shared memory as Unity's (the bodies differ where HLSLcc
writes the same operations differently for GLES: `uint(...)` around a value that is one, `for`
loops for `while(true)` ones). 2017.4 and 2018.4's GLES3 kernels give their uniform block an
explicit `binding = 0` (the default binding of a block without one), and 2017.4's read typed
buffers through `samplerBuffer` / `imageBuffer`, which ES 3.10 lacks without
`GL_EXT_texture_buffer`; ours keep the storage buffers of the GLCore kernels. On ten free games'
compute shaders (2018.1 - 6000.6: the Post-processing Stack v2's, a game's own particle and swarm
kernels, Unity 6's GPU culling and depth pyramids) 109 kernels translate and 22 are left out (PPv2's
ambient occlusion: `rg32f` images, or write-only images whose Vulkan kernel gives no format;
Unity 6's color pyramid). Of all 495 translated kernels (built-ins and games,
`tests/test_compute_corpus.py`), 374 compile and link on Mesa (llvmpipe, ES 3.2; `shadercheck`,
below) and 121 exceed its 8 storage buffers per compute shader: Unity's batched skinning and
blend shape kernels (15 - 16 buffers; Unity's own GLES3 kernels of them declare the same) and one
Unity 6 kernel (9). A GLES 3.1 device need only allow 4; Unity then reports the kernel as not
supported, as for an Android build. Nainai's Recipe: all its 1818 GLES3 programs and 29 kernels
link on Mesa.

```sh
cd convert && PMUNITY_COMPUTE_CORPUS=<file>[:...] PMUNITY_COMPUTE_EXPORT=/tmp/comp \
    python -m unittest tests.test_compute_corpus -v                 # then shadercheck /tmp/comp
```

PPv2 keeps its compute paths off on Android's OpenGL ES (`RuntimeUtilities.isAndroidOpenGL`:
auto exposure, ambient occlusion, the 3D LUT), so its kernels convert but are not used there.
A Windows build's compute shaders (Direct3D 11 bytecode) are not translated yet.
`"shaders": {"compute": false}` leaves compute shaders as they are.

## Unity versions

The converter reads the data's Unity version from the data (`python -m pmunity version <data>`)
and handles 5.6 - 6000.6; other versions are refused unless `--any-unity-version` is given
(then the layouts are guessed and checked by round trip). What changes between the LTS lines:

| | 2017.4 | 2018.4 | 2019.4 | 2020.3 | 2021.3 | 2022.3 |
|---|---|---|---|---|---|---|
| serializer (program entry layout) | 201708220 | 201802150 | 201806140: global + local keyword lists | 201806140 | 202012090: parameters as entries of their own | 202012090 |
| blob container | one LZ4 block per platform, 8-byte entry table | same | 2019.3+: segments (Unity's 16 MB variant chunks), 12-byte entries (offset, length, segment) | same | same | same |
| where the subprograms are | `m_SubPrograms`, hardware tier per subprogram | same | same, keyword indices global + local | same, parameters grouped in `m_Parameters` (+ program `m_CommonParameters`) | `m_PlayerSubPrograms`: a list per tier (desktop builds use the 4th, tier-independent one) with the parallel `m_ParameterBlobIndices` | same, + `m_SerializedKeywordStateMask` |
| also kept in step | - | - | - | pass `m_Platforms` | + Shader `stageCounts` | same |
| GL program text | plain `#extension` lines | same | `#ifdef`-wrapped extensions; macro blocks for uniform buffers and uniform locations (`UNITY_LOCATION(n)`, `UNITY_BINDING(n)`) | same | same | same |
| GLCore41/43 programs, stripped shaders | not translated by default (2017.x: the output stays as before) | translated / empty GLES3 blob | same | same | same | same |

The other lines use these layouts too (their built-ins read and write back with them):

| lines | serializer | container, subprograms |
|---|---|---|
| 5.5 - 2017.2 | 201609010, written as 201708220 (4 statistics, one keyword list) | one block per platform, `m_SubPrograms`; 5.x GraphicsSettings has no `m_ShaderDefinesPerShaderCompiler` |
| 2023.1 - 6000.5 | 202012090 | as 2022.3 |
| 6000.6 | 202605290, written as 202012090 | as 2022.3; the parameter entries in a layout of their own (typed lists, bindings as Unity's (encoded data, slot) structs; `m_CommonParameters` with `m_OffsetInConstantBuffer`, `m_Binding`): the GL path keeps them as they are, the Vulkan path reads and writes them ([below](#vulkan-only-builds-vulkan-programs-spir-v)), the Direct3D 11 and Metal paths do not read them yet |

5.5 writes 201609010 too but has not been checked (refused with the rest of 5.x before 5.6); 5.0 -
5.4 keep their programs in another Shader layout (no `compressedBlob`).

In Unity's own built-in resources of each line, which hold GLCore and GLES3 programs of the same
shaders (desktop players ship one file for every API), the GLES3 blob differs from the GLCore one
only in the code and type of the program entries (same keywords, statistics, binding info, and
2021.2+ parameter entries), and every GLES3 subprogram equals the GLCore subprogram of its variant
but for the type: what the converter writes. Segments: the GLES3 blob keeps the segment of every
entry of the GLCore one.

The macro blocks of 2019+ programs are kept as Unity keeps them in its GLES3 programs: GLSL ES
3.00 has no `layout(location)` on uniforms nor `layout(binding)` on blocks, and libunity
(2019.4 - 2022.3) carries the line `#define UNITY_SUPPORTS_UNIFORM_LOCATION 1` to switch it off by
device. `shaders-export` writes each stage as the driver then gets it (`as_driver_sees`: the
switch at 0 in `#version 300 es` stages). Unity's own Android GLES3 programs compile on Mesa that
way (78 of 136; the others are VR and frame-debugger variants), and 41 without it.

### Checked on Unity's data and on games

- **Built-in resources** of every LTS line's last patch (Linux player's "unity default resources",
  with Unity's GLES3 programs in the same file and the Android player's copy as references;
  `tests/test_glcore_corpus.py`): every GL and D3D11 blob reads and writes back byte for byte,
  every shader converts but 2022.3's `FrameDebuggerRenderTargetDisplay` (a `sampler2DMSArray`
  variant, editor-only); twins, parameter entries, `m_Platforms` and `stageCounts` equal Unity's;
  every non-VR program declares the same uniforms, inputs, varyings, outputs and samplers as
  Unity's GLES3 program (desktop file and Android player alike): 7, 7, 7, 8, 8, 9 programs from
  2017.4 to 2022.3, and 77 converted programs link on Mesa (llvmpipe, ES 3.2); the 27 that do not
  are the `STEREO_MULTIVIEW_ON` ones (`GL_OVR_multiview2`, which llvmpipe lacks). The same test
  on the "unity default resources" that free Linux games of 5.6.0 and 6000.6.3 carry (built-ins
  "5.6.0f3", "6000.6.0b3"; with the 5.6.0f3 Android player's copy): every blob reads and writes
  back, the GLES3 programs and subprograms equal Unity's (92 subprograms for 5.6, 44 and 16
  parameter entries for 6000.6).

  ```sh
  cd convert && PMUNITY_GLCORE_CORPUS=<linux udr>[:...] PMUNITY_GLCORE_ANDROID=<android udr>[:...] \
      PMUNITY_GLCORE_EXPORT=/tmp/glsl python -m unittest tests.test_glcore_corpus -v    # then shadercheck /tmp/glsl
  ```
- **Six free Linux Mono games**, one per LTS line (not in the repository): all their shaders
  convert but VR-only ones, one with `noperspective` and URP's HDR debug view (an image without a
  format), every converted program declares the same uniforms, blocks, inputs, varyings and
  outputs as the GL program it comes from, and every exported program compiles and links on Mesa
  (llvmpipe, ES 3.2: Mesa 21.2 in `ua-build-arm64` and 22.3 in `ua-emu-armhf`;
  `PMUNITY_GAMES=<Game>_Data[:...] PMUNITY_GAMES_EXPORT=<dir> python -m unittest
  tests.test_games_corpus`, then shadercheck each `<dir>/<n>`):

  | game data | formats | shaders given GLES3 (for GLCore41/43 programs) | programs (GLCore41/43; GLES 3.1) | left out | distinct programs linked on Mesa |
  |---|---|---|---|---|---|
  | 2017.4.40f1 | SF 17, one block, 201708220 | 75 (- : 2017.x) | 280 | `BlitToDepth`, `BlitToDepth_MSAA` (2017.x; with `glcore4x` on: 77 shaders, 282 programs, 241 / 241 link) | 239 / 239 |
  | 2018.4.36f1 | SF 17, one block, 201802150 | 36 (2: the blits) | 156 (2) | - | 147 / 147 |
  | 2019.4.8f1 | SF 21, segments, 201806140 | 36 (2, + particles' procedural instancing variants) | 183 (16; 14) | VR blit (`gl_Layer`; its GLCore41 SubShader: a geometry stage) | 164 / 164 |
  | 2020.3.49f1 | SF 22, segments, 201806140 | 53 (2) | 213 (2) | VR blit | 185 / 185 |
  | 2021.3.16f1 (URP) | SF 22, segments (URP UberPost: 3 segments, 2048 entries), 202012090 | 84 (6: error shaders, FSR upscaler, blits, `StencilDeferred` empty; + post-processing SubShaders) | 2607 (112; 10) | VR blit, URP XR occlusion mesh | 1008 / 1008 |
  | 2022.3.36f1 (URP) | SF 22, segments, 202012090 | 84 (4: FSR upscaler, blits, `BlitHDROverlay` empty; + post-processing SubShaders, particles' instancing variants) | 1199 (59; 57) | VR blit, a third-party outline shader (`noperspective`: ES has it only with `GL_NV_shader_noperspective_interpolation`), URP `HDRDebugView` (writes an image declared without a format; HDR output debugging) | 948 / 948 |

  Before GLCore41/43 programs were translated: 34, 34, 51, 78 and 80 shaders from 2018.4 on. The
  players log "Desired shader compiler platform 9 is not available in shader blob" for each shader
  without a GLES3 blob they load: under emulation 5 times in the 2021.3 game and 3 in the 2022.3
  one before, once in each now (a shader left out above); twice in the 2017.4 game (its two
  blits, 2017.x). Both URP games still stop on Unity's graphics thread after two frames, at the
  same code address as before.

What was not checked: nothing of this has run in a Unity player of these versions; Unity's own
GLES3 programs of a game's shaders were not available to compare with (only built-ins), and for
GLCore41/43 programs there is no Unity reference at all (Unity's built-in resources have none):
their rules come from the GLSL ES 3.00 / 3.10 specifications and Mesa's compiler, and no Mali
compiler has seen them. Storage buffers in a vertex stage (particles' procedural instancing) need
a driver that has vertex shader storage blocks, which ES 3.1 does not require. Desktop
builds put every subprogram in the tier-independent place (tier 3, 2021.2+: the 4th list), where
Android builds have one per tier: Unity 2017.4's Android player takes the desktop form (Cuphead),
later players are assumed to (the selection code is shared with the desktop players).

## Windows builds: Direct3D 11 programs

```sh
tools/hlslcc/build.sh                                   # once: HLSLcc (pinned commit) + pmhlslcc
python -m pmunity shaders <Game>_Data <out> --from d3d11    # or --from glcore,d3d11; config: "from": ["d3d11"]
```

A Windows build's Shader assets carry Direct3D 11 programs only (platform blob 4). Unity makes
its own OpenGL programs by compiling the HLSL with fxc to DXBC and translating that with HLSLcc
(MIT, github.com/Unity-Technologies/HLSLcc), so the same translator applied to the Windows
build's DXBC gives the programs Unity would have written, except for what the HLSL itself decided
by platform at compile time (below). `tools/hlslcc/build.sh` fetches HLSLcc at a pinned commit and
builds `pmhlslcc`, a small front end (one-shot, or a server the converter keeps open); macOS
clang, Linux gcc (aarch64 in `ua-build-arm64`, armhf with `ua-host-armhf`'s cross compiler) give
byte-identical output. Ship the binary with `pmhlslcc.LICENSE.txt` (HLSLcc MIT, bstrlib BSD).
A port's setup translates on the handheld: `ARCH=arm64 tools/hlslcc/build.sh` builds the aarch64
binary in that image (into `tools/hlslcc/out/linux-arm64/`; libstdc++ and libgcc linked in, so it
needs only glibc 2.17's symbols), `pmunity port package` ships it as `setup/pmhlslcc` with its
licence for a port with `shaders.from = ["d3d11"]`, and the setup sets `PMHLSLCC` (docs/porting.md,
"Routes"). In the rehearsal container (aarch64, tools/port/rehearse.sh) Tacticsweeper's 45 shaders
(177 programs) translate in 4 s.

What the path does (`dxbc.py`, `convert.add_gles3_d3d11`), for Unity 2017.3 - 2022.3 (all four
program serializers):

1. **Entries.** One D3D11 blob entry is one stage: Unity's header (6 bytes up to 2019.3: 1, then
   the numbers of texture, constant buffer and sampler slots and two more; 38 bytes from 2019.4),
   the DXBC container, and after the code Unity's binding info. Unity strips the container's
   reflection chunk (RDEF: constant buffer layouts, resource names) in builds; the binding info
   replaces it: the vertex channel source map and inputs, every constant buffer with its used
   members (name, type, rows, columns, matrix, array size, offset) and struct members (GPU
   instancing data), and the bindings (constant buffers and textures by register, textures with
   their dimension and sampler, inline sampler states). In 2018.2+ a texture binding's two ints
   are the sampler index and `dimension << 1 | multisampled`; from 2021.2 the bindings are a
   parameter entry of their own (`m_ParameterBlobIndices`); from 2020.1 what all the
   subprograms of a program share is stored once, in the parsed form's `m_CommonParameters`, and
   left out of the entries (`with_common` puts it back). Every entry is checked by writing it back
   byte for byte, and against the subprogram's parsed form where it lists parameters (up to 2019).
2. **Reflection.** The registers, texture types and sampler modes come from the declarations in
   the bytecode (SHDR/SHEX); with the binding info's names and layouts they become an RDEF chunk
   in the form HLSLcc reads. Every constant buffer location an instruction reads must be a member
   Unity described, else the variant is refused (HLSLcc would fail on it).
3. **Variants.** In each pass and hardware tier, a D3D11 vertex subprogram is paired with the
   pixel subprograms Unity runs it with. From 2019 Unity strips the keywords a stage does not use
   from that stage's variants (a full-screen pass's one vertex program goes with all its pixel
   variants; a vertex-lit variant's pixel program lacks `VERTEXLIGHT_ON`): two subprograms go
   together when each has the other's keywords that its stage uses (the keywords any of its
   subprograms has), and the GL program gets both's keywords, as Unity's own GL programs have
   (one program for both stages, not stripped per stage). Each pair is translated together (one
   `GLSLCrossDependencyData`, pixel shader first, as HLSLcc requires), with Unity's GLCore
   settings: GLSL 1.50, `HLSLCC_FLAG_TRANSLATE_MATRICES` (`hlslcc_mtx4x4<name>` vec4 arrays) and
   `HLSLCC_FLAG_DISABLE_GLOBALS_STRUCT` (every constant buffer member a plain uniform: Unity's GL
   backends set built-ins and material values by name, also those that are in cbuffers on D3D11,
   e.g. `UnityPerDraw`; a device without uniform buffers gets the same from Unity's own programs).
   Inputs are `in_POSITION0`..., varyings `vs_TEXCOORD0`..., outputs `SV_Target0`..., one GLSL
   sampler per texture named as the texture. Up to 2018 the newer HLSLcc's macro blocks for
   uniform buffers and explicit uniform locations, which those players do not know, are written
   out as switched off, and `#ifdef`-wrapped extensions plainly, as Unity 2017 stored them; from
   2019.1 they stay as Unity's own programs have them, with each texture's unit
   (`UNITY_LOCATION(n)`, `HLSLCC_FLAG_FORCE_EXPLICIT_LOCATIONS`), and the uniforms are declared
   in Unity's order (the members of Unity's named constant buffers before the shader's own
   `$Globals`, as the GL compile's single `$Globals` has them from the HLSL source). A cube shadow
   map compared at level 0 (D3D's `SampleCmpLevelZero`, point-light shadows: `textureLod` on a
   `samplerCubeShadow`, which no GLSL ES has) is sampled with `texture()`, as Unity's Android
   builds have it. The program then goes through the GLSL ES rules above (precision, rewrites,
   unsupported constructs) like a GLCore program, as GLSL ES 3.10 (a GLES 3.1 program) where it
   needs that (multisampled textures, `textureGather`).
4. **GL data**, as Unity writes GL programs of the data's version:
   - up to 2018: GL subprograms have no parameters, bindings or vertex channels, only the source
     map (its GL backends find uniforms, textures and inputs by name in the linked program), and
     every GL entry's binding info is the source map followed by `0 1 0 0 0 0 0` (all 5782
     GLCore entries of Cuphead and all GL entries of Unity's Windows and Android player
     resources). The program is stored in the vertex subprogram's entry and the fragment
     subprogram's entry has no code; the GLES3 blob keeps the D3D11 blob's indices (a vertex entry
     shared by variants with different pixel shaders gets more entries);
   - 2019.x: the same, but the vertex entry carries the GL program's binding info
     ([below](#gl-binding-info-unity-20191)), which the vertex subprogram lists too
     (`m_ConstantBuffers`, `m_TextureParams`, with names added to the pass's `m_NameIndices` where
     needed);
   - 2020.1+: Unity's GL programs have vertex subprograms only (one program for both stages):
     the GLES3 blob has one entry per program (2021.2+: and a parameter entry, shared where
     equal), the twins' parameter lists stay empty, the pass lists the GLES3 platform
     (`m_Platforms`) and 2021.2+ the Shader's `stageCounts` gets it with 1. The vertex program's
     `m_CommonParameters`, which the player would add to every subprogram of the program (ours
     too), go back into its D3D11 subprograms' binding info and are cleared.
5. **What is left out.** A SubShader with a variant that cannot be translated gets no GLES3
   programs, so Unity uses the next SubShader or the Fallback (as for a SubShader an Android build
   has no programs for); a shader none of whose SubShaders translate gets none. Left out instead,
   the SubShader keeping its other variants: VR-only variants (`STEREO_INSTANCING_ON` writes
   `gl_Layer`, which ES 3.00 lacks), which Unity never asks for on a handheld, and GPU instancing
   variants (`INSTANCING_ON`, `PROCEDURAL_INSTANCING_ON`, `DOTS_INSTANCING_ON`: instance data in
   arrays of structs or structured buffers), whose pass then has its 2021.2+ instancing flags
   (`m_HasInstancingVariant`, `m_HasProceduralInstancingVariant`) cleared so the player draws such
   objects one at a time. A shader whose variants were all stripped from the build (an empty D3D11
   blob, e.g. URP's post-processing shaders a game does not use) gets an empty GLES3 blob
   (2018.1+), as the GLCore path does. The run logs each case.

| translates | does not (SubShader left out) |
|---|---|
| vertex + pixel shaders, shader model 4.0 and 5.0 | geometry, hull, domain, compute shaders; shader model 5.1 |
| constant buffers of float, int and uint scalars, vectors, matrices, arrays | struct members in constant buffers (GPU instancing data; such variants are left out, above); other constant types |
| textures 2D, 3D, cube, 2D array, 2D multisampled (GLES 3.1); samplers, comparison (shadow) samplers, inline sampler states (as the texture's own sampler on GL) | UAVs, structured / raw buffers, RW textures; 1D, cube array and multisampled array textures; LOD and sample count queries |
| everything else HLSLcc writes as GLSL ES 3.00 / 3.10 (integer and bit operations, loops, immediate constant buffers, derivatives, discard, MRT, depth output, `textureGather`) | GLSL with no ES equivalent (the GLSL ES rules above, e.g. `noperspective`) |

Not carried over, because it is not in the bytecode: what the HLSL decided per platform at
compile time. Where a shader tested `UNITY_REVERSED_Z`, `UNITY_NEAR_CLIP_VALUE` or
`UNITY_UV_STARTS_AT_TOP`, the program keeps Direct3D's choice: clip-space depth arithmetic for
reversed Z (the shadow caster bias `UnityApplyLinearShadowBias`, fog distance from clip-space z
`UNITY_Z_0_FAR_FROM_CLIPSPACE`, effects that turn depth values around or draw at the near plane)
and flips for Direct3D's top-left texture origin (GrabPass coordinates in `ComputeGrabScreenPos`).
Ordinary drawing is not affected: matrices, `_ProjectionParams`, `_ZBufferParams`,
`_ScreenParams` and texel sizes are set by the engine for the API it runs on, so sprites, UI,
lighting and post effects that only sample textures come out as on PC. A row-major matrix
(`row_major`, rare in Unity shaders) would be transposed: the binding info does not record the
packing, column-major is assumed. Inline sampler states (`sampler_PointClamp`) have no
equivalent in Unity's GL binding info as seen (every texture of every GL program in the data
checked samples with its own unit's sampler): such a texture is sampled with its own sampler
state, as Unity's GL programs of shaders using them are assumed to.

**Full-screen passes of the scriptable render pipelines** are the exception made: their flips
are undone (`dxbc.gl_uv_origin`, logged as `uv-origin` among the step's rewrites). Every SRP
full-screen pass (URP/HDRP 13+ blits, post-processing, SSAO, the colour-grading LUT, Shader
Graph full-screen materials) draws a triangle or quad made from `SV_VertexID` by the Core RP
library (`GetFullScreenTriangleTexCoord`, `GetQuadTexCoord`), whose texture coordinate is
`1 - v` under `UNITY_UV_STARTS_AT_TOP`, and URP's C# side only flips for Direct3D when it runs on
it (`SystemInfo.graphicsUVStartsAtTop`). With Direct3D's code on GL each blit turned the picture
upside down, and the passes that compute values from the coordinate got them mirrored: URP's
`LutBuilderLdr`/`LutBuilderHdr` wrote the colour-grading LUT upside down (green rows where the
dark ones belong), so with post-processing on, the final `UberPost` turned a dark picture
green (Die to Play, Unity 6000.1, URP 17). In a vertex stage that reads `gl_VertexID`, HLSLcc's
`<t> = (-<v>) + 1.0;` where `<v>` is traced back (through the stage's temporaries) to
`float(id & 2)` or `float(((id >> 1) + (id & 1)) & 1)` becomes `<t> = <v>;`, which is what
Unity's own GL compile of the same HLSL gives; Shader Graph's full-screen target flips under the
macro unless its global `_FlipY` is set (URP never sets it), so its `_FlipY<0.5` becomes `false`.
Other flips of such geometry stay (the lens flare's own `u = 1 - u`). Still Direct3D's: the
fragment-side reconstruction of clip-space positions (`ComputeClipSpacePosition`'s `y = -y`,
used where a pass rebuilds world positions from depth: SSAO, motion vectors, depth of field) and
reversed-Z depth arithmetic, which shift such effects rather than the picture.

### GL binding info (Unity 2019.1+)

From 2019.1 Unity's GL programs (GLCore and GLES3 alike) carry binding info of their own, which
`dxbc.gl_bindings` writes from the program's text the way Unity's GL compiler does from HLSLcc's
reflection (HLSLcc reports each loose uniform with offset 0; Unity lays them out):

- constant buffers: the empty one, then per stage in HLSLcc's order (fragment first) a
  `$Globals` of the stage's loose uniforms, members listed vectors first, then matrices (names
  without the `hlslcc_mtx4x4` prefix, rows and columns 4, array the number of matrices), ints and
  bools as uint (5); then the stage's uniform blocks (each once);
- offsets in declaration order: 2020.1+ aligned to 4 / 8 / 16 / 16 for 1 - 4 components and to 16
  for arrays and matrices, arrays packed by their element size (as `glUniform*v` reads them), the
  size the end of the last member; 2019.x vec4s and matrices on 16, a smaller vector that would
  cross a 16-byte boundary moved on by as many bytes as it would cross it by (Unity 2019's rule,
  as its own programs have it: `vec3, vec3` at 176 and 196);
- 2022.1+: one `$Globals` for all stages, its members keyed by offset (a later stage's member at
  an offset already taken is not listed: Unity's own programs have it so, e.g. a vertex
  `unity_ObjectToWorld` at 0 next to a fragment `_Color` at 0), listed by offset, with a binding
  `$Globals` 0; uniform blocks after it in reverse order;
- bindings: the textures (each name once, `hlslcc_zcmp<name>` being `<name>`; index and sampler
  index the unit, dimension and multisampling from the sampler type), then the blocks.

Checked against every GLCore and GLES3 program (not VR-only) of Unity's Windows player resources
2019.4 - 2022.3, the 2019.4 Android player's and nine Linux games of 2019.4 - 2022.3 (four of the
corpus and five of other agents', `tests/test_gl_bindings_corpus.py`): all 6813 programs with
loose uniforms (what this path writes) match, 6209 of them byte for byte and the others with the
program's shared parameters put back (2020.1+, compared as sets); of the 1238 with uniform blocks
(not written by this path) 1180 match (the rest: a one-element array in a block, which HLSLcc
declares as a plain variable, and the order of the blocks of several stages in 2022.1+).

```sh
cd convert && PMUNITY_GL_BINDINGS=<file or data folder>[:...] python -m unittest tests.test_gl_bindings_corpus -v
```

### Checked on Unity's data and on Windows games

**Unity's Windows player resources** ("unity default resources" of the Windows support
installers, which also hold Unity's own GLCore and GLES3 programs of the same shaders;
`tests/test_d3d11_corpus.py`): every D3D11 entry reads and writes back byte for byte, every
shader converts (2022.3's editor-only `FrameDebuggerRenderTargetDisplay` aside: a
`sampler2DMSArray` variant), every variant that is not VR-only translates, and every non-VR
translated program declares the same uniforms, inputs, varyings, outputs and samplers as Unity's
GLCore and GLES3 programs of the variant, with the same code up to temporaries:

| player resources | D3D11 subprograms | variants translated (VR-only left out) | same code as Unity GLCore (byte for byte) | binding info as Unity's (GLCore / GLES3) | GLES3 subprograms equal to Unity's |
|---|---|---|---|---|---|
| 2017.4.9f1, 2017.4.40f1 | 94 | 36 (11) | 14 (9) | - (source map only, as Unity's) | 22 |
| 2018.4.36f1 | 94 | 36 (11) | 14 (0: temporaries) | - | 22 |
| 2019.4.40f1 | 94 | 36 (11) | 14 (13) | 14 / 14 | 28 (14 vertex, 14 fragment) |
| 2020.3.48f1 | 102 | 39 (12) | 15 (14) | 15 / 15 | 15 |
| 2021.3.45f2 | 102 | 39 (12) | 15 (13) | 14 / 14 | 14 |
| 2022.3.62f3 | 122 | 50 (12; 3 editor-only) | 26 (25) | 25 / 25 | 19 |

The programs that differ from Unity's GLCore code byte for byte differ in the order of
declarations or the names of temporaries (Unity's GL compile sees `fixed`/`half` values);
`Hidden/Internal-Loading` (2021.3+) has uniform blocks in Unity's GL program, which declare
their unused members too, and loose uniforms in ours (a subset, same code otherwise). (2017.4.40f1:
the Linux player's file, which has the D3D11 programs too.)

```sh
cd convert && PMUNITY_D3D11_CORPUS="<path>/unity default resources" PMUNITY_D3D11_EXPORT=/tmp/glsl \
    python -m unittest tests.test_d3d11_corpus -v                  # then shadercheck /tmp/glsl
```

**Six free Windows-only Mono games** (not in the repository), each converted with
`pmunity convert --set shaders.from=d3d11`, every distinct GLES3 program exported and compiled
and linked on Mesa (llvmpipe, ES 3.2, Mesa 21.2 in `ua-build-arm64`), then packed and run on
the runtime in the emulated rig with the game's exact Android player:

| game data | shaders given GLES3 | programs (GLES 3.1) | left out | distinct programs linked on Mesa | rig |
|---|---|---|---|---|---|
| 2018.3.7f1, 3D, SF 17, 201802150 | 33 of 34 | 196 | `BlitToDepth_MSAA` (`textureSamples`); 8 particle procedural-instancing variants (structured buffer) | 181 / 181 | first screen (the game starts in its first level) |
| 2019.2.5f1, 2D, SF 20, one block, 201806140 (two keyword lists) | 42 of 44 | 205 | `BlitToDepth_MSAA`, VR blit (`gl_Layer`), UIElements' `Internal-UIRDefault` SubShader 0 (structured buffer; SubShader 1 kept) | 180 / 180 | title screen |
| 2019.3.14f1 (URP 7), SF 21, segments | 60 of 62 | 918 (3) | `BlitToDepth_MSAA`, VR blit (2 SubShaders) | 504 / 504 | Unity splash, title menu (URP post-processing on) |
| 2020.3.44f1, SF 22, `m_CommonParameters` | 50 of 52 | 212 | `BlitToDepth_MSAA`, VR blit | 192 / 192 | splash, then its first (click to start) screen |
| 2021.3.28f1, compressed build (`pmunity unpack`), 202012090 | 49 of 51 | 270 | `BlitToDepth_MSAA`, VR blit | 240 / 240 | title menu (with `pmunity.winbcl`; before: Mono asserted in the Input System's start on the Windows class libraries, then SIGSEGV) |
| 2022.3.62f3 (URP 14), Burst plugin | 82 of 85 (6 stripped: empty GLES3 blobs) | 569 (55) | URP `HDRDebugView` (UAV), `BlitToDepth_MSAA`, VR blit; 8 particle procedural-instancing variants | 440 / 440 | title menu |

Every converted program declares the same uniforms, inputs and outputs in its GLES3 form as in
its GL one, and HLSLcc's translation of each variant is checked as above only where Unity's own GL
program is at hand (built-ins). Textures of these games (RGBA32, RGB24, Alpha8, DXT1/5, crunched
DXT, BC6H cube maps) go through the textures step as before (BC6H left as it is); the rig's Mesa
samples BC textures itself, so the runs use the unconverted ones. The 2021.2+ games need Unix
class libraries in place of their Windows ones (`pmunity winbcl`, which a port's setup runs;
docs/porting.md).
Not checked: a run on the handheld, pictures against the Windows build, and any game with BC7,
BC4 or BC5 textures (the step's decoding of those is tested on hand-made blocks,
`tests/test_textures.py`).

`tests/test_dxbc.py` checks the path on hand-assembled vertex and pixel programs (no Unity files;
the HLSLcc part runs when pmhlslcc is built).

## Vulkan-only builds: Vulkan programs (SPIR-V)

```sh
tools/spirvcross/build.sh                                  # once: SPIRV-Cross + SMOL-V (pinned commits) + pmspirvcross
python -m pmunity shaders <Game>_Data <out> --from vulkan     # config: "shaders": {"from": ["vulkan"]}
```

Unity compiles its Vulkan programs as it does its GL ones: HLSLcc turns the HLSL's DXBC into
Vulkan GLSL, glslang into SPIR-V. So the SPIR-V keeps HLSLcc's shape (matrices as vec4 arrays, one
uniform block per constant buffer, a separate texture and sampler per texture), and Khronos'
SPIRV-Cross (Apache 2.0) writes GLSL ES from it. `tools/spirvcross/build.sh` fetches SPIRV-Cross
and Aras Pranckevicius' SMOL-V decoder at pinned commits and builds `pmspirvcross`, a small front
end (one-shot, or a server the converter keeps open); `ARCH=arm64` builds the aarch64 binary for
the handheld in `ua-build-arm64` (libstdc++ and libgcc linked in; it writes the same GLSL as a
macOS clang build, byte for byte, on the 61 modules of a URP game compared), which `pmunity port package`
ships as `setup/pmspirvcross` for a port with `shaders.from = ["vulkan"]` (the setup sets
`PMSPIRVCROSS`), with its licences (THIRD_PARTY.md).

What the path does (`vulkan.py`, `spirv.py`), for Unity 2017.4 - 6000.x:

1. **Entries.** The Vulkan blob (platform 18, gpu program type 25) has the GL layout of the same
   Unity version: up to 2019.x a vertex and a fragment subprogram per variant (the fragment
   entry without code), from 2020.1 one subprogram per variant, 2021.2+ with a parameter entry.
   An entry's code is an int32 (0 up to 2019; from 2020.1 32 | requirement bits), a table of
   (offset, size) per stage slot (5 in 2017.x, 6 in 2019.x, 9 from 2020.1: vertex, fragment,
   hull, domain, geometry, ...; 2022.1+ followed by an int32 1 and two 16-byte hashes), then each
   stage's SPIR-V module compressed with SMOL-V. Unity 2017 - 2019 wrote SMOL-V's older encoding
   (smolv's 2016-08-31 code; the header does not tell): pmspirvcross takes the decoding SPIRV-Cross
   parses. After the code, Unity's binding info in the Direct3D 11 layout (`dxbc.Bindings`): the
   vertex channels (source ShaderChannel, target 13 + the input's location; 12 before 2018.2),
   the constant buffers with the members the program reads, and the bindings, whose index is
   stage bits << 24 | descriptor set << 16 | binding (stage bits 4 vertex, 8 fragment, 64 geometry;
   a texture's first int its sampler's index alike). Every entry writes back byte for byte.
   **6000.6** (serializer 202605290; 6000.5 still writes the layout above) lays the parameter entry
   out anew (`dxbc.Bindings` with `typed`): a constant buffer's members as a vector list and a
   matrix list, each (name, dimension or rows | type << 16, array size, offset), structs alike; then
   the bindings as lists by kind, textures, UAVs, constant buffers, buffers, inline samplers, each
   binding Unity's Binding struct (int32 encoded data, int32 slot: the slot is the index above, the
   encoded data 0 in all data seen), a texture with its sampler's binding and dimension << 1 |
   multisampled, an inline sampler without a name. The program's `m_CommonParameters` use 6000.6's
   names too (`m_OffsetInConstantBuffer`, `m_Binding`, `m_SamplerBinding`, slots stored unsigned).
   The GLES3 twins' GL binding info is written in that layout, as Unity's own 6000.6 GL programs
   have it (the same buffers and bindings as before, the `$Globals` binding at slot -1):
   `dxbc.gl_bindings` writes it byte for byte for 1450 of the 1526 GLCore programs of Phasing Gun,
   IronQuiet and Cold Apples it reads (all without uniform blocks; the others differ as on the lines
   before, a one-element array in a block, the order of several stages' blocks;
   `tests/test_gl_bindings_corpus.py`), and for Unity's own GLES3 programs of the 6000.6.0b3
   built-ins IronQuiet carries but three VR variants (66 of 69: the order of the stereo blocks).
2. **Names.** Unity strips the modules' debug names (but some varyings'). The path names, in the
   SPIR-V (OpName / OpMemberName), everything the way Unity's GL backend binds HLSLcc's GL programs:
   vertex inputs by their channel (`in_POSITION0`, `in_NORMAL0`, `in_TANGENT0`, `in_COLOR0`,
   `in_TEXCOORD0-7`, `in_BLENDWEIGHTS0`, `in_BLENDINDICES0`: libunity's list; 2017.x's channel
   numbering before 2018.2), varyings by location (`vs_LOCATION<n>` in both stages), fragment
   outputs `SV_Target<n>`, constant buffer members by offset from the binding info (matrices
   `hlslcc_mtx4x4<name>`; Unity's array size is the part the program reads, so the SPIR-V's
   declared size is kept), textures and samplers by their binding.
3. **GLSL.** SPIRV-Cross writes GLSL ES 3.00 (3.10 where the program needs it: `textureGather`,
   `bitfieldInsert` in URP's full-screen vertex shader, multisampled textures), each texture
   combined with its sampler as one GLSL sampler named as the texture (`hlslcc_zcmp<name>` for a
   comparison, after the texture's plain sampler, as HLSLcc declares both; a texture sampled with
   a second sampler uses its own: GL has one sampler state per texture), without descriptor sets,
   bindings or input locations, default precision highp. Then, as Unity's GL programs have it:
   each stage's globals (`VGlobals<hash>`, `PGlobals<hash>`) as plain uniforms, only those the
   stage reads; other constant buffers (URP's `UnityPerDraw`, `UnityPerMaterial`, ...) as uniform
   blocks of their name (unread members `Xhlslcc_UnusedX_<block>_<offset>`; up to 2018 as plain
   uniforms too); from 2019.1 Unity's macro block with each sampler's unit (`UNITY_LOCATION(n)`,
   program-wide, the fragment stage's first) and each block's binding (`UNITY_BINDING(n)`);
   `ddx_fine` and the other derivative kinds as GLSL ES's one (`dFdx`, as HLSLcc writes them for
   GL). Unity's Vulkan programs end their vertex stage with `gl_Position.y = -gl_Position.y`
   (before every return; Vulkan's clip space has y down): left out, since the engine gives a GL
   device GL's matrices.
4. **GL data**, as for the GLCore path: up to 2019.x the GLES3 blob keeps the Vulkan indices
   (`dxbc.gl_rest` up to 2018; 2019.x the GL binding info, `dxbc.gl_bindings`, in the vertex entry
   and subprogram), from 2020.1 entries numbered anew with Unity's GL binding info (2021.2+ in
   parameter entries, shared where equal), the Vulkan program's `m_CommonParameters` put back
   into its Vulkan entries and cleared. The twins keep the Vulkan subprograms' keywords, tier and
   requirements.
5. **Left out**, with the reason, as for Direct3D 11: a SubShader with a variant that cannot be
   translated (geometry and tessellation stages, storage buffers and images, subpass inputs, cube
   map arrays, what GLSL ES lacks); VR-only, GPU instancing and sprite skinning variants are left
   out instead, the SubShader keeping its other variants.

What the Vulkan compile decided by platform stays as for Direct3D 11 ([above](#windows-builds-direct3d-11-programs)):
`UNITY_REVERSED_Z`, `UNITY_UV_STARTS_AT_TOP` and `UNITY_NEAR_CLIP_VALUE` are Direct3D's on Vulkan,
and so are platform-sized arrays (URP's additional lights: 256 for Vulkan, 32 for GL). The same
exception is made as for Direct3D 11: the full-screen passes' texture coordinates of the SRP core
library get GL's origin (`vulkan.uv_origin`, the same trace as `dxbc.gl_uv_origin` in SPIRV-Cross's
form of the code: declarations, constructors, `+=`; logged as `uv-origin`). In a URP 2022.3 game 962
of 2071 programs of the shaders in `globalgamemanagers.assets` get it (UberPost's 720 variants,
FinalPost, the blits, Bloom, TAA, SMAA, depth of field, the LUT builders, ...); the lens flare's own
`u = 1 - u` stays. Compute
shaders' Vulkan kernels are not translated yet (the GLCore path's `compute` needs GLCore kernels).

**Checked** (`tests/test_vulkan_corpus.py`, sixteen free Linux games that have both OpenGL Core and
Vulkan programs, translated from Vulkan alone and compared with the same pass's GLCore program of
the variant; `tests/test_vulkan.py` on hand-assembled SPIR-V, `tests/test_dxbc.py` on hand-made
6000.6 parameter entries): every Vulkan entry writes back byte for byte, every twin's GL binding
info reads back, and every distinct translated program compiles and links on Mesa (llvmpipe, ES
3.2; `ua-build-arm64`'s Mesa 21.2):

| game data | shaders given GLES3 (empty blobs) | programs (GLES 3.1) | left out | interface as the GL program's | distinct programs linked on Mesa |
|---|---|---|---|---|---|
| 2017.4.40f1 | 77 of 77 | 282 | - | 283 of 290 (the editor-only Meta pass's `bool4` is `uvec4`; a post effect's `UNITY_UV_STARTS_AT_TOP` branch) | 243 / 243 |
| 2018.4.36f1 | 36 of 36 | 156 | - | 160 of 160 | 147 / 147 |
| 2019.4.8f1 | 36 of 37 | 169 | VR blit (geometry stage); 14 instancing variants | 175 of 175 | 154 / 154 |
| 2020.3.49f1 | 53 of 54 | 213 | VR blit | 220 of 220 | 184 / 184 |
| 2021.2.9f1 (URP) | 71 (2) of 79 | 633 (8) | VR blit, 7 with structured buffers | 671 of 677 | 541 / 541 |
| 2021.3.16f1 (URP) | 84 (1) of 86 | 2556 (33) | VR blit, XR occlusion mesh (`gl_Layer`) | 2541 of 2548 | 988 / 988 |
| 2022.2.15f1 (URP) | 84 (5) of 86 | 1026 (324) | VR blit, a UAV | 689 of 1061 (URP's light arrays sized for Vulkan) | 798 / 798 |
| 2022.3.36f1 (URP) | 84 (1) of 87 | 1177 (866) | VR blit, a `noperspective` outline, a UAV; 3 instancing variants | 1248 of 1255 | 934 / 934 |
| 2023.1.8f1 (URP) | 97 (4) of 102 | 1666 (529) | VR blit, 4 with buffers; 186 instancing variants | 995 of 1704 (light arrays) | 1438 / 1438 |
| 6000.3.11f1 (URP 2D) | 66 (4) of 76 | 1342 (13) | VR blit, framebuffer fetch, 2D MS arrays, 7 with buffers | 1341 of 1356 | 1002 / 1002 |
| 6000.4.6f1 (URP) | 112 (8) of 123 | 1553 (1181) | as 6000.3; 192 instancing variants | 1355 of 1557 (light arrays) | 1419 / 1419 |
| 6000.5.7f1 (URP) | 80 (4) of 90 | 2180 (842) | as 6000.3 | 2176 of 2194 | 1673 / 1673 |
| 6000.5.9f1 (URP, IL2CPP) | 82 (7) of 92 | 871 (595) | as 6000.3 | 834 of 882 (light arrays; `_ScaleBiasRt`, which the Vulkan fragment stages read) | 811 / 811 |
| 6000.6.2f1 (URP) | 97 (12) of 107 | 526 (228) | as 6000.3; 57 instancing variants | 466 of 498 (light arrays, `_ScaleBiasRt`) | 464 / 464 |
| 6000.6.3f1 (built-in pipeline) | 48 of 54 | 300 | VR blit, 5 with buffers; 28 instancing variants | 307 of 315 (`UI/Default`'s `_SpaceWarpNDCModifier`, read in Vulkan only) | 273 / 273 |
| 6000.6.4f1 (URP) | 83 (10) of 93 | 426 (124) | as 6000.3 | 422 of 440 (light arrays) | 361 / 361 |

(The other differences: a fragment output named `SV_TARGET0` in GL, varyings a GL stage leaves
out.) The 6000.6 games gave no program before the path read 6000.6's parameter entries (the 2021.2
reader took the string `PGlobals...` for a count: "binding info does not match the known layout");
the games of 2017.4 - 6000.5 are unchanged by it (every converted Shader the same, checked in
memory), and so is the Direct3D 11, Metal and GL paths' output. **Suvalf** (6000.3.5f1, HDRP, Vulkan only, compressed: `pmunity unpack` first): 116 of 142
shaders given GLES3 programs (16 of them empty), 718 programs (345 GLES 3.1), 553 distinct, all
linking on Mesa; 26 left out (HDRP's lit and volumetric passes: structured buffers, 13 of them in
the shared parameters; XR's 2D multisampled arrays; cube map arrays; framebuffer fetch); its 118
compute shaders have Vulkan kernels only. HDRP itself does not run on OpenGL ES: converted so and run in the emulated rig, the
player logs "graphics API OpenGLES3 is not supported with HDRP" and HDRP's `Render` throws every
frame, the screen black. `port check` blocks the Android route for an HDRP build.

## macOS builds: Metal programs (MSL)

```sh
python -m pmunity shaders <Game>.app/Contents/Resources/Data <out> --from metal    # config: "shaders": {"from": ["metal"]}
```

macOS builds of Unity 2020.3 and later have Metal programs only (17 of 18 free macOS builds
sampled; 2017.3 - 2020.1 ones also have OpenGL Core). No open tool reads MSL (Apple's compiler is
closed; SPIRV-Cross, Tint, glslang only write it), but Unity's MSL is HLSLcc's too: its Metal
backend writes the same DXBC instruction stream it writes as GL programs, one statement per
instruction with the same names (`u_xlat0`, `hlslcc_mtx4x4...`, `VGlobals`, `input.TEXCOORD0`).
`metal.py` reads that subset of MSL (a tokenizer, an expression parser with the types MSL's
expressions have) and writes HLSLcc's GL form of it:

- entries: a vertex and a fragment subprogram per variant (types 23, 24), their code Unity's
  header (0xf00dcafe, sizes, the entry point's name `xlatMtlMain`) and the MSL, the binding info in
  the Direct3D 11 layout; the two are paired, and each pair stored as a GL program, as for
  Direct3D 11 ([above](#windows-builds-direct3d-11-programs), item 4);
- the constant buffer structs (`constant VGlobals_Type& VGlobals [[ buffer(0) ]]`) as plain
  uniforms, those the stage reads (MSL's structs list every member, for the offsets); the
  `Mtl_VertexIn` members as `in_<semantic>`, `Mtl_VertexOut` / `Mtl_FragmentIn` members
  (`[[ user(TEXCOORD0) ]]`) as `vs_<semantic>` (the fragment input's `[[ flat ]]` on both), outputs
  at their `[[ color(n) ]]`, `[[ position ]]` as `gl_Position` / `gl_FragCoord`, vertex and
  instance ids;
- textures (`texture2d<float, access::sample>`, `texturecube`, `texture3d`, `texture2d_array`,
  `depth2d`, `depthcube`, `texture2d_ms`) as GLSL samplers named as the texture (a depth texture
  compared: its plain sampler and `hlslcc_zcmp<name>`), `.sample` with `level`, `bias` and
  `gradient2d`, `.sample_compare`, `.read`, `.gather` (with a component and an offset) as
  `texture`, `textureLod`, `textureGrad`, `texelFetch`, `textureGather`; inline sampler states
  (`constexpr sampler`) as the texture's own, as on the other paths;
- Metal's functions as GLSL's (`fma`, `rsqrt`, `rint`, `saturate`, `select`, `as_type`, `dfdx`,
  `fmod`, HLSLcc's `bitFieldInsert` template as an ES 3.00 function), its types (`half` as float,
  for desktop precision) and what GLSL ES needs spelled out: vector comparisons (`lessThan`, ...),
  the scalar conversions MSL makes implicitly, `&&`/`||` of bool vectors per component;
- function constants at their defaults (no base vertex, no variable rasterization rate: Unity's
  `#if __METAL_VERSION__ >= 230` blocks and the code behind `has_vrr_api` are left out, the
  identity render target remap), immediate constant buffers as `const` arrays; from 2019.1 the
  samplers' units in Unity's macro, as the other paths.

Unity's Metal compile has Metal's platform choices as Direct3D's (`UNITY_REVERSED_Z`,
`UNITY_UV_STARTS_AT_TOP`); Metal's clip space has y up, as GL's. The full-screen passes' texture
coordinates get GL's origin as for Vulkan (`vulkan.uv_origin` on metal.py's output: 870 of the 957
programs of the URP game's `globalgamemanagers.assets`). Left out: device buffers
(structured buffers), writable textures, VR's render target array index, tessellation.

**Checked** (`tests/test_metal_corpus.py`, `tests/test_metal.py` on hand-written MSL): every entry
writes back byte for byte, and every distinct translated program compiles and links on Mesa (llvmpipe,
ES 3.2):

| game data | shaders given GLES3 (empty blobs) | programs (GLES 3.1) | left out | compared | distinct programs linked on Mesa |
|---|---|---|---|---|---|
| 2019.1.15f1 (Metal + OpenGL Core) | 41 (3) | 208 | UIElements' SubShader with a structured buffer (its fallback kept) | 223 of 223 as the same pass's GLCore program | 194 / 194 |
| 2022.3.62f3 (URP, Metal only) | 92 (4) of 93 | 1250 (8) | URP's HDR debug view (a writable texture) | 610 of 1369 as Unity's own GLES3 programs of the game's Android build (shader, pass, keywords) | 991 / 991 |
| 6000.2.6f2 (Metal only; Addressables) | 113 (2) of 114 | 320 | VR blit | - | 209 / 209 |

Against Unity's own GLES3 programs of the same game (its Android build), the differences are what
the HLSL decided by platform: Metal-only uniforms (foveated rendering keywords and its distortion
values, which are 0 on a GL device, so their branches stay off), the desktop quality tier's
light probe volumes and box-projected reflections, mobile's light counts (16, desktop 256), a
`float` output Android's compile widens to `vec4`. In the 2019.1 game, where the same pass has
OpenGL Core programs, all 223 compared programs declare the same uniforms, samplers, inputs,
varyings and outputs as the GL ones. The 6000.2 game's 44 Addressables bundles (46 shaders of
their own) were built with the Unity version left out of their headers, which UnityPy then does
not open: the shaders step gives it the data's version for them (`step.bundle_version_fallback`;
before, the step stopped on the first such bundle, whatever the source).

## Checking a conversion

```sh
python -m pmunity shaders-export <converted data> glsl/       # every distinct program as .vert/.frag
runtime/host/out/shadercheck glsl/                            # on the handheld: compile + link each
```

`shaders-export` also writes each GLES3 compute kernel as `<nnnn>.comp`, which `shadercheck`
compiles and links as a compute program (OpenGL ES 3.1+). `shadercheck` uses the device's own
driver (a pbuffer, or the fbdev window on Mali). Run it in
`tools/emu` to compile with Mesa too. For Cuphead: 1566 distinct programs; all compile and link on
the RG34XX-SP's Mali-G31 (driver r20p0); on Mesa all but one, the stereo multiview variant of one
font shader (Mesa's llvmpipe lacks `GL_OVR_multiview2`; the game never uses it).

To compare pictures, replay the same frames with two builds and diff the screenshots (for Cuphead
the port's deterministic replay mode and `tools/emu/imgdiff.py`). The rewrites gave the same
pixels as the plain translation on the Mali in every frame compared; full-precision samplers move
1-13 % of the pixels by 1-2 levels of 255 against ES's lowp default, toward the desktop result.

## What costs what on a Mali-G31 (RG34XX-SP)

`shadercheck -t <dir> <n>...` draws full-screen quads (720x480, blended like sprites, so none is
skipped as hidden) with a program and reports the time per quad. Measured on the RG34XX-SP (one
full-screen layer = 345,600 pixels; medians of 5 interleaved rounds, GPU clock as the governor
sets it):

| fragment shader | ms per full-screen layer |
|---|---|
| constant colour | 0.37-0.42 |
| one varying (vertex colour), highp or mediump | 0.72 |
| one texture fetch, highp or mediump | 0.71 |
| Unity's `Sprites/Default` (texture x vertex colour, premultiplied), as converted | 0.96-0.97 |
| the same with everything mediump (Unity's own GLES style) | 0.95-0.96 |
| Cuphead's 7-tap blur pass, as converted | 2.54-2.66 |
| the same with const weights | 2.49-2.55 |
| the same with const weights + unrolled | 2.47-2.54 |

In the game (Cuphead, Glumstone the Giant, alternating runs on the device): the rewrites changed
nothing measurable (+1.2 % and -2.5 % in two phases, within run-to-run noise), and full-precision
samplers cost 0.9 % in one phase and up to ~6 % in another. Cuphead's port therefore builds with
`default-samplers` (pixel-identical to its earlier builds, and what Unity's own GLES compiler gives
those samplers); `highp` stays the converter's default because another game may sample depth or
float textures, which lowp would visibly quantize.

So on this GPU a fragment costs about one unit per texture fetch, per interpolated varying and
for the blended write; arithmetic and precision barely matter (mediump saves ~1 % on sprites).
What a frame costs is mostly the number of pixels drawn times the fetches per pixel: overdraw,
full-screen passes and render target sizes, not shader code. The rewrites above help the blur
passes by ~4 %; fewer fetches (e.g. a 7-tap blur as 4 bilinear fetches) would help more, but
bilinear weights are quantized by the hardware, so the picture changes: that kind of change
belongs in a game's port as an option, not in the converter's defaults.

That holds for sprites and short post-processing passes. The URP 3D games' passes are long
programs in highp, and there arithmetic does decide the cost ([below](#exact-rewrites-for-the-mali-g31s-compiler-looked-for-2026-10-23)).

The timing options (2026-10-23):

```sh
shadercheck -t <dir> <n>... [-r R] [-n N] [-u name=v,v,...]... [-b|-B <out>] [-w <out>] [-o|-p] [-s WxH]
```

- `-r R`: R rounds over the list, the programs interleaved (a GPU clock that moves affects each
  the same way); the median and the range of the rounds;
- `-u name=v,...`: a uniform's values (a plain uniform, a member of a uniform block, an array from
  its first element), e.g. a light count that decides a loop's trips, or an SSAO radius that keeps
  every pixel off the early exit. Uniform blocks are bound to buffers of 0.5 floats (ints 0),
  every sampler type gets a texture (3D, cube, 2D array, depth compared for shadow samplers), and
  Unity's matrices (`hlslcc_mtx4x4unity_ObjectToWorld`, `..._MatrixVP`, ...) are the identity
  in blocks too;
- `-b`: each program's binary size (`glGetProgramBinary`), the compiled code and the driver's
  tables: a static check of what a rewrite of the source changed; `-B <out>`: the binaries
  themselves (`<out>/<n>.bin`; their Bifrost code is in the `OBJC` chunks, which Mesa's
  disassembler reads: [profiling.md](profiling.md));
- `-w <out>`: what each program draws, one quad unblended into an RGBA32F target
  (`<out>/<n>.f32`): a rewrite's picture compared with the original's bit for bit, on the
  device's driver;
- blending is additive (ONE, ONE). With premultiplied alpha (`-p`, as before) a program that writes
  alpha 1 makes its quads opaque, and the Mali's forward pixel kill drops the fragments of the
  quads under the last ones: such programs measured less than a constant colour costs (if the
  table's constant colour above wrote alpha 1, its 0.37-0.42 ms was such a measurement).
- a second of quads before the rounds, and the GPU's devfreq clock at the start and the end (the
  governor starts at 240 MHz: short programs' first rounds ran at a lower clock; compare the
  medians, and the range).

## Exact rewrites for the Mali-G31's compiler (looked for, 2026-10-23)

The question: can the GLSL ES the converter writes be rewritten, with the same IEEE operations
and the same precision, so that the RG34XX-SP's driver (Mali-G31, r20p0) compiles faster code?
The six GPU-bound games of [compat/games.md](compat/games.md#handheld-benches-rg34xx-sp-2026-10-23)
(and Hollow Knight) were traced pass by pass (`UA_GL_TRACE`, `UA_GL_TRACE_DRAWS` with
`UA_SHADER_DUMP`), the programs of their expensive passes taken from the dumps, the patterns
translated code carries timed in small programs, and each hot program timed with `shadercheck -t`
next to its rewrites on the handheld. Where the passes go (GPU ms a frame, serialized,
`UA_GL_TRACE_GPU=3`):

| game, route | the expensive passes | what they run |
|---|---|---|
| Phasing Gun, 6000.6 glcore, URP 3D | SSAO 40.3, its three blurs 4.2 - 4.7 each, 2048x2048 shadow map 17.0, forward 13.9 (of 88.6) | URP's ScreenSpaceAmbientOcclusion (8 samples: sin, cos, 9 fetches, divides), its bilateral blurs, URP Lit (Forward+) |
| The Third Eye, 2022.2 glcore, URP 3D | two cameras: the same SSAO blurs 1.8 - 4.4 each, forward 1.3 + 3.7 (of 39.7) | the same blur programs, URP Lit (a light loop) |
| Arthur's Nightmare, 2017.3 glcore | the 3D room's forward pass 65.2 (of 81.7): 132 draws, 84,000 indices | Standard (forward base, spot-shadow and point forward add) |
| Chapéu de Papel, 2022.3 Metal | the window pass 23.2 (of 26.1) | UberPost, a full-screen watercolour wash (6 fetches), UI with a paper shader |
| White Mirror, 2021.2 glcore, URP 2D | Bokeh depth of field 5.5 a camera (two cameras; of 29.9), UberPost 2.0 - 3.6 | URP's BokehDepthOfField blur: a 42-tap loop |
| Cell Machine, 2019.3 D3D11, URP | the window pass 14.4 (of 23.8) | UberPost with lens distortion and chromatic aberration |
| Hollow Knight, 6000.0 glcore | ([profiling.md](profiling.md#gpu-pass-by-pass)) | sprites (one texture fetch), a 5-tap blur; no loops, no arrays |

What every route hands the driver is HLSLcc's code: Unity's GL programs are HLSLcc's GLSL, its
D3D11 programs go through HLSLcc here too, and its Metal programs are HLSLcc's MSL, read back
statement by statement. It is the code of Unity's own GLES3 programs for the same shaders, up to
the precision qualifiers (Chapéu de Papel's Android build has both: the same statements, with
`mediump` where the HLSL says `half`). Timed on the handheld (ms per full-screen 720x480 layer,
medians of 5 interleaved rounds at 696 MHz, every program shaded in full):

| pattern | as translated | exact rewrite | result |
|---|---|---|---|
| a SPIR-V round trip: glslang -> spirv-opt (only passes that keep every float operation: SSA, dead code, CSE by value numbering, block merging, dead branches, copy propagation, vector DCE) -> SPIRV-Cross, declarations kept | SSAO 59.9, its blurs 4.27 / 4.24 / 3.93, Chapéu's UberPost 5.92, wash 3.65, UI paper 5.28 | 59.5, 4.25 / 4.27 / 3.94, 5.91, 3.65, 5.34 (without passes the same) | nothing: the binaries are the same size within 1 % (16,368 -> 16,464 bytes for SSAO); the driver's own optimizer does what spirv-opt's exact passes do. spirv-opt's folding is not exact (it turns `x*0` into 0 and `x+0` into `x`), so it was not used |
| HLSLcc's identity table `ImmCB_0[k]` in URP's light loop (`dot(unity_LightIndices[i >> 2], ImmCB_0[i & 3])`) | The Third Eye's Lit, 4 lights: 40.6 | `vec4(equal(ivec4(k), ivec4(0,1,2,3)))`: 43.2 | slower; constant arrays indexed at run time are cheap on this driver (a table of 8 offsets read by a pixel's index: 1.20 ms, as a chain of selects 1.93) |
| a chain of uniform-only arithmetic in the fragment stage (divide, `exp2`, `log2`, `inversesqrt`; HLSLcc writes these per pixel) | 0.567 | its result as a uniform: 0.577; computed in the vertex stage, a flat varying: 0.753 | nothing to gain: the driver computes uniform-only expressions once a draw |
| a temporary array indexed at run time (in memory on Mali) | 11.8 | selects: 1.66 | 7 times slower, but no converted program of the six games has one (none of their 7,464 stages but one vertex stage of Phasing Gun's). Unity's own GLES3 programs do: HLSLcc fills its constant tables element by element in `main()` for GLES, 26 stages in Chapéu de Papel's Android build; the converter's `const-arrays` rewrite (and metal.py) write them as `const` arrays |
| the Metal route's `i = 0; while(true){ if(i >= 42) break; ... i = i + 1; }` (HLSLcc's MSL form) | Chapéu's Bokeh blur: 45.0 | the same as `for(int i = 0 ; i<42 ; i++)`: 45.0 | the same code |
| loops written out (the converter's `unroll`, up to 16 trips) | see below | | never faster; up to 10 times slower: now limited by size (default) |
| the varyings a program reads packed into as few `vec4`s as their components fill (each varying read costs the G31 about 4 cycles a warp whatever its width: [profiling.md](profiling.md#gpu-counters-gpucnt)) | see below | | exact (the same bits), but no gain on the blob's code: not kept |

Loops. A blur of N taps (a fetch and a multiply-add each, the kernel in a uniform array) and the
same N trips of URP's Bokeh body (15 statements: two accumulators, two divides), as a loop, written
out, and as a loop whose bound the compiler cannot see is constant (`i < N + int(gl_FragCoord.x <
0.0)`: a fragment's window coordinates are never negative, so the same trips):

| trips | 4 | 8 | 12 | 16 | 24 | 32 | 42 |
|---|---|---|---|---|---|---|---|
| blur, loop | 1.60 | 2.84 | 4.20 | 8.55 | 32.5 | 196 | 24.7 |
| blur, written out | 1.61 | 2.84 | 4.19 | 8.55 | 31.9 | 196 | 260 |
| blur, loop with an opaque bound | | | 7.91 | 10.2 | 14.9 | 19.6 | 25.4 |
| Bokeh body, loop | | 7.48 | 12.6 | 16.4 | 24.1 | | 41.2 |
| Bokeh body, written out | | 7.48 | 51.2 | 167 | | | |
| Bokeh body, loop with an opaque bound | | | 13.1 | 16.9 | 24.6 | | 41.9 |

and the real 42-tap Bokeh blur: White Mirror's (glcore) 41.4 ms as a loop, 758 written out;
Chapéu's (Metal) 45.0 and 1,957. Written out, a loop costs the same as the loop up to the size
where the driver runs out of registers, and then several times more (spilled to memory, as a
temporary array is). The driver itself writes out short loops of a small body (the blur's 24 and 32
trips cost far more than its 42; with the bound hidden they cost what the trips predict) and keeps
loops of a larger body. Hidden bounds would therefore pay only for 17 - 32 trips of a body of a
few statements, and no program of the corpus has one (its loops of more than 16 trips are the
depth-of-field kernels: 22 - 71 trips of 16 - 24 statements), so that is not done.

**The default change: written-out loops limited by size.** The converter wrote out every plain loop
of up to 16 trips. On the 49 data folders of the free-game corpus and Cuphead's that is a few
small blurs (3 - 12 trips of 3 - 6 statements: Cuphead's 7-tap blur passes, Unsung Warriors,
Forsaken Castle) and the Post-processing Stack v2's depth of field with its small kernel, 16 trips
of 22 statements, in five games (Forsaken Castle, Mineral Ph, Adrorium, Nainai's Recipe and
Cuphead), which written out costs 2.7 times its loop: Forsaken Castle's program, 62.3 ms per
full-screen layer as converted, 22.8 as the loop. `unroll` now also stops at 120 statements written
out (trips x statements of the body, `glsl.UNROLL_STATEMENTS`: the largest size measured as cheap
as the loop, the Bokeh body 8 times; the smallest measured slower is 180): the small blurs are still
written out, the depth of field stays the loop HLSLcc wrote. Exact: the loop is the program as
translated (what Unity's own GL program runs), and on the handheld the two forms of Forsaken
Castle's program draw the same picture bit for bit (`shadercheck -w`, an RGBA32F target: every
float equal). None of the GPU-bound games' benched scenes draws such a program (their converted
data does not change), so there is no in-game frame rate to show; a game whose scene uses PPv2's
depth of field with the small kernel gets that pass 2.7 times cheaper.

The other two candidates on the handheld, in the games:

- *The SPIR-V round trip* for every fragment program Phasing Gun's first scene compiles (14,
  through `UA_SHADER_OVERRIDE`; A B A B, each run from a cooled handheld at 696 MHz all the time):
  11.8, 11.8 fps as converted, 11.8, 11.8 round-tripped. The screenshots of the two A runs are the
  same; the B runs differ from them, and from each other, only in a strip of the picture (4,300 -
  4,600 pixels, by up to 5 of 255) where two unchanged runs of the gpu-survey differ the same way.
- *Packed varyings* (the mesa-kbase measurement: five varyings packed into four `vec4`s cost 11 %
  less in a synthetic shape of Hollow Knight's lit sprites). Built: the fragment stage's float
  varyings packed, texture coordinates whole and first, the varyings every path reads before
  those read only in a branch, the vertex stage writing the packed components. Exact: each packed
  program below draws the same RGBA32F picture as its original (`shadercheck -w`), and the packed
  programs of the six games' data compile and link on the handheld as their originals do (all
  but 16 of Chapéu de Papel's, which fail unpacked too: below). But on the blob's code
  (ms per layer, 200 quads, 7 rounds after a second of warm-up):

  | program | as converted | packed |
  |---|---|---|
  | Hollow Knight: lit sprites (5 varyings -> 4; 3 on the path without light probes) | 0.535 | 0.535 |
  | Hollow Knight: bloom prefilter (4 -> 2: the texture coordinates two to a `vec4`) | 1.65 | 1.73 (the fused varying-and-texture fetch is lost) |
  | Hollow Knight: sprites with a separate alpha texture (3 -> 2) | 1.22 | 1.16 |
  | the built-in procedural skybox (The Third Eye, Phasing Gun: 4 -> 3) | 1.67, 1.66 | 1.57, 1.58 |
  | White Mirror's 2D lit sprite (3 -> 2), Arthur's Nightmare's outline buffer pass (3 -> 2) | 2.95, 1.31 | 3.01, 1.42 |
  | The Third Eye's URP Lit, three variants (4 -> 3) | 53.7, 42.3, 50.6 | 54.4, 42.1, 51.8 (the driver spills: 3 - 4 more stores) |

  and in the game, Hollow Knight's King's Pass with its lit sprites packed: 34.6, 34.9 fps as
  converted (GPU at 696 MHz 54 / 61 % of the time), 34.1, 34.1 packed (48 / 51 %). Not kept in the
  converter.

Precision. Not exact, so not a default, but the one difference between these translations and
Unity's own GLES3 programs, and the one change that moved the long programs (all floats
`mediump`, which is more than Unity gives: only its `half` values):

| program | as converted (highp) | everything mediump | Unity's own GLES3 (Android build) |
|---|---|---|---|
| Phasing Gun's SSAO | 59.9 | 46.3 (-23 %) | |
| the SSAO blurs | 4.27 / 4.24 / 3.93 | 4.09 / 4.01 / 3.69 (-4 to -6 %) | |
| Chapéu's Bokeh blur | 45.0 | | 43.7 (-3 %) |
| Chapéu's UberPost (title menu) | 5.92 | 6.93 (+17 %) | 6.51 (+10 %) |
| Chapéu's UberPost, sRGB variant | 7.30 | | 7.91 (+8 %) |
| Chapéu's wash | 3.65 | 3.65 | 3.64 |
| Chapéu's UI paper | 5.28 | 4.85 (-8 %) | 5.19 (-2 %) |

So Unity's own output is not faster across the board on this GPU (its UberPost, where colour
grading mixes `half` colours with float LUT coordinates, is slower: conversions), and the
translation is clearly slower than it would be only where most of a program is `half` arithmetic:
URP's SSAO, about a fifth. Two more differences from what an Android build would run, neither of
them exact either: the built-in pipeline's Standard shader in a desktop build is compiled for the
desktop tier (BRDF1, the full GGX; an Android build's tiers use the cheaper BRDF2/BRDF3), and it
draws Arthur's Nightmare's 65 ms room; and the desktop builds' quality settings (Phasing Gun's
shadow map pass, 17 ms) are the PC's.

Unity's own Android precision from the game's data (looked for, 2026-10-25). The idea: an Android
build compiles HLSL `half` / `fixed` as mediump / lowp; a Linux build's Vulkan programs carry the
same information as SPIR-V `RelaxedPrecision` decorations and a macOS build's MSL as `half` types,
so a GLCore / D3D11 program with such a twin in the same pass and keyword set could take its
qualifiers from it (vulkan.py and metal.py read both). Surveyed over the 60-game corpus and Hollow
Knight (every shader; the Vulkan modules decoded, their float-typed values counted, the MSL's
`half` tokens, and the GL programs' HLSLcc temporaries, which HLSLcc names by their precision:
`u_xlat16_<n>` for `half`, `u_xlat10_<n>` for `fixed`):

| Unity | builds | GLCore / D3D11 programs with a Vulkan or Metal twin | the twins' `RelaxedPrecision` float values | the GL programs' `half` / `fixed` temporaries | MSL `half` |
|---|---|---|---|---|---|
| 5.5 - 2019.4 (Linux) | 10 | 13,152 of 21,012 GLCore (every Linux build with both APIs; two ship GLCore alone or partly) | 1,086,329 of 5,506,626 (20 %; 7 - 34 % per build) | 69,511 of 188,953 (37 %; 26 - 45 % per build) | |
| 2019.1 (macOS) | 1 | 440 of 449 | | 32 % | 2 tokens in 440 stages |
| 2020.1 - 6000.6 (Linux) | 25 | 51,358 of 53,340 | **0 of 53,874,270** (every build) | **0 of 825,924** | |
| 2022.3, 6000.2 (macOS) | 2 | | | | 1 token in 2,455 stages |
| 2018.3 - 6000.3 (Windows) | 8 | no twin (D3D11 alone) | | | |

From Unity 2020.1 on, a desktop build's shaders have no `half` left anywhere: Unity's compiler makes
`half` a `float` for desktop targets (GLCore, Vulkan on Linux, Metal on macOS, D3D11) before HLSLcc
sees the code, and the SPIR-V, the MSL and the GL programs all agree. Hollow Knight (6000.0): 867 of
868 GLCore programs have a Vulkan twin, 0 of 82,582 float values relaxed, 0 of 4,851 temporaries
`half`; its Vulkan modules keep their names (734 of 734 stages; stripped up to 2018.1, kept from
2018.2). The same for every GPU-bound game of the handheld benches but Arthur's Nightmare (2017.3:
25 of the 93 temporaries of its hot programs are `half`, 32 % of its Vulkan values relaxed). So the
precision an Android build would run is not in the data of any game of 2020.1 or later, and
"everything mediump" above is the only version of it that can be made, not exact and not Unity's:
no converter option was built. For a 5.5 - 2019.4 build it is in the data, and the GL program
itself names its `half` temporaries; the Vulkan twin would add the uniforms', varyings' and block
members' precision (by name from 2018.2, by location / binding / offset before).

Not a cost but a failure, found on the way (the Metal route; not changed here): 24 of Chapéu de
Papel's 993 programs as converted do not link on the handheld, its URP Lit and terrain-detail
variants with per-pixel additional lights (`_ADDITIONAL_LIGHTS`): "the number of fragment uniform
components (4,156 - 9,597) is greater than the maximum number allowed (4,096)". metal.py writes
URP's constant buffers as plain uniforms, and the macOS build's light arrays have 256 entries
(`_AdditionalLightsPosition[256]` and three more: 4,096 components alone). Unity's own GLES3
programs of the same game have plain arrays of 16 (URP's light limit for mobile targets), and the
glcore route's (The Third Eye's) keep their 32 in a uniform block, which that limit does not count.
Those variants cannot run on a Mali (the title menu benched does not use them) until the Metal
route keeps such a constant buffer as a uniform block.

## Blend modes without GrabPass copies (`grab_fetch`, opt-in)

```sh
python -m pmunity shaders <data> <out> --grab-fetch on        # or always; config: "shaders": {"grab_fetch": true}
```

A pass after an unnamed `GrabPass {}` samples `_GrabTexture`, Unity's copy of what the camera drew
so far, taken again for every batch drawn with the shader. On the Mali-G31 each copy is a full-screen
blit into another framebuffer, and the camera stores its attachments and loads them again around it
([profiling.md](profiling.md#gpu-pass-by-pass): Hollow Knight's King's Pass has three a frame, its
main menu six). Where a pass samples the copy only at its own fragment's screen position (a blend
mode: screen, overlay, vivid light ... of a sprite against what is under it), the colour it reads is
the framebuffer's own at that pixel, which a fragment shader can read directly: framebuffer fetch,
`GL_EXT_shader_framebuffer_fetch` (an `inout` output) or `GL_ARM_shader_framebuffer_fetch`
(`gl_LastFragColorARM`). `shaders.grab_fetch` (`shaders/grabfetch.py`) gives such shaders a SubShader
that does that, without the GrabPass; off by default, because the picture is not always the same:

- sprites of one batch that overlap see each other's output (with the copy, the picture before the
  batch: Unity grabs once per object, or per batch where it batches them);
- ARM's fetched colour is mediump (16-bit floats: 1/255 steps of an RGBA8 target are not all exact),
  EXT's has the output's precision (highp here); with the copy, a highp sample of an RGBA8 texture;
- a GrabPass no pass of its SubShader samples is left out too (the same picture, unless another
  shader samples `_GrabTexture` without a GrabPass of its own, counting on this one's copy).

**Which shaders.** A SubShader is converted when every pass that samples `_GrabTexture` comes right
after an unnamed GrabPass (SerializedPass `m_Type` 2, `m_TextureName` empty; a named
`GrabPass {"_Name"}`, copied once a frame and shared by every object, is left as it is, with its
SubShader), and each of its GLES3 programs samples it only at the fragment's screen position. The
check (`grabfetch.screen_uv`) evaluates both stages of each program symbolically (values as
polynomials over the stage's inputs and uniforms; branches, loops and functions it does not follow
become opaque): each `texture(_GrabTexture, uv)` (also `textureProj`, `textureLod` at 0) must have
`uv = N / D + c` per component with N and D linear in the varyings, the varyings written by the
vertex stage as linear combinations of its final `gl_Position` P without a constant term (so they
interpolate as P does), and then D = k P.w, u = 0.5 P.x / P.w + 0.5, v = 0.5 P.y / P.w + 0.5: the
fragment's window position in OpenGL's convention (row 0 at the bottom, as the copy is). That is
what `ComputeGrabScreenPos` gives on OpenGL, and a clip position divided in the fragment stage.
Refused, with the reason in the log: offsets and blur taps (`_GrabTexture_TexelSize`), distortion,
flipped or scaled coordinates, `ComputeScreenPos` (its `_ProjectionParams.x` is a uniform), a UV
computed from the position before Unity's pixel snapping (`PIXELSNAP_ON` variants: the sample is
up to half a pixel off), any other use of the sampler (`textureSize`). One variant that does not
qualify keeps the whole SubShader as it is; variants that do not sample `_GrabTexture` are kept
unchanged.

**The data edit.** The GrabPass is an entry of the SubShader's pass list in the Shader's parsed form
(`m_SubShaders[i].m_Passes`, the same SerializedPass fields `m_Type`, `m_Name`, `m_TextureName`,
`m_UseName` in Unity 2017.4 - 6000.x), so leaving it out is a list edit; nothing else in the Shader
refers to a pass by its index (a script that draws one pass by index, `Material.SetPass`,
`Graphics.Blit` with a pass, would get another pass: leave such a shader out with
`grab_fetch_names`). For each SubShader that qualifies, a copy is put in front of it:

- without its GrabPasses; each remaining pass with only its GLES3 / GLES 3.1 subprograms (OpenGL
  Core, Vulkan and others stay in the original), listing only the GLES3 platform (2020.1+
  `m_Platforms`); 2021.2+ `m_ParameterBlobIndices` kept parallel;
- the sampling pass's programs as new entries appended to the GLES3 blob (in the segment of the
  entry they come from; 2021.2+ the same parameter entry), their fragment stage reading the
  framebuffer; up to 2019.x the fragment subprograms (entries without code) keep their entries;
- the vertex stage, parameters, keywords, render state and blend state as before; the program
  still declares `_GrabTexture` (unused, so Unity's bindings stay valid).

The fragment stage: after `#version`,

```glsl
#ifdef GL_EXT_shader_framebuffer_fetch
#extension GL_EXT_shader_framebuffer_fetch : enable
#elif defined(GL_ARM_shader_framebuffer_fetch)
#extension GL_ARM_shader_framebuffer_fetch : enable
#else
#error framebuffer fetch: ...
#endif
```

its one output `inout` where the driver has EXT's extension (`#ifdef`), `out` otherwise, and at the
start of `main()` `vec4 ua_FramebufferColor = SV_Target0;` (EXT: the output's value is the colour
under the fragment until the shader writes it) or `= gl_LastFragColorARM;`; each sample of
`_GrabTexture` becomes `ua_FramebufferColor`. The driver's extension macros choose, so one program
serves both kinds of driver.

**Which devices: the two modes.** Unity picks programs by GLES level, but SubShaders by their
requirements: a SubShader whose programs ask for what the device lacks is skipped for the next one,
as `#pragma require` makes it in an Android build.

- `grab_fetch: true` (`--grab-fetch on`): the copy's sampling subprograms require framebuffer fetch
  (`m_ShaderRequirements` bit `framebufferfetch`, 0x80000, libunity's own name table), so Unity runs
  the copy where its GLES backend reports framebuffer fetch and the original SubShader, GrabPass and
  all, everywhere else. Unity 6000.0's backend reports it with `GL_EXT_shader_framebuffer_fetch`
  (its extension table has no ARM or NV fetch extension; 2022.3's lists the same one, not run);
  2017.4 - 2021.3 have no fetch extension name in libunity at all: assume they never report it, so
  the original runs. Mali's own drivers have only ARM's extension (the RG34XX-SP's r20p0 libmali:
  `GL_ARM_shader_framebuffer_fetch`, no EXT), so there this mode changes nothing.
- `grab_fetch: "always"`: no requirement: Unity takes the copy wherever its programs compile, which
  is where the driver has either extension (Mali's own drivers: ARM's; Mesa: EXT's where it lists
  it). On a driver with neither they stop at the `#error`, and Unity 6000.0 then uses the original
  SubShader after the copy: in the rig (Mesa 21.2, the program binary cache emptied) each such
  program was compiled once and the frame was the original's, 24 passes with the three grab copies,
  the same picture. Older players were not tried; a player that does not fall back would leave out
  what is drawn with those shaders. The mode for Mali handhelds.

`grab_fetch_names` (a regex) limits it to the shaders whose names match. The run logs each shader
with a GrabPass (`grab_fetch: <shader>: changed ...` or why not) and a summary; the step applies it
to the GLES3 programs a shader has after the translation, also to ones it already had (data
converted before; a second run finds its own programs, by the comment line after `#version`, and
leaves the shader alone).

**Checked** (`tests/test_grabfetch.py`: hand-written programs, 2019 and 2021.2+ layouts):

- Hollow Knight (6000.0.61f1): 64 shaders have GrabPasses; 34 get a copy, the `UI/BlendModes/*`
  shaders with an unnamed GrabPass (33 sample at the screen position, `Font/LinearBurn` has a
  GrabPass nothing samples), 35 programs; left: the 28 `UI/BlendModes/Optimized/*` (named
  `_GUIBlendingSharedGT`), `UI/Blur/UIBlur` (blur taps), `Custom/Heat Effect` (distortion). The 230
  distinct programs of `resources.assets` afterwards compile and link on Mesa 22.3 (llvmpipe, EXT's
  coherent fetch) and on Mesa 21.2 with `MESA_EXTENSION_OVERRIDE=+GL_EXT_shader_framebuffer_fetch`
  (21.2 lists only the non-coherent one: there the 35 stop at the `#error`, as meant).
- In the arm64 rig (the game's 64-bit IL2CPP build to King's Pass, `UA_GL_TRACE`): with `true` and a
  driver without EXT (Mesa 21.2) Unity runs the original SubShaders: 24 passes and 93 draws a frame,
  the scene split into four by three grab copies, as without the option, the picture the same on
  every pixel that does not animate (80 % of the window); with EXT (the override) it runs the copies,
  in either mode: 18 passes, 90 draws, the scene one pass of 51 draws, no copies. (`"always"` without
  EXT: above, the original after one failed compile of each program.) The three blend-mode sprites (each
  its own batch) cover 91 % of the picture (found by drawing them magenta, `UA_SHADER_OVERRIDE`);
  on the 229,667 of their pixels that do not change during either run, the picture equals the
  original's bit for bit (EXT's highp fetch of an RGBA8 target is the copy's texel); the animated
  rest (particles, fireflies, the knight) differs between the two runs no more than between two
  frames of one run 3 s apart (7 % of the pixels).
- The corpus's 46 data folders (free games, Unity 5.x - 6000.x): 14 have shaders with GrabPasses, none
  qualifies: Unity's own `Particles/Standard Unlit` (a named GrabPass, `"_GrabTexture"`, in 11 of
  them), refraction, blur, outline and distortion shaders, and one game's blend-mode shaders
  (2018.3), whose `PIXELSNAP_ON` variants compute the UV before the snap.

On the RG34XX-SP (Mali-G31, r20p0), Hollow Knight converted with `"always"`:
- `shadercheck` on the handheld: all 230 GLES3 programs of `resources.assets` compile and link (the
  first try did not: Mali's compiler refuses a `"` even in a skipped `#else`, so the `#error` line
  has no quotes).
- King's Pass, three runs each, interleaved with runs without it: 33.2 -> 37.0 fps (+11 %; +8 % at
  the same GPU clock), 18 passes a frame instead of 24 (`UA_GL_TRACE`).
- Pictures: about 1.6 % of the pixels that are static in both runs differ, by 1 - 6 of 255 (ARM's
  fetched colour is mediump); two runs without it differ on 48 of them, by 1.

Not checked: Unity players before 6000.0.
