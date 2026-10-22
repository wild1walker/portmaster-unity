// Pointer mode (UA_POINTER) and pad -> keyboard keys (UA_KEYMAP), see pointer.c
#pragma once
int  pointer_init(int width, int height, int *next_id);  // before up_start: reads the switches, registers
                                                         // the virtual mouse/keyboard; 1 if pointer mode is on
int  pointer_enabled(void);                     // pointer mode configured and currently on
void pointer_start(void);                       // after up_start: the first hover position
void pointer_frames(unsigned long presented);   // UI thread, every poll: frames Unity has presented
                                                // (the hover again once Unity takes input events)
int  pointer_key(int keycode, int down);        // a pad button (Android keycode): 1 if taken (click, key, toggle)
void pointer_axes(int slot, float *v);          // a pad's axes in input.c's order (lx ly rx ry hx hy lt rt):
                                                // takes the cursor's controls (zeroed for the game)
void pointer_warp(double x, double y, int fraction);   // UA_INPUT_SCRIPT ptr=<x>/<y>
void pointer_tick(void);                        // UI thread, every poll: moves the cursor, sends events
void pointer_draw(void);                        // render thread, right before the swap
int  pointer_in_shots(void);                    // UA_POINTER_SHOTS: draw before screenshots too
