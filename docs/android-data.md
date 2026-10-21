# android-data: a game's converted data as Unity's Android player expects it

`python -m pmunity android-data --src <converted Game_Data> --pkg <Android player> [--pack all]`
(convert/pmunity/android_data.py) builds the data from the converted desktop data (which it only
reads) and the Android player package. For Cuphead a run takes about 15 s; big files are clones
(APFS clonefile, else hard links), so the tree costs about 0.4 GB of real disk (48 rewritten
bundles, explained below). The output is in `--out` (default `android-out/`).

Sections 1-3 come from libunity.so 2017.4.9f1 armeabi-v7a (sha256 `b7d24200…b604315e`),
disassembled with the symbols in libunity.sym.so (`tools/re/arm.py`); section 4 is what changes
for the other Unity versions (5.6 - 6000.6).

## 1. How the player finds its data

`UnityInitApplication` runs these steps:
1. `Mount(Context.getPackageCodePath())`. It `stat()`s the path. Only for a **regular file** does it
   call `apkAddCentralDirectory` (Unity's own zip reader: `fopen`/`fseek(long)`/`fread`, STORE or
   inflate) and then `SetApplicationPath(path)`. For a directory it prints `[VFS] Mount` and returns,
   **without setting the application path**.
2. `MountObbs()`: for each `getObbDirs()` (or `getObbDir()`) it tries
   `<dir>/main.<versionCode>.<package>.obb` and `patch.…obb`. An OBB is mounted only if its zip has an
   entry `assets/<unity.build-id>` (from the manifest meta-data, read through PackageManager).
3. Data folder = `"assets/bin/Data"`. This is relative to the application path: `File::SetCurrentDirectory`
   is applied, then `LocalFileSystemAndroid::ToAbsolute` gives `<app>/assets/bin/Data/…`. Mono
   assemblies are read from `<app>/assets/bin/Data/Managed` through Unity's VFS
   (`mono_file_map_override`). `machine.config` is built in (`mono_register_machine_config`). The Mono
   config dir is the *relative* path `assets/bin/Data/Managed/etc`, which Mono opens with plain
   fopen relative to the process cwd.
4. Built-ins: `library/unity default resources` is remapped to `assets/bin/Data/unity default resources`
   (not under Resources/). `resources/unity_builtin_extra` is remapped to `assets/bin/Data/Resources/unity_builtin_extra`.
   The player requires `globalgamemanagers` and checks for a `data.unity3d` archive.
5. `boot.config` is read through the **Java** AssetManager: `getAssets().open("bin/Data/boot.config")`.
   If it is missing, the defaults are used. The command line comes from the Intent extra `"unity"`.
6. StreamingAssets = `"jar:file://" + app + "!/assets"`. ToAbsolute converts that back to `<app>/assets/…`.
   Cuphead loads every bundle with `AssetBundle.LoadFromFile(streamingAssetsPath/AssetBundles/x)`,
   which goes through the same VFS. Code that reads it with System.IO instead: section 6.

Lookup: `LocalFileSystemAndroid::Open/Exists` tries the zip handler first, then **the real file
system with the same path**. Zip lookup: the archive whose path prefixes the file wins. Otherwise
every mounted archive is searched, OBBs first. A path containing `.apk/`, `.obb/`, `.zip/` or `.jar/`
auto-mounts that archive, so the directory root must not be named `*.apk`.
AAssetManager is not used at all (libunity imports no AAsset*).

So two layouts work:

| route | what the host gives the player | libunity patch | Mac disk | notes |
|---|---|---|---|---|
| **directory** `out/Cuphead/` | getPackageCodePath = that directory; chdir there | `Mount`: at file offset/vaddr **0x2eab20**, `1a00001d` (bne 0x2eab9c) → **`1a000007`** (bne 0x2eab44): a non-regular path is then taken as the app path (zip files still mount normally) | ~0.4 GB | Application.dataPath is a real directory, so the DLC check (`dataPath/../goggame-2048763853.info`) and helper files work |
| **zip** `out/zip/` (`--pack all`) | getPackageCodePath = `Cuphead.apk`; getObbDir = `out/zip`; versionCode 1; package `com.cupheadport.game`; meta-data `unity.build-id` (all in `zip/android_package.json`) | none | +2.3 GB | each archive must stay **< 2 GiB** (32-bit fseek). APK = `assets/bin/Data/**` (389 MiB). Main OBB = `assets/AssetBundles/**` + Logo + `assets/<build-id>` marker (1.857 GiB, 146 MiB headroom). Overflow goes to `patch.1.….obb`. STORE, zip32, 4-byte aligned |

