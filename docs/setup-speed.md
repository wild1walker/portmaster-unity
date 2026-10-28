# The setup's time on a handheld

Where a port's first-launch setup (`setup/patchscript`: unpacking, `pmunity convert`, `pmunity
android-data`, installing) spends its time on a handheld, and what was done about it. Every
change here leaves the output byte for byte as it was (the same converted files, the same APK and
OBB), except guided ASTC encoding ([below](#guided-astc-encoding-2026-10-26)), which writes some
texture blocks itself: a measured, small change of the picture.

## Measured on the handheld

Anbernic RG34XX-SP (4x Cortex-A53 1.5 GHz, 1 GB RAM, Knulli: the card's `/userdata` is exFAT
through FUSE), before these changes:

| setup | unpacking | convert | packing (android-data) |
|---|---|---|---|
| Cuphead 64-bit (GOG installers + DLC, textures medium) | (`macpkg`, 5.2 GB of gzip'd payload) | 2 h 39 min, textures ~85 % | 20 min |
| Cuphead 32-bit | | ~3 h in all | |
| Hollow Knight (GOG's Linux installer) | PortMaster's 7-Zip | not run yet (32 min rehearsed in the container, its work folder on a slow USB stick) | |

## Where the time goes

Profiled on a Mac (cProfile, `textures.threads=4`) and measured in an arm64 Linux container (4
CPUs, 1.5 GB, the setup's own Python: python-build-standalone 3.12 with the port's wheels) that
the textures step is told has 973 MB, so big textures are converted alone as on the handheld
(`tools/port/stepstat.py`, below). The samples: Cuphead's serialized files and 39 of its 419
bundles (the largest atlases, the 8192x8192 BC7 one, fonts, music; 1730 MB of DXT), Hollow
Knight's base files and scenes 10-19.

| step | what it spends (before) |
|---|---|
| unpacking (`macpkg`) | inflate a megabyte, write it, inflate the next: the CPU and the card took turns |
| shaders, settings, audio, animclips | UnityPy parsing each file's type and object tables through a file object (one small read call per field, and it reads about twice the file: 220 of Hollow Knight's files, 248 MB, read 462 MB); a full garbage collection after each file (UnityPy's ~130,000 module objects walked every time: 11 ms a file on a Mac, ~0.1 s on the handheld); a shader's compressed blob written with one Python call per byte |
| textures | CPU: ASTC encoding 61 %, BC/DXT decoding 15 %, the Lanczos downscale 15 %, colour conversions and flips 4 %, the rest under 5 % (Cuphead sample). The card: a bundle over 64 MB (75 of Cuphead's, 10.1 GB of pixels) unpacked into node files and read back, every bundle written through a second file, LZ4 HC on one core; the sample read 2597 MB and wrote 1871 MB for 285 MB of output |
| android-data | the card: per bundle with texture streams, node files, the serialized file and the bundle each written through a second file; each inlined serialized file (Hollow Knight: 380) written twice; then everything copied into the APK and OBB. The Hollow Knight sample read 1167 MB and wrote 1032 MB for 395 MB of APK, with 6 s of CPU in 10.6 s |
| installing | renames on the same card and a few MB of player libraries: nothing to gain |

## What changed

| change | where | what the handheld no longer does |
|---|---|---|
| a chunked bundle's streams (`.resS`, `.resource`) are read from the bundle a 128 KiB block at a time when a texture needs them; only its serialized file goes to the work folder | `bigbundle.unpack(lazy=True)`, `unity_io.Loaded`, android-data's `inline_bundle` and `verify` | Cuphead's textures step: write 10.1 GB of node files and read them back; its packing: write ~2 GB of node files |
| a bundle is written in one pass: the header with its sizes zero first, patched at the end | `bigbundle.pack` | write and read each converted bundle a second time (Cuphead: ~2 GB in convert, ~2 GB in packing) |
| LZ4 HC chunks compressed on every core (each chunk on its own: the same blocks) | `bigbundle.pack`, UnityPy's `chunk_based_compress` | 3/4 of the LZ4 HC time (~110 MB/s on one Mac core, far less on an A53) |
| a serialized file written in one pass: a dry run gives the header and metadata length, the object data goes straight to its place | `unity_io.save_serialized_to` | write and read each inlined file a second time (Hollow Knight: ~3.5 GB) |
| serialized files parsed from a read-only map of them | `unity_io.load`, used by `Loaded` and android-data | ~2x the file read per load, and the per-field read calls (the 220 files: 16 MB of pages touched instead of 462 MB read; loading 2.5x faster) |
| after the first garbage collection the long-lived objects are frozen (`gc.freeze()`; once more after UnityPy reads its type tree database, never again) | `unity_io.collect` | ~0.1 s a file in every step (Hollow Knight's shaders + audio in the container: 29 s of their 34 s of CPU) |
| type-tree arrays of numbers written with one `struct.pack` | UnityPy's `write_value` (recompiled) | one Python call per byte of a shader blob |
| installer payloads inflated on a thread of their own while the files are written | `macpkg.Ahead` | wait for the card and the inflate in turn |

### Before and after

Container as above (main = before, branch = after): two runs on main before PR #4 was merged
and, where a third figure is shown, one on main with PR #4 and the branch rebased on it. "Read"
counts read calls (`rchar`): a mapped file's pages do not appear in it.

| step | wall | CPU | peak RSS (VmHWM) | anonymous | read | written |
|---|---|---|---|---|---|---|
| Cuphead: shaders | 13.4/11.9 -> 8.6/7.9 s | 12.9/11.6 -> 8.2/7.6 s | 300/304 -> 309/307 MB | 286/289 -> 287/287 MB | 299 MB -> (mapped) | 138 MB |
| Cuphead: textures | 156/138 -> 138/131 s | 538/489 -> 443/421 s | 1120/1125 -> 806/803 MB | 1104/1108 -> 788/786 MB | 2597 -> 918 MB | 1871 -> 484 MB |
| Cuphead: audio | 3.6/3.4 -> 2.3/2.1 s | 3.0/3.0 -> 1.5/1.6 s | 157/158 -> 175/175 MB | 142/143 -> 144/144 MB | 314 MB -> (mapped) | 230 MB |
| Cuphead: animclips | 2.5/2.3 -> 1.1/1.1 s | 2.4/2.3 -> 1.0/1.0 s | 100 -> 102 MB | 84 -> 85 MB | 107 MB -> (mapped) | 0 |
| Cuphead: android-data | 20.5/14.3/14.2 -> 9.5/8.2/7.8 s | 10.0/9.7/9.5 -> 8.7/8.5/8.2 s | 125-126 -> 137 MB | 98 -> 96-103 MB | 2337 -> 1161-1243 MB | 2037 -> 1257 MB |
| Hollow Knight, whole game: shaders + settings + audio | 38.8/40.6/36.6 -> 9.8/7.8/7.9 s | 36.6/37.7/34.3 -> 5.8/5.6/5.6 s | 216-218 -> 279 MB | 183 -> 184-201 MB | 3227 MB -> (mapped) | 151 MB |
| Hollow Knight: textures | 33.7/33.6 -> 32.8/42.4 s | 122/122 -> 121/158 s | 296/302 -> 295/294 MB | 277/281 -> 198/197 MB | 484 -> 420 MB | 312 MB |
| Hollow Knight: android-data | 8.3/8.0 -> 6.3/6.5 s | 5.7/5.4 -> 4.5/4.7 s | 166/167 -> 168/169 MB | 145 -> 144/146 MB | 1167 -> 783 MB | 1032 -> 720 MB |
| Cuphead's DLC installer: macpkg | 17.9/19.5 -> 12.5/16.5 s | 6.7 -> 7.1 s | 25 -> 34 MB | | 1576 MB | 1666 MB |

The container's disk is a Mac's SSD behind a large page cache, so its wall times hide the I/O that
the card pays for; the read and written columns are what changes on the handheld. The setup's
peak is the textures step (Cuphead: one 8192x8192 BC7 atlas decoded and scaled): ~320 MB lower
than main's before PR #4 was merged (the table); with PR #4 on both sides (its strips still hold
the whole image) 1123 -> 1103 MB (anonymous 1106 -> 1086 MB); Hollow Knight's textures step
with PR #4: 306 -> 322 MB VmHWM, 289 -> 279 MB anonymous. Where VmHWM rises (shaders, audio, Hollow Knight's textures: up
to ~60 MB) it is mostly the mapped pages of the file being read (`unity_io.load`), which belong
to the page cache (read() calls would put the same pages there, uncounted) and which the kernel
can drop; reading through the maps also adds up to ~18 MB of anonymous memory in shaders and
audio, far below the setup's peak. Mac, Hollow Knight's whole setup in chunks (below): shaders +
settings + audio 37.5 -> 7.7 s, textures 170 -> 146 s, android-data 65 -> 53 s; Cuphead's
textures step over the whole game 568 -> 550 s (all CPU there).

What that should make of the handheld's times (an estimate: the device was busy with other
tests): Cuphead's textures step no longer writes 10 GB and reads 10 GB it did not need (at the
card's 20-30 MB/s through FUSE, minutes, partly hidden behind the encoder) and takes ~15 % less
CPU (LZ4 on every core, less garbage collection); its packing reads half and writes 40 % less;
Hollow Knight's shaders, settings and audio steps take a quarter of the time (and read a few
percent of what they read), its packing reads and writes 30 % less.

Measured on the RG34XX-SP afterwards (the setup's own Python, the data on the card's exFAT
through FUSE): a Cuphead sample (Managed, `globalgamemanagers`, `level1`, `resources.assets` and
`sharedassets8.assets` with their `.resS`, one font bundle and the 70 MB atlas bundle
`atlas_world_4_spotlightsback`, spilled) through `convert --steps shaders,settings,textures,audio`
at medium: 283 s -> 226 s (-20 %), all 72 files the same (sha256). Mapping the serialized files
works on that file system, and so does replacing a file while it is still mapped.

### Same output

- Hollow Knight, the whole setup with main and with the branch, file for file (sha256): shaders,
  settings and audio over all of the data, then textures and android-data (`--consume-src --pack
  all --streaming-assets both`) in chunks of scenes (one with `resources.assets`): every
  converted file, APK, OBB and tree file the same.
- Cuphead: the textures step over every texture file (all 237 bundles and the serialized files),
  in chunks, every converted file the same; the sample above through the whole setup in place as
  on the handheld (convert with the port's plugin, android-data `--consume-src --pack all`): every
  converted file, the APK and the OBB the same.
- `tests/test_setup_speed.py`: lazy nodes against the files they replace, pack with 1 and 4
  threads, the patched `chunk_based_compress` and array writes against UnityPy's own, the
  read-ahead; with `PMUNITY_GAMES=<data>` also `save_serialized_to` against `sf.save()` and
  `unity_io.load` against `UnityPy.load` on real files.
- `tools/regress/regress.py fast --checkout <the branch>`: every game passes, every value as in
  the baseline.

## What is left

- **ASTC encoding** is most of a texture-heavy setup (Cuphead: ~60 % of the textures step's CPU)
  and cannot get cheaper without changing the blocks: it already runs on every core. (Guided
  encoding, below, changes some blocks for a measured share of it.)
- **Decoding and scaling** run on one core: PR #4 splits them over the cores. After it, the
  full-size `convert('RGBA')` copy in `texture_image` and the premultiply in `downscale` are the
  next single-threaded passes over a big atlas.
- **fsync** after each converted file waits for the card while the encoder could keep working; a
  commit and journal entry made on a thread, in order, would hide it (a resume must still never
  see a file marked done before it is on the card).
- **The tree and the archives**: android-data writes each inlined file into the tree, then copies
  it into the APK or OBB (Hollow Knight: ~3.5 GB written twice). Writing those entries straight
  into the archives would save the second write, but the regression run and the rig read the
  tree. Likewise a bundle's inlined serialized file still goes through a file before the bundle
  is packed (a dry run of the object sizes could stream it into the packer).
- **Structured arrays** (a sprite atlas's render data: Cuphead has 88,000 entries) are still
  written field by field in Python (~1 minute on the handheld for Cuphead).
- The textures step still asks for twice a spilled bundle's uncompressed size in free space,
  which only the old node files needed.

## The 64-bit setup: compiling and encoding (2026-10-24)

The release's whole 64-bit Cuphead setup on the RG34XX-SP (GOG's installers and the DLC, textures
medium; 2026-10-22, before the changes above), from its log:

| step | time | what it does |
|---|---|---|
| converting | 2 h 39 min | shaders 4.4 min, settings 1 s, **textures 2 h 32 min**, audio 1.2 min, animclips 0.7 min |
| IL2CPP | 61 min | UnityLinker 0.6 min, the conversion to C++ 6.3 min (one CPU), **compiling 755 files 50 min** with two compilers, then one killed and the rest with one: 3.6 min more, link and strip in that |
| packing (android-data) | 19 min | not changed here (mostly the card: the changes above read half and write 40 % less) |

**Textures.** Cuphead converts 1185 textures: 860 BC7 (13.2 Gpix decoded, 1879 Mpix of ASTC
out), 224 crunched DXT5 (1.6 Gpix, 223 Mpix out), the rest small; 851 of them 4096x4096 or
larger. The encoder is the step: on the handheld it encodes 0.42 Mpix/s at medium with
four threads (one thread: 0.105; it scales by 3.99x over four cores), so the
game's 2104 Mpix of ASTC are 84 min of encoding alone at medium. What changes the blocks
was not touched (the same astc-encoder build and preset), so the default stays as long as that;
what was left around it:

- On a system under 2 GB of RAM the step decoded and scaled every texture over 16 MB decoded
  alone, only then encoding it: on the handheld that is each of the 851 pages of 4096x4096 and up,
  so the single-threaded passes around their decode and scale (full-size copies and conversions,
  ~1 s a page on an A53 by the Mac's ratio) left three cores idle each time. What overlaps a decode
  is the encoding of the page before, which holds only its scaled levels, so now only decodes over
  128 MB (`textures.solo_mb`: Cuphead's 8192x8192 and 4096x8192 pages) go alone, and
  `texture_image` no longer copies an image that is already RGBA. The five biggest bundles in the
  arm64 container told it has 973 MB: the same files, peak 1051-1089 MB (main, two runs) -> 1025
  MB; no faster there (the M3 does those passes in ~0.03 s a page). **Not measured on the handheld
  yet**: expected some minutes of the step (up to ~10, by the Mac's ratio of the passes).
- Checked and left: BC7 is decoded by Pillow, which releases the GIL, so the decode strips do run
  on every core; the encoder releases it too.

**IL2CPP: the compiles had two of four cores.** The handheld's build chose two compilers from
`MemAvailable` at the start (200 MB each) and pinned them to two CPUs, while a generated file's
compile needs 60-80 MB of anonymous memory (measured over all 755 compiles: the libil2cpp sources
10-32 MB, the 2-3 MiB `Bulk_` files at most 77 MB; only the metadata tables are big:
`Il2CppAttributes.cpp` 508 MB, `Il2CppTypeDefinitions.cpp` 446 MB). With four slots the old
wrappers ran one compiler at a time (il2cpp hands out its largest files first, and three of its
four threads waited inside wrappers of metadata tables for every slot). The compiles now get their
turns from a scheduler (il2cpp.md, "Fitting a 1 GB, 4 x Cortex-A53 handheld"): one per CPU as the
predicted memory allows, the largest first; il2cpp's builder keeps three requests per CPU in
flight; a compile the OOM killer stops runs again alone. And .NET's tiered compilation is off
for il2cpp.exe (the conversion's CPU -30 %).

| IL2CPP, measured | old slots | branch |
|---|---|---|
| 1 GB arm64 container (M3): the compile step, all 755 files | 4.9 min (one compiler at a time) | 2.2 min (2.5 before the wrappers were woken by a signal) |
| 1 GB container: the conversion to C++, CPU | 30.7 s | 21.5 s |
| 1 GB container: the whole build (UnityLinker, conversion, compile) | 334 s | 179 s (its compile step still the 2.5 min one) |
| RG34XX-SP: 37 of the compiles (`Il2CppAttributes.cpp`, eight `Bulk_` files, 28 libil2cpp sources), the toolchain and the files on the card | 391 s (2 slots, as the setup chose) | 276 s (4; the 17 MiB metadata table still alone: 188 s of it) |
| RG34XX-SP, the whole compile step: simulated from every file's measured CPU time and memory, scaled so the old slots take the 50 min they took | 50 min | **27-28 min** (all four cores busy throughout would be 23.5) |

On the handheld the sample's objects are the same with both. The whole IL2CPP step then comes to
about 35 min instead of 61 (UnityLinker 0.6 min, the conversion ~4.5 instead of 6.3, the compile
~28 instead of 50 plus 3.6 after the OOM kill, link and strip ~2); the 1 GB handheld's
`MemAvailable` at the compile's start (750-780 MB) holds four `Bulk_` compiles (113 MB predicted
each) beside the 160 MB reserve.

**Shaders.** A stage's sampler types came from one regex search per type over the stage (18-30 of
them, for each of the game's 6,075 stages: a fifth of the step); they are now the type names among
the stage's words, what `\b<type>\b` matched. The whole game's shaders step on a Mac 8.9 -> 6.9 s,
every converted file the same. **Not measured on the handheld yet** (expected ~-20 % of the step's
2.5-4 min there).

### Same output

- IL2CPP: a fresh build with the branch (UnityLinker, conversion, compile) in a 1 GB arm64
  container: the C++ and `Data/Managed` identical to main's build, 754 of 755 objects identical,
  the other one libil2cpp's `Runtime.cpp`, which holds `__DATE__ __TIME__` and differs in any two
  builds; with main's copy of that object the linked `libil2cpp.so` is byte-identical
  (53,828,488 bytes). The conversion with tiered compilation off: the C++ identical.
- Shaders: the whole game's shaders step, main and branch: every converted file identical.
- Textures: the five biggest bundles (above) with main and the branch: every file identical; the unit
  tests (`tests/test_textures.py`, `tests/test_setup_speed.py`).
- `tools/regress/regress.py fast` on the branch: 54 pass, 0 fail, every count and digest as in the
  baseline.

### The visual option: texture quality "fast"

The port already asks for the texture quality (medium, the default, or fast). Measured, for
deciding the default on 1 GB handhelds (where a visual change may be the default):

| preset | encoder on the handheld (4 threads) | the game's 2104 Mpix | PSNR against the image encoded (49 Cuphead textures, 86 Mpix) | texels more than 8/255 from medium's |
|---|---|---|---|---|
| thorough | | | 42.76 dB (visible texels 39.77) | 0.95 % |
| **medium** (default) | 0.42 Mpix/s | 84 min | 42.28 dB (39.29) | - |
| fast | 1.24 Mpix/s | 28 min | 41.37 dB (38.40) | 1.53 % |
| fastest | 2.13 Mpix/s | 16 min | 39.87 dB (36.91) | 2.75 % |

Fast is 0.9 dB below medium, about twice the step from thorough to medium (0.48 dB), and saves
about 56 min of the setup; fastest 2.4 dB.

### Not done, or did not pay

- **A faster build of the encoder** (clang, `-mcpu=cortex-a53`, profile-guided; the same
  astc-encoder release, its invariance option kept, every block compared): needs astc-encoder's
  sources, a download the setup's tools do not make yet. The wheel's NEON build (GCC 10, `-O3`,
  LTO) already scales by 3.99x over the four cores.
- **Encoding a block or a texture once when it repeats**: no two of the 1185 textures have the
  same pixels; of a sample's 4x4 blocks 56 % are one colour (which the encoder already handles at
  once) and 93 % of the others unique: a cache would save a few percent of the encoder's work.
- **IL2CPP**: `-O1` compiles in 95 % of `-Os`'s time (different code), a precompiled header could
  save at most a tenth (il2cpp.md); stripping the game's own assemblies (UnityLinker "Medium")
  would change what the code may reach by reflection.
- **Overlapping steps** (IL2CPP while textures are encoded): both use every core, and the
  conversion to C++ (800 MB) cannot share a 1 GB handheld with anything big.

## ASTC guide files (2026-10-28)

`pmunity astc-guide` (astc_guide.py) runs the texture step on a PC with a recorder: each texture
level's hard blocks (the ones guided_astc leaves to astc-encoder) are encoded at eleven tiers
(presets with partition limits, cheapest first) and each block gets the cheapest tier whose error
is at most the target preset's. The guide holds a tier byte per hard block (zlib), keyed by file,
path id and level, with the hard blocks' count and CRC; `textures.guide` makes the texture step
encode each hard block at its tier only. No pixels are in it. Two Cuphead atlases (14 levels,
363k hard blocks), the Mac, 4 threads:

| run | encoding | PSNR |
|---|---|---|
| fast (heuristic tiers) | 1.46 s | 41.41 dB |
| medium (heuristic tiers) | 2.64 s | 42.43 dB |
| guide, target medium | 1.90 s | 42.48 dB |
| guide, target fast | 1.26 s | 41.48 dB |

Measured block by block (every hard block at every tier, one call each) a medium-target guide
cost what fast does and a fast-target one 0.55x; in a conversion each tier is its own, smaller
astc-encoder call, and the gap is that overhead. Not measured on a handheld yet. A guide for all of
Cuphead: about 12 MB.

## Textures inlined by the texture step (2026-10-28)

Unity 2017.4's Android player needs every texture's pixels inside its object (android-data,
`inline_texture_streams`), so packing unpacked each converted bundle again, inlined its .resS and
compressed it once more (LZ4 HC): a second full pass over every atlas, on the handheld's card
and CPU. `textures.inline_streams` (Cuphead: on) writes the bundles that way in the first place,
from the new stream file, one texture in memory at a time; packing then only sets the platform and
moves them. Three Cuphead bundles packed both ways: every node byte-identical once unpacked;
packing 17 s -> under 0.1 s (Mac), the texture step the same (2.3 s on the Mac's SSD either way).

## Guided ASTC encoding (2026-10-26)

`textures.mode = "guided"` (the default; `"search"` is the encoder alone, as before): the blocks
that need no search are written by a small C extension, `guided_astc` (`convert/native/guided_astc`),
and only the others go to astc-encoder, at the preset the port chose (`textures.quality`).

**What Cuphead's textures are.** 1185 textures converted (the inventory above): 860 BC7 (13.2 Gpix
decoded, 1879 Mpix of ASTC out), 224 crunched DXT5 (1582 Mpix, 223 out), 65 DXT5 (10.5, 1.5), 3
DXT1, 32 RGBA32, 1 RGB24. The port scales them by 0.375 (0.5 for text): only 0.05 Mpix of the
2104 Mpix of ASTC come from unscaled source blocks, so what a BC7 or DXT block knows does not
reach the encoder; what it is given is a Lanczos-scaled RGBA image. Its 4x4 blocks (110 textures
from 17 bundles and `resources.assets`, 165 Mpix, 10.3 M blocks): one colour 50.2 % (38.4 %
fully transparent), fully transparent with colours that vary 0.01 %, opaque 26.5 %, with alpha
23.3 % (none with only 0 and 255: scaling leaves no 1-bit alpha).

**What each block gets.**

| block | what is written | share (the sample) |
|---|---|---|
| one colour | a void-extent block, the bytes astc-encoder writes for it | 50.2 % |
| close to one line in RGBA (squared error of the fit at most `guided_max_err` = 64 over the 16 texels' four channels, an RMS of 1/255) | fitted here: the texels' principal axis, endpoints from its extremes, one least-squares refinement, every weight chosen against the exact decode; direct RGB endpoints (8 bits) with 12 weight levels, or RGBA (192 levels) with 8; one more try with 16 levels / 10 levels when it comes within 4x the bound | 22.0 % |
| from an unscaled DXT1/DXT5 texture (scale 1, the first mip level) | also tried: the source block's own endpoints and indices as an ASTC block (4 or 3 weight levels, 8-bit endpoints; a block with one alpha) | (Cuphead: none) |
| the rest, by the residual of the principal axis: up to `guided_line` = 128 | astc-encoder at the preset with one partition (`tune_partition_count_limit` 1) | 9.4 % |
| up to `guided_near` = 1024 | astc-encoder, at most two partitions | 6.0 % |
| the rest | astc-encoder at the preset | 12.3 % |

The blocks left to the encoder are gathered into an image of whole blocks (past an edge, the last
row and column repeated, as the encoder reads them) and put back in their places; astc-encoder
encodes each block on its own, so they are exactly the blocks it writes for the whole image with
the same settings (checked on every block of the sample, and by the unit tests). The extension
runs on every core, without the GIL, in plain C (no SIMD): on the M3, 3.7 s of one core for the
sample's 165 Mpix, about 4 % of the encoder's time.

**Quality and time**, the sample on the M3 (4 threads; CPU seconds, other work running on the
machine: search-medium measured at 86 and 94 s); PSNR over every texel and over the texels
the source shows (alpha > 0) against the image the encoder is given:

| | CPU | PSNR | visible | against medium | texels > 8/255 from medium |
|---|---|---|---|---|---|
| search, medium (as before) | 86-94 s | 41.444 dB | 38.991 dB | | |
| guided, fits only (`guided_line` = `guided_near` = -1: every other block searched at medium) | 77.8 s | 41.425 | 38.972 | 60.3 dB | 0.000 % |
| **guided** (default: 64, 128, 1024) | **62.5-63.6 s** | **41.389** | **38.936** | 55.5 dB | 0.011 % |
| guided, 64, 256, 4096 | 57.9 s | 41.349 | 38.896 | 52.8 dB | 0.057 % |
| search, fast | 56.4 s | 40.572 | 38.133 | 41.9 dB | 1.03 % |

Guided saves about 30 % of the encoder's CPU for 0.055 dB against the source; the preset fast
saves 40 % for 0.87 dB, and both together (guided, fast) 66 % (32.1 s) for 0.88 dB. Per texture
(the 86 of 110 whose medium PSNR is under 50 dB; above that the textures are nearly flat and a
fitted block's error of 1/255 shows as several dB): median -0.05 dB, the 67 1536x1536 pages
-0.06 (worst -0.44, DiceKingMainLevel at 49.5 dB instead of 49.9), the worst a 114x29 icon
-1.5 dB (48.1 instead of 49.6); the largest error of a texel is medium's in every one of them
(the fitted blocks are at most 1/255 RMS, a few levels at one texel).

Where the time goes (one thread): medium spends 18 % of its time on the blocks fitted here (45 %
of the blocks that are not one colour: the ones it finishes early) and about half of it on the
blocks far from any line (12 % of the blocks), which keep the full search.

What did not pay: more weight ranges per block (six instead of two), a second least-squares pass,
or a search over the quantized endpoints (each step up or down while it lowers the error) fitted
1-4 % more of the blocks for 2-5 times the extension's time; the fitted blocks' error bound at 32
(-0.005 dB) or 128 (-0.04 dB) moves 10 % of the blocks either way for a few percent of the
encoder's time; `tune_candidate_limit` 2 for the far blocks saved 14 % of their time for 1.1 % more
error (not done: those blocks keep the preset as it is).

One thread, the sample: search 80.3 s; guided 55.7 s (-31 %): the extension 3.7 s, gathering 0.1,
the encoder on the line blocks 3.9, the near blocks 8.7, the rest 39.3.

**The whole textures step**, Cuphead's 318 texture files and 1185 textures on the M3 (the step's
work: reading, decoding, scaling, encoding; the output kept in memory, the disk being full; four
threads, other work on the machine): search 507 s wall, 1468 s CPU; guided 478 s wall, 1140 s CPU
(-22 % CPU; peak RSS 768 -> 763 MB; blocks: one colour 49.8 %, fitted 22.8 %, line 5.3 %, near
9.5 %, the rest 12.6 %). On the M3 the wall time is mostly the single-threaded decoding and
scaling around the encoder; on the handheld the encoder is half of the step.

**On the handheld** (offline while this was measured; an estimate): the encoder's 84 min at medium
for Cuphead's 2104 Mpix, scaled by the one-thread split above (the encoder's share 0.646, at the
encoder's own Mac-to-handheld ratio), is about 54 min, plus the extension: 4 min at the same
ratio, up to 8 if the A53 does its scalar C twice as badly as the encoder's NEON. About 58-62 min
instead of 84: the textures step ~2 h 8 min instead of 2 h 32. Not measured on the device yet.

**DXT sources at scale 1** (Hollow Knight's port converts at 1.0): Cuphead's plain DXT1/DXT5
textures at their own size (11.5 Mpix, mostly flat): guided 58.54 dB, with the DXT blocks as
seeds 58.96 (7 % of the blocks are the transcoded source blocks), medium 60.88; a 3D corpus game's
(Arthur's Nightmare, 20.3 Mpix): guided 55.33 dB, seeded 55.62 (23 % transcoded), medium 55.68.

Same output: with `guided_max_err`, `guided_line` and `guided_near` at -1 the blocks are byte for
byte the encoder's (only one-colour blocks are written here, as astc-encoder writes them): checked
by the unit tests (`tests/test_guided_astc.py`: void-extent bytes, the reported error against
astc-encoder's own decoder, the classes, the gathering at the edges, DXT1/DXT5 seeds against
texture2ddecoder's decode, strips against the whole image, the fall-back without the module), on
the Mac and with the module built for aarch64 Linux run by the setup's Python in the arm64
container.

The extension: `convert/native/guided_astc/guided_astc.c` (CPython's limited API: one build for
3.11 and later), `setup.py` for the wheel (`tools/port/build_pybundle.sh` builds it in the
manylinux container with UnityPy's), `build.py` for a local build (tests, Mac). Without it the
step logs that and searches every block.

## Measuring

`tools/port/stepstat.py` runs one pmunity command as the setup does and prints its wall and CPU
time, peak memory (VmHWM, and RssAnon sampled every 20 ms) and what it read and wrote; run it in
the arm64 container with the setup's own Python (unpack a port's `setup/python.tar.gz`):

    docker run --rm --platform linux/arm64 --memory 1500m --cpus 4 -v <checkout>:<checkout> -v <data>:<data> \
      ua-port-rehearse <python>/bin/python3.12 <checkout>/tools/port/stepstat.py <checkout> textures -- \
      convert <data> --config <the port's convert.json> --steps textures

`STEPSTAT_EACH=1` adds the memory after each file. `tools/port/rehearse.sh` runs a whole setup in
the same container.
