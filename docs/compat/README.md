# Unity version coverage: what each LTS Android player needs

The runtime and converter were built for Unity 2017.4.9f1 (Mono, armeabi-v7a; Cuphead). This
survey reads the Android players of the last patch of every LTS line, 2017.4 to 2022.3, and lists
what each one needs that the tools do not have yet. It is static analysis of Unity's own files
(imports, JNI tables and string references, the Java side, the built-in resources); none of these
players has been run. Per-version details:
[2017.4.40f1](unity-2017.4.40f1.md) · [2018.4.36f1](unity-2018.4.36f1.md) ·
[2019.4.40f1](unity-2019.4.40f1.md) · [2020.3.48f1](unity-2020.3.48f1.md) ·
[2021.3.45f2](unity-2021.3.45f2.md) · [2022.3.62f3](unity-2022.3.62f3.md).

## Matrix

✅ works as for 2017.4.9 / provided · ⚠️ works with a caveat or untested path · ❌ missing (breaks
loading, startup, input or data conversion) · "-" not applicable.

| | 2017.4.9 (done) | 2017.4.40 | 2018.4.36 | 2019.4.40 | 2020.3.48 | 2021.3.45f2 | 2022.3.62f3 |
|---|---|---|---|---|---|---|---|
| Mono ABIs | v7a, x86 | v7a, x86 | v7a, x86 | v7a | v7a | v7a | v7a |
| IL2CPP ABIs | v7a, x86 | v7a, **arm64**, x86 | v7a, arm64, x86 | v7a, arm64, x86, x86_64 | same | same | same + armv9 variant |
| Mono runtime | libmono ✅ (+bdwgc) | same ✅ | same ✅ (+PosixHelper) | **bdwgc only** ⚠️ never run | ⚠️ | ⚠️ +mono-native | ⚠️ |
| NDK / min API | old / - | old / - | r16b / 16, 21 | r19 / 19, 21 | r19 / 19, 21 | r21d / 22 | r23b / 22 |
| armeabi-v7a code | ARM | ARM | ARM | Thumb-2 | Thumb-2 | Thumb-2 | Thumb-2 |
| relocations, TLS, IFUNC | REL/RELA ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| segment alignment | 4K ✅ | 4K ✅ | 4K, arm64 64K ✅ | 4K, arm64 64K ✅ | 4K, arm64 64K ✅ | 4K ✅ | **16K all** ✅ (4K kernels) |
| DT_NEEDED | ✅ | ✅ | ✅ | ✅ (no GLES/stdc++) | ✅ | ✅ (mono-native → SONAME) | ❌ **libmediandk** |
| imports nobody provides | 0 | 0 | 0 | ❌ 4 | ❌ 4 | ❌ 9 | ❌ 9 + libmediandk set |
| imports with ABI risk | - | - | - | ⚠️ getpwuid_r, nftw | ⚠️ + eglSurfaceAttrib | ⚠️ + wcstod/wcstof (softfp), statvfs | ⚠️ + `*_l` locale |
| RegisterNatives (count) | 45 ✅ | 45 | 44 | 47 | 45 | 48 | 48 |
| natives runtime/jni calls | ✅ | ❌ SendMessage(byte[]) | ❌ same | ❌ + nativeDone()Z | ❌ same | ❌ same | ❌ + **nativeInjectEvent(·,int)** |
| AndroidJavaProxy | ✅ (int handle) | ✅ | ✅ | ❌ long handle | ❌ + new newProxyInstance | ❌ | ❌ |
| Java members bound (count) | 398 | 402 | 458 | 487 | 487 | 487 | 395 |
| UnityPlayer Java model | ✅ | ✅ | ✅ | ✅ (+new callbacks ⚠️) | ✅ ⚠️ | ✅ ⚠️ | ✅ ⚠️ (GameActivity only from 2023.1) |
| frame pacing (Swappy) | - | - | - | ⚠️ falls back (untested) | ⚠️ | ⚠️ | ⚠️ |
| SerializedFile format | 17 ✅ | 17 ✅ | 17 ✅ | 21 ✅ | 22 ✅ | 22 ✅ | 22 ✅ |
| shader serializer (converter) | 201708220 ✅ | 201708220 ✅ | 201802150 ✅ | 201806140 ✅ segments | 201806140 ✅ | 202012090 ✅ param entries | 202012090 ✅ |
| D3D11 programs (Windows builds) | ✅ | ✅ | ✅ | ❌ GL cbuffer layouts | ❌ + common params | ❌ | ❌ |
| assembly list (converter) | fixed 48 slots ✅ | fixed 48 ✅ | flags (`m_AssemblyTypes`) ✅ | flags ✅ | `ScriptingAssemblies.json` ✅ (2020.2+) | json ✅ | json ✅ |
| texture formats (converter) | ASTC RGB/RGBA ✅ | ✅ | ✅ | `ASTC_nxn` ✅ | ✅ | ✅ | ✅ |
| Android engine modules | 45 | 45 | 59 | 58 | 63 | 64 | 64 |
| OBB matching | build-id ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ not traced; `unity_obb_guid` written | ⚠️ `unity_obb_guid` written, unchecked |
| IL2CPP API looked up | - | 175 | 194 | 222 | 225 | 230 | 234 |
| IL2CPP metadata (header, unverified) | - | 24 | 24 | 24 | 27 | 29 | 29 or 31 |