Zip-route caveats: `Application.dataPath` is the .apk file. The DLC check
(`File.Exists(dataPath + "/../goggame-2048763853.info")`, from codepatch DlcFromGogInstall) and
`Tuning.cs` (`dataPath/animclips.txt`) then fail with ENOTDIR. The libc stat/open shim would have to
fold `x.apk/..` lexically, or codepatch would need a change. Mono's `etc/mono/config` (dllmaps for SDL2, libc,
libmono.so) is in `out/zip/assets/bin/Data/Managed/etc` outside the zip, so the host should chdir to `out/zip`.
**Only the APK has been built so far.** `--pack obb` checks for 1.86 GiB + 1 GiB free first, and the Mac had ~4 GB free.

Loader note: Mount reads `st_mode` at offset 0x10 of a **bionic** `struct stat`. bionic's arm32
layout differs from glibc armhf (st_size etc.), so stat/fstat/lstat need converting shims.

## 2. Platform differences

- **Target platform check: yes, strict.** `SerializedFile::ReadMetadata<false>` at 0x88854c (and
  `<true>` at 0x8892dc) has `cmp r2,#13; bne` "The file can not be loaded because it was created for
  another build target". Every serialized file must say 13, including the CABs inside asset bundles.
  The script sets the platform in the file header:
  - plain files: 4 bytes at offset 31, in a clone;
  - bundles: in the LZ4 block that holds the CAB header. A one-byte literal patch was never enough,
    because later matches copy that byte. So the block is re-encoded to **exactly the same compressed length**
    (LZ4HC, then matches turned back into literals until the size fits). The file layout is
    unchanged, and 371 of 419 bundles are done this way. 48 near-incompressible music blocks don't fit,
    and those bundles are rewritten with one re-encoded block (381 MiB of real copies).
  - Code alternative, if the loader patches libunity anyway: NOP (`e1a00000`) both `bne` words
    (`1a000224` at 0x88854c and 0x8892dc). With `--platform-patch none` the data can then stay
    byte-identical clones/hard links of the converted data. On the device that would allow
    sharing the existing 1.9 GB of bundles through path mapping, since exFAT has no symlinks.
- **Type trees:** main data has none (bundles have them). The Android player therefore reads with its own
  layouts. I compared the field names in the `Transfer<GenerateTypeTreeTransfer>` functions of all 63
  classes in the data with UnityPy's 2017.4 release trees (`tools/cmp_layout.py`).
  - All match except `PlayerSettings`: the Android player has no `mobileMTRenderingBaked` bool after
    `m_MTRendering`. Both are followed by Align, so the offsets come out the same.
  - The other reported differences are fields that sit in helper functions (Transform children,
    Component m_GameObject, AnimatorController::TransferRuntimeData).
- **globalgamemanagers edits** (UnityPy):
  - platform 13;
  - `BuildSettings.m_GraphicsAPIs` [11,17] → [11] (GLES3 only; GLES2 is impossible because the shaders have
    only GLCore and GLES3 programs);
  - `GraphicsSettings.m_ShaderDefinesPerShaderCompiler` gets a GLES3 (platform 9) entry copied from GLCore,
    because the GLES3 programs are converted GLCore ones;
  - MonoManager no longer lists `UnityEngine.Cluster{Input,Renderer}Module.dll` (standalone-only,
    absent on Android, unused).
  - PlayerSettings is left as is: m_MTRendering on, androidBlitType 0 = Always. Use `--mt-rendering`
    and `--blit never` to try others.
