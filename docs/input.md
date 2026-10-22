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
| [Rewired](https://guavaman.com/projects/rewired/) | decides its platform from the desktop build's compile-time symbols, so it looks for the desktop's input APIs (Linux: libudev) and leaves the Android pad unused | `asmpatch` `rewired-android` (via `pmunity android-data --asmpatch`): Rewired runs as on Android, reading pads through Unity's input. android-data always adds the `Joy<j>Axis<a>` axes it reads (`--rewired-joysticks`). Its UI input module (`RewiredStandaloneInputModule`) needs nothing: it reads Unity's mouse and touches as Unity's own module does (below) |
| InControl | chooses device profiles at run time from the running platform: Android profiles, matched by the pad's name (below) | `UA_PAD_NAME` that one of its Android profiles knows (`asmpatch --survey` lists them), e.g. `Microsoft X-Box 360 pad` |
| Input System package (`Unity.InputSystem.dll`) | the engine's Android backend reports keyboards, mice and touchscreens in the package's common formats (they work as they are) and pads as Android game controllers, for which the package has layouts only in Android builds: a desktop build's copy has its own platform's (Linux: SDL) | `asmpatch` `inputsystem-android` (below): the pad is a `Gamepad`. The player setting "Active Input Handling" decides whether the legacy stack runs too |
| soft keyboard (`TouchScreenKeyboard`, UI `InputField` / `TMP_InputField` on Android) | Unity opens its input dialog (`UnityPlayer.showSoftInput`) | nothing: ua_host's on-screen keyboard (below) |

## The Input System package: `inputsystem-android`

On the Android player the engine's input backend (the one the package reads) reports a pad as
interface `Android`, device class `AndroidGameController`, with state format `'AGC '`: the
pressed Android key codes as bits from byte 0 (`KEYCODE_BUTTON_A` = bit 96) and the
MotionEvent axes as 48 floats from byte 28 (`AXIS_X` at 28, `AXIS_HAT_X` at 88). Keyboards
(`'KEYS'`), mice and touchpads (`'MOUS'`) and touchscreens (`'TSCR'`) come in the package's common
formats, whose layouts every build has. A desktop build's `Unity.InputSystem.dll` lacks the
Android layouts (the package compiles them only into Android and editor builds), so the pad is
an unsupported device: no `Gamepad.current`, no gamepad bindings, no UI navigation with
`InputSystemUIInputModule`.

`asmpatch`'s `inputsystem-android` patch (tools/asmpatch) puts calls to the package's own public
`InputSystem.RegisterLayout(json)` at the end of `InputSystem.PerformDefaultPluginInitialization`,
where the package registers its platform support. The layouts are written in the package's
JSON layout format from the player's state format above (no package code is used): they extend
the package's `Gamepad` layout and place each of its controls, so the pad is a `Gamepad` with
`buttonSouth/East/West/North` = A/B/X/Y, the shoulders, stick presses, Start/Select, both sticks
(`AXIS_X/Y`, `AXIS_Z/RZ`; Android's y axes point down and are inverted), the triggers
(`AXIS_BRAKE`/`AXIS_GAS`) and the d-pad from `AXIS_HAT_X/Y` (ua_host's pads report these axes).
A pad with Microsoft's USB vendor id (0x045E) becomes an `XInputController` (Xbox glyphs), as
the package's Android support makes it, where the build has that class. They match by interface
and device class, so the package versions 1.0-1.8 take them the same way (the patch fails, and
changes nothing, if `PerformDefaultPluginInitialization` or `RegisterLayout(string, string,
InputDeviceMatcher?)` is missing). Not covered: pads without `SOURCE_GAMEPAD` (Android
joysticks) and sensors, which ua_host does not present.

```sh
python -m pmunity android-data ... --asmpatch "/path/to/asmpatch --only=inputsystem-android"
```

## InControl

[InControl](https://gallantgames.com/pages/incontrol-introduction) reads pads through Unity's
legacy joystick input and picks a device profile for each joystick at run time: profiles name
the platforms they apply to (`IncludePlatforms` / `SupportedPlatforms`, compared with
`SystemInfo.operatingSystem`, which says Android on the Android player) and the joystick names
they match (`JoystickNames` or a matcher's `NameLiteral`, and patterns). Nothing of this is
compiled per platform, so a desktop build runs its Android profiles on the Android player
unchanged: no patch is needed. What matters is the pad's name (`Input.GetJoystickNames()`, the
InputDevice name ua_host reports): a name no Android profile matches makes the pad an *unknown
controller* (raw buttons and axes, not InControl's Action1-4, bumpers, sticks), so the game's
bindings do nothing. ua_host's pads have the standard Android layout of an Xbox pad, so a port
sets the name of InControl's Xbox Android profile:

```sh
UA_PAD_NAME="Microsoft X-Box 360 pad"     # InControl's Xbox360AndroidProfile (also matches /360/)
```

`asmpatch --survey <Managed>` lists the Android profiles of the game's own InControl (read from
its profile classes, old constructor style and newer `Define()` style) with the names they
match, Xbox profiles first, and suggests the first name.

## The on-screen keyboard

When a game opens Unity's soft keyboard (`TouchScreenKeyboard.Open`; UI `InputField` and
`TMP_InputField` do so on Android when selected), the Android player calls
`UnityPlayer.showSoftInput`, and Unity's Java side opens a dialog with a text field over the
IME. runtime/jni/up_softinput.c is that dialog: the field holds the game's text with the caret at
the end, the character limit (as Android's `LengthFilter`, in UTF-16 units) and the input type's
characters (the number and phone pads' `DigitsKeyListener`/`DialerKeyListener`), and reports
every change to libunity as the dialog of the player's version does: the text
(`nativeSetInputString`), the selection (`nativeSetInputSelection`, 2018.4+), done or cancelled
and closed (`nativeSoftInputClosed`/`Canceled`/`LostFocus`), the keyboard's visibility and area
(`nativeSetKeyboardIsVisible`, `nativeSetInputArea`, 2019.4+). The game's own updates
(`setSoftInputStr`, `setSelection`, `setCharacterLimit`, `setHideInputField`, `hideSoftInput`)
change the field as they change the dialog.

ua_host (runtime/host/osk.c) is the IME: while the game's keyboard is open it draws a keyboard
and the field at the bottom of the screen and the pad types:

| control | |
|---|---|
| d-pad, left stick | move over the keys (held: repeats) |
| A | press the key |
| B | delete (held: repeats) |
| X | space |
| Y | shift; twice: caps lock; three times: off |
| L1 / R1 | caret left / right |
| Start | done (the dialog's OK: the game gets the text) |
| Select | cancel (Back: the game keeps its old text) |

The keys: letters with shift, a symbols page (`?123` / `ABC`: all of ASCII), space, backspace and
Enter (a newline in a multi-line field, else done); number and phone pads for the game's numeric
keyboard types (the sign and decimal point only where the type allows them). Letters are
capitalised where the field's input type asks for it (Unity's name and social types), a
password field shows `*`, and a game that hides its input field (`shouldHideMobileInput`) gets
the keyboard without the field. The pad is the keyboard's while it is open: the game sees a
centred pad, and buttons held from before reach the game when released; pointer mode's cursor is
hidden. Nothing changes for a game that never opens its keyboard: nothing is drawn, no input is
taken, no memory is allocated.

The keyboard is drawn like pointer mode's cursor, over Unity's frame right before the swap: on a
GLES 3 context it is rendered on the CPU when it changes, uploaded to a texture of its own and
copied with one `glBlitFramebuffer` (an sRGB window gets an sRGB texture, so colours stay
right); on GLES 2 or a multisampled window it is drawn with scissored clears. The GL state used
is restored. It is in `<root>/shot.req` screenshots (`UA_OSK_SHOTS=0`: not). The font is the
public-domain misc-fixed 6x13 (runtime/host/osk_font.h, made by fontgen.py).

Switches: `UA_OSK=0` (no on-screen keyboard: the field stays open for a hardware keyboard),
`UA_OSK=done` / `UA_OSK=cancel` (answer every keyboard at once, as OK or Back would: for ports
where text entry does not matter; with `done` the game keeps the text it offered),
`UA_OSK_SCALE=<n>` (glyph scale; default screen height / 240).

## A hardware keyboard

With an SDL window (`UA_DISPLAY=sdl`), SDL's key events reach ua_host. While the game's soft
keyboard is open they type into its field, as into Android's dialog: text as typed, Backspace,
Delete, the arrows, Home, End, Enter (done, or a newline in a multi-line field) and Escape
(cancel). With `UA_KEYBOARD=1` they also reach the game as an Android keyboard while no soft
keyboard is open (KeyEvents from a `Hardware Keyboard` device, `SOURCE_KEYBOARD`, US layout, the
modifier keys' meta state): `Input.GetKey`, the Input System's `Keyboard`. Off by default, so a
handheld whose buttons appear as keyboard keys does not press them twice. (With Mali fbdev,
`UA_DISPLAY=fbdev`, there are no SDL key events.)

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
- **The start position** (`UA_POINTER_START`, the screen's centre) is sent as a hover when Unity
  starts, and again once Unity has presented its second frame. Unity's Android player empties its
  queue of input events when it sets up its input (in its first frame) and when it is unpaused
  (ua_host resumes it at start), so the first hover never arrives: Unity's mouse stays at (0,0),
  the bottom left corner, and the first motion's delta (the `Mouse X` / `Mouse Y` axes, Rewired's
  mouse axes, a game cursor moved by them) is measured from there, so a game's own cursor jumps
  towards the top right corner. Unity's delta for a frame is its last mouse event's, so the second
  hover is a pair, half a pixel beside the cursor and then on it: Unity's mouse lands on the cursor
  with a delta of half a pixel, and the first motion moves a game's cursor by what the pointer
  moved (Baldi's launcher, whose cursor reads the mouse from its first frame, in the rig).
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
- **UI input modules.** Unity's `StandaloneInputModule` and Rewired's `RewiredStandaloneInputModule`
  (its source edition compiles it into `Assembly-CSharp`; it replaces Unity's on the game's
  EventSystem) both read `Input.mousePosition`, the mouse buttons and `Input.touches`, and hit-test
  the UI at that position, so pointer mode clicks the uGUI elements either serves; both leave the
  mouse alone while the game has locked the pointer (`Cursor.lockState = Locked`). One difference
  on Android: `Input.touchSupported` is true there (desktop players report false), and Rewired's
  module with its "Allow Mouse Input If Touch Supported" setting off then takes only touches: such
  a game wants `UA_POINTER=touch`.

### Games with their own cursor

Some games draw a cursor of their own and move it themselves: they lock and hide the system
pointer (`Cursor.lockState = Locked`, `Cursor.visible = false`), add the mouse's motion (and often
a stick's) to the cursor's position each frame, and hit-test their UI at that position with their
own raycast; their buttons are not uGUI `Selectable`s, and the EventSystem's input module, which
ignores a locked pointer anyway, never clicks them. Baldi's Basics Classic Remastered is one: its
launcher's `CursorController` moves the cursor by Rewired actions (`CursorX`/`CursorY` from the
left stick, `CursorXDelta`/`CursorYDelta` from the mouse's motion), raycasts the canvas at it,
and presses the button under it on `MouseSubmit` (the left mouse button or the pad's A). Pointer
mode's position does not reach such a cursor, and neither do touches (a touch is a click
wherever the game's cursor is); the mouse's motion does, one screen pixel per pixel. So:

- the pad: without `UA_POINTER` the game's own controller support moves its cursor (Baldi: left
  stick, A clicks). This is the usual answer;
- or `UA_POINTER=1 UA_POINTER_CURSOR=0`: the stick moves the game's cursor through the mouse's
  motion and the pointer buttons click, with no second cursor drawn. ua_host's hidden pointer
  still stops at the screen's edges: a game cursor that moves one pixel per pixel inside the
  screen reaches all of its area, one with a sensitivity setting below 1 may not.

`asmpatch --survey` reports the pattern (a pointer lock, and a UI hit test outside an input
module at a position not read from the mouse or a touch): "verdict: likely its own cursor". A
lock alone is usually mouse look.

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
  keymap, toggle; the start position sent again, once, after Unity's second frame, as a half-pixel
  pair), the cursor's pixels, Unity's GL state kept, nothing at all with the switches off.
- `runtime/jni/build.sh`: libunity's view of a mouse MotionEvent (source, action, tool type,
  button state, `isButtonPressed`, the wheel axis, its InputDevice) in every Unity version shape.
- `runtime/host/test/pointer_test.sh` also runs `test_osk` (osk.c against a stand-in field and the
  fake GL): nothing taken or drawn while no keyboard is open; typing with the pad (focus moves,
  every button, shift and caps, the symbols page, Enter, repeats, buttons held from before);
  the picture on the copy path, with scissored clears on GLES 2 and on a multisampled window,
  Unity's GL state kept; the number and phone pads, auto-capitalisation, a hidden field;
  `UA_OSK=done|cancel|0` and a hardware keyboard.
- `runtime/jni/build.sh`: the soft keyboard in every Unity version shape: the natives each
  version's dialog calls, in order, for showing, typing (a two-byte character, the caret, the
  character limit), the game's own text and selection, done, cancel, lost focus, the game hiding
  it, the number and decimal pads' filters, a password and a multi-line field, and a pause.
- `tools/asmpatch/build.sh test`: the Rewired patch on fake Rewired assemblies; the survey's
  InControl Android profiles on fake profile classes of both styles; the Input System
  patch on a fake `Unity.InputSystem` (built with Cecil, no package code): the layouts it
  registers on every return, parsed and checked against the player's state format; already
  applied, an Android build of the package, missing methods.
  The survey's pointer verdicts on fake games: a cursor of its own (Rewired's UI input module and
  demos, an asset's examples, the game's own input module and a hit test at the mouse's position
  not counted), mouse look, and pointer-driven menus.
