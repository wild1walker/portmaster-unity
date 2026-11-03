# ua_host's Wwise player: the data format (format 1)

There is no Wwise for ARM Linux, and running a game's own x86 Wwise under box64 costs most of a
handheld core. So a port of a Wwise game plays its sound with a player of its own: the setup reads
the game's soundbanks once (`pmu wwise`, setup-rs's crate `wwise`) and writes them in the format
below; on the handheld a backend of ua_host's built-in Wwise ([plugins.md](plugins.md#wwise),
`UA_WWISE_BACKEND`) reads that format and plays it. Nothing of Wwise's own code is used.

Step 1 (this format): events and their actions, sounds, random / sequence / switch / layer
containers, actor-mixers, the music hierarchy (switch, playlist, segment, track), buses with their
volumes, RTPC curves on volume and pitch, states that change volume or pitch, the game syncs'
defaults, and the media (Wwise Vorbis as standard Ogg Vorbis, IMA ADPCM as it is). Not in it:
effects (insert and aux), HDR, 3D positioning and attenuation, LPF / HPF, beat-synced music
transitions (a music change happens at the end of the segment playing, or at once: see "Music"),
clip automation, ducking curves. Readers ignore keys they do not know; a later format adds keys
(and bumps `format` only for a change an old reader would misread).

## Files

`pmu wwise <banks> <out>` (`<banks>`: a folder of `.bnk` files, or one) writes, per bank:

- `<out>/<Bank>.uaww.json`: the bank's objects (below).
- `<out>/<Bank>.uaww.pak`: its media, one after another, each at an offset that is a multiple of 16;
  no header (the offsets are in the json).