- Shaders: the Android GLES3 device uses shader platform 9, the same one the converter added.
- Textures in main data: ASTC 4x4 / Alpha8 / RGBA32. Audio: Vorbis + PCM in FSB5 `.resource`
  files, the same FMOD; streamed clips are read through the VFS.
- `unity default resources` comes from the Android package. It has the same PathIDs as the Linux one
  (checked) and an embedded Arial. `unity_builtin_extra` is the converted one.
- Fonts are embedded in the game; there is no /system/fonts fallback.
- **Video (`pmunity/video.py`).** Unity's Android VideoPlayer draws frames with
  `Hidden/VideoDecodeAndroid` (`ScriptMapper::FindShader`, passes `RGBAExternal_To_RGBA` and
  `RGBASplitExternal_To_RGBA`, matched ignoring case; the same in 2017.4 - 2022.3). An Android
  build's `unity_builtin_extra` has it; a desktop build's has only `Hidden/VideoDecode`, and its
  ScriptMapper lists the Android name with a null pointer. When the data has `Hidden/VideoDecode`,
  android-data adds our own `Hidden/VideoDecodeAndroid` to `unity_builtin_extra` (a free path id,
  16001 when free) and points the ScriptMapper entry at it:
  - the Shader object is the data's `Hidden/VideoDecode` with the name, two passes (made from its
    `FLIP_RGBA_TO_RGBA` and `FLIP_RGBASPLIT_TO_RGBA`: vertex inputs, `_MainTex`, `_MainTex_ST`,
    `_AlphaParam`, render state), one GLES3 program each and no other platform, so its layout is
    that Unity version's;
  - the programs are `pmunity/video_decode_android.shader` (GLSL ES 3.00, `samplerExternalOES`),
    written for the runtime's SurfaceTexture (`runtime/media/README.md`): Y, U and V read from the
    packed picture and converted as Unity's desktop players convert the same clips; the converter
    picks the conversion from the game's clips (their `.resource` headers: VP8/VP9 → Unity's
    BT.601 software conversion, H.264/HEVC → BT.709 as Unity's NV12 pass), logged as
    `video: clips {...}: BT.601 conversion`;
  - `--no-video-shader` leaves it out (then no frame shows).

## 3. Managed assemblies

