# Shaders

Unity's Android player only runs GPU programs of the GLES types. A desktop build has OpenGL Core
programs (GLSL 1.50, written by Unity's HLSLcc from the game's HLSL), Metal on macOS, and maybe
Vulkan. `python -m pmunity shaders` (convert/pmunity/shaders/) gives every OpenGL Core program a
GLSL ES 3.00 twin in the same Shader asset, so the Android player finds a GLES3 program for every
variant the game uses. A Windows build has only Direct3D 11 programs (DXBC bytecode):
`--from d3d11` translates those with Unity's HLSLcc ([below](#windows-builds-direct3d-11-programs)).

## What the converter does

1. **Program blobs** (`blob.py`). A Shader keeps its compiled programs per shader compiler
   platform in LZ4 blocks. The GLCore platform's blob (15) is copied as a GLES3Plus blob
   (9); every entry a GLCore32 subprogram uses is translated and retyped GLES3 (gpu program type
   4). Entries are parsed with the layout of their serializer version, checked by writing every
   entry back byte for byte ([Unity versions](#unity-versions) below; another version's layout is
   guessed and reported).
2. **Subprograms** (`convert.py`). Every GLCore32 subprogram gets a GLES3 twin with the
   same blob index, keywords, channels and parameter bindings (the GL and GLES backends bind by
   name the same way). GLCore41/43 variants (desktop-only features) get none.
3. **GLSL** (`glsl.py`), per stage block (`#ifdef VERTEX` / `#ifdef FRAGMENT`, each compiled by
   Unity as one shader):
   - `#version 300 es` instead of `#version 150` / `330`;
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
   `sampler2DMS`, `textureGather`, bit-field functions) is written as `#version 310 es` and typed
   GLES31 (gpu program type 3, with all subprograms of its variant): Unity runs it where the device
   has OpenGL ES 3.1+, as it does the GLES 3.1 programs of an Android build. (Unity's own GLES3
   output keeps such a program as `#version 300 es`, which no driver compiles.)
5. A shader with a program that uses GLSL with no ES equivalent (fixed-function built-ins,
   rectangle or 1D samplers, `noperspective`, images and atomics, storage buffers) gets no GLES3
   programs at all, and the run says which. Unity then treats it as unsupported on the device, as
   it would a shader without GLES3 programs in an Android build. For Cuphead that is only
   `Hidden/VideoDecodeOSX` (the macOS video decoder; Android decodes video elsewhere). Variants
   only VR uses (`STEREO_INSTANCING_ON` and the other VR keywords) that do not translate (they
   write `gl_Layer`) are left out instead, with their subprograms; Unity never asks for them on a
   handheld.

## Unity versions

The converter reads the data's Unity version from the data (`python -m pmunity version <data>`)
and handles 2017.3 - 2022.3; other versions are refused unless `--any-unity-version` is given
(then the layouts are guessed and checked by round trip). What changes between the LTS lines:

