# Shaders

Unity's Android player only runs GPU programs of the GLES types. A desktop build has OpenGL Core
programs (GLSL 1.50, written by Unity's HLSLcc from the game's HLSL), Metal on macOS, and maybe
Vulkan. `python -m pmunity shaders` (convert/pmunity/shaders/) gives every OpenGL Core program a
GLSL ES 3.00 twin in the same Shader asset, so the Android player finds a GLES3 program for every
variant the game uses.

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

So on this GPU a fragment costs about one unit per texture fetch, per interpolated varying and
for the blended write; arithmetic and precision barely matter (mediump saves ~1 % on sprites).
What a frame costs is mostly the number of pixels drawn times the fetches per pixel: overdraw,
full-screen passes and render target sizes, not shader code. The rewrites above help the blur
passes by ~4 %; fewer fetches (e.g. a 7-tap blur as 4 bilinear fetches) would help more, but
bilinear weights are quantized by the hardware, so the picture changes: that kind of change
belongs in a game's port as an option, not in the converter's defaults.
