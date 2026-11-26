# The lockstep checker: a speedup proven to leave the game play alone

Every speedup a port takes must leave the game exactly as it plays on the player it came with. The
pixel side has its checks ([profiling.md](profiling.md), "Pictures compared call by call"); this one
covers the game's state. A play session is recorded once (every input event and the frame it reached the
game in), replayed twice with the game's clock fixed (A: the baseline, B: with the change), the state of
every script and transform hashed after every frame, and the two runs compared frame by frame: the first
frame they part at, the classes that differ there, and from two dumps of that frame the object and field.

Nothing here is game-specific: it reads the scene through the game's own Mono (`Object.FindObjectsByType`
/ `FindObjectsOfType`, Mono's field tables). Mono games only; an IL2CPP game gets the fixed clock and the
input record but no state hash. Off unless `UA_LOCKSTEP` is set: no hook, no tap, nothing looked up.

## Use

1. **Record** a session (a person playing, or `UA_INPUT_SCRIPT` presses):

       UA_LOCKSTEP=record:rec.txt UA_LOCKSTEP_HASH=rec.hash UA_LOCKSTEP_FRAMES=6000 UA_VIRTUAL_PAD=1 ua_host <game>

   `UA_LOCKSTEP_FRAMES` ends the run (Unity asked to quit after that frame). The record holds each event
   the host sent (pads, keyboard, pointer, touch) with the frame after which it was given to the game.