| | 2017.4 | 2018.4 | 2019.4 | 2020.3 | 2021.3 | 2022.3 |
|---|---|---|---|---|---|---|
| serializer (program entry layout) | 201708220 | 201802150 | 201806140: global + local keyword lists | 201806140 | 202012090: parameters as entries of their own | 202012090 |
| blob container | one LZ4 block per platform, 8-byte entry table | same | 2019.3+: segments (Unity's 16 MB variant chunks), 12-byte entries (offset, length, segment) | same | same | same |
| where the subprograms are | `m_SubPrograms`, hardware tier per subprogram | same | same, keyword indices global + local | same, parameters grouped in `m_Parameters` (+ program `m_CommonParameters`) | `m_PlayerSubPrograms`: a list per tier (desktop builds use the 4th, tier-independent one) with the parallel `m_ParameterBlobIndices` | same, + `m_SerializedKeywordStateMask` |
| also kept in step | - | - | - | pass `m_Platforms` | + Shader `stageCounts` | same |
| GL program text | plain `#extension` lines | same | `#ifdef`-wrapped extensions; macro blocks for uniform buffers and uniform locations (`UNITY_LOCATION(n)`, `UNITY_BINDING(n)`) | same | same | same |

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
  are the `STEREO_MULTIVIEW_ON` ones (`GL_OVR_multiview2`, which llvmpipe lacks).

  ```sh
  cd convert && PMUNITY_GLCORE_CORPUS=<linux udr>[:...] PMUNITY_GLCORE_ANDROID=<android udr>[:...] \
      PMUNITY_GLCORE_EXPORT=/tmp/glsl python -m unittest tests.test_glcore_corpus -v    # then shadercheck /tmp/glsl
  ```
- **Six free Linux Mono games**, one per LTS line (not in the repository): all their shaders
  convert but VR-only ones and one with `noperspective`, and every exported program compiles and
  links on Mesa (`PMUNITY_GAMES=<Game>_Data[:...] PMUNITY_GAMES_EXPORT=<dir> python -m unittest
  tests.test_games_corpus`, then shadercheck each `<dir>/<n>`):

  | game data | formats | shaders given GLES3 | programs (GLES 3.1) | left out | distinct programs linked on Mesa |
  |---|---|---|---|---|---|
  | 2017.4.40f1 | SF 17, one block, 201708220 | 75 | 280 | - | 239 / 239 |
  | 2018.4.36f1 | SF 17, one block, 201802150 | 34 | 154 | - | 145 / 145 |
  | 2019.4.8f1 | SF 21, segments, 201806140 | 34 | 167 | VR blit (`gl_Layer`) | 150 / 150 |
  | 2020.3.49f1 | SF 22, segments, 201806140 | 51 | 211 | VR blit | 183 / 183 |
  | 2021.3.16f1 (URP) | SF 22, segments (URP UberPost: 3 segments, 2048 entries), 202012090 | 78 | 2495 (6) | VR blit, URP XR occlusion mesh | 933 / 933 |
  | 2022.3.36f1 | SF 22, segments, 202012090 | 80 | 1140 | VR blit, a third-party outline shader (`noperspective`) | 906 / 906 |

What was not checked: nothing of this has run in a Unity player of these versions; Unity's own
GLES3 programs of a game's shaders were not available to compare with (only built-ins). Desktop
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

What the path does (`dxbc.py`, `convert.add_gles3_d3d11`):

Unity versions: 2017.3 - 2018.4 (serializers 201708220 and 201802150; in 2018.2+ a texture
binding's two ints are the sampler index and `dimension << 1 | multisampled`). From 2019.1
Unity's GL programs carry constant buffer layouts of their own (a `$Globals` of every loose
uniform, which the D3D11 binding info does not give), its D3D11 headers are 38 bytes, and from
2020.1 the parameters a program's variants share are stored once in the parsed form
(`m_CommonParameters`): such data is refused with that reason.

1. **Entries.** One D3D11 blob entry is one stage: Unity's 6-byte header (1, then the numbers of
   texture, constant buffer and sampler slots and two more), the DXBC container, and after the
   code Unity's binding info. Unity strips the container's reflection chunk (RDEF: constant buffer
   layouts, resource names) in builds; the binding info replaces it: the vertex channel source
   map and inputs, every constant buffer with its used members (name, type, rows, columns, matrix,
   array size, offset), and the bindings (constant buffers and textures by register, textures with
   their dimension). Its layout (serializer 201708220, Unity 2017.3/2017.4) is checked by writing
   every entry back byte for byte, and against the subprogram's `m_ParsedForm` (constant buffer
   bindings, member offsets, texture registers and dimensions must agree).
2. **Reflection.** The registers, texture types and sampler modes come from the declarations in
   the bytecode (SHDR/SHEX); with the binding info's names and layouts they become an RDEF chunk
   in the form HLSLcc reads. Every constant buffer location an instruction reads must be a member
   Unity described, else the variant is refused (HLSLcc would fail on it).
3. **Variants.** In each pass, a D3D11 vertex subprogram is paired with the pixel subprogram of
   the same keywords and hardware tier; the two are translated together (one
   `GLSLCrossDependencyData`, pixel shader first, as HLSLcc requires), with Unity's GLCore
   settings: GLSL 1.50, `HLSLCC_FLAG_TRANSLATE_MATRICES` (`hlslcc_mtx4x4<name>` vec4 arrays) and
   `HLSLCC_FLAG_DISABLE_GLOBALS_STRUCT` (every constant buffer member a plain uniform: Unity's GL
   backends set built-ins and material values by name, also those that are in cbuffers on D3D11,
   e.g. `UnityPerDraw`). Inputs are `in_POSITION0`..., varyings `vs_TEXCOORD0`..., outputs
   `SV_Target0`..., one GLSL sampler per texture named as the texture. The newer HLSLcc's macro
   blocks for uniform buffers and explicit uniform locations, which Unity 2017 players do not
   know, are written out as switched off, and `#ifdef`-wrapped extensions plainly, as Unity 2017
   stored them. The program then goes through the GLSL ES rules above (precision, rewrites,
   unsupported constructs) like a GLCore program.