`<Bank>` is the bank file's name without `.bnk` (`Init`, `SFX`). On the handheld the files are in
the port's `wwise/` folder on the card (outside the APK and OBB, so the pak can be mapped); the
runtime finds them through `UA_WWISE_DATA` (ua_host has no default: the port's launcher sets it to
`<port dir>/wwise` when that folder is there, and so does the rehearsal's rig, `/zip/wwise`). The game's `LoadBank`
names a bank as `SFX.bnk` or `SFX` (or by its ID: the json's `bank.id`). Wwise's `Init` bank comes
first: it has the buses and the game syncs' defaults that the other banks use.

## The pak: media

- `vorbis`: a standard Ogg Vorbis stream (one logical stream, the three header packets, then the
  audio packets on pages of up to 4 KB with their granule positions; the last page's granule is the
  WEM's sample count, so a decoder trims the end to it), rebuilt from Wwise's packets without
  decoding (ww2ogg's method, with the codebooks of the game's own Wwise: below). Any Vorbis
  decoder plays it (stb_vorbis, libvorbis, tremor).
- `adpcm`: Wwise's IMA ADPCM, the WEM's `data` chunk as it is. Blocks of `block_align` bytes
  (36 x channels), each holding the channels one after another, 36 bytes a channel: s16 sample
  (little-endian), u8 step index (clamp to 0..88), u8 unused, 32 bytes of 4-bit codes, low nibble
  first. A channel's block is 64 samples: the header's sample, then 63 codes (the last code is
  unused). Standard IMA expansion. A last, short block holds fewer samples (`samples` says how many).
- `pcm`: little-endian signed 16-bit, interleaved.

### Vorbis codebooks: from the game's own Wwise

A Wwise Vorbis WEM leaves the codebooks out of its setup header and names them by index in a
library its engine carries. Nothing of Wwise's is in this repository or a port's release:
`pmu wwise` reads the library from the copy of the game the player has, from its Wwise engine
(`Plugins/x86_64/libAkSoundEngine.so`, `Plugins/x86/...`; `AkSoundEngine.dll` of a Windows build,
`AkSoundEngine.bundle` of a macOS one): `--wwise-lib <the library or a folder>`, by default the
`Plugins` folder nearest above the banks (the setup passes the game's).

In the engine the codebooks are one block in its read-only data, back to back in Wwise's packed
form (the form ww2ogg's codebook files have), with an array of pointers to them elsewhere (in
Wwise 2017.2.5's Linux libraries: 598 codebooks, 71,991 bytes, at file offset 0x1f8a80 of the x86
build and 0x1f9780 of the x86-64 one, in `.rodata`). `pmu wwise` finds the block by its
structure in any binary: each codebook must read as a Vorbis codebook (dimensions and entries set,
codeword lengths that fill a Huffman tree exactly, a value lookup that fits) and take exactly
`bits / 8 + 1` bytes, the next one starting there; a run of at least 64 is a library (the codeword lengths' tree is what tells
codebooks from other data: those libraries have no other run of 64, and none of 20 starts in the
4 KB before the block). With more than one library, a media takes the first
whose stream decodes. The log names each library found, with its SHA-256 in ww2ogg's layout
(the codebooks, their u32 offsets, the table's offset): Wwise 2017.2.5's is
`00a93eab267d281401b1efd54e888a2e183299b9e6c446c48d09f701a89d9d27`, the digest of ww2ogg's
`packed_codebooks_aoTuV_603.bin` (aoTuV 6.03's codebooks; ww2ogg's other set, the older one, is
not in that engine). Without a library, or one without codebooks, the Vorbis media are left out
(one warning a bank: those sounds stay silent); ADPCM and PCM still convert. `port check` says
whether it found the library and its codebooks.

## The json

```
{
  "format": 1,
  "bank": {"name": "SFX", "id": "393239870", "bank_version": 128, "wwise": "2017.2"},
  "media":   {"<media id>": MEDIA, ...},
  "nodes":   {"<node id>": NODE, ...},
  "events":  {"<event id>": [ACTION, ...], ...},
  "buses":   {"<bus id>": BUS, ...},
  "rtpc":    {"<rtpc id>": [CURVE, ...], ...},
  "defaults": {"rtpc": {"<rtpc id>": value}, "states": {"<group id>": "<state id>"},
               "switches": {"<group id>": "<switch id>"},
               "state_groups": {"<group id>": {"fade_ms": 0, "transitions": [["<from>", "<to>", ms], ...]}},
               "switch_rtpc": {"<switch group id>": {"rtpc": "<rtpc id>", "curve": [[x, "<switch id>"], ...]}}}
}
```

Ids are Wwise's 32-bit ids as decimal strings (an event's id is the FNV-1 hash of its lower-case
name, as `GetIDFromString` gives it). Volumes are in dB, pitch in cents, times in milliseconds. A
key with its default value is left out: the defaults are given below. `wwise` is the release the
bank format names (bank version 128 = 2017.2).

### MEDIA

```
{"off": 0, "len": 12345, "codec": "vorbis" | "adpcm" | "pcm", "rate": 44100, "channels": 2,
 "samples": 123456, "block_align": 72, "loop": [start, end]}
```

`off` / `len`: the bytes in the pak. `samples`: per channel. `block_align`: adpcm only. `loop`: the
WEM's loop points in samples, `end` exclusive (a looping sound loops there instead of the whole
media), absent when the WEM has none.

### NODE

Every node has `type` and `parent` (the node above it in Wwise's hierarchy, or absent at a root),
and may have:

| key | default | meaning |
|---|---|---|
| `vol` | 0 | dB: the node's Volume, Make-Up Gain and Output Bus Volume summed |
| `pitch` | 0 | cents |
| `rand_vol`, `rand_pitch` | none | `[lo, hi]` added to `vol` / `pitch`, a new random value each play |
| `bus` | none | the output bus id, set where the node overrides its parent's (else the parent's applies) |
| `states` | none | `{"<state group>": {"<state>": {"vol": dB, "pitch": cents}}}`: added while that state is set |
| `limit` | 0 | at most that many instances of this node play (0: no limit) |
| `limit_newest` | false | at the limit a new instance does not play (else the oldest one stops) |
| `pan` | none | `[left/right, front/rear]` 2D panning, -100..100 |
| `prob` | 100 | the chance in percent that the node plays when it is picked |

The gain of a playing sound is the sum of `vol` over the sound and every node above it (its `parent`
chain, to the root: containers, actor-mixers, music nodes), its states and RTPCs, plus its output
bus's chain (below); the same for `pitch`. Its output bus is the first `bus` met going up the chain.

Types and their own keys:

```
sound:    {"media": "<media id>", "loop": 1, "delay_ms": 0}
          loop: 1 plays once, 0 loops forever, n plays n times. The media is in this bank's pak, or
          (rarely) another loaded bank's. A source plugin (a tone, silence) has "plugin": its
          Wwise plugin id, and no media: step 1 plays nothing for it
random, sequence:
          {"children": ["<id>", ...],            the playlist, in its order
           "weights": [w, ...],                  random only, when the container uses weights (0..100)
           "avoid_repeat": 0, "shuffle": false,  random: the last n are not picked again; shuffle mode
           "continuous": false,                  false: one child each play (step); true: all of them
           "loop": 1,                            continuous: the playlist n times (0: forever)
           "transition": "none" | "xfade_amp" | "xfade_power" | "delay" | "sample" | "trigger",
           "transition_ms": 0,                   continuous: between children
           "reset": false, "global": false}      step: restart the playlist at each play; scope
switch:   {"group": "<switch or state group>", "state": false,   true: a state group
           "map": {"<value id>": ["<child id>", ...]}, "default": "<value id>",
           "continuous": false}                  true: a change of the value while playing switches
layer:    {"children": ["<id>", ...]}            all children together
mixer:    {"children": ["<id>", ...]}            an actor-mixer: volume, pitch, bus for what is below
music_switch:
          {"children": [...], "args": [{"group": "<id>", "state": true}, ...],
           "tree": DECISION, "continue": false, "rules": [RULE, ...]}
playlist: {"children": [...], "items": ITEM, "rules": [RULE, ...]}
segment:  {"children": ["<track id>", ...], "duration_ms": 0.0, "entry_ms": 0.0, "exit_ms": 0.0}
track:    {"clips": [CLIP, ...], "subtracks": 1, "kind": "normal" | "random" | "sequence" | "switch",
           "switch": {"group": "<id>", "state": true, "default": "<id>", "map": {"<value>": subtrack}}}
```

### Music

A music switch picks a child with its decision tree, `args` long (`tree` a node id when `args` is
empty). DECISION is `{"<value id>": DECISION or "<node id>", ...}`, one level an argument; the key
`"0"` matches any value (Wwise's default path). Look up the current value of each argument; where
it is not in a level, take `"0"`; no match, or the node id `"0"`: nothing plays.

A playlist plays ITEM, a tree:

```
ITEM: {"segment": "<segment id>"}                                      a leaf
      {"kind": "sequence" | "sequence_step" | "random" | "random_step", "loop": 1,
       "children": [ITEM, ...], "weight": 100, "avoid_repeat": 0, "shuffle": false}
```

`loop` as for sounds (1 once, 0 forever, n times); `weight` (on any item) for a random parent;
`sequence`: the children in order, each play of the parent; `random`: one at a time, picked by
weight, `loop` times; `*_step`: one child each time the parent is played.

A segment plays its tracks together for `duration_ms`. The next segment's entry cue (`entry_ms`)
lines up with this one's exit cue (`exit_ms`): the next segment starts at `exit_ms - next.entry_ms`
on this one's timeline (its pre-entry overlaps this one's end), and this one's audio after
`exit_ms` (post-exit) goes on over the next one's start. Gungeon's segments all have entry 0 and
exit = duration.

CLIP: `{"media": "<media id>", "sub": 0, "at_ms": 0.0, "begin_ms": 0.0, "end_ms": 0.0, "src_ms": 0.0}`:
the media plays from `at_ms` on the segment's timeline (`at_ms` may be negative), trimmed by
`begin_ms` at its start and by `end_ms` at its end (a negative `end_ms` trims), `src_ms` its
length; `sub` the subtrack it belongs to (a `random` / `sequence` track plays one subtrack a time,
a `switch` track the subtrack its group's value maps to).

RULE (transitions: step 1 uses the fades only):
`{"src": ["<id>" | "-1" any], "dst": [...], "fade_out_ms": 0, "fade_in_ms": 0, "sync": n,
"segment": "<transition segment id>"}`; `sync`: Wwise's AkSyncType (0 immediate, 1 next grid,
2 next bar, 3 next beat, 4 next cue, 5 custom cue, 6 entry cue, 7 exit cue, ...).

### ACTION

```
{"action": "play" | "stop" | "pause" | "resume" | "stop_all" | "pause_all" | "resume_all"
           | "set_state" | "set_switch" | "set_bus_volume" | "set_volume" | "set_rtpc"
           | "reset_bus_volume" | "reset_volume" | "other",
 "type": 1027,            Wwise's action type (0x0403 Play)
 "target": "<node or bus id>", "bus": false,      the target is a bus
 "object": false,         true: only the game object the event was posted on (Wwise's _O actions)
 "except": ["<id>", ...], stop_all / pause_all / resume_all: these are not touched
 "group": "<id>", "value": "<id>",                set_state, set_switch (no target); set_rtpc: group = the rtpc
 "amount": 0.0, "meaning": "offset" | "absolute", set_bus_volume, set_volume (dB), set_rtpc
 "fade_ms": 0, "curve": 4, "delay_ms": 0,          the action's fade (curve: AkCurveInterpolation)
 "bank": "<bank id>"}                              play: the bank holding the target
```

An event runs its actions in order, each after its `delay_ms`. `stop`/`pause`/`resume` act on
what plays from `target` (and below it); the `_all` ones on everything (or the game object's, with
`object`). `other` is an action outside step 1 (its `type` says which).

### BUS

`{"parent": "<bus id>", "vol": 0.0, "pitch": 0, "states": {...}}`: `vol` is the bus's Volume and Bus
Volume summed; a sound's gain adds its bus's `vol` and every parent bus's to the master (no
`parent`).

### CURVE (rtpc)

`{"target": "<node or bus id>", "prop": "volume" | "pitch", "scaling": "none" | "db",
"curve": [[x, y, interp], ...]}`: the RTPC's value x (clamped to the first / last point) gives y by
the segment's interpolation (the left point's `interp`, AkCurveInterpolation: 4 linear, 9 constant
(the left y), others Wwise's fade shapes; linear is a good stand-in for them). With `"db"` y is a
linear amount minus one (-1 silence, 0 unity, 1 +6 dB): interpolate y, then dB = 20 log10(y + 1);
with `"none"` y is dB (volume) or cents (pitch). The result adds to the target's `vol` / `pitch`.
An RTPC not set by the game has its `defaults.rtpc` value (from the Init bank), else 0.

### Defaults

`defaults` (the Init bank's): `rtpc` the game parameters' starting values; `state_groups` the fade
time between a group's states; `switch_rtpc` a switch group driven by an RTPC (the switch whose
x is the last one at or below the RTPC's value); `states` / `switches` a group's value before the
game sets one (absent: none, id "0").

## Checks

`pmu wwise --check` decodes every packet of every converted Vorbis stream (lewton) and checks the
stream's length (its last granule) against the WEM's sample count and the decoded samples (Wwise's
streams end with up to a long block past their length, which a decoder drops); checks every ADPCM
and PCM media's size against its blocks; and resolves every node an event reaches (children,
switch maps, decision trees, playlists, tracks' clips, media, buses), across the banks given. It
exits 1 on a problem. `cargo test -p wwise` runs it on made-up banks (`tests/madeup.rs`: every
object type, ADPCM, PCM and a streamed WEM, an object that does not read, another bank version).

On Enter the Gungeon (Unity 2017.4, Wwise 2017.2.5, bank version 128: Init.bnk and SFX.bnk, 177 MB,
2026-11-02; the codebooks read from its `Plugins/x86/libAkSoundEngine.so`, either library giving
the same): all 1821 media (273 Vorbis, 1548 IMA ADPCM) converted, every Vorbis stream decodes
(lewton) and stb_vorbis gives each exactly the WEM's sample count, every ADPCM media whole blocks;
all 1196 events resolve (3242 nodes). Compared with wwiser's dump of the same banks: every event's
actions and every node they reach (3218: types, parents, buses, volumes, pitch, children, switch
maps, decision trees, playlists, segments, clips, media) and the 16 buses match. `pmu wwise` took
3.9 s (0.5 s of it finding the codebooks) and 22 MB at most (arm64 container on an M3; `--check`
20 s); the pak is 177.4 MB, the same bytes as with ww2ogg's own codebook file.

## Not read (yet)

Bank versions other than 128 (status 3: the port stays silent); RIFX (big-endian) banks; codecs
other than Vorbis, IMA ADPCM and PCM (a media of another codec is left out with a warning); layer
containers' crossfade curves; music stingers; clip automation; effects, attenuation, HDR, aux sends.
State and switch groups have no values before the game sets them (`defaults.states` and
`defaults.switches` stay empty: the Init bank keeps none).
