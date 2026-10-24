# The setup's time on a handheld

Where a port's first-launch setup (`setup/patchscript`: unpacking, `pmunity convert`, `pmunity
android-data`, installing) spends its time on a handheld, and what was done about it. Every
change here leaves the output byte for byte as it was: the same converted files, the same APK and
OBB.

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
  and cannot get cheaper without changing the blocks: it already runs on every core.
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

## Measuring

`tools/port/stepstat.py` runs one pmunity command as the setup does and prints its wall and CPU
time, peak memory (VmHWM, and RssAnon sampled every 20 ms) and what it read and wrote; run it in
the arm64 container with the setup's own Python (unpack a port's `setup/python.tar.gz`):

    docker run --rm --platform linux/arm64 --memory 1500m --cpus 4 -v <checkout>:<checkout> -v <data>:<data> \
      ua-port-rehearse <python>/bin/python3.12 <checkout>/tools/port/stepstat.py <checkout> textures -- \
      convert <data> --config <the port's convert.json> --steps textures

`STEPSTAT_EACH=1` adds the memory after each file. `tools/port/rehearse.sh` runs a whole setup in
the same container.
