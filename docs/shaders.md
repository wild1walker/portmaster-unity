# Shaders

Unity's Android player only runs GPU programs of the GLES types. A desktop build has OpenGL Core
programs (GLSL 1.50, written by Unity's HLSLcc from the game's HLSL), Metal on macOS, and maybe
Vulkan. `python -m pmunity shaders` (convert/pmunity/shaders/) gives every OpenGL Core program a
GLSL ES 3.00 twin in the same Shader asset, so the Android player finds a GLES3 program for every
variant the game uses. A Windows build has only Direct3D 11 programs (DXBC bytecode):
`--from d3d11` translates those with Unity's HLSLcc ([below](#windows-builds-direct3d-11-programs)).

## What the converter does

1. **Program blobs** (`blob.py`). A Shader keeps its compiled programs per shader compiler
   platform in one LZ4 block each. The GLCore platform's blob (15) is copied as a GLES3Plus blob
   (9); every entry a GLCore32 subprogram uses is translated and retyped GLES3 (gpu program type
   4). Entries are parsed with a layout table checked by writing every entry back byte for byte
   (Unity 2017.3/2017.4: serializer 201708220; other versions are tried and reported).
2. **Subprograms** (`convert.py`). Every GLCore32 `SerializedSubProgram` gets a GLES3 twin with the
   same blob index, keywords, channels and parameter bindings (the GL and GLES backends bind by
   name the same way). GLCore41/43 variants (desktop-only features) get none.
3. **GLSL** (`glsl.py`), per stage block (`#ifdef VERTEX` / `#ifdef FRAGMENT`, each compiled by
   Unity as one shader):
   - `#version 300 es` instead of `#version 150` / `330`;
   - the desktop extensions ES 3.00 has built in are dropped (explicit attribute locations,
     float/int bit casts); any other extension is kept, so a program that needs one fails on a
     driver without it, as it would in a real Android build;
   - default precisions: **highp** for floats, ints and every sampler type the stage declares.
     Desktop GL computes and samples in 32-bit floats; ES would otherwise sample `sampler2D` at
     lowp. `--precision default-samplers` keeps ES's sampler defaults (for A/B tests);
   - two rewrites that keep every operation and operand (`--optimize`, both on):
     **const-arrays** (HLSLcc fills constant tables, e.g. blur weights, element by element at the
     start of every invocation; they become `const` arrays) and **unroll** (loops with literal
     bounds and a plain body are written out, the counter becoming a literal).
4. A shader with a program that uses GLSL with no ES 3.00 equivalent (fixed-function built-ins,
   rectangle or 1D samplers, `noperspective`, ES 3.1+ functions, storage buffers) gets no GLES3
   programs at all, and the run says which. Unity then treats it as unsupported on the device, as
   it would a shader without GLES3 programs in an Android build. For Cuphead that is only
   `Hidden/VideoDecodeOSX` (the macOS video decoder; Android decodes video elsewhere).

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
with Mesa (llvmpipe). The comparison is `convert/tests/test_d3d11_corpus.py`:

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
