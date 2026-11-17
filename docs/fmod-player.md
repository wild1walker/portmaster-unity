# ua_host's FMOD player: the data format (format 1)

A desktop build of a Unity game made with FMOD Studio ships FMOD only for x86 / x86-64, and FMOD's
ARM libraries need an FMOD account. So a port of an FMOD game can play its sound with a player of
its own: the setup reads the game's banks once (`pmu fmod`, setup-rs's crate `fmod`) and writes them
in the format below; on the handheld ua_host's built-in FMOD ([plugins.md](plugins.md#fmod),
runtime/host/fmodstub.c) answers the integration's calls and its player (runtime/host/fmplay.c)
plays that format, selected by `UA_FMOD_DATA=<dir>`. Nothing of FMOD's code, headers or data is in
the tools, the port or this repository: the Vorbis setup headers the samples need are read at setup
from the player's own copy of the game's FMOD library.

Bank format 0x67 (FMOD Studio 1.10; Blasphemous, FMOD 1.10.13). Step 1 plays: event timelines
(asynchronous and time-locked instruments, loop and transition markers and regions with their
parameter conditions, chance and bar / beat quantization), markers and tempo for the game's
TIMELINE_MARKER and TIMELINE_BEAT callbacks, parameter sheets and parameter conditions on timeline
instruments, single, multi (sequential, random, shuffle, weights) and programmer instruments,
volume, pitch, loop count, chance, trigger delay, random volume / pitch / start modulators, AHDSR
envelopes (event master and instruments; a stop with fade-out plays their release), tracks and
master volumes, automation (controllers) of track / master / instrument volume, pitch and fade and of
the panner's position, by the timeline or a parameter with its seek speed, the mixer's group buses
(volumes, the game's bus volume, pause, mute, stop), event polyphony, the sound tables of programmer
sounds (voice-over). Not in step 1: effects other than the panner (reverb sends and returns, EQs,
compressors, echo), snapshots, 3D, event instruments, scatterers, sustain points, transition
timelines (their fades), VCAs. Readers ignore keys they do not know.

## Files

`pmu fmod <banks> <out> [--check] [--fmod-lib PATH]` (`<banks>`: the StreamingAssets folder, or
one `.bank`) writes, per bank:

- `<out>/<Bank>.uafm.json`: the bank's objects, resolved (below).
- `<out>/<Bank>.uafm.pak`: its samples, standard Ogg Vorbis streams one after another, each at an
  offset that is a multiple of 16; no header (the offsets are in the json).

`<Bank>` is the bank file's name without `.bank` (`Master Bank`, `Master Bank.strings`). The game
loads a bank by its file path; the player takes the name and finds `<dir>/<name>.uafm.json`.

## The pak: samples

Every sample of an FMOD 1.10 bank is FMOD's Vorbis (FSB5 codec 15): its audio packets, each after a
u16 size. FMOD keeps the three header packets out of the bank: the identification header is FMOD's
(the sample's rate and channels, block sizes 256 / 2048), and the setup header (the codebooks,
floors, residues, mappings, modes) is one of a set in FMOD's library, named by a 32-bit id in the
sample's FSB5 header. `pmu fmod` rebuilds each sample as a standard Ogg Vorbis stream: an
identification header with those values, a comment header, the setup header, the audio packets on
pages of up to 4 KB with their granule positions (counted from each packet's block size, without
decoding), the last packet alone on the last page with the sample count as its granule (so a decoder
trims FMOD's padding: short samples carry up to a few hundred samples past their count). Any Vorbis
decoder plays it (stb_vorbis on the handheld, lewton in `--check`).

### Vorbis setup headers: from the game's own FMOD

`pmu fmod` reads the setup headers from the copy of the game the player has: its FMOD library
(`--fmod-lib <libfmod.so or a folder>`, by default `libfmod.so` in the `Plugins` folder nearest above
the banks). In the library each setup header is a block of read-only data starting `\x05vorbis`; a
table pairs each header's id (u32) with a pointer to it (the next pointer-sized word after the id,
a virtual address mapped to the file through the ELF's load segments). The header's length is the
shortest that reads as a whole setup header for the sample's channel count (lewton's reader).
Blasphemous (Linux build, `Plugins/x86_64/libfmod.so`): 33 headers in the library, its banks use two
(id 0x84d3ac87, mono, 3189 bytes; 0x38aa59ce, stereo, 3832 bytes). Without the library, or without
a header the samples name, those samples are left out (a warning; they stay silent).

## The json

```
{
  "format": 1,
  "bank": {"name": "Master Bank", "id": "<guid>", "version": 103},
  "paths":  {"event:/SFX/UI/ChangeTab": "<guid>", "bus:/MUSIC": "<guid>", ...},   strings bank only
  "media":  [MEDIA, ...],                    by the sample's index in the bank's FSB5
  "buses":  {"<guid>": BUS, ...},            the mixer's buses (not those inside events)
  "events": {"<guid>": EVENT, ...},
  "sounds": {"<key hash>": index, ...}       a sound table (programmer sounds), see below
}
```

GUIDs are FMOD's 16 bytes as the bank stores them (FMOD's GUID struct: Data1, Data2, Data3
little-endian, then Data4; the same bytes as a .NET `Guid` in memory), as 32 lower-case hex
digits. Volumes are in dB, pitch in semitones, timeline positions in timeline units (samples at
48 kHz), times in seconds. A key with its default value is left out.