- Shipped:
  - the **Android `UnityEngine*.dll` set** (45 files, matching the player's icalls);
  - the game's own assemblies: patched Assembly-CSharp, -firstpass, CupheadPort, Rewired_*, GalaxyCSharp;
  - the Unity extension DLLs (UI, Timeline, Networking, SpatialTracking, StandardEvents);
  - the BCL (mscorlib, System*, Mono.Security). The Android package has no BCL; Unity uses the
    editor's 2.0 profile for every platform.
- `*.dll.so` (x86-64 AOT) is not shipped.
- Assemblies a desktop build compiled for its own platform where the Android build differs:
  `--asmpatch` runs `tools/asmpatch` on the game's Managed folder and ships what it changed (e.g.
  Rewired's `DetectPlatform` reporting Android); a port's own patches: `--assembly-patch`,
  `--managed-file`.
- `tools/apicheck` (Mono.Cecil) results:
  - All 3035 references from the game assemblies into UnityEngine* resolve against the Android set.
    The 9 "missing" ones are multi-dim array runtime methods.
  - mscorlib `mono_corlib_version` = 82, which is what the Android libmono.so checks.
  - The macOS-only icalls are MovieTexture, GameCenter, ClusterInput and Windows.Speech, which the game doesn't use.

## 4. Unity versions (5.6 - 6000.6)

Sections 1-3 come from the 2017.4.9 player. What `android-data` does for the other LTS lines,
from the static survey of their Android players ([compat](compat/README.md)) and runs on Unity's
built-ins and six free Linux games (one per line):

- **Versions.** The data's Unity version is read from `globalgamemanagers` (or a bundle header);
  outside 5.6 - 6000.6 the run stops unless `--any-unity-version`. The player package's version
  comes from its `player.json` (`pmunity player fetch`), its `ivy.xml` (5.x - 2018.x), the version
  string in its libunity, or, failing that, from its "unity default resources"
  (which gives only the line: the file is often from an earlier patch). Data and player of
  different lines are refused (main data has no type trees: the player reads its own layouts).
  Another patch release of the same line is **retargeted** (`--data-version player`, the default;
  `retarget.py`): the player opens a data file only when it names the player's own version (2017.4.40f1
  with 2017.4.9f1 data: "Invalid serialized file version ... Expected version: 2017.4.40f1. Actual
  version: 2017.4.9f1", then "Unable to initialize the Unity Engine"), so every plain serialized file
  gets the player's version in its metadata (in place, or with the object data moved by 16 bytes when
  the space before it is too small; the objects' bytes do not change), and objects of the classes
  whose layout differs between the two releases (UnityPy's type tree package, AssetRipper's dumps of
  every release) are converted field by field (2017.4.9 -> 2017.4.40: PlayerSettings, whose
  `iosAppInBackgroundBehavior` int became `iosUseCustomAppBackgroundBehavior`, a bool, in another
  place). Bundles carry type trees and load in other releases as they are. A player package whose
  exact version is unknown (only its "unity default resources" tells the line) is not retargeted
  to. The report has both versions, and the Mono runtime the game
  needs: `.NET 3.5 (libmono)` (a `Mono/` folder, 2017.x - 2018.x) or `.NET 4.x (libmonobdwgc-2.0)`
  (`MonoBleedingEdge/`, and every 2019.1+ game).
- **SerializedFile headers.** Formats 17 (5.6 - 2018.x), 21 (2019.x), 22 (2020.1 - 6000.4, a
  48-byte header with 64-bit sizes: the version string at 48, the platform word after it) and 23
  (6000.5+, the same header; type trees carry a hash and their size). The platform word is set in
  place for all of them, in plain files and in bundles' CABs. (6000.7 betas write format 25:
  refused.)
- **Built-in resources.** 2023.2+ players read `assets/bin/Data/Resources/unity default
  resources` (libunity: "Resources/unity default resources"), earlier ones `assets/bin/Data/unity
  default resources`; the package keeps its copy at `Data/Resources/` from 2023.2 too. android-data
  takes it from either place and puts it where the player's line reads it.
- **Assemblies.** 2017.3 - 2017.4: MonoManager's fixed slots (section 3). Older lines
  (`GetAvailableDllAssemblyMask` of 5.6.7, 2017.1.5, 2017.2.5 disassembled): 5.x and 2017.1 have one
  engine assembly (`UnityEngine.dll`, slot 0), slot 1 reserved, the game from slot 2; 2017.2 (the
  first line with engine modules) takes slots 0 - 43 without looking for the files, skips 44 and
  checks 45+. A game assembly that references a type of an engine module (by the module's name,
  2017.2+) which the Android copy lacks but the game's desktop copy defines (Adventure Creator and
  others: `UnityEngine.MovieTexture` in `UnityEngine.AudioModule`, standalone-only) would stop the
  player at a `TypeLoadException`: that module is shipped as the game's desktop copy (a warning
  names the types); only its standalone-only members fail, when called. `--managed-file
  UnityEngine.XModule.dll` does the same by hand. From 2018 (2018.4 - 2022.3
  disassembled) `GetAvailableDllAssemblyMask` takes every listed assembly that loaded, filtered by
  its `m_AssemblyTypes` flags: no slot is reserved, a missing engine module is skipped wherever it
  is. From 2020.2 the list is `ScriptingAssemblies.json` in the data folder (MonoManager keeps only
  script hashes); it is shipped with `RuntimeInitializeOnLoads.json`. The check (game assemblies
  present, engine modules the Android set lacks) reads whichever list the version has.
- **Mono config.** `--mono-etc` default: `Mono/etc` or `MonoBleedingEdge/etc`, in the data folder,
  next to it (Windows) or (macOS) in `Contents/`. A Windows build's native plugins
  (`Plugins/x86_64/*.dll`) are named in the warning; Unity 2021.2+ Windows builds' class libraries
  are Windows-only (`pmunity.winbcl` gives the Unix ones, shipped with `--managed-file`).
