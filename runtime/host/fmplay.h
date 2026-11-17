// ua_host's own FMOD Studio player (fmplay.c): plays a game's FMOD banks from the files pmu's setup
// writes (docs/fmod-player.md: <Bank>.uafm.json, <Bank>.uafm.pak). The FMOD stand-in (fmodstub.c)
// answers the game's FMOD calls with it; fmrender.c renders with it offline. Nothing of FMOD's code
// or data is in it.
//
// Threads: the game's calls come on its threads and never wait for a mix (they are queued and run
// at the start of the next mix block; only loading a bank takes the player's lock); fmp_mix runs
// on the audio output's thread. What the game reads back (a playback state, a parameter, a
// timeline position) is kept on the game's side or published by the mixer.
#pragma once
#include <stdint.h>
#include <stdio.h>

typedef uint8_t fmp_guid[16];           // FMOD's GUID in memory (the same bytes as a .NET Guid)

// playback states, as FMOD_STUDIO_PLAYBACK_STATE
enum { FMP_PLAYING = 0, FMP_SUSTAINING = 1, FMP_STOPPED = 2, FMP_STARTING = 3, FMP_STOPPING = 4 };

// callbacks the mixer raises for an instance (delivered by fmp_poll on the game's thread), as
// FMOD_STUDIO_EVENT_CALLBACK_TYPE bits
#define FMP_CB_CREATED 0x1u
#define FMP_CB_DESTROYED 0x2u
#define FMP_CB_STARTING 0x4u
#define FMP_CB_STARTED 0x8u
#define FMP_CB_STOPPED 0x20u
#define FMP_CB_START_FAILED 0x40u
#define FMP_CB_CREATE_PROGRAMMER_SOUND 0x80u
#define FMP_CB_DESTROY_PROGRAMMER_SOUND 0x100u
#define FMP_CB_TIMELINE_MARKER 0x800u
#define FMP_CB_TIMELINE_BEAT 0x1000u
#define FMP_CB_SOUND_PLAYED 0x2000u
#define FMP_CB_SOUND_STOPPED 0x4000u

typedef struct {
    uint32_t inst;              // the instance (fmp_create's id)
    uint32_t type;              // one FMP_CB_ bit
    const char *name;           // a marker's name, a programmer instrument's name (valid until the next poll)
    int position_ms;            // a marker's or beat's position
    int bar, beat, upper, lower;
    float tempo;
    int prog;                   // CREATE / DESTROY_PROGRAMMER_SOUND: the request's number (fmp_programmer_sound)
    int prog_sound;             // DESTROY: the sound it was given
} fmp_cb;

// the player on the banks' files in dir (UA_FMOD_DATA); 0 when dir has none. Any earlier one closes.
int fmp_open(const char *dir);
void fmp_close(void);
int fmp_is_open(void);

// banks: by file name ("Master Bank.bank", "Master Bank", a path ending so): a bank number (>= 0),
// or -1 when its data is not there (the game still gets a bank; it plays nothing)
int fmp_load_bank(const char *name);
void fmp_unload_bank(int bank);         // its events stop; its data stays mapped
const char *fmp_bank_name(int bank);
int fmp_bank_count_events(int bank);
int fmp_bank_event(int bank, int i);    // the bank's i-th event description

// paths and GUIDs (from the strings bank): 0 when unknown
int fmp_lookup(const char *path, fmp_guid out);
int fmp_path(const fmp_guid id, char *buf, int size);   // the path's length (without NUL); 0 unknown

// event descriptions: an index >= 0, or -1
int fmp_event(const fmp_guid id);
void fmp_event_id(int ev, fmp_guid out);
int fmp_event_params(int ev);
// a parameter's description: its name (valid while the bank is loaded), range, default
const char *fmp_event_param(int ev, int i, float *min, float *max, float *def);
int fmp_event_param_index(int ev, const char *name);    // -1 unknown (case-insensitive)
int fmp_event_length_ms(int ev);
int fmp_event_oneshot(int ev);
int fmp_event_instances(int ev);
int fmp_event_is_snapshot(int ev);
float fmp_event_user_property(int ev, const char *name, int *found);

// instances: an id (never 0) or 0 when the table is full
uint32_t fmp_create(int ev);
int fmp_valid(uint32_t inst);
int fmp_inst_event(uint32_t inst);
void fmp_start(uint32_t inst);
void fmp_stop(uint32_t inst, int immediate);
void fmp_release(uint32_t inst);
int fmp_state(uint32_t inst);
void fmp_set_param(uint32_t inst, int i, float value);
float fmp_get_param(uint32_t inst, int i);
void fmp_set_volume(uint32_t inst, float v);
float fmp_get_volume(uint32_t inst);
void fmp_set_pitch(uint32_t inst, float v);
float fmp_get_pitch(uint32_t inst);
void fmp_set_paused(uint32_t inst, int paused);
int fmp_get_paused(uint32_t inst);
int fmp_timeline_ms(uint32_t inst);
void fmp_set_timeline_ms(uint32_t inst, int ms);
void fmp_set_callback_mask(uint32_t inst, uint32_t mask);
// a programmer instrument's sound: the request number from CREATE_PROGRAMMER_SOUND, and a sound
// (fmp_sound) or -1 for none
void fmp_programmer_sound(uint32_t inst, int prog, int sound);

// buses: an index >= 0 (by path, "bus:/" the master), or -1
int fmp_bus(const char *path);
int fmp_bus_by_id(const fmp_guid id);
void fmp_bus_set_volume(int bus, float v);
float fmp_bus_get_volume(int bus);
void fmp_bus_set_paused(int bus, int paused);
int fmp_bus_get_paused(int bus);
void fmp_bus_set_mute(int bus, int mute);
int fmp_bus_get_mute(int bus);
void fmp_bus_stop_all(int bus, int immediate);
void fmp_master_volume(float v);        // the core master channel group's volume
float fmp_get_master_volume(void);
void fmp_pause_all(int paused);         // mixer suspend: nothing advances

// programmer sounds: a key of a loaded bank's sound table -> a sound number (>= 0) or -1
int fmp_sound(const char *key);
int fmp_sound_length_ms(int sound);
int fmp_sound_channels(int sound);

// the game's frame: runs its queued calls if no output reads the mix (the mix advances silently
// after 0.5 s without one), then hands the callbacks raised since the last poll, one by one
void fmp_update(void);
int fmp_poll(fmp_cb *out);

// the sound: add `frames` frames of `channels` interleaved floats at `rate` Hz into buf
void fmp_mix(float *buf, int frames, int rate, int channels);

typedef struct {
    unsigned long started, voices_started, dropped, decode_errors;
    int instances, voices, voices_peak;
    double mix_cpu_s, call_cpu_s, mixed_s;
} fmp_stats;
void fmp_get_stats(fmp_stats *s);
void fmp_set_log(void (*log)(const char *line));
