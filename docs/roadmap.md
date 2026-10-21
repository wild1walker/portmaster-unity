# Roadmap: Unity games on PortMaster, as many as possible

Goal: one tool set that ports nearly any Unity game to ARM Linux handhelds through PortMaster,
from whatever build the player owns. Status: Cuphead (Unity 2017.4.9f1, Mono, macOS build, 32-bit
ARM) is the first game done end to end.

## Ways in

| the player owns | how it runs | needs |
|---|---|---|
| macOS / Linux desktop build, Mono | the same Unity version's Android player; desktop data converted (GLCore shaders -> GLES3, textures, settings) | done for 2017.4 (32-bit ARM); version coverage below |
| Windows desktop build, Mono | as above, but the shaders are Direct3D bytecode (DXBC) only | a DXBC -> GLSL ES translator in the converter (Unity's open-source HLSLcc does this) |
| the game's own Android build (APK / OBB / AAB), Mono or IL2CPP | as is, on our runtime (32- or 64-bit): `pmunity apk` (docs/apk.md) | runtime coverage of that Unity version's Java side and native imports; 2019.4 - 2022.3 Mono and IL2CPP builds run in the emulated rig, arm64-v8a ones on the 64-bit host |
| desktop build, IL2CPP (GameAssembly) | its code is compiled for x86: not portable this way | (out of scope) |
| desktop build, Mono, 64-bit ARM wanted | the game's assemblies compiled with Unity's IL2CPP for the Android player's arm64-v8a variation | Unity's il2cpp tool and libil2cpp of that version, Android NDK; AOT limits (no Reflection.Emit) |

Unity ships 64-bit ARM Android players only with IL2CPP (2017.4.16+ and 2018.2+); Mono players
are 32-bit ARM in every version.

## Work items

1. **64-bit runtime** (arm64-v8a players): ELF64/AArch64 relocations (RELA, packed Android
   relocations, RELR), bionic arm64 TLS (stack guard slot at tpidr_el0+0x28), setjmp/longjmp,
   64-bit bionic structure layouts, JNI calls with AAPCS64 (no softfp), a native aarch64 host
   build (the handheld's own 64-bit GLES/SDL2, no 32-bit userland needed).
2. **Version coverage**: Android players of the LTS line (2017.4, 2018.4, 2019.4, 2020.3, 2021.3,
   2022.3): their libraries' imports (`tools/re/elfimports.py`), the Java classes and natives they
   use (`FJ_DUMP`), the startup order, data paths, the OBB/asset-pack layout; IL2CPP
   (`libil2cpp.so`, `global-metadata.dat`) next to Mono.
   Surveyed statically for the last patch of each line: [compat/README.md](compat/README.md)
   (matrix and the prioritized gap list).
3. **Converter coverage**: shader program blob layouts per version, GraphicsSettings and
   MonoManager per version (the engine-module slot count), compressed builds (`data.unity3d`),
   bundles of all versions, Addressables; DXBC shaders (Windows builds).
4. **IL2CPP pipeline** for Mono desktop games on 64-bit ARM.
5. **Test corpus**: games across versions and build types, each with a deterministic smoke test
   (load to the first scene, screenshot) in the emulated rig (`tools/emu`) and on a handheld.

Each item keeps the rule the Cuphead port follows: the game looks and plays as the original;
anything that changes that is an option, off by default.