MEDIA: `{"off": 0, "len": 12345, "ch": 1, "rate": 48000, "samples": 236544, "name": "..."}`; a
sample left out has no `off` / `len`.

BUS: `{"kind": "group" | "master" | "return", "out": "<guid>", "vol": 0.0}`: `out` the bus it
routes to (none: the master). `bus:/` is the master; the game's `Bus.setVolume` multiplies.

### EVENT

```
{"bus": "<guid>",            the mixer bus the event's output goes to
 "in_vol": 0, "vol": 0, "pitch": 0,   its input bus's and master track's volume, master pitch
 "max": 0, "steal": 0,       polyphony (0 unlimited); steal 4: a new instance fails at the limit
 "snapshot": false,
 "len": 0,                   the timeline's length (the end of its last box)
 "params": [{"name", "id", "min", "max", "def": 0, "up": 0, "down": 0, "vel": 0}, ...],
                             up / down: the seek speed (units a second, 0: at once)
 "tracks": [{"vol": 0}, ...],
 "insts": [INST, ...],       every instrument, a multi instrument's entries included
 "async": [[inst, start, length], ...],    asynchronous boxes (timeline list 1)
 "locked": [[inst, start, length], ...],   time-locked boxes (list 2)
 "sheets": [[param, [[inst, start, length], ...]], ...],   instruments on a parameter's axis
 "markers": [[pos, "name"], ...],          sorted; unnamed ones are jump destinations
 "tempo": [[pos, bpm, upper, lower], ...],
 "jumps": [{"at": pos, "end": pos, "to": pos, "if": [[param, min, max], ...], "chance": 100,
            "quant": [unit, n]}, ...],
 "ctrl": [CTRL, ...],
 "ahdsr": [initial, peak, sustain, attack, hold, decay, release]}   on the master's volume
```

The cursor moves at 48,000 units a second from 0. An asynchronous box's instrument starts when the
cursor enters the box (at its exact frame), plays its whole sound (its loops), and when the cursor
leaves the box its current pass is its last. A time-locked box's instrument plays from where the
cursor is in the box (a seek), and is cut at the box's end. A jump with `end` = `at` is a transition
marker (taken when the cursor crosses it); otherwise a region (taken while the cursor is in it, at
the next grid point when `quant` is set: unit 1 bars, 2 beats, every n), when all of its `if`
parameters are within [min, max] and (markers) its chance draws. After a jump time-locked
instruments restart from the new position; asynchronous ones play on while their box holds the new
position. The cursor stops at `len` (an event without jumps), and the event ends there once nothing
plays; an event with sheets, an instrument that loops forever or an unconditional jump back plays
until stopped. Markers and beats (from the tempo markers; bar numbers from 1) reach the game as its
callbacks.

A sheet's box triggers its instrument while the parameter is in [start, start + length) (or at the
parameter's maximum, for a box ending there). An instrument that is on the timeline and also has a
box on a sheet (format 0x67) takes that box as a condition (`"if"` on the instrument).

INST:

```
{"k": "wave" | "multi" | "prog" | "event" | "scatter" | ..., "m": sample index,
 "kids": [inst, ...], "w": [weight, ...], "mode": 0, "sel": 1,   a multi instrument's playlist:
     mode 0 sequential, 1 random, 2 shuffle, 3 global sequential, 4 instance sequential
 "name": "...",              a programmer instrument's name
 "vol": 0, "pitch": 0, "loop": 0,   loop: -1 forever, 0 once, n: n + 1 passes
 "chance": 100, "delay": [min, max], "seek": 0, "seek_pct": 0, "max": 0, "flags": 0,
 "track": 0,                 the event track it plays into (none: the master)
 "rvol": 0, "rpitch": 0, "rstart": 0,   random modulators: volume (down to -rvol dB),
     pitch (+-rpitch / 2 semitones), start offset (up to rstart % of the sample)
 "ahdsr": [...],             on the instrument's volume
 "if": [[param, min, max], ...]}   conditions (above)
```

A voice's gain is the sum in dB of the instrument (and the multi instrument above it), its track,
the master, the input bus, the random draws, the envelopes and the controllers, times the instance's
and the buses' game volumes; its pitch likewise.

CTRL: `{"on": "master" | "track" | "inst" | "pan", "i": index, "prop": "vol" | "pitch" | "fade" |
"pos", "by": -1 | param, "pts": [[x, y, shape], ...]}`: the value of the timeline cursor (`by` -1, x
in timeline units) or of the parameter `by` gives y on the curve (clamped to the first / last point;
between points by the left point's shape, 0 a straight line). `vol` adds dB, `pitch` semitones,
`fade` multiplies (0..1: an instrument's fade in / out), `pos` is the panner's 2D stereo position
(-100 left .. 100 right) of a track (`i`) or the master (-1): a mono voice is panned at constant
power, a stereo one balanced.

### Sound tables (programmer sounds)

A bank of voice-over keeps its lines in a sound table: `sounds` maps a key's 64-bit hash (16 hex
digits) to a sample of the bank. The hash is Bob Jenkins' lookup3 `hashlittle2` of the key's UTF-8
bytes with both seeds 0, (b << 32) | c. The game's programmer-sound callback asks the Studio system
for a key's sound (`getSoundInfo`), creates it, reads its length (Blasphemous times its dialog lines
by it) and hands it to the instrument; the built-in FMOD answers each step from the table.

## The bank format (0x67), as read

A bank is RIFF `FEV `: `FMT ` (u32 version 0x67, u32 compatible version), `LIST PROJ` with the
objects, `SND ` with the FSB5 sample data (at the offset its `SNDH` header gives). Counts are X16:
a u16, or (top bit set) 15 bits plus the next u16's 15. A list is X16 (count * 2 + 1), then (if not
empty) a u16 element size and the elements; some lists give each element its own u16 size. Each
object is a `LIST` whose first chunk is its body (its GUID first); chunks after it belong to it (an
instrument's `INST`, a bus's `BUS `, a multi instrument's `PLST`, a timeline's `CTRO` and its
transition regions `TRNB`). Layouts of the bodies at 0x67 (the field order follows FModBankParser,
github.com/Masusder/FModBankParser, Apache-2.0, read as documentation; at 0x67 every chunk the
converter reads parses to exactly its size, `--check`):

