# Shaders

Unity's Android player only runs GPU programs of the GLES types. A desktop build has OpenGL Core
programs (written by Unity's HLSLcc from the game's HLSL: GLSL 1.50 for most shaders, GLSL 4.x
for `#pragma target 4.5` and up), Metal on macOS, and maybe Vulkan. `python -m pmunity shaders`
(convert/pmunity/shaders/) gives every OpenGL Core program a GLSL ES 3.00 (or 3.10) twin in the
same Shader asset, so the Android player finds a GLES3 program for every variant the game uses. A
Windows build has only Direct3D 11 programs (DXBC bytecode): `--from d3d11` translates those with
Unity's HLSLcc ([below](#windows-builds-direct3d-11-programs)).

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
     bounds and a plain body are written out, the counter becoming a literal).
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
| 6000.6 | 202605290, written as 202012090 | as 2022.3 |

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
class libraries in place of their Windows ones (`python -m pmunity.winbcl`, docs/porting.md).
Not checked: a run on the handheld, pictures against the Windows build, and any game with BC7,
BC4 or BC5 textures (the step's decoding of those is tested on hand-made blocks,
`tests/test_textures.py`).

`tests/test_dxbc.py` checks the path on hand-assembled vertex and pixel programs (no Unity files;
the HLSLcc part runs when pmhlslcc is built).

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
