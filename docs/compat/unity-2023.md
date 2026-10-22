# Unity 2023.1 and 2023.2 (tech stream), Mono armeabi-v7a, vs 2022.3.62f3

Installers: `UnitySetup-Android-Support-for-Editor-2023.1.22f1.exe` (changeset c1416845ad01) and
`...-2023.2.22f1.exe` (6b19bf4f8115), the last patches of the two lines: the 2020.3 large-installer
NSIS layout, per-file blocks (one range request per file). Run in the emulated rig: a free Linux
game of 2023.2.20f1 (its main menu); no 2023.1 Mono game was found in the corpus feeds.

## What changes, in short
2023.1 is where the Java side becomes Unity 6's (unity-6000.md): `UnityPlayer` abstract,
`UnityPlayerForActivityOrService` holding the per-instance natives (81 mentions in 2023.1's
javap, 91 in 2023.2's), the GameActivity variant as sources in the package, `initJni(Context,
int)`, `nativeMemoryUsageChanged` in place of `nativeLowMemory` (from 2023.2; 2023.1 has neither),
`nativeInjectEvent(InputEvent)Z` again. 2023.2 moves `unity default resources` to
`Data/Resources/` in the package and the player reads it from `assets/bin/Data/Resources/`
(libunity 2023.1: "Data/unity default resources", 2023.2: "Resources/unity default resources").

| | 2023.1.22f1 | 2023.2.22f1 |
|---|---|---|
| toolchain | NDK r23b, API 22, Thumb-2, 4 KiB segments (2022.3.62 had 16 KiB) | same |
| libunity imports | 440, all provided | 423, all provided |
| RegisterNatives | 48: + `initJni(Context, I)`, `nativeOnApplyWindowInsets`, `permissionResponseToNative(JZ)`; - `initJni(Context)`, `nativeLowMemory`, `nativeGetSoftInputType`, `nativeProxyJNIFreeGCHandle`, `nativeInjectEvent(InputEvent, I)` | 60: + `nativeMemoryUsageChanged`, `nativeUnityPlayerSetRunning`, accessibility (`getRootNodeIds`, `hitTest`, `populateNodeInfo`, ...) |
| Java members bound | 413 (+33 -15): Dialog, KeyEvent/MotionEvent getters | 432 (+20 -1): Rect, `View.getLocationOnScreen`, AccessibilityNodeInfo |
| `unity default resources` | format 22, "2023.1.5f1", serializer 202012090, in `Data/` | format 22, "2023.2.2f1", 202012090, in `Data/Resources/` |
| Mono | `libmonobdwgc-2.0.so` (NDK r19 build, as 2021.3+) | same |

## What the tools need
Nothing beyond the 6000.x work: the converter's 2022.3 formats, the default resources path from
2023.2 (`DEFAULT_RESOURCES_IN_RESOURCES_FROM`), the runtime's natives on both player classes, the
2023.1+ UnityPlayer members and the UI thread's ALooper (unity-6000.md); 2023.2.20f1's
PlayerSettings is one byte longer than UnityPy's tree (typetrees.py keeps it).

2023.1 in a game (2023.1.8f1, 2026-10-22): its libunity reads the Android Game SDK's Memory
Advice models from the APK's assets at start (`realtime_features.json`, then `realtime.tflite`,
through AAssetManager; the strings sit with `predictRealtime` among the GL/Vulkan feature names,
and are not in 6000.0's libunity). A desktop build has none: the player logged "TuningForkUtils:
Can't find realtime_features.json in APK" and aborted (`std::length_error: basic_string`); with an
empty JSON it crashed on the missing model. The installer has them in `Apk/MemoryAdvice/`
(`available`, `oom`, `realtime` models and their feature lists, about 20 KB): `pmunity player
fetch` takes them and android-data ships them (loose beside the APK, where ua_host's
AAssetManager looks) to players whose libunity names them. The game then starts ("Initialized
TensorFlow Lite runtime") and draws its loading screen.
