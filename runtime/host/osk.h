// On-screen keyboard for Unity's soft keyboard (TouchScreenKeyboard), see osk.c
#pragma once
void osk_init(int width, int height);   // before up_start: reads UA_OSK*, sets the keyboard's area
int  osk_tick(void);                    // UI thread, every poll: follows the game's keyboard, repeats;
                                        // 1 when it opened or closed (the pads' axes are re-sent)
int  osk_active(void);                  // the game's keyboard is open and the OSK takes the pad
int  osk_typing(void);                  // the game's keyboard is open (a hardware keyboard types into it)
int  osk_in_shots(void);                // UA_OSK_SHOTS: drawn before screenshots (default) or after
int  osk_key(int keycode, int down);    // a pad button (Android keycode): 1 if the OSK took it
void osk_axes(int slot, float *v);      // a pad's axes in input.c's order (lx ly rx ry hx hy lt rt):
                                        // while open, the d-pad and left stick move the focus and the
                                        // game sees a centred pad (all zeroed)
void osk_hw_key(int keycode, int down); // a hardware keyboard's editing key (Android keycode)
void osk_hw_text(const char *utf8);     // a hardware keyboard's typed text (UTF-8)
void osk_draw(void);                    // render thread, right before the swap (nothing when closed)