4. **GL data.** Unity's GL subprograms have no parameters, bindings or vertex channels, only the
   source map (its GL backends find uniforms, textures and inputs by name in the linked program),
   and every GL blob entry's binding info is the source map followed by `0 1 0 0 0 0 0` (all
   5782 GLCore entries of Cuphead and all GL entries of Unity's Windows and Android player
   resources). As in Unity's builds, the program is stored in the vertex subprogram's entry and
   the fragment subprogram's entry has no code; the GLES3 blob keeps the D3D11 blob's indices (a
   vertex entry shared by variants with different pixel shaders gets more entries).
5. **What is left out.** A SubShader with a variant that cannot be translated gets no GLES3
   programs, so Unity uses the next SubShader or the Fallback (as for a SubShader an Android build
   has no programs for); a shader none of whose SubShaders translate gets none. VR-only variants
   (`STEREO_INSTANCING_ON` writes `gl_Layer`, which ES 3.00 lacks) that do not translate are left
   out instead; Unity never asks for them on a handheld. The run logs each case.

| translates | does not (SubShader left out) |
|---|---|
| vertex + pixel shaders, shader model 4.0 and 5.0 | geometry, hull, domain, compute shaders; shader model 5.1 |
| constant buffers of float and int scalars, vectors, matrices, arrays | struct members in constant buffers (GPU instancing data: Unity's layout for them is not known); other constant types |
| textures 2D, 3D, cube, 2D array; samplers, comparison (shadow) samplers | UAVs, structured / raw buffers, RW textures; 1D, multisampled and cube array textures; texture gather and LOD queries (ES 3.1+) |
| everything else HLSLcc writes as GLSL ES 3.00 (integer and bit operations, loops, immediate constant buffers, derivatives, discard, MRT, depth output) | GLSL with no ES 3.00 equivalent (the GLSL ES rules above, e.g. `noperspective`) |

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
packing, column-major is assumed.

Checked on Unity 2017.4.9f1's Windows player resources ("unity default resources" of
`Variations/win64_nondevelopment_mono`, from Unity's Windows support installer), which also hold
Unity's own GLCore and GLES3 programs of the same shaders: 5 shaders, 36 D3D11 entries, 47
variants. 36 variants translate; the other 11 are `STEREO_INSTANCING_ON` (left out). Of the 14
variants that are not VR-only, all declare the same uniforms, inputs, varyings, outputs and
samplers as Unity's GLCore program of the variant and have the same code (9 byte for byte; 5 up to
the order of declarations and the names of temporaries, Unity's GL compile having seen `fixed`
values), and the same interface as Unity's GLES3 program; the 72 GLES3 subprograms equal Unity's
own GLES3 subprograms but for the blob index. The 15 GLES3 programs (11 distinct) compile and link
with Mesa (llvmpipe). The same holds for the built-ins of 2017.4.40f1 and 2018.4.36f1 (the Linux
player's file carries the D3D11 programs too): 36 variants translate per version, the 14 non-VR
ones match Unity's GLCore code (2018.4: up to temporaries) and Unity's GLES3 interface, the 72
subprograms equal Unity's, and the 30 GLES3 programs link on Mesa. The comparison is
`convert/tests/test_d3d11_corpus.py`:

```sh
cd convert && PMUNITY_D3D11_CORPUS="<path>/unity default resources" PMUNITY_D3D11_EXPORT=/tmp/glsl \
    python -m unittest tests.test_d3d11_corpus -v                  # then shadercheck /tmp/glsl
```

`tests/test_dxbc.py` checks the path on hand-assembled vertex and pixel programs (no Unity files;
the HLSLcc part runs when pmhlslcc is built).

## Checking a conversion

```sh
python -m pmunity shaders-export <converted data> glsl/       # every distinct program as .vert/.frag
runtime/host/out/shadercheck glsl/                            # on the handheld: compile + link each
```

`shadercheck` uses the device's own driver (a pbuffer, or the fbdev window on Mali). Run it in
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
