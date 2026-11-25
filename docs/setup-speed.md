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
| Hollow Knight (GOG's Linux installer) | PortMaster's 7-Zip, 8 min | with pmu (2026-11-02): 29 min, textures 26 min; 23.5 min (textures 22.7) after the changes in [rust-setup.md](rust-setup.md#on-the-handheld-time-memory-bad-conditions-2026-11-02), which says what limits each step | 13 min (the card) |

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
| a big scaled texture's bands each hold two copies of their pixels at most, and memory blocks from 1 MB up are mapped on their own and handed back when freed (glibc's `M_MMAP_THRESHOLD`, fixed at 1 MB) | `textures.decode_downscale`, `unity_io.map_big_blocks` | hold four copies of every band on each core, and keep freed image blocks in the heap between files: the textures step's peak memory on 31 Cuphead-sized bundles (202 BC7 pages of 4096 x 4096 and an 8192 x 8192 atlas, scaled 0.375; 4 CPUs, told 895 MB of RAM) 380 MB on average and 524 MB at the atlas, now 261 and 362 MB, the same files (an RG351MP's setup was killed for memory at that atlas) |

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
  see a file marked done before it is on the card). Done for bundles, shaders and audio
  (2026-11-19, `write_deferred`, [below](#the-textures-steps-cpu-and-its-commits-2026-11-19)); a
  plain file and its new .resS still commit together at once (`commit_then_mark`).
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

## A setup stopped often now goes on (2026-11-21)
Found running Blasphemous's 64-bit setup on the RG34XX-SP (Knulli) in devlock holds of 9 minutes, each
stopped and resumed (the setup's step marks and the convert's journal are meant for that):
- each resume of the textures step rebuilt the `exact_shaders` index before it converted a file: 162 s
  and 154 s in two resumes (the 109 textures the palette programs sample), the same work each time;
- the journal moves on per finished file only, and the next file there was `resources.assets` (972
  textures, the bulk of the game's): the 377 s left of a hold never finished it, so the stop threw its
  work away;
- after four holds (the unpack, then three of the textures step) the step stood at 2 of 3945 files.
A player whose handheld sleeps or whose battery runs out every few minutes would have seen the same.
One hold of 70 minutes (4170 s) then set it up: the textures step from its index to 3945 of 3945 in about 25
minutes (`resources.assets` among the first), exactindex and fmodcatalogs 2 minutes, the video step's
15 clips halved to 960x540 in 28 minutes, the FMOD banks 1 minute, packing 12.6 minutes; 3237 MB
installed. The four short holds before it (unpacking the game's data.unity3d took two of them) cost
about 36 minutes, of which the three in the textures step bought nothing.

Fixed in pmu (setup-rs `crates/convert/src/steps/exactindex.rs`, "The index kept", and `texmemo.rs`). Two
dot files beside the journal, both best effort (without one the run does that work again, as before),
neither listed by a step nor packed:
- **The index is kept** (`<data>/.pmunity-exact.json`, written through a .tmp file, synced and renamed as
  the journal is): what the walk found (the textures left as they are; why there is no `exact_index` plan
  or no such shader, so the exactindex step does not walk again either; a plan found is in the journal
  already). A run that goes on reads it instead of walking. It holds only while nothing it was made from
  has changed: the settings the walk reads (`exact_shaders`, `exact_keywords`, `exact_index`,
  `exact_index_skip`), the program (pmu's size and time: another build walks again), and the data files:
  the same names, each with the size and time it had when the walk began, or named in the journal as done
  by a step (the conversion's own change). A file that changed and that no step has marked (other game
  files; a file put in place a moment before the stop and not yet marked) makes the run walk again and
  keep the new index; so does a damaged or missing file.
- **A big file's textures are kept as they are finished** (`textures.checkpoint_mb`, default 64: a file,
  or a bundle with one serialized file, with at least that many MB of textures; `null`: none):
  `<data>/.pmunity-texmemo`, one record a converted texture (path id, a hash of what it was converted from,
  the new format and size, the blocks, checksums of the record's head and of the blocks), appended on a
  thread as the textures are finished and synced every 4 MB. The journal still marks whole files: the run
  that goes on reads the file from its start, and where the encoder would be called it takes the texture
  from the checkpoint, in the same order, so the stream, the objects and the file come out as an
  uninterrupted run writes them. Its key names the data file (path, size, time), the step's settings as
  used (after `lowmem_*`), the program and the ASTC guide: anything else and the file starts empty. The
  records count up to the first whose head does not check (a tail that never reached the card), a record
  whose blocks do not check is encoded again, and what a stop can lose is the last 4 MB of blocks not yet
  synced and the writer's 1 MB (about 20 MB of RGBA32). It never takes room the conversion needs (the free
  space is looked at every 16 MB; a card with less than the file's own need left ends the checkpoint and
  removes the file, as does any error writing it), and it is removed when its file is in place and
  marked (a bundle's with the next checkpoint or at the step's end, its batch may not be in place yet).
  Scene bundles (several serialized files) have none: each of them is seconds.

Is one file really minutes? Yes: `resources.assets` holds 2119 of the game's 5280 MB of textures (more
than 377 s on the RG34XX-SP; 135 - 173 s in the container below), and 9 of Blasphemous's 3943 files have 64 MB
or more (3736 MB together, 969 MB of blocks).

Measured on a Mac (the arm64 Linux container with 4 cores; Blasphemous's unpacked data, 3943 files, read
over a bind mount; the textures step alone, into a second folder; a release build of pmu), the conversion
killed with SIGKILL:

| | before | after |
|---|---|---|
| the index, a first run | the walk: 76.7, 79.6 and 101.7 s (three runs) | the same walk, then kept (200 KB) |
| the index, a run that goes on | the walk again | 0.7 - 1.1 s (eight such runs) |
| killed after 227 s (1988 files done), again 122 s into the next run (3232), then to the end | 77 s or more of walk before each run's first file | each run's first file converted 2.2 s after its start; done 52 s into the third |
| killed 45 s into `resources.assets`, again 40 s into the next run, then to the end | each run starts the file again: the two stops buy nothing, and a stop every 45 s never gets past the file (it needs 135 - 173 s there) | the checkpoint held 186 MB of blocks at the first kill and 299 MB at the second; the third run took 424 textures (285 MB) from it and had the file done (533 MB) 118 s after its start |
| `resources.assets` alone, not stopped (the index kept) | 101.7 and 112.0 s (`checkpoint_mb = null`) | 115.1 and 116.2 s |

Every file and the journal of both stopped conversions were compared with an uninterrupted run's: the same
(`diff -r` of the output folders, 217 files, 2.0 GB each), and `resources.assets` with and without the
checkpoint is the same file.

On the handheld (not measured there yet): a resume's 154 - 162 s walk becomes reading a 200 KB file and
one `stat` of each of the 3945 data files, and a hold inside `resources.assets` keeps what it encoded,
less the last 5 MB of blocks: the run that goes on reads the objects it had done again (at the card's
speed: the whole of that file's 2.6 GB is about two minutes of reading at 20 MB/s) and encodes from
there. The three 9-minute holds that bought nothing would each have bought about their length of
encoding after the first walk. The price on a setup that is never stopped: the checkpoints' writes,
969 MB for Blasphemous over its nine big files (at a card's 10 - 20 MB/s, 50 - 100 s of writing on a
thread of its own while the encoder works; in the container above, where the encoder is fast and the bind mount slow, `resources.assets` took 4 - 14 s longer with its 533 MB checkpoint, about 7 %; `checkpoint_mb = null` in the port's `[convert.textures]` turns it off).

Not changed: `pmu journal-files` still counts finished files only, so the setup's rerun after an
out-of-memory kill (it reruns while that count grows) is as before; a file the kernel kills the
conversion in again and again stops the setup as it did.

Tests (`cargo test -p convert --test textures`, made-up data): `textures_stopped_inside_a_file` stops a
run at each of a file's 8 textures (and twice; with a bundle and with worker threads at each of 11) and
runs it again: every file and the journal as an uninterrupted run's, only the textures not yet finished
encoded again; a checkpoint with a damaged record, with a tail of noise, cut inside a record, or made
with other settings costs a texture or the file again, never a different file.
`textures_exact_index_kept`: the run that goes on uses the kept index (shown by taking a texture out of
the kept file: it is then converted), writes the same files and journal, and walks again after a data
file the journal does not vouch for changed, with other settings, or with a damaged kept file.

## The textures step's CPU and its commits (2026-11-19)

Profiled on the muOS RG34XX-SP (H700, 4x Cortex-A53, 1 GB, the card writing 6-9 MB/s) with
`PMU_PROF=1`, Cuphead's whole setup took ~76 min, the textures step 2943 s at 3.07 of 4 cores
busy. Its stages summed to ~1950 s: ~1000 s were in none of them. They were the decoding and
downscaling of the big atlases: a texture of 16 Mpix or more that is scaled (Cuphead's 4096x4096
BC7 pages at 0.375) goes through `decode_downscale` (bands of rows on every core), which was
outside the `prepare` stage. It is timed now, with three stages more: `decode-cpu` and
`resize-cpu` (the CPU time of the bands' decoding and of their premultiply + resample, summed over
the threads), `wait` (the main thread waiting for a worker's texture) and `objects` (type trees
read, the other streams copied, the sprites and atlases fixed).

**The sample** (S1): `globalgamemanagers`, `resources.assets`, `sharedassets3`, `sharedassets8`
(with their `.resS`) and the bundles `atlas_bakery`, `atlas_baronesslevel`, `atlas_beelevel`,
`atlas_player`, `atlas_bad_ending`, `atlas_bg_smoke` and `tex_screen_fx` (264 MB; 257 textures,
698 MB of pixels to 108 MB of ASTC), Cuphead's config and ASTC guide (fast), `convert --steps
textures` in place on a fresh copy, the card's cache dropped before each run, one hold:

| S1, `PMU_PROF` stage (s) | muOS before (2 runs) | muOS after | Knulli before | Knulli after |
|---|---|---|---|---|
| **wall** | **165.0 / 173.0** | **134.0** (-19 %) | **173.0** | **133.6** (-23 %) |
| prepare (the main thread in decoding and scaling) | 58.7 / 58.7 | 36.7 | 62.9 | 37.8 |
| decode-cpu (the bands' decoding, CPU summed) | 64.7 / 65.4 | 27.1 | 70.4 | 27.4 |
| resize-cpu (premultiply, resample, CPU summed) | 84.0 / 84.3 | 56.6 | 89.1 | 57.0 |
| astc (astc-encoder, wall) | 56.5 / 57.4 | 56.2 | 61.8 | 61.6 |
| guided | 9.0 / 8.8 | 8.9 | 9.5 | 9.6 |
| write (packing, LZ4 HC) | 13.0 / 14.7 | 7.9 | 9.0 | 8.3 |
| commit (syncs, renames) | 8.7 / 13.2 | 11.6 | 10.6 | 3.6 |
| read | 6.8 / 7.9 | 3.3 | 10.2 | 3.2 |
| objects | 7.3 / 6.0 | 2.7 | 3.4 | 2.8 |
| journal | 0.6 / 1.3 | 2.0 | 0.3 | 0.2 |
| pmu's peak RSS | 123 MB | 151 MB | 121 MB | 153 MB |
| written by pmu (muOS; FUSE hides it on Knulli) / read from the card | 283 / 274 MB | 89 / 274 MB | - / 418 MB | - / 269 MB |

(muOS: main's pmu with the stage timers only, then all changes but the vector resampler's last form
(lane-indexed coefficients, which the A53 runs at the same speed); Knulli: main's, then the same. Every converted file and the journal the same in all of
them. The CPU stages are their threads' sums; prepare and astc wall time on the main thread, astc
on every core.)

What each change does (every one byte-identical: the converted files' md5s and the journal the
same as before, on the handheld and in the arm64 container):

| change | where | what it saves |
|---|---|---|
| Pillow's resampling passes for 4-byte pixels in NEON: the same coefficients (Pillow's own code, `Resample.c` unchanged, included by `csrc/pillow_resample.c`), the same 32-bit sums (wrapping as `-fwrapv` makes Pillow's), clip8 as two saturating narrows (what its lookup table holds); horizontal: four taps a step, their coefficients one vector taken by lane; vertical: 16 channels a step | texcodec `pillow_resample.c` | one band of an atlas (4096 x 532 -> 1536 x 192, muOS, one thread): 211 -> 111 ms (horizontal 138 -> 85, vertical 78 -> 38) (the coefficients taken by lane, four sums a step: the same on the A53, vertical 1.7 -> 1.2 ms on the M3) |
| premultiply in NEON (16-bit lanes: no lane overflows, the scalar's bytes), unpremultiply by an exact reciprocal (checked for every value and alpha), both on every core (per band, per strip) instead of once over the whole image on one thread | texcodec `image.rs`, `texture.rs` | the single-threaded passes over each atlas |
| BC7 blocks decoded as one 128-bit number, one function per mode, each subset's colours worked out once per block instead of once per texel (checked against Pillow's decoder on 400,000 random blocks of every mode) | texcodec `decode.rs` | the bands' decoding: 65 -> 27 s of CPU on S1 (a random-mode band: 437 ns a block on the A53, warm) |
| a bundle's texture streams read ahead (the reader thread reads and decompresses the next textures' LZ4 blocks; it read only plain files' streams before) | textures step | the main thread's reads of the atlases' 16 MB of BC7 while the cores wait |
| a bundle's new stream held in memory (up to 32 MB, else a file as before), not written to the card, synced every 16 MB and read back to be packed; the bundle's new serialized file held in memory when the nodes as they are now (the new stream, not the old .resS) come to at most `spill_mb`: it was decided on the old nodes (128-544 MB for Cuphead's atlases), so every atlas's serialized file, with its inlined ASTC, went through a scratch file | textures step, unityfs `write_into` | two of the three writes of each atlas's ASTC (S1 on muOS: pmu wrote 283 -> 89 MB) |
| converted files committed in batches (`Job::write_deferred`): written to `.pmunity-work/commit/`, synced on a thread of their own while the next file is converted, renamed together through `pending` once 16 files, 64 MB or 15 s are waiting (and at the end of the step), their marks then in the order the step finished them | convert `job.rs`; the textures step's bundles, the shaders and audio steps | the main thread's syncs (Cuphead: commit 111 s in textures, 26 + 31 s in shaders and audio) and two journal writes a file |

The commits keep their guarantees: a file is renamed into place only once it is synced, a batch's
renames are in the journal before they are made, a file is marked done only after its rename. A
run killed before a batch went in place converts its files again (`Job::new` removes the scratch
folder of deferred files; they are never among the game's files, where a step would list them).
Container, S1 with shaders, textures and audio: killed (-9) eight times in a row (1-6 s), then run
to the end: every file and the journal the same as an uninterrupted run's.

Memory: the bundle's stream in memory adds at most 32 MB, and its serialized file in memory at most
`spill_mb` (64 MB) while the bundle is packed (when the converted textures are no longer held);
plain files (Blasphemous's `resources.assets`, Hollow Knight) go as before. On S1 pmu's peak rose from 121-123 to 151-153 MB (the handheld has ~700 MB free beside the firmware). Blasphemous's `resources.assets` (the exact_shaders walk and 848 textures, 2121 -> 534 MB) in the arm64 container at 600 MB without swap: peak 363 MB before, 362 MB after, the same files.

The shaders and audio steps of S1 (three files each, muOS, two runs of each): 25.0 / 24.0 s before,
25.0 / 21.0 s with deferred commits, the same files and journal; the syncs move behind the next
file's work, which S1 has little of: Cuphead's whole shaders and audio steps (54 and 50 files,
commit 26 + 31 s) were not measured again.

Cuphead's whole textures step was not run again (the muOS card has 1 GB free); from S1's stages,
decoding and scaling (the profile's ~1000 uncovered seconds) about halve, the reads and the card's
writes of bundles fall to a third, the encoder (~1000 s with its workers) stays.

**Hollow Knight** (6x6 blocks, medium, scale 0.375) is the encoder's: on a sample of its files
(globalgamemanagers, sharedassets 0, 6 and 34) in the container, astc-encoder is 17 of the step's
21 s of CPU; these changes cut its prepare from 3.1 to 1.7 s, the same bytes.

**Guided blocks for 6x6** (not adopted): the guided pass fits 4x4 blocks only (`pack_block` writes
a 4x4 weight grid). Fitting other footprints on a 4x4 weight grid, which the decoder infills
(astc-encoder's `init_decimation_info_2d`), at the 4x4 blocks' rule (an error of at most
`guided_max_err` for every 16 texels against the exact decode, checked against astc-encoder's own
decoder) was tried on 60 of Hollow Knight's textures (sharedassets 0, 6, 34 and 392, medium, every
level decoded by astc-encoder): it fits 33,060 of the 414,146 blocks (8 %), all of them blocks the
guided pass already routes to the cheap one-partition search, so the encoder's time did not move
(4.0 -> 4.1 s, 8 threads); the PSNR over all of them is the same (34.257 dB), but 21 of the 60
textures came out lower (worst by 0.197 dB): at 6x6, astc-encoder's own one-partition blocks with
a full 6x6 weight grid are better than a 4x4 grid within 1/255 RMS. Since the guided pass promises
no block worse than its rule and the release's textures must not get worse, it stays as it was;
the time of a 6x6 setup is in the blocks far from a line (the full search), which only a change of
their blocks could cut.

**Not needed, or not the setup's**: the profile's 53 texture batches were the profiler's own (each
a pmu run of six files, `TEXB`); the setups run the textures step in one pmu. GPU encoding: no gain
at the same picture (worktree gpu-astc).

## Big textures with mip levels on every core (2026-11-25)

A 2D game whose 4096x4096 BC7 atlases keep their mip levels (752 textures, 2827 MB, scale 0.5, ASTC
5x5, `fast`), on an RG Cube XX (Knulli; H700, 4x Cortex-A53, 1 GB, the card reading about 18 MB/s)
and an RG34XX-SP (ROCKNIX, the same chip). The sample S2: three of its data files with their
`.resS` (331 MB, 297 textures), `convert --steps textures` on a fresh copy, the cache dropped
before each run. Over the step only 2.5 of the 4 cores were busy (`/proc/stat`); sampled every
second: seconds at 0.2 - 0.6 cores (reading), at 1 (mip levels), at 2.4 (decoding and scaling), at
3.8 (encoding). Two of the causes were texcodec's:

| change (each leaves every converted file the same) | where | S2 on the Cube XX |
|---|---|---|
| data that holds a texture's other mip levels after the first is cut in rows too: the rows are the first level's, whose size is known for the formats decoded here (a whole decode reads only that level as well). `row_units` took only data whose length the rows divide, so a big texture with mip levels was decoded on one core and never went through `decode_downscale`'s bands | `texture.rs` `row_units`, `decode.rs` `level_size` | `prepare` 21.7 - 22.8 -> 16.7 - 17.1 s; pmu's peak RSS 186 - 194 -> 160 - 169 MB (the bands never hold a whole 4096x4096 RGBA image) |
| the mip levels made from 1 Mpix or more are scaled by `downscale_parallel` (the same bytes as `downscale`), not on one core | `texture.rs` `mip_chain` | `mips` 7.2 -> 4.1 - 4.3 s |

S2 with both (a test build with a switch for each change): 86.9 - 91.6 s over five runs -> 78.8 s,
2.3 - 2.5 -> 2.8 cores busy; on the RG34XX-SP 80.2 - 82.7 -> 71.6 s. Same output: the digest of
S2's converted files is the same with the changes and without, on both handhelds; unit tests
`data_with_mip_levels_is_cut_in_rows_too` (it fails with the old row count) and
`mip_levels_on_every_core_are_the_same`.

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

## The setup on a PC: tools/prepatch (2026-10-31)

[tools/prepatch](../tools/prepatch/README.md) is a web page that runs a port's setup in the browser, with
the converter's steps ported to JavaScript and WebAssembly and checked byte for byte against `pmunity`
on the same input. For every port it unpacks the game and converts the textures, the settings, the audio
and known port plugins, so the handheld's converter skips that work. For the Cuphead port (2026.10.29)
it does the whole setup and writes a ready-to-run zip: the port folder is file for file what the
release's setup writes, and the game starts on a handheld on the first launch, without Python or a
network. On a 12-core PC the textures of Cuphead with its add-on take 7.4 min.

## Measuring

`tools/port/stepstat.py` runs one pmunity command as the setup does and prints its wall and CPU
time, peak memory (VmHWM, and RssAnon sampled every 20 ms) and what it read and wrote; run it in
the arm64 container with the setup's own Python (unpack a port's `setup/python.tar.gz`):

    docker run --rm --platform linux/arm64 --memory 1500m --cpus 4 -v <checkout>:<checkout> -v <data>:<data> \
      ua-port-rehearse <python>/bin/python3.12 <checkout>/tools/port/stepstat.py <checkout> textures -- \
      convert <data> --config <the port's convert.json> --steps textures

`STEPSTAT_EACH=1` adds the memory after each file. `tools/port/rehearse.sh` runs a whole setup in
the same container.