2. **Replay A** and **replay B** (the same pads: `UA_VIRTUAL_PAD=1` as in the record; the pad's own
   events are dropped; the run ends at the record's last frame):

       UA_LOCKSTEP=replay:rec.txt UA_LOCKSTEP_HASH=a.hash ua_host <game>
       UA_LOCKSTEP=replay:rec.txt UA_LOCKSTEP_HASH=b.hash UA_NATIVE_BEH=1 ua_host <game>

   Reset the game's saves and preferences (its `data/`) before each run: a run that saved changes the next.
3. **Compare** (setup-rs/crates/lockstep; built and run in the `ua-rust` image, [rust-setup.md](rust-setup.md)):

       lockstep compare a.hash b.hash

   prints the frames compared and how many are the same, and at the first difference the parts that
   differ (Time.time, Random.state, the counts of active scripts and transforms, all transforms) and each
   class whose scripts' hash differs. Exit status 0: every frame the same.
4. **Name the field**: replay A and B again with `UA_LOCKSTEP_DUMP=<that frame>` and

       lockstep diff a.hash.<frame>.dump b.hash.<frame>.dump

   lists each object (`/<GameObject path with sibling indices>|<class>`) and field path that differs.

The first bar for any game is **A against A**: two replays of one record without a change must give 0
differences, or the game is not deterministic under the checker and nothing it says about a change counts.

## Settings

| Variable | What |
|---|---|
| `UA_LOCKSTEP=record:<file>\|replay:<file>\|hash` | record the input events, replay them, or neither (the clock and seeds are still fixed) |
| `UA_LOCKSTEP_HASH=<file>` | the per-frame hashes (format in runtime/host/lockstep.c); without it nothing is hashed |
| `UA_LOCKSTEP_FRAMES=<n>` | quit after frame `<n>` (a replay: the record's last frame) |
| `UA_LOCKSTEP_DUMP=<f>[,<f>...]` | every hashed value of frame `<f>`, by object and field: `<hash file>.<f>.dump` |
| `UA_LOCKSTEP_FPS=<n>` | the game's time step, 1/`<n>` s a frame (default 60) |
| `UA_LOCKSTEP_SEED=<n>` | `UnityEngine.Random.InitState` (default 1) |
| `UA_LOCKSTEP_DEPTH=<n>`, `UA_LOCKSTEP_ELEMS=<n>` | how deep plain objects are followed from a script (3), elements of an object array hashed (32) |
| `UA_LOCKSTEP_SETTLE=<ms>` | after each frame wait up to `<ms>` (5000; 0: never) for Unity's loading threads to sleep |
| `UA_LOCKSTEP_CLOCK=0` | leave the managed clocks alone (diagnosis) |
| `UA_LOCKSTEP_EVENT_CLOCK=1` | the input events' times from the frame clock (default: the real uptime) |
| `UA_LOCKSTEP_ICALLS=0` | read transforms and `enabled` through `mono_runtime_invoke` only, not Unity's internal calls directly |
| `UA_LOCKSTEP_IGNORE=<class>[.<field>][,...]` | fields left out of the hash; a class alone: its scripts and their transforms left out (what the checker cannot fix, such as an object living as long as its particles; a class by its short or full name) |
| `UA_LOCKSTEP_UNITY_CLOCK=0` | leave Unity's own monotonic clock alone |
| `UA_LOCKSTEP_AUDIO=0` | the silent sound output on the clock again, not paced by frames |
| `UA_LOCKSTEP_LOADS=0` | scene loads not held to the frame |
| `UA_LOCKSTEP_NUDGE=<class>.<field>@<frame>[+<delta>]` | a test aid that changes the game: `<delta>` added to that number field of every script of the class after `<frame>` |

## What makes a replay the same

- **The clock.** Once Mono is up, `Time.captureDeltaTime = 1/fps` (`captureFramerate` before 2019.2), and
  again whenever a frame finds it otherwise (Hollow Knight's 6000.0 player had it back at 0 after its first
  scene load): `Time.time`, `deltaTime` and the number of `FixedUpdate`s a frame no longer follow the real time.
  Unity's own monotonic clock on UnityMain (and on Android's UI thread until the first frame, where Unity
  takes its time base) answers from the frame count through the loader's clock hook (`so_set_clock_hook`,
  runtime/loader/fastclock.c): Unity's frame pacing and the milliseconds a load's main-thread work gets a
  frame (`Application.backgroundLoadingPriority`, the async upload's time slice) are the same every run.
  `Time.realtimeSinceStartup(AsDouble)` (wrapped where Unity registers it), `Environment.TickCount(64)`,
  `Stopwatch.GetTimestamp` and `DateTime.UtcNow/Now` (registered over Mono's own internal calls when Unity
  registers its first, so also on the Mono route's bridge) read the same virtual clock: frames done times
  the step: the same for a frame's first 2000 reads on a thread (work timed in slices does not depend on
  how often it looked), then 1 us more for each read, so a loop that waits for the clock still ends. `new System.Random()` seeds from `TickCount`, so from the frame. The date is the record's start.
  Unity's own `Time.unscaledTime` still counts from its real start and its first frames' real lengths
  (whatever clock it reads them from is not the one hooked): the managed getters (`unscaledTime`,
  `unscaledTimeAsDouble`, `unscaledDeltaTime`) are wrapped to answer from the frame count; the native value
  is only logged (`ut=`, with `deltaTime` `dt=` and `captureDeltaTime` `cdt=`), and what the engine itself
  times by it (an Animator on unscaled time) is not fixed.
- **Random.** `UnityEngine.Random.InitState(seed)` in the same frame; `Random.state` is in every frame's line.
- **Input.** The host's input events go through the runtime's input tap (`up_set_input_tap`,
  runtime/jni/unityplayer.c): held, and given to Unity on UnityMain after a frame, so each reaches the
  game in the next frame, the frame written beside it. The events' own times (`getEventTime`) stay the
  real uptime: Hollow Knight (6000.0) took no press stamped with the frame clock, far from the uptime
  (`UA_LOCKSTEP_EVENT_CLOCK=1` stamps them so anyway). A game that reads input event times (the Input
  System's `InputAction` times) can therefore differ between runs.
- **Sound.** The silent output test runs use (`ANDROID_SLES_OUTPUT=none`) no longer takes the players' buffers
  on the clock: after each frame the checker takes that frame's time of sound (rate / fps) from each player on
  UnityMain, each write after the game's mixer queued the next, and waits until the mixer has refilled its
  queue (`android_sles_pace`, runtime/android/opensles.c). The game's sound clock (`AudioSource.time`,
  `isPlaying`, a clip ending) then follows the frames (`UA_LOCKSTEP_AUDIO=0`: on the clock again). Only with
  the silent output: with a sound device the clock is the card's.
- **Loads.** Unity reads a scene on its loading threads (`Loading.PreloadManager`, `Loading.AsyncRead`)
  while frames go on; how many frames that takes depends on the card. The internal call behind
  `SceneManager.LoadScene(Async)` is wrapped (64-bit players) to note the `AsyncOperation` it returns, and after
  each frame UnityMain waits until every noted load has read its scene (progress 0.9 or done; `L` lines: how
  long; 10 s gives up, logged; `UA_LOCKSTEP_LOADS=0`: off). After every frame the checker also waits until the
  loading threads sleep (`W` lines). Waiting for the threads alone was not enough: Hollow Knight's save load
  once finished a frame later.

## What it sees, and what it does not

Hashed each frame: every active script (`MonoBehaviour`): its class, `enabled`, its transform's local
position, rotation and scale (raw float bits), and its instance fields (numbers by their bits, strings by
their characters, structs field by field, arrays element by element, engine objects referenced by class and
alive or destroyed, other objects followed to `UA_LOCKSTEP_DEPTH`); every active transform; `Time.time`;
`Random.state`; the active scene and how many are loaded.

Not seen (a change there shows only when it reaches the above):
- inactive GameObjects and their scripts; static fields; physics bodies' velocities and other native-only
  state (Rigidbody2D, Animator, particle systems), except through the transforms and scripts they move;
- which engine object a reference points at (only its class and whether it is alive), `IntPtr` fields,
  `UnityEngine.Object`'s own fields (native pointers, instance ids), the port's own patches' bookkeeping
  (fields named `<asmpatch>...`: PlayMaker's `loopCountsReset` counts writes across every FSM, the left-out
  objects' too), `System.Guid` fields (`Guid.NewGuid` is
  random each run: InControl names its devices so), delegates, reflection objects, and the hash-derived fields
  of `System.Collections` and their nested types (`_buckets`, `Entry.hashCode`, `next`: object hash codes
  follow addresses and instance ids, a `Dictionary` keyed by GameObjects has others each run; the entries'
  order and keys and values are hashed);
- elements past `UA_LOCKSTEP_ELEMS` of an object array, bytes past 64 KB of a number array, objects
  past 512 per script.

What can still part two runs of one record (and would fail A against A):
- a load whose main-thread part is cut by Unity's time budget (`Application.backgroundLoadingPriority`,
  `QualitySettings.asyncUploadTimeSlice`: real milliseconds); the compare names it ("a load finished in
  another frame") when the active scene changes at another frame;
- the sound with a real output (the card's clock), a video's clock, or network replies;
- particle systems: their simulation is Unity's own (worker jobs, an automatic random seed the checker
  does not reach), so an object that waits for its particles to die (Hollow Knight's
  `ParticleSystemAutoDestroy`) can go a frame or three earlier or later: `UA_LOCKSTEP_IGNORE=<class>` leaves a
  whole class's scripts and their transforms out;
- other threads of the game's own (results joined in by real time), finalizers, `Object.GetHashCode` of
  managed objects (address-based, so a `Dictionary` keyed by objects iterates in another order);
- `Guid.NewGuid` and `System.Security.Cryptography` random numbers.

## Cost

The hashing and the waits run only in check runs; with `UA_LOCKSTEP` unset ua_host does nothing more (no hook,
no tap, the clock hook unset). On the RG34XX-SP (H700, 64-bit Hollow Knight, 6000.0 Mono route): 5-7 ms a frame
at the title (about 250 scripts), 43 ms a frame in King's Pass (about 1500 scripts, 4000 transforms, 5800 plain
objects followed at depth 2; depth 3 cost 53 ms): a 5000-frame replay takes about 5.5 minutes. The waits for the
loading threads came to 6-10 s a run. A hash file is about 8 MB for 5000 frames; a dump of a King's Pass frame
9 MB.

