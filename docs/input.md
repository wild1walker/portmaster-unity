# Input

How a handheld's controls reach a Unity game on the Android player, and what a port sets when the
game's desktop build expected something the handheld does not have: a pad the game's input
library does not look for, a mouse, a keyboard.

## The pad

`ua_host` (runtime/host/input.c) opens each SDL2 GameController (the built-in pad, mapped by
`SDL_GAMECONTROLLERCONFIG`, which PortMaster's launchers set) and presents it to Unity as an
Android gamepad with the standard Android layout: KeyEvents `KEYCODE_BUTTON_A/B/X/Y/L1/R1/L2/R2/
THUMBL/THUMBR/START/SELECT/MODE` and joystick MotionEvents with `AXIS_X/Y` (left stick),
`AXIS_Z/RZ` (right stick), `AXIS_HAT_X/Y` (d-pad), `AXIS_LTRIGGER/RTRIGGER` and `AXIS_BRAKE/GAS`.
An InputDevice with those motion ranges is registered for it; `UA_PAD_NAME` sets the name Unity
sees (libraries that pick a layout by name, Rewired among them, know e.g.
`Xbox Wireless Controller`). A joystick MotionEvent the game does not handle becomes DPAD
KeyEvents, as Android's own fallback does.

`UA_INPUT_SCRIPT` presses buttons at given times for unattended tests (docs/runtime.md); without
a real pad it registers a virtual one that looks the same.

## What the game reads

`tools/asmpatch` (`asmpatch --survey <Game>_Data/Managed`) lists the input stacks in the game's
own assemblies. On the Android player:

| stack | on the Android player | what a port does |
|---|---|---|
| legacy `UnityEngine.Input`: axes and buttons | Unity's Android joystick input: the game's Input Manager axes apply, joystick buttons are numbered the way the Android player numbers them (not necessarily as the desktop build's own platform did) | usually nothing; a game whose bindings are off needs its own remapping (its settings, or a port patch) |
| `Input.GetKey` (keyboard) | only from keyboard KeyEvents | `UA_KEYMAP` (below): pad buttons press keys |
| mouse (`GetMouseButton*`, `mousePosition`, `OnMouse*`, UI pointer events) | Android mouse MotionEvents | pointer mode, `UA_POINTER=1` (below) |
| touch (`Input.touches`) | Android touch MotionEvents | `UA_POINTER=touch` |
| [Rewired](https://guavaman.com/projects/rewired/) | decides its platform from the desktop build's compile-time symbols, so it looks for the desktop's input APIs (Linux: libudev) and leaves the Android pad unused | `asmpatch` `rewired-android` (via `pmunity android-data --asmpatch`): Rewired runs as on Android, reading pads through Unity's input. android-data always adds the `Joy<j>Axis<a>` axes it reads (`--rewired-joysticks`) |
| InControl | chooses device profiles at run time from the running platform: Android profiles, matched by the pad's name (not tested here yet) | `UA_PAD_NAME` if the built-in pad's name matches no profile |
| Input System package (`Unity.InputSystem.dll`) | the package compiles its Android device support (layouts for Android pads) only into Android builds: a desktop build's copy has its own platform's (Linux: SDL) and none for the pads the Android player reports | open: no pad until a port registers layouts for Android gamepads; the player setting "Active Input Handling" decides whether the legacy stack runs too |

## Pointer mode

For games whose menus (or whole game) want a mouse. Off unless the port sets `UA_POINTER`; with
it off nothing below sends an event or draws a pixel. runtime/host/pointer.c, switches in
docs/runtime.md.

- **Movement.** The left stick and the d-pad (`UA_POINTER_MOVE`) move a cursor: the stick
  progressively (radial dead zone 0.15, half deflection = a quarter of the speed), the d-pad at a
  quarter of the speed, full speed after 0.8 s held; full speed is `UA_POINTER_SPEED` (1.2) screen
  heights per second. The controls the pointer uses are taken from the game (the pad's
  MotionEvents carry zeros for them); everything else stays the pad's.
- **Buttons.** `UA_POINTER_BUTTONS` (default `a:left,b:right`) press mouse buttons; those pad
  buttons no longer reach the game as pad buttons. `UA_POINTER_SCROLL=rstick` turns the right
  stick into the wheel. `UA_POINTER_TOGGLE=<button>` switches pointer mode off and on at run time,
  for games that want the mouse in menus and the pad in play.
- **Events: mouse, not touch, by default.** With mouse MotionEvents (`SOURCE_MOUSE`,
  `TOOL_TYPE_MOUSE`; `ACTION_HOVER_MOVE` while no button is held, `ACTION_DOWN`/`MOVE`/`UP` with
  the button state while one is, `ACTION_SCROLL` for the wheel, a registered `Virtual Mouse`
  InputDevice) Unity's Android player keeps `Input.mousePosition` on the cursor between clicks,
  so hover works: `OnMouseOver`/`OnMouseEnter`, UI highlight states, tool tips, and the cursors
  games draw themselves at the mouse position. Touch events (`UA_POINTER=touch`) put a finger down
  only while the button is held: Unity simulates the mouse from it (`Input.simulateMouseWithTouches`)
  and the UI's input module handles touches, so clicks work, but nothing follows the cursor in
  between, and there is no right button or wheel. Touch is for a game that reads only
  `Input.touches`.
- **The cursor.** ua_host draws an arrow (scaled with the screen, `UA_POINTER_SIZE`) over Unity's
  frame in the swap hook, right before `eglSwapBuffers`, with scissored `glClear`s only: no
  shader, texture, buffer or vertex state of Unity's is touched, and the five states the clears
  use (scissor test and box, clear colour, colour mask, draw framebuffer, rasterizer discard) are
  restored. Screenshots (`<root>/shot.req`) are taken before it is drawn, so frame comparisons see
  the game's picture; `UA_POINTER_SHOTS=1` puts it in them. Games that draw their own cursor at
  the mouse position: `UA_POINTER_CURSOR=0`. `UA_POINTER_HIDE=<s>` hides it when idle.
- **Scripts.** `UA_INPUT_SCRIPT` takes `ptr=<x>/<y>` (fractions of the screen if written with a
  `.`, else pixels) to put the pointer somewhere; scripted buttons and sticks go through pointer
  mode like the pad's own, so `"60:ptr=0.5/0.74 75:a~1.5"` clicks there (hold a click for more
  than a frame: the emulated rig draws a frame in about 0.3 s).

## Keys from the pad

`UA_KEYMAP=<button>:<key>,...` (e.g. `start:escape,x:space,select:tab,up:up,down:down`) turns pad
buttons, and the d-pad's directions, into keyboard KeyEvents from a registered `Virtual Keyboard`
(`SOURCE_KEYBOARD`), for games that read `Input.GetKey`. Mapped controls no longer reach the game
as the pad's. Works with or without pointer mode. Keys: `a`-`z`, `0`-`9`, `f1`-`f12`, `escape`,
`enter`, `space`, `tab`, `backspace`, `delete`, `up`, `down`, `left`, `right`, `shift`, `ctrl`,
`alt`, `pageup`, `pagedown`, `home`, `end`, `insert`, `minus`, `equals`, `comma`, `period`,
`slash`, `grave`, `back` (Android's back key, which Unity reads as Escape).

## Tests

- `runtime/host/test/pointer_test.sh` (`ARCH=arm64` too): pointer.c against stand-ins for the
  runtime and a fake libGLESv2 window: what each scenario sends and takes (off, mouse, touch,
  keymap, toggle), the cursor's pixels, Unity's GL state kept, nothing at all with the switches off.
- `runtime/jni/build.sh`: libunity's view of a mouse MotionEvent (source, action, tool type,
  button state, `isButtonPressed`, the wheel axis, its InputDevice) in every Unity version shape.
- `tools/asmpatch/build.sh test`: the Rewired patch on fake Rewired assemblies.