- **Texture streams.** Inlined in every version (the 2017.4 reason, section 2, is not known for
  later players; inline pixels load either way). From 2018 meshes can stream their vertex data
  from the same `.resS`: a `.resS` that a mesh still reads is shipped beside its file (2021.3 URP
  game: 19 meshes in 3 files). The textures step likewise carries those blobs when it rewrites a
  `.resS`, and writes ASTC as `ASTC_<n>x<n>` (48-53) from 2019.1, where the RGBA numbers are
  deprecated.
- **GraphicsSettings.** `m_ShaderDefinesPerShaderCompiler` has the same layout in every version
  that has it (2017.1+; 5.x has none, and nothing is added); the GLES3 entry is copied from GLCore
  (from D3D11 for a Windows build).
- **Type trees UnityPy lacks.** Main data has no type trees; UnityPy reads it with the trees it
  ships (UnityPy 1.25.4: up to 6000.6 alphas / 6000.7 alphas). `PlayerSettings` of 6000.0.58f2,
  6000.3.11f1, 6000.5.7f1 and 6000.6.3f1 is 4 bytes longer than those trees (6000.6.3f1's
  `PlayerSettingsState::Transfer` has `captureStartupLogsBaked` after `insecureHttpOption`), so
  reading it failed; 2023.2.20f1's is 1 byte longer, at its end. `pmunity/typetrees.py` then reads
  it with that field added, or else with the bytes left over kept as unknown fields at the end,
  only when UnityPy's own tree fails, the fixed one reads every byte, and no field of the missing
  size fits before the fields android-data changes (`m_MTRendering`, `mobileMTRenderingBaked`,
  `androidBlitType`, `androidUseSwappy`: the conversion stops otherwise). In all five the missing
  bytes can only be among the last fields, and the object writes back byte for byte.
- **PlayerSettings.** As in section 2, plus `--swappy off|keep|on`: `androidUseSwappy` (2019.3+,
  Android Frame Pacing), off by default. A desktop build never ran it, and the runtime keeps Swappy
  off; with the setting on, the 2019.4 player presents nothing and 2021.3+ stop after their first
  frames (runtime/android/README.md, "Frame pacing"). Off, Unity paces frames itself (vsync count,
  `targetFrameRate`), as on the desktop. Only 2019.3+ conversions carry the option in their state.
- **OBB.** 5.x: Java mounts the APK and each OBB with `nativeFile(path)` (an OBB only when the
  APK's `settings.xml` lists the MD5 of its last 64 KiB); the runtime mounts the APK and every
  main/patch OBB of the package that way (runtime/jni/up_legacy.c). 2017.1 - 2020.3: the
  `assets/<build-id>` marker and `unity.build-id` meta-data. 2021.3+
  match an OBB by GUID: the APK's `assets/unity_obb_guid` (2021.3 reads it with
  `AssetManager.open("unity_obb_guid")`, 2022.3 as `<apk>/assets/unity_obb_guid`) must equal the
  OBB's `unity_obb_guid` at the root of the archive (2021.3: `ZipFile.getEntry("unity_obb_guid")`,
  2022.3: its own zip reader, which reads the archive's first entry when the name is missing:
  "Application OBB has mismatching GUID ... got ''", and the OBB is not mounted). The packer writes
  both, the build id as text, and names it in `android_package.json`; checked with both players in
  the emulated rig. The build id comes from `BuildSettings.buildGUID`, which 2022 dropped: then
  `PlayerSettings.productGUID`, else a hash of the settings file.