## Results (Hollow Knight, 64-bit, muOS RG34XX-SP, 2026-11-26)

A 5000-frame record (title, menu, the save continued, about 63 s of King's Pass: jumps every 2.5 s, attacks,
walks right and left; `UA_INPUT_SCRIPT` on `UA_INPUT_CLOCK=frames:60`), replayed again and again while the
causes were found one by one with the checker's own reports:

| what parted two replays | first frame | fixed by |
|---|---|---|
| InControl's device `Guid` (`Guid.NewGuid`) | 2 | `System.Guid` fields left out |
| a logo's scale following `Time.time`; `Time.captureDeltaTime` back at 0 after the first scene load | 43 | the capture step kept; Unity's clock on UnityMain from the frame count |
| `ObjectPool`'s `Dictionary` entries' `hashCode` (instance ids) | 778 | hash-derived fields of nested collection types left out |
| the Input System device's update time | 904 | left out (it follows the events' times) |
| `CheckpointSprite.frameTimer` (`unscaledDeltaTime`), `Time.unscaledTime` from the real start | 788 | the unscaled getters wrapped; the clocks' zero before the libraries load |
| `AudioLoopMaster` (music sync on `AudioSource.time`) | 879 | the silent sound output paced by frames |
| `ParticleSystemAutoDestroy` (an effect destroyed when its particles die, 1-3 frames apart) | 1289 | not fixed: `UA_LOCKSTEP_IGNORE` |
| `PlayAudioAndRecycle` (a sound object back to its pool when its clip ends, 8-13 frames apart) | 2917-3323 | not fixed (FMOD's own mixer timing): `UA_LOCKSTEP_IGNORE` |
| `ParticleSystemAutoRecycle` (the same as `ParticleSystemAutoDestroy`, back to a pool) | 3987 | not fixed |

The last set (`UA_LOCKSTEP_IGNORE=ParticleSystemAutoDestroy,PlayAudioAndRecycle,ObjectPool`, sound paced):

| runs | frames the same | first difference, as the checker names it |
|---|---|---|
| A against A (A1, A2) | 1212 of 5000 | 1213: the save's load finished a frame later in A1 (1216 against 1215), `PlayMakerFSM` |
| A2 against C (C: run 2 with `UA_LOCKSTEP_NUDGE=HeroController.RUN_SPEED@2500+1`) | 2500 of 5000 | 2501, class `HeroController`; `lockstep diff` of the frame's dumps: `/Knight\|HeroController  RUN_SPEED: A 8.30000019 B 9.30000019`, the only line |
| A2 against B (`UA_NATIVE_BEH=1`, exact by its own verify mode) | 4985 of 5000 | 3987, `ParticleSystemAutoRecycle` (an effect back to its pool when its particles die: the particle case again, not the behaviour loop) |

So the checker finds a one-field change in the frame it is made and names the object and field; an exact
change agrees with the baseline for 3986 frames and differs only where two baselines also can.

Best A against A so far: 4992 of 5000 frames the same with `UA_LOCKSTEP_IGNORE=AudioLoopMaster,ParticleSystemAutoDestroy`
(the 8 that differ: one `PlayAudioAndRecycle` returned to the pool 8 frames apart), 4987 of 5000 with the sound
paced and only `ParticleSystemAutoDestroy` ignored (the same object, 13 frames). Every load landed in the same
frame in those runs; one later pair had the save's load finish a frame apart (1215 against 1216), so the
loading-thread wait was not a guarantee (now loads are held: below).

With the loads held (`L` lines; 2026-11-27), same record, `UA_LOCKSTEP_IGNORE=ParticleSystemAutoDestroy,PlayAudioAndRecycle,ObjectPool`:

| pair | frames the same | what still differed |
|---|---|---|
| x1, x2 | 4992 of 5000 | 3731: PlayMaker's `<asmpatch>loopCountsReset` (the patch's write count, which counts the left-out objects' FSMs too): now left out |
| v1, v2 | 4991 of 5000 | 3989-3997: a `ParticleSystemAutoRecycle` (the particle case) |
| t1, t2 (also `ParticleSystemAutoRecycle` ignored) | 896 of 5000 | 897: `AudioLoopMaster` (the menu's music sync) a frame apart, so the save's load started a frame later |

All three loads landed in the same frames whenever they started in the same frame (held 1.2-2.7 s each, none
gave up). A vs A with 0 differing frames is still not reached: the sound output paced by frames does not make
the mixer's progress exact (FMOD's mixer runs ahead by its own buffer), and the menu's music sync then moves the
game a frame now and then.

Open: the sound mixer's progress (FMOD's own thread; exact would be FMOD's output in non-realtime mode, mixed
on the frame); sound objects recycled on a clip's end; particle systems (their seed and simulation are Unity's
own).
