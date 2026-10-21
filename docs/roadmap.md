# Roadmap: Unity games on PortMaster, as many as possible

Goal: one tool set that ports nearly any Unity game to ARM Linux handhelds through PortMaster,
from whatever build the player owns. Status: Cuphead (Unity 2017.4.9f1, Mono, macOS build, 32-bit
ARM) is the first game done end to end.

## Ways in

| the player owns | how it runs | needs |
|---|---|---|
| macOS / Linux desktop build, Mono | the same Unity version's Android player; desktop data converted (GLCore shaders -> GLES3, textures, settings) | Cuphead (2017.4) done; games of 2017.4 - 2022.3 reach their titles on a handheld, 5.6 - 6000.6 in the emulated rig ([compat](compat/README.md)) |
| Windows desktop build, Mono | as above, but the shaders are Direct3D bytecode (DXBC) only | done in the converter (`pmunity shaders --from d3d11`, Unity's HLSLcc; 2017.3 - 2022.3): six games of 2018.3 - 2022.3 reach their first screens in the emulated rig; not yet run on a handheld |
| the game's own Android build (APK / OBB / AAB), Mono or IL2CPP | as is, on our runtime (32- or 64-bit): `pmunity apk` (docs/apk.md) | runtime coverage of that Unity version's Java side and native imports; 2019.4 - 2022.3 Mono and IL2CPP builds run in the emulated rig, arm64-v8a ones on the 64-bit host; 2020.3 - 2022.3 IL2CPP arm64 builds reach their titles on a handheld |
| desktop build, IL2CPP (GameAssembly) | its code is compiled for x86: not portable this way; the game's own **Linux** build runs under box64 instead ([box64-route.md](box64-route.md)) | menus reached in the emulated rig (IL2CPP 2020.3, 2022.3, Unity 6); not yet run on a handheld |
| Linux build of a version or with plugins the Android route does not take (5.0 - 5.5, 6000.7+; x86-only FMOD/Wwise/...) | the same: the game's Linux x86_64 build under box64, GL through the player's own OpenGL ES mode (`-force-gles` + glxes), Mesa or gl4es | `pmunity route` picks the route; see box64-route.md for what is left |
| desktop build, Mono, 64-bit ARM wanted | the game's assemblies compiled with Unity's IL2CPP for the Android player's arm64-v8a variation | `pmunity il2cpp` on the porter's PC/Mac (Unity's il2cpp and libil2cpp of that version, Android NDK parts); AOT limits (no Reflection.Emit); 2018.4 - 2020.3 games reach their titles on a handheld |

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
4. **IL2CPP pipeline** for Mono desktop games on 64-bit ARM: `pmunity il2cpp` (docs/il2cpp.md) compiles a
   Mono game with the exact version's il2cpp and NDK parts on the porter's PC/Mac; 2018.4, 2019.4 and
   2020.3 games reach their titles on the arm64-v8a IL2CPP player in the emulated rig, and a 2017.4
   .NET 3.5 game (NDK r13b, the old Mono's class libraries). Left: 2021.x/2022.x (NDK r21d/r23b), 2017.4's
   FMOD output on the 64-bit player, plugins, a port template for the route (2018.4 - 2020.3 have since
   run on a handheld: compat/README.md).
5. **Test corpus**: games across versions and build types, each with a deterministic smoke test
   (load to the first scene, screenshot) in the emulated rig (`tools/emu`) and on a handheld.
6. **box64 route** for the games above ([box64-route.md](box64-route.md)): `pmunity route`,
   `tools/box64` (box64/gl4es builds, glxes, test rig), the `box64` route of `pmunity port`
   (launcher, on-device setup with the GLES conversion). Left: a handheld run (GLES mode through
   Westonpack's `crusty_x11egl`), Mono AOT for the JIT's first-use cost, 32-bit x86 builds (box64's
   BOX32 ran a 5.3 game in the rig; box86), Unity 6 / 2023 data in the converter for GLES mode.

Each item keeps the rule the Cuphead port follows: the game looks and plays as the original;
anything that changes that is an option, off by default.