- **What is refused or reported.** A compressed build (`data.unity3d` without
  `globalgamemanagers`): unpack it first with `python -m pmunity unpack <data> [--remove]`
  (stored, LZ4 and LZMA blocks; the files come out as an uncompressed build has them, which is what
  an Android player reads from `assets/bin/Data`). An IL2CPP build (no `Managed/*.dll`;
  `GameAssembly`, `il2cpp_data`): the Mono player cannot run it. Native plugins in `Plugins/`
  (e.g. `lib_burst_generated.so`, FMOD, Steam) are not shipped; the report names them. Burst
  falls back to its managed code; `libsteam_api.so` is answered by the runtime's stub (Steam not
  running, `runtime/host/steamstub.c`); FMOD and other plugins need ARM builds of their own.

Six free Linux Mono games (2017.4.40f1, 2018.4.36f1, 2019.4.8f1, 2020.3.49f1, 2021.3.16f1,
2022.3.36f1) go through every step (shaders, textures, settings, audio, android-data with
`--pack all`) with the Android player package of their own version (its `unity default resources`
and Mono `Managed` set, fetched from Unity's installers): every serialized file loads back in
UnityPy with the Android platform, every assembly the list names is present, APK and OBB verify.
Eight more (5.6.0f3, 2017.2.0f3, 2023.2.20f1, 6000.0.58f2, 6000.3.11f1, 6000.5.7f1, 6000.6.2f1,
6000.6.3f1)
go through shaders, settings and android-data the same way; all of them run in the emulated rig
(compat/README.md, "Players run").

## 5. Native plugins

`check_source` runs the plugin survey (`pmunity/plugins.py`, [plugins.md](plugins.md)) on the source
data: one warning per library the game P/Invokes (what the runtime gives it), the full report in
`android_data_report.json` (`plugins`), `plugins.map` in the output and zip folders (`--plugin-map`
for the port's own entries). FMOD opens its banks with plain file I/O, so for a game that uses FMOD
the `*.bank` files of StreamingAssets stay out of the OBB and go to `zip/assets/` beside the APK
(`--loose-assets <glob>` for other files); ua_host gives FMOD's `jar:file://` and
`file:///android_asset/` bank paths those files.

## 6. StreamingAssets, files the game reads, saves

On a desktop, `Application.streamingAssetsPath` is a folder (`<Game>_Data/StreamingAssets`; macOS:
`<Game>.app/Contents/Resources/Data/StreamingAssets`) and a game may read it with any file API. On
the Android player it is `jar:file://<archive>!/assets`, the archive being the OBB when one is
mounted, else the APK. Unity's own APIs open that path through the VFS of section 1
(`AssetBundle.LoadFromFile*`, `UnityWebRequest`, `WWW`, `VideoPlayer.url`); System.IO does not
(Mono takes it for a relative path: `DirectoryNotFoundException`). `asmpatch --survey` lists the
reads of each folder getter and what they reach (tools/asmpatch/README.md); `port check` suggests
the option below when reads of StreamingAssets reach System.IO.

**Bundles anywhere in StreamingAssets.** Every UnityFS file there gets the Android platform
(section 2), not only Cuphead's `AssetBundles/`: Addressables' `aa/<platform>/`, a game's own
folders. Bundles of 2022.3 (format 8) may start their blocks 16-aligned after the blocks info
(flag 0x200); LZMA blocks (BuildAssetBundleOptions.None) are read, and the one with the platform
byte is written back as LZ4HC; a bundle the reader cannot patch in place is saved again by UnityPy
(LZ4 chunks, same objects).

**Addressables.** Its settings and catalog are text read by Addressables' providers. Release 1.21
(Unity 2022.3) reads them with UnityWebRequest when the path has `://` and the runtime platform
is Android, so the jar path works; a desktop build of older releases reads them with
`File.ReadAllText` (then `streaming-assets` below gives it the folder). Each bundle location's
`AssetBundleRequestOptions` hold the bundle's CRC (`m_Crc`: CRC-32 of the uncompressed bundle
data, the value `AssetBundle.LoadFromFile(path, crc)` checks), and a patched bundle has another
one. android-data rewrites the catalog (`aa/catalog*.json` with a non-zero `m_Crc`): each CRC of a
patched bundle becomes the new one; the records that grow or shrink move the entries' offsets
after them; the rest of the file stays byte for byte (`streaming.py`). A 2022.3 game whose
localization tables are Addressables bundles printed "The AssetBundle ... can't be loaded because
it was not built with the right version or build target" and had no locales; now its language
screen leads to its title menu (emulated rig). Binary catalogs (`catalog.bin`, Addressables 2.x)
are not handled yet.

**StreamingAssets as files: `--streaming-assets obb|both|loose`.**

| mode | the files | asmpatch (opt-in patch, added to `--asmpatch`) | for |
|---|---|---|---|
| `obb` (default) | in the OBB | none | code that reads StreamingAssets with Unity's APIs only (Cuphead) |
| `both` | in the OBB and beside the APK in `Game_Data/StreamingAssets/` | `streaming-assets`: the reads whose value reaches System.IO get the folder; the others keep the jar path | a game that reads JSON, XML, SQLite, level or mod files there |
| `loose` | only in `Game_Data/StreamingAssets/` | `streaming-assets-all`: every read gets the folder, so Unity's APIs read it from there too | big StreamingAssets (`both` keeps them twice) |

The folder mirrors the desktop layout (a data folder with StreamingAssets in it); on the handheld
it is `ports/<name>/Game_Data/StreamingAssets/`, a plain folder on the card (exFAT or ext4) that
System.IO lists, reads and, as on a desktop, writes. The patched read takes the archive's folder
from the jar path itself, so it follows the APK and OBB wherever they are, and gives the original
value back for anything else (a desktop player, the directory route). Without `--asmpatch` the
folder is made but the code still reads the jar path (a warning says so). FMOD's banks stay in
`assets/` beside the APK too, for ua_host's FMOD path mapping (section 5).

In the emulated rig: a 2018.3 game whose menus take their text from localization XML files that it
lists and opens in StreamingAssets (`DirectoryInfo.GetFiles`, `File.Exists`, `FileStream`) showed
a dark screen and `KeyNotFoundException`s as before; with `both` its title menu. A 2020.3 game
that loads its subtitles with `File.Exists`/`File.ReadAllText` at startup finds them (no "Cannot
find file!"; Mono's trace shows the call). In a survey of the
corpus (`asmpatch --survey`), reads of StreamingAssets that reach System.IO came from Unity
Services Core's configuration reader (three games), a localization package's file storage, the
Addressables release above, and the games' own localization or subtitle files; Cuphead's reach only
`AssetBundle.LoadFromFile*`.

Caveats:
- `loose`: code that runs only on Android in the desktop build too (a run-time
  `Application.platform == RuntimePlatform.Android` branch) gets a folder where it expects the
  jar path; `UnityWebRequest.Get("/a/b")` turns a bare path into `http://localhost/a/b`
  (`WebRequestUtils.MakeInitialUrl`), where the desktop code it was written for adds `file://`.
  `both` keeps such reads on the jar path.
- Code that builds the jar path itself (`"jar:file://" + Application.dataPath + "!/assets/"`, in
  such Android branches) reads the OBB: keep the files there (`obb` or `both`).
- The folder must be writable even for code that only reads: `new FileStream(path, FileMode.Open)`
  asks for read and write access (desktop code does that). On the card it is; the emulated rig
  copies `Game_Data/` into its device folder for the same reason (`tools/emu/run_emu.sh`).
- A port's setup run again (a new release) replaces `Game_Data/StreamingAssets/`; anything else
  the game wrote into `Game_Data/` stays.

**Files at `Application.dataPath`.** On the player `dataPath` is the OBB file when one is mounted,
else the APK: `dataPath + "/x"` fails (`ENOTDIR`). Its folder, `Path.GetDirectoryName(dataPath)`
(and `dataPath + "/../x"` with .NET 4.x's class libraries, whose `Path.GetFullPath` folds the
`..`), is the folder beside the APK, where `--beside` files (a GOG build's `goggame-*.info`) and
`--root-file` files go. For code that reads or writes inside its data folder
(`dataPath + "/config.ini"`, a mods folder, a log or a screenshot next to the data), `--data-path`
makes `Game_Data/` beside the APK, a writable folder with the data folder's own files (top-level
files and folders Unity does not use: not the serialized files, `Managed/`, `Resources/`,
`StreamingAssets/`, `Plugins/`, `Mono*/`, `GI/`), and runs asmpatch's opt-in `data-path` patch:
the reads of `dataPath` that reach System.IO get that folder (a `dataPath` ending in `.apk` or
`.obb` becomes `<its folder>/Game_Data`; anything else, e.g. a desktop player's, stays). Then
`dataPath + "/StreamingAssets"` is the folder `--streaming-assets` makes, and
`GetDirectoryName(dataPath)` is still the folder beside the APK. Reads that reach only Unity's
APIs (an Android branch's `"jar:file://" + dataPath + "!/assets/"`) keep the archive. In the corpus,
the reads of `dataPath` that reach System.IO were debug output (heat maps, screenshots), editor
tools, a plugin folder ua_host maps itself (FMOD), or code with an Android branch that uses
`persistentDataPath` on the player: none needed `--data-path`, which the port check therefore only
mentions (`android_data.data_path`), never suggests.

**The working directory** (`Directory.GetCurrentDirectory`, relative paths) is ua_host's root, the
port folder, where a desktop build started from its own folder would look too.

### Saves

| | desktop player | the handheld (ua_host) |
|---|---|---|
| `Application.persistentDataPath` (save files) | Linux `~/.config/unity3d/<Company>/<Product>/`; Windows `%USERPROFILE%\AppData\LocalLow\<Company>\<Product>\`; macOS `~/Library/Application Support/<Company>/<Product>/` (older Unity: `…/unity.<Company>.<Product>/`) | `ports/<name>/data/external/` (`getExternalFilesDir`) |
| PlayerPrefs | Linux the `prefs` file in that folder (XML, strings base64); Windows the registry, `HKCU\Software\<Company>\<Product>`; macOS `~/Library/Preferences/unity.<Company>.<Product>.plist` | `ports/<name>/data/shared_prefs/<package>.v2.playerprefs.prefs` (the Android player's SharedPreferences: one `<i\|f\|s>\t<key>\t<value>` line each, `%XX` for tab, newline, `%`) |
| `temporaryCachePath` | the system's cache folder | `ports/<name>/data/cache/` (also Unity's shader cache) |

`<Company>` and `<Product>` are the game's PlayerSettings (a Linux or Windows build's data folder
has them in `app.info`, two lines; the Linux player writes `unknown/unknown` when that is missing).
The box64 route runs the Linux player, which keeps the Linux paths below its `HOME`
([box64-route.md](box64-route.md)). `data/` is not in a port's
release zip and the setup never touches it, so updates keep it. The package (`android_data.package`,
default `com.pmunity.<name>`) names the PlayerPrefs file: a port keeps it from release to release.

To move saves between a PC and the handheld: the files of `persistentDataPath` copy as they are
(the same names below the folder; a few games tie a save to a machine by encrypting it with a
device id); PlayerPrefs with `python -m pmunity prefs copy <from> <to>` between the Linux player's
`prefs` file and the handheld's `.prefs` file (`prefs.py`; `prefs show <file>` lists one). It keeps
the display settings each machine has its own of (`Screenmanager *`) unless `--all`. Windows'
registry and macOS's plist are not read: there a game's own save files are what copies.

## Unsure / untested

- Nothing has run on the player yet.
- The Android layouts are compared by field names, not by running GenerateTypeTree.
- UnityFS bundles were patched/rewritten by my own code and load in UnityPy (419 CAB headers checked,
  12 bundles parsed fully), but Unity itself has not read them yet.
- The Mount patch and the OBB route have not been tried.
- MT rendering and blit type are left at the game's values.
- StreamingAssets (section 6): the rig ran the `obb` route on an Addressables game and `both` on
  games reading localization XML and subtitle JSON with System.IO; `loose` and `--data-path` only
  in the unit tests and the asmpatch tests; an FMOD game with `both` not run. Binary Addressables
  catalogs, Windows and macOS PlayerPrefs not handled.