| chunk | fields |
|---|---|
| EVTB event | guid, snapshot, timeline, input bus, master bus, i32 max instances, i32 priority, u8, i32 scheduling, list of layouts, user properties (float, string), f32 doppler, i32 steal mode, f32 cooldown, u32 flags |
| TLNB timeline | guid, guid (its event), list async boxes (guid, u32 start, u32 length), list time-locked boxes, list u32, list markers (each sized: guid, u32 position, string name), list tempo (guid, u64 signature: upper, lower, u32 position, f32 bpm) |
| TRNB transition | guid, destination marker, u32 start, u32 end, list conditions (parameter guid, f32 min, f32 max), quantization (u32 unit, i32 multiplier if unit <= 2), f32 chance, u32 flags, u8, u8 |
| PMLB sheet | guid, parameter, event, list boxes (instrument, f32 start, f32 length) |
| PRMB parameter | guid, u8, u32 type, string name, f32 min, max, default, velocity, seek speed, u8, f32 seek speed down, u8 |
| WAIB / MUIB / PRIB / EVIB | guid, then: waveform resource / - / string name / event |
| INST | timeline guid, f32 volume, f32 pitch, i32 loop count, u8 flags, f32 3D offset, f32 chance, f32 delay min, max, quantization, guid control parameter, f32 auto-pitch reference, f32 seek, i32 max polyphony, routable (u16 size, guid output bus, u32 layout, u32 mask), i32 steal, u32 left trim, f32 seek %, f32 auto-pitch minimum |
| PLST | i32 play mode, i32 selection mode, list entries (instrument, f32 weight), u8 |
| WAV  | guid, u16 size, i32 sound bank, i32 subsound, u32 loading mode |
| IBSB / GBSB / MBSB / RBSB bus | guid, routable; then its `BUS ` body: u8 flags, u32 layout, list pre-fader effects, list post-fader effects, mixer strip (u16 size, f32 volume, f32 pitch), ... |
| BEFB effect | guid, u32 DSP type (27 panner, 28 three-band EQ, 18 compressor, ...), u8 bypass |
| CTRL controller | guid, owner (the object), curve, i32 property (0 volume, 1 pitch, 4 fade, 1000 + n an effect's parameter n) |
| CURV curve | guid, owner (its controller: a controller's points are every curve it owns), list points (f32 x, f32 y, f32 shape, u32 type) |
| CTRO | list of controllers: those a timeline (x: the position, stored as a u32 in the float's bits) or a sheet's parameter drives |
| MODB modulator | u16 size, guid, owner, i32 property, i32 type (0 AHDSR: 10 floats, 1 random: f32 amount), i32 property type |
| STBL sound table | u32, i32 sound bank, X16 count, u64 key hashes (sorted), X16 count, u24 sample indices |
| STDT strings | u32 version 1; nodes: X16 (n * 2 + 1), u16 8, each u24 label offset (0xffffff none), u8 first character, u24 first child (a leaf: its GUID's index), u8 child count; GUIDs: X16 (n * 2 + 1), u16 16, the GUIDs; u16 text size, NUL-terminated labels. A path is the labels from the root to a leaf. |

FSB5 (public layout): "FSB5", version, sample count, sample-header size, name-table size, data
size, codec; 60 bytes of header (version 1); per sample a u64 (bit 0: chunks follow, bits 1-4 rate
index, bits 5-6 channels 1 / 2 / 6 / 8, bits 7-33 data offset / 32, bits 34-63 samples) and its
chunks (u32: bit 0 more, bits 1-24 size, bits 25-31 type; 11: Vorbis, the setup header's id);
samples are not stored in the order of their data.

## Checking it

`pmu fmod --check` decodes every stream it writes (lewton) and compares the decoded length with the
sample count, and checks that every event's instruments name a sample of the bank, a track and a
parameter it has; it exits 1 on a problem. On Blasphemous (Linux Steam build, 9 banks, 2026-11-16):
3,113 samples, all decoded to exactly their counts; 995 events (882 in Master Bank); 1030 paths;
`pmu fmod` took 56 s with the check (18 s without) and 321 MB at most (arm64 container on an
M-series Mac); the paks are 414 MB (the banks' sample data 401 MB).

`fmrender` (built with ua_host: `host/out/fmrender`) plays the data without a game or a sound device
into a WAV file: `fmrender -o out.wav <data dir> <script>`, lines `<seconds> bank <name>`, `play
<event path> [as <name>]`, `param <name> <parameter> <value>`, `stop <name> [now]`, `busvol <bus>
<value>`, `key <sound table key>` (for programmer instruments), `end`; it prints the callbacks
(markers, beats, stops, programmer sounds) as they come. `fmrender --bench [-s secs] [-e events/s]
[-m music event] <data dir> <events.txt>` plays random one-shot events over a music event and
prints the player's CPU per second of audio.

On Blasphemous: `event:/Boss Music/OssuaryMusic` (Isidora's fight) gives the bars and beats of its
90 bpm 4/4 tempo marker (a bar every 2.667 s, as the game's IsidoraAudio assumes) and its "Attack"
markers; `BossPhase` 2 takes the region jump to the "Bridge" loop, 3 the quantized jump (every 2
bars) to "Phase3"; `IsidoraVoice` 1 lets the voice layers (instruments with that condition) play. A
voice-over key (`CTS04-Dagger_Lady_0`) finds its line (4,928 ms) through the hash table.