Notes on the rows:
- "imports nobody provides": undefined dynamic symbols of `libmain`, `libunity` (Mono v7a, IL2CPP
  v7a and arm64) and the Mono libraries that are neither in the runtime's shim tables nor in glibc
  (calls land in the loader's abort stub). 2019.4/2020.3: `__FD_SET_chk`, `__FD_ISSET_chk`,
  `android_set_abort_message`, `__mmap2`. 2021.3 adds `__strlen_chk`, `__strchr_chk`,
  `__strrchr_chk`, `__strlcpy_chk`, `sys_signame` (and the FD ones on v7a too).
- "natives runtime/jni calls": the 19 natives `runtime/jni` looks up by name and signature,
  checked against what each libunity/libmain registers.
- Converter rows (shaders, assembly list, textures, OBB): what `convert/` does for each version,
  checked on Unity's built-in resources of each line (`tests/test_glcore_corpus.py`: conversion
  equal to Unity's own GLES3 data but for the program text) and on six free Linux games, one per
  line, taken through every step to packed APK/OBB ([shaders](../shaders.md#unity-versions),
  [android-data](../android-data.md#4-unity-versions-20173---20223)). Nothing of it has run in
  a player of these versions. Serializers: 2019.1 - 2021.1 entries have a second (local) keyword
  list; 2019.3+ blobs are segmented (12-byte entry table); 2021.2+ keep parameters as entries of
  their own and subprograms in per-tier lists. D3D11: 2019.1+ GL programs carry constant buffer
  layouts the D3D11 data does not give (refused with that reason).
- IL2CPP metadata: the header's integer version (`global-metadata.dat` starts with 0xFAB11BAF
  and the version). These values come from public reverse-engineering tools, not from these
  packages (`libil2cpp.so` and the metadata are built per game), and tools also use sub-versions
  (24.1 for 2018.3/2018.4, 24.2-24.5 across 2019.x, 27.1 for 2020.2.4+, 29.1 for 2022.x; late
  2022.3 patches are reported with 31). Read the header of the game's own file.

## Gaps, most impact for the most games first

1. **JNI signatures that drifted** (every version after 2017.4.9). runtime/jni looks natives up
   by exact signature; make it take what libunity registered and adapt:
   `nativeUnitySendMessage(String, String, byte[])` (2017.4.40+), `nativeDone()Z` (2019.4+),
   `nativeInjectEvent(InputEvent, int)` (2022.3: **no input otherwise**), and the
   `ReflectionHelper` proxies with `long` handles (2019.4+) and `newProxyInstance(long, ...)` /
   `newProxyInstance(UnityPlayer, long, ...)` (2020.3+), used by every AndroidJavaProxy.
2. **Missing bionic symbols** (2019.4+, more in 2021.3+): `__FD_SET_chk`, `__FD_ISSET_chk`
   (add `__FD_CLR_chk`), `__strlen_chk`, `__strchr_chk`, `__strrchr_chk`, `__strlcpy_chk`,
   `android_set_abort_message`, `__mmap2` (32-bit), `sys_signame` (data). Small additions to
   `loader/hybris_compat.c`. Same place: softfp wrappers for `wcstod`/`wcstof` (2021.3+), and
   32-bit layout translation for `getpwuid_r` (`struct passwd` without `pw_gecos`), `nftw`
   (bionic `struct stat` in the callback), `statvfs`/`fstatvfs` (Mono.Posix only).
3. **libmediandk stub** (2022.3: libunity cannot load without it). A shim table with the
   `AMediaCodec_*`/`AMediaExtractor_*`/`AMediaFormat_*` functions failing cleanly and the
   `AMEDIAFORMAT_KEY_*` string objects; register it so the loader's libmediandk refusal does not
   apply. Video then fails the way it does today on the Java path.
4. **Converter data formats**: done in the converter (SerializedFile format 22, the four shader
   serializers, segmented blobs, parameter entries and per-tier subprograms, assembly lists by
   flags and `ScriptingAssemblies.json`, 2019+ ASTC numbers, compressed builds via `pmunity
   unpack`). Left: D3D11 programs of 2019.1+ Windows builds (GL constant buffer layouts), the
   2021.3 OBB check (not traced; `assets/unity_obb_guid` is written for 2021+).
5. **Mono 5/6 (`libmonobdwgc-2.0.so`)**: the only Mono runtime from 2019.x on (and of any
   .NET 4.x game of 2017.4/2018.4), never run on this runtime. Bring-up test: Boehm GC thread
   suspend/restart signals through the loader's signal translation, `sigaltstack`,
   `libmono-native.so` (2021.3+, needs its SONAME dependency `libmonoboehm-2.0.so` loaded first).
6. **UnityPlayer callbacks added since 2019.4** that libunity can call and runtime/jni lacks
   (they answer 0/null now): `isUaaLUseCase`, `getUaaLLaunchProcessType`, `getLaunchURL`,
   `getNetworkConnectivity`, `getKeyboardLayout`, `startOrientationListener`, the 10-argument
   `showSoftInput`, clipboard, `getScreenBrightness`/`setScreenBrightness` (2021.3),
   `setMainSurfaceViewAspectRatio`, activity indicator (2022.3). Give each a sane default.
7. **Swappy** (2019.3+): decide its behaviour instead of relying on its fallback: mark
   `com.google.androidgamesdk.*` and `dalvik.system.InMemoryDexClassLoader` absent in fakejni (or
   implement `AChoreographer_*` from the host's vsync). The converter could also turn
   `androidUseSwappy` off; it changes pacing, not the picture.
8. **EGL**: `eglSurfaceAttrib` (2020.3+) on the SDL display path must map the glue's fixed surface
   handle to the real surface.
9. **Packaging of Android builds** (games owned as APK/AAB): split APKs
   (`ApplicationInfo.splitPublicSourceDirs`, 2017.4.40+), Play Asset Delivery
   (`PlayAssetDeliveryUnityWrapper.getAssetPackPath`, 2019.4+; Unity data in `UnityDataAssetPack`),
   OBB GUID file `assets/unity_obb_guid` (2022.3).
10. **IL2CPP Android games**: loading already works through `ClassLoader.findLibrary` +
    `System.load`; libunity then copies IL2CPP data to `<filesDir>/il2cpp` (writable files dir
    needed; 2022.3 re-extracts when `assets/bin/Data/unity_app_guid` changes) and binds 175-234
    `il2cpp_*` functions from the game's `libil2cpp.so`.
11. Small: `nativeSendSurfaceChangedEvent` after a resize (2018.4+); the 2022.3 `armv9` libunity
    must not be used on ARMv8 CPUs; 16 KiB-page kernels stay unsupported (the 2022.3.62 libraries
    would allow them).

What needs nothing: no player uses packed (APS2) or RELR relocations, IFUNC or ELF TLS (all
REL/RELA + PLT; no PT_TLS segment or TLS relocation anywhere), no new DT_NEEDED before 2022.3, and the
Java side keeps the 2017.4 model throughout (`UnityPlayer` FrameLayout + SurfaceView, NativeLoader,
`initJni(Context)`, a `UnityMain` looper thread posting "next frame" messages that run GL-thread
jobs and `nativeRender`).

## How this was surveyed (repeatable)

Downloads: the Windows "Android Build Support" installer of each version from Unity's archive
(Unity's release API gives each version's changeset and URL). The installers are non-solid NSIS
(from 2020.3 a large-installer variant: 36-byte first header, 64-bit file offsets),
so `tools/re/nsisrange.py` fetches only the needed files with HTTP range requests (20-30 MB per
version); the installers were never stored. Per version:

```
python3 tools/re/nsisrange.py list    <installer URL>
python3 tools/re/nsisrange.py extract <installer URL> pkg '$_OUTDIR/Variations/mono/Release/*' ...
tools/re/survey_player.sh pkg/_OUTDIR notes      # docker: ua-spec (pyelftools, capstone), ua-jni-build (javap)
python3 tools/re/unitydata.py "pkg/_OUTDIR/Data/unity default resources"   # UnityPy + lz4
python3 tools/re/javacalls.py notes/javap_s.txt <libunity.so> > notes/javacalls.tsv
gcc ... tools/re/jnicover.c runtime/jni/*.c && jnicover < notes/bindings.tsv   # see its header
python3 tools/re/playerdiff.py <base notes> notes runtime --all-missing
```

| tool | does |
|---|---|
| `nsisrange.py` | list/extract NSIS installers over HTTP ranges (both NSIS layouts Unity uses) |
| `elfimage.py` | relocations of every format (REL/RELA, APS2, RELR), relocated pointer reads, ELF facts (NDK note, alignment, TLS, IFUNC) for both ABIs |
| `elfimports.py` | undefined symbols with provider and ABI notes (now both ABIs, packed relocations) |
| `strrefs.py`, `jnibindings.py` | Java members libunity binds (now also Thumb-2 code and literal-pool strings) |
| `jnitables.py` | RegisterNatives tables |
| `dis.py` | disassemble functions by name, or find functions by the strings they reference (AArch64) |
| `javacalls.py` | player Java methods/fields libunity can reach by name |
| `jnicover.c` | which bound classes/members runtime/jni defines |
| `shimnames.py` | names the runtime provides (loader, libhybris hooks, Android shims) |
| `unitydata.py` | SerializedFile header, shader blob layout and serializer versions of the built-in resources |
| `playerdiff.py` | the comparisons above between two players, against the runtime |
