// Gamepads (SDL2 GameController) -> Android KeyEvent/MotionEvent for the Android player
#pragma once
int  input_init(int width, int height);   // before up_start: registers the pads present as Android input
                                          // devices (and pointer mode's mouse, UA_POINTER; screen size in pixels)
void input_started(void);   // after up_start: events may be injected (starts UA_INPUT_SCRIPT)
void input_poll(void);      // on the UI thread: SDL events -> up_inject_key / up_inject_axes
