// The lockstep checker (UA_LOCKSTEP): a play session recorded and replayed frame by frame with the game's
// clock fixed, and the game's state hashed after every frame, so two runs (one with a speedup) can be
// compared frame by frame (setup-rs/crates/lockstep). See lockstep.c and docs/lockstep.md.
#pragma once
void lockstep_early(void);                                // before the Android libraries load: the clocks' zero
void lockstep_init(void);                                 // once, before Unity starts (after input_init)
void *lockstep_dlsym_hook(const char *symbol, void *found);   // the host's dlsym hook chain
void lockstep_frame(void);                                // UnityMain, after each frame
void lockstep_flush(void);                                // at the end: the files written out
int lockstep_on(void);
