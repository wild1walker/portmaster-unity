# The handheld quality profile

A desktop build keeps the quality its PC players chose: most often the Ultra or High level of its
QualitySettings, a URP asset made for a desktop GPU (4096x4096 shadow atlases, 4x MSAA, SSAO at
full resolution with a three-pass blur), the Standard shader's desktop SubShader. An Android build
of the same project would have started from Unity's Android defaults (the Medium level: one pixel
light, hard shadows, no MSAA) and a mobile URP asset. On a handheld with 1 GB of RAM (the RG34XX-SP:
a single-core Mali-G31, every game below 60 fps GPU-bound) the converter's `quality` step makes the
picture cheaper by default, the way such a build would, and leaves the game play as it is. On
bigger devices it changes nothing, and a port's setup question gives the player either choice.

    python -m pmunity convert <data> --config game.json        # with "steps": [..., "quality"]
    "quality": {"profile": "auto"}                             # auto (default) | handheld | full

The rule it follows (the user's, 2026-10-24): on 1 GB devices the sacrifices may be visual, never
game play. So it never changes timing, physics, scripts or what the player is told, and nothing the
player needs to see may disappear:

- no LOD bias or maximum LOD level (a LODGroup's last, culled transition would come earlier), no
  layer cull distances, no shadow distance (shadows keep showing where things are, also far away);
- shadows stay on where they are, made cheaper (a smaller map, hard instead of soft);
- lights stay lights: point lights beyond the per-pixel budget are lit per vertex (or by spherical
  harmonics past four), as Unity's forward path does for any light over its budget; spot lights and
  lights with shadows or a cookie stay per pixel (they are made Important), since a light without a
  cone or a shadow would light what the player should not see;
- post effects that the game's own code or its animations drive (a damage flash in a chromatic
  aberration, a focus pull in depth of field) are left as they are, with the volumes they are in;
- no shader variant the build stripped is ever asked for: a setting is changed only where the
  converted data has the variant it needs (the converter knows every program's keywords), so Unity
  never falls back to another variant. Changes that keep the keywords (downsampling, a blur that is
  another pass of the same shader, shadow resolution, a uniform) are always safe.

## What it changes

| | what | where | condition |
|---|---|---|---|
| built-in | pixel lights per object at most 1 (`pixelLightCount`); Auto spot lights and lights with shadows or a cookie Important | QualitySettings (every level), the scenes' and prefabs' Light components | ForwardBase passes with `VERTEXLIGHT_ON` variants in the data |
| built-in | soft shadows hard (`shadows` 2 -> 1) | QualitySettings | every `SHADOWS_SOFT` variant has its hard twin |
| built-in | shadow resolution at most Medium (`shadowResolution`, lights' own), cascades at most 2 | QualitySettings, Light components | (no variant changes from 4 to 2 cascades) |
| built-in | MSAA off, anisotropic filtering per texture (not forced), soft particles off | QualitySettings | |
| built-in | the Standard shaders' LOD-150 SubShader first (below) | Shader (`Standard`, `Standard (Specular setup)`, `Standard (Roughness setup)`, `Autodesk Interactive`) | forward rendering (no deferred tier or camera); that SubShader has GLES3 programs for its forward, forward-add and shadow passes |
| URP | MSAA off (`m_MSAA` 1), shadow atlases at most 2048 (main and additional lights), soft shadow quality Low (URP 14+) | UniversalRenderPipelineAsset (every one in the data) | soft shadow quality: where it is a uniform, or `_SHADOWS_SOFT_LOW` variants exist |
| URP | SSAO downsampled, its blur Kawase (one pass), the fewest samples a compiled variant has (URP 14+; URP 10 - 12: at most 4) | the SSAO renderer feature's settings | the Kawase pass, and for the sample count every keyword set the current one is compiled with |
| URP | chromatic aberration, lens distortion and motion blur off (film grain stays: one fetch in the uber pass, and it dithers dark gradients that band without it) | volume profiles' components | the game's code and animations do not drive volumes |
| opt-in | HDR targets R11G11B10 instead of FP16 (`hdr_format`) | GraphicsSettings tiers | an image effect that keeps data in the alpha channel breaks |
| opt-in | URP render scale (`render_scale`, e.g. 0.75) | UniversalRenderPipelineAsset | |

Each item can be switched off in the config (`quality.<item>`; `null` or `false`): `pixel_lights`,
`shadow_resolution`, `hard_shadows`, `shadow_cascades`, `msaa`, `anisotropic`, `soft_particles`,
`standard_lod150`, `urp_shadow_resolution`, `urp_soft_shadows`, `ssao`, `post` (a list of component
names); `ram_mb` (1536) is the `auto` threshold (MemTotal of a 1 GB handheld is 900 - 1000 MB, of a
2 GB one about 1900). The step logs each change, and each thing it found and left alone, with why
(`quality: kept: ...`).

### The Standard shader's LOD-150 SubShader

Unity's `Standard.shader` (its built-in shader source is MIT licensed) has two SubShaders: LOD 300
(`#pragma target 3.0`) and LOD 150 (`#pragma target 2.0`, `skip_variants SHADOWS_SOFT
DIRLIGHTMAP_COMBINED`). Below shader target 3.0 `UnityPBSLighting.cginc` takes `BRDF3_Unity_PBS`
(a lookup-texture specular), and `UnityStandardConfig.cginc` turns off box projection, reflection
probe blending, detail normal maps and parallax: Unity's mobile class of Standard, which a desktop
build compiles too and never uses, since Unity takes the first SubShader the GPU runs. The step
moves the LOD-150 SubShader first (the programs are referenced by blob index, so the order is free);
it has no deferred pass, so only for forward rendering. What changes: the specular response, no
box-projected or blended reflections, no parallax or detail normals, hard spot and point shadows.

### URP's assets: type trees from the game's assemblies

URP's settings are MonoBehaviours (ScriptableObjects), whose fields a desktop build's files carry
without type trees. `monotree.py` builds them from the class metadata in the game's own `Managed/`
assemblies (pure Python, `clrmeta.py`), by Unity's serialization rules (public or `[SerializeField]`
instance fields that are not readonly or `[NonSerialized]`, base classes first; primitives, strings,
enums as their underlying type, UnityEngine.Object references as PPtrs, Unity's value types,
`[Serializable]` classes and structs inline, arrays and `List<T>`, generic field types from 2020.1;
1- and 2-byte fields, strings and arrays padded to 4 bytes). A tree is used only when it reads its
object's bytes exactly, every one of them, and the object is written back with that same tree, so a
rule it gets wrong shows up as an object left alone, never as a wrong edit. Not built:
`[SerializeReference]` fields and the native classes with layouts of their own (AnimationCurve,
Gradient, GUIStyle): those objects are left alone (none of the ones the profile edits has them).

Checked on the five URP games of the benches below (Unity 2019.3 - 6000.6; `tests/test_quality.py`'s
opt-in corpus test): 4,516 of their 4,524 MonoBehaviours read exactly (the other 8 have AnimationCurve
or `[SerializeReference]` fields), and where TypeTreeGeneratorAPI (K0lb's, MIT; a Mac-side check,
not a dependency) reads them too the values are the same, but for what that package misreads: its
MonoBehaviour base reads `m_Script` from the wrong offset, and it reads a `List<string>` as one
string (URP 17's `SerializedDictionary` keys, a dialogue box's lines, FMOD's platform properties,
an options stepper's options).

## At run time: ua_host keeps the caps

A game that sets quality itself while it runs (an options menu: `QualitySettings.antiAliasing = 4`)
would undo the caps. The step writes the QualitySettings caps it applied into the conversion's
journal, android-data copies them into `android_package.json` (`"quality": "msaa=0,pixel_lights=1,..."`),
and ua_host wraps those setters and getters as libunity registers them (runtime/host/quality.c, as
framerate.c does for the frame rate): Unity gets min(the game's value, the cap), the game reads back
the value it set. After each frame the values Unity holds are checked again, since a quality level
change (`SetQualityLevel`) sets them without setter calls. `UA_QUALITY=off` turns the run-time caps
off (the data keeps what the setup chose), `UA_QUALITY=<caps>` sets others ([runtime.md](runtime.md)).
A game reads back its levels' values as the profile left them, and the values it sets as it set them.

## In a port

`port new` puts the step into `convert.steps` with `profile = "auto"` and adds a `PICTURE` question
([porting.md](porting.md#porttoml)): by this handheld (the default), lighter, or as on PC. The
answer applies when the setup converts the data; changing it means running the setup again.

## Measured on the handheld

The six GPU-bound games of [games.md](compat/games.md#handheld-benches-rg34xx-sp-2026-10-23), each
folder packed as before (the same converter, the same `ua_host`), its APK once as it was and once
with the step's output in it; the bench plans of profiling.md's
[six games](profiling.md#six-more-gpu-bound-games-pass-by-pass-2026-10-23), from a first launch,
every run from a cool start (CPU and GPU at most 50 C), the two kinds interleaved in one hold.
What the step changed in each:

| game | changed |
|---|---|
| Arthur's Nightmare (2017.3, built-in forward) | Ultra (the current level): 4 -> 1 pixel lights (27 point lights, no shadows, lit per vertex; the spot light, with soft shadows, made Important), soft -> hard shadows, shadow resolution High -> Medium, 4 -> 2 cascades, 2x MSAA off, anisotropic per texture, soft particles off; the same caps on the other levels; `Standard`'s LOD-150 SubShader first |
| Phasing Gun (6000.6, URP 17) | the URP asset: main light shadow atlas 4096 -> 2048 (four cascades of 1024), soft shadow quality High -> Low; SSAO downsampled, bilateral blur (3 passes) -> Kawase (1); the samples stay Medium (the only count compiled); motion blur, chromatic aberration and lens distortion off (all at intensity 0 already: no visible change); anisotropic per texture |
| The Third Eye (2022.2, URP 14) | High Fidelity's asset: 4x MSAA off, shadow atlases 4096 -> 2048 (nothing casts shadows there), soft shadow quality Medium -> Low; its SSAO downsampled, Kawase blur, samples High -> Low with normals rebuilt from depth (the Balanced asset's variant, compiled); QualitySettings 4x MSAA off |
| White Mirror (2021.2, URP 12, 2D) | 2x MSAA off (asset, QualitySettings). Its post effects stay: its code drives depth of field and chromatic aberration (a VolumeController), and Cinemachine's volume settings are in its scenes |
| Cell Machine (2019.3, URP 7) | chromatic aberration and lens distortion off (its two volume profiles) |
| Chapéu de Papel (2022.3, URP 14) | soft shadow quality Medium -> Low; QualitySettings shadow caps (URP ignores them). Its post effects stay: its code drives volumes |

Frame rates (fps over the measured seconds, the runs in the order they ran; the GPU at 696 MHz in
99 - 100 % of every run's seconds unless noted):

| game | where | as the PC build has it | handheld profile | memory (the game's resident set, MemAvailable at the end) |
|---|---|---|---|---|
| Arthur's Nightmare | the 3D room | 12.9, 12.9 | **42.4, 42.3 (+229 %)**; frames over 50 ms 448 / 432 -> 3 / 2 in 35 s | 258 / 261 -> 250 / 252 MB; 641 / 639 -> 650 / 647 MB |
| Phasing Gun | first scene | 11.8, 11.7 | **31.9, 31.9 (+171 %)** | 284 / 288 -> 238 / 236 MB (the 4096x4096 shadow atlas, 64 MB, is 16 MB at 2048); 620 / 616 -> 661 / 659 MB |
| The Third Eye | first scene | 34.2, 34.2 | **50.3, 50.8 (+48 %)** with film grain off too (the first build of the profile); the main thread is the limit then (UnityMain 94 % of a core, from 65 %), and the GPU governor runs the Mali at 420 - 504 MHz in 90 % of the seconds (696 MHz in every second before) | 264 / 267 -> 253 / 255 MB; 633 / 630 -> 640 / 632 MB |

Arthur's Nightmare item by item (one run each, the GPU at 696 MHz in 99 - 100 % of the seconds): the
Standard shader's LOD-150 SubShader alone 23.0 fps; everything but it 23.1; everything but the pixel
light cap 33.5 (the room keeps its dark look: the point lights stay per pixel); all of it 42.4. Its
traced frame (`UA_GL_TRACE_GPU=3`, serialized): 81.7 -> 26.2 ms of GPU work; the forward pass 65.2 ->
15.9 ms (132 -> 104 draws: no additive pass for the point lights), the spot light's shadow map 1024
-> 512 (2.8 -> 1.3 ms), the copy back into the camera's target 4.2 -> 2.0 ms (no 2x MSAA RGBA16F
target), and 2017.3's MSAA present path gone (7 -> 6 passes).

The pictures (static-frame shots of the same plan, judged by eye; these changes are visual by
design, never claimed identical):

- **Phasing Gun**: the same picture; no pixel of the first scene differs by more than 11/255 (the
  far shadows' edges a little coarser).
- **Arthur's Nightmare**: the title the same; the 3D room lit differently. The 27 point lights, lit
  per vertex, spread their light over whole wall polygons (Unity's vertex attenuation is softer than
  its per-pixel falloff): the back wall and the door show where the PC picture is dark; the
  flashlight (the spot light, per pixel), the outlines of the objects to collect and the HUD are as
  before. The biggest look change of the profile: `pixel_lights = null` keeps the per-pixel point
  lights (33.5 fps instead of 42.4, above).
- **The Third Eye**: the dark room the same but for its vignette, whose 8-bit steps showed as a hard
  ellipse once film grain was off in the first build; film grain is now kept by default (its
  frame rate with grain kept was not measured again).

Not measured (stopped to save device time): White Mirror, Cell Machine (already at the 60 fps
refresh rate with `UA_GL_MEMO`), Chapéu de Papel (its frame is UI overdraw: nothing of the profile
applies), the single items of the URP games, URP render scale against ua_host's display scaling,
and the run-time caps of quality.c on the handheld (unit-tested: `runtime/host/test/test_quality.c`).
None of the six games calls a QualitySettings setter (their assemblies reference none).

What did not go in: realtime reflection probes off (a mirror can show the player things), LOD bias
(moves the culled transition), shadow distance, Bokeh depth of field to Gaussian (another focus;
White Mirror's game code drives its depth of field), bloom quality (no game here had it on where
it mattered).
