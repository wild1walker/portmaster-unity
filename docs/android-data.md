# android-data: a game's converted data as Unity's Android player expects it

`python -m pmunity android-data --src <converted Game_Data> --pkg <Android player> [--pack all]`
(convert/pmunity/android_data.py) builds the data from the converted desktop data (which it only
reads) and the Android player package. For Cuphead a run takes about 15 s; big files are clones
(APFS clonefile, else hard links), so the tree costs about 0.4 GB of real disk (48 rewritten
bundles, explained below). The output is in `--out` (default `android-out/`).

Sections 1-3 come from libunity.so 2017.4.9f1 armeabi-v7a (sha256 `b7d24200…b604315e`),
disassembled with the symbols in libunity.sym.so (`tools/re/arm.py`); section 4 is what changes
for the other Unity versions (2017.3 - 2022.3).

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
   which goes through the same VFS.

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
- `tools/apicheck` (Mono.Cecil) results:
  - All 3035 references from the game assemblies into UnityEngine* resolve against the Android set.
    The 9 "missing" ones are multi-dim array runtime methods.
  - mscorlib `mono_corlib_version` = 82, which is what the Android libmono.so checks.
  - The macOS-only icalls are MovieTexture, GameCenter, ClusterInput and Windows.Speech, which the game doesn't use.

## 4. Unity versions (2017.3 - 2022.3)

Sections 1-3 come from the 2017.4.9 player. What `android-data` does for the other LTS lines,
from the static survey of their Android players ([compat](compat/README.md)) and runs on Unity's
built-ins and six free Linux games (one per line):

- **Versions.** The data's Unity version is read from `globalgamemanagers` (or a bundle header);
  outside 2017.3 - 2022.3 the run stops unless `--any-unity-version`. The player package's version
  comes from its `ivy.xml` (2017.x, 2018.x) or, failing that, from its "unity default resources"
  (which gives only the line: the file is often from an earlier patch). Data and player of
  different lines are refused (main data has no type trees: the player reads its own layouts);
  another patch of the same line is a warning. The report has both, and the Mono runtime the game
  needs: `.NET 3.5 (libmono)` (a `Mono/` folder, 2017.x - 2018.x) or `.NET 4.x (libmonobdwgc-2.0)`
  (`MonoBleedingEdge/`, and every 2019.1+ game).
- **SerializedFile headers.** Formats 17 (2017.x - 2018.x), 21 (2019.x) and 22 (2020.1+, a 48-byte
  header with 64-bit sizes: the version string at 48, the platform word after it). The platform
  word is set in place for all three, in plain files and in bundles' CABs.
- **Assemblies.** 2017.x: MonoManager's fixed slots (section 3). From 2018 (2018.4 - 2022.3
  disassembled) `GetAvailableDllAssemblyMask` takes every listed assembly that loaded, filtered by
  its `m_AssemblyTypes` flags: no slot is reserved, a missing engine module is skipped wherever it
  is. From 2020.2 the list is `ScriptingAssemblies.json` in the data folder (MonoManager keeps only
  script hashes); it is shipped with `RuntimeInitializeOnLoads.json`. The check (game assemblies
  present, engine modules the Android set lacks) reads whichever list the version has.
- **Mono config.** `--mono-etc` default: `Mono/etc` or `MonoBleedingEdge/etc`, in the data folder
  or (macOS) in `Contents/`.
- **Texture streams.** Inlined in every version (the 2017.4 reason, section 2, is not known for
  later players; inline pixels load either way). From 2018 meshes can stream their vertex data
  from the same `.resS`: a `.resS` that a mesh still reads is shipped beside its file (2021.3 URP
  game: 19 meshes in 3 files). The textures step likewise carries those blobs when it rewrites a
  `.resS`, and writes ASTC as `ASTC_<n>x<n>` (48-53) from 2019.1, where the RGBA numbers are
  deprecated.
- **GraphicsSettings.** `m_ShaderDefinesPerShaderCompiler` has the same layout in every version;
  the GLES3 entry is copied from GLCore (from D3D11 for a Windows build).
- **PlayerSettings.** As in section 2, plus `--swappy keep|off|on`: `androidUseSwappy` (2019.3+,
  Android Frame Pacing). Off changes pacing, not the picture; use it if the runtime's choreographer
  path misbehaves.
- **OBB.** 2017.4 - 2020.3: the `assets/<build-id>` marker and `unity.build-id` meta-data. 2022.3's
  libunity has `/assets/unity_obb_guid` ("Application OBB has mismatching GUID"); 2021.3 has neither
  string (its check was not traced). For Unity 2021+ the packer writes `assets/unity_obb_guid`
  (the build id as text) into the APK and every OBB, and names it in `android_package.json`; not
  run against a player. The build id comes from `BuildSettings.buildGUID`, which 2022 dropped:
  then `PlayerSettings.productGUID`, else a hash of the settings file.
- **What is refused or reported.** A compressed build (`data.unity3d` without
  `globalgamemanagers`): unpack it first with `python -m pmunity unpack <data> [--remove]`
  (stored, LZ4 and LZMA blocks; the files come out as an uncompressed build has them, which is what
  an Android player reads from `assets/bin/Data`). An IL2CPP build (no `Managed/*.dll`;
  `GameAssembly`, `il2cpp_data`): the Mono player cannot run it. Native plugins in `Plugins/`
  (e.g. `lib_burst_generated.so`, FMOD, Steam) are not shipped; the report names them.

Six free Linux Mono games (2017.4.40f1, 2018.4.36f1, 2019.4.8f1, 2020.3.49f1, 2021.3.16f1,
2022.3.36f1) go through every step (shaders, textures, settings, audio, android-data with
`--pack all`) with the Android player package of their own version (its `unity default resources`
and Mono `Managed` set, fetched from Unity's installers): every serialized file loads back in
UnityPy with the Android platform, every assembly the list names is present, APK and OBB verify.
None has run in a player.

## Unsure / untested

- Nothing has run on the player yet.
- The Android layouts are compared by field names, not by running GenerateTypeTree.
- UnityFS bundles were patched/rewritten by my own code and load in UnityPy (419 CAB headers checked,
  12 bundles parsed fully), but Unity itself has not read them yet.
- The Mount patch and the OBB route have not been tried.
- MT rendering and blit type are left at the game's values.
