# Input bindings

Rebindable keyboard and controller controls for the emulated PS2 pad. One small library decides
what can be bound, what the defaults are, how bindings are written to a file, and how host input
becomes pad state. The runtime uses it to read the pad, and a front end (such as a launcher) can
use the same code to let people rebind controls, so a binding means exactly the same thing in both.

| File | What it is |
| --- | --- |
| [`ps2xRuntime/include/runtime/input_bindings.h`](../ps2xRuntime/include/runtime/input_bindings.h) | The whole API, namespace `ps2x::input` |
| [`ps2xRuntime/src/lib/input_bindings.cpp`](../ps2xRuntime/src/lib/input_bindings.cpp) | Names, defaults, the controls file, `evaluate()` |
| [`ps2xRuntime/src/lib/ps2_pad.cpp`](../ps2xRuntime/src/lib/ps2_pad.cpp) | The runtime's use: `PSPadBackend::readState` |
| [`ps2xTest/src/input_bindings_tests.cpp`](../ps2xTest/src/input_bindings_tests.cpp) | Tests |

The library needs only the C++ standard library (C++20, for the defaulted `==`). It does not
include GLFW, raylib or any window code, so a tool can compile the two files on their own.

## Concepts

### Actions

An `Action` is one thing on the PS2 pad that can be bound. The first 16 are buttons, in this
order: Up, Down, Left, Right, Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, Start,
Select. The other 8 are the four directions of each stick: `LeftStickUp` ... `RightStickRight`.
`kButtonActionCount` (16) marks where the stick directions start, and `kActionCount` (24) is the
total.

Each action has an id, used in the controls file (`actionId`: `cross`, `left_stick_up`), and a
label for people (`actionLabel`: `Cross`, `Left stick up`).

### Bindings

A `Binding` is one host input:

| Kind | `code` | `direction` | Made with |
| --- | --- | --- | --- |
| `None` | | | `Binding{}` |
| `Key` | a GLFW key code (`GLFW_KEY_*`; raylib's `KEY_*` are the same numbers) | | `Binding::key(code)` |
| `Button` | a `GamepadButton` | | `Binding::button(b)` |
| `Axis` | a `GamepadAxis` | +1 or -1, the way it must be pushed | `Binding::axis(a, dir)` |

`GamepadButton` and `GamepadAxis` are numbered like GLFW's standard gamepad layout
(`GLFW_GAMEPAD_BUTTON_*`, `GLFW_GAMEPAD_AXIS_*`), so GLFW's `GLFWgamepadstate` copies across
index for index. Buttons are named by position, not by letter: `South` is A on an Xbox pad and
Cross on a PlayStation pad. Stick axes run from -1 (left, up) to 1 (right, down); triggers rest
at -1 and reach 1.

An axis binding counts as pressed once the axis passes `axisThreshold()`: 0.5 for a stick, 0.1
for a trigger (just past half way, as GLFW's own trigger buttons use).

### The bindings set

`Bindings` holds everything a controls file can say:

- `keyboard[action]`: two key slots per action (`kKeyboardSlots`).
- `controller[action]`: two controller slots per action (`kControllerSlots`), each a button or an
  axis direction.
- `leftStick`, `rightStick`: a `StickSettings` for each PS2 stick: which controller stick drives
  it (`LeftStick`, `RightStick` or `None`), and whether to invert X and Y.

`Bindings::defaults()` gives the built-in layout:

| PS2 pad | Keys | Controller |
| --- | --- | --- |
| D-pad | arrow keys | D-pad |
| Cross / Circle / Square / Triangle | X or Space / C / Z or Keypad 0 / V or Keypad 1 | South / East / West / North |
| L1 / R1 | Q / E | shoulders |
| L2 / R2 | Left Shift / Right Shift | triggers |
| L3 / R3 | Left Control / Right Control | stick presses |
| Start / Select | Enter / Tab | Start / Back |
| Left stick | WASD | left stick (from `leftStick`) |
| Right stick | IJKL | right stick (from `rightStick`) |

The analog sticks do not need controller bindings for their directions: `StickSettings` routes a
whole controller stick to a PS2 stick. The stick-direction actions exist so keys (or controller
buttons) can push a stick too.

### Reserved keys

F1 (PCSX2 overlay or debug panel), F3 (VSync counter), F5 (super-sampling) and F12 (screenshot)
are used by the runtime or the games built on it.
`isReservedKey()` reports them, and a front end should refuse to bind them.

## The controls file

`serialize()` and `parse()` convert a `Bindings` to and from text; `saveFile()` and `loadFile()`
do the same with a file. The format is INI-style:

```ini
# PS2 pad controls. Each line is a pad button or stick direction, then up to two
# bindings separated by commas, or none. Missing lines keep their defaults.

[keyboard]
up = Up
cross = X, Space
l2 = LeftShift
left_stick_up = W
...

[controller]
cross = South
l2 = LeftTrigger
left_stick_up = none
...

# Where each PS2 stick reads the controller from: left, right or none.
[sticks]
left_stick = left
left_stick_invert_x = no
left_stick_invert_y = no
right_stick = right
right_stick_invert_x = no
right_stick_invert_y = no
```

Rules `parse()` follows:

- Parsing starts from `Bindings::defaults()`. A missing line, or a missing file section, keeps
  the default, so an empty file means the defaults.
- A line replaces both slots of that action. `cross = Space` leaves Cross with only Space;
  `cross = none` unbinds it.
- Names are case-insensitive. Lines starting with `#` or `;` are comments.
- `[keyboard]` takes only keys and `[controller]` only controller inputs.
- Anything it cannot understand is skipped with a warning (`"line 12: unknown binding Foo"`), and
  the rest of the file still applies. Pass a `std::vector<std::string>*` to collect them.
- Extra bindings past the two slots are dropped with a warning.
- Stick inversion accepts yes/no, true/false, on/off and 1/0.

Binding names:

| Kind | Names |
| --- | --- |
| Keys | `A`-`Z`, `0`-`9`, `F1`-`F25`, `Keypad0`-`Keypad9`, and `Space`, `Enter`, `Tab`, `Backspace`, `Escape`, `Up`, `Down`, `Left`, `Right`, `LeftShift`, `RightShift`, `LeftControl`, `RightControl`, `LeftAlt`, `RightAlt`, `LeftSuper`, `RightSuper`, `Menu`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `CapsLock`, `ScrollLock`, `NumLock`, `PrintScreen`, `Pause`, `KeypadDecimal`, `KeypadDivide`, `KeypadMultiply`, `KeypadSubtract`, `KeypadAdd`, `KeypadEnter`, `KeypadEqual`, `Apostrophe`, `Comma`, `Minus`, `Period`, `Slash`, `Semicolon`, `Equal`, `LeftBracket`, `Backslash`, `RightBracket`, `GraveAccent` |
| Controller buttons | `South`, `East`, `West`, `North`, `LeftShoulder`, `RightShoulder`, `Back`, `Start`, `Guide`, `LeftStickPress`, `RightStickPress`, `DpadUp`, `DpadRight`, `DpadDown`, `DpadLeft` |
| Controller axes | `LeftX-`, `LeftX+`, `LeftY-`, `LeftY+`, `RightX-`, `RightX+`, `RightY-`, `RightY+`, `LeftTrigger`, `RightTrigger` |

`bindingId()` gives the file name of a binding and `bindingFromId()` reads one back;
`bindingLabel()` gives a name for people (`Left Shift`, `South (A / Cross)`, `Left stick left`).

## From host input to pad state

```cpp
ps2x::input::PadState evaluate(const Bindings &bindings,
                               const std::function<bool(int)> &keyDown,
                               const std::vector<GamepadState> &gamepads);
```

- `keyDown(glfwKey)` answers whether a key is held.
- `gamepads` has one `GamepadState` (15 buttons, 6 axes) per connected controller. Every
  controller counts, so one that sits in the first slot cannot shut out the others.
- The result's `pressed` has bit *n* set when button action *n* is held (`isPressed(action)`), and
  `sticks` holds left X, left Y, right X, right Y, each from -1 to 1.

An action is held when any of its keyboard or controller bindings is active, on any controller.
Each stick axis takes whichever source is pushed furthest from centre: the stick-direction actions
(as -1, 0 or 1), or the controller stick that `StickSettings` routes to it, after inversion. So a
key and a stick can both drive the same PS2 stick without one masking the other.

`evaluate()` does no I/O and keeps no state, so it is easy to test and safe to call every frame.

## Using it in a runtime

This is what [`ps2_pad.cpp`](../ps2xRuntime/src/lib/ps2_pad.cpp) does; another runtime or port
follows the same four steps.

1. **Build the files.** Add `src/lib/input_bindings.cpp` to the target and
   `ps2xRuntime/include` to its include path.

2. **Load the bindings once.** Start from the defaults and let a file override them:

   ```cpp
   const ps2x::input::Bindings &controls()
   {
       static const ps2x::input::Bindings s_bindings = []()
       {
           ps2x::input::Bindings bindings = ps2x::input::Bindings::defaults();
           const char *env = std::getenv("PS2X_CONTROLS");
           const std::string path = env && env[0] ? env : "controls.ini";
           std::vector<std::string> warnings;
           if (ps2x::input::loadFile(path, bindings, &warnings))
               for (const std::string &w : warnings)
                   std::fprintf(stderr, "[pad] %s: %s\n", path.c_str(), w.c_str());
           return bindings;
       }();
       return s_bindings;
   }
   ```

   `loadFile()` returns false, and leaves `bindings` alone, when the file can't be opened, so a
   missing `controls.ini` means the defaults.

3. **Gather the host input each pad read.** Fill a `GamepadState` per connected controller in
   GLFW's standard order. With GLFW directly, that is a straight copy from `glfwGetGamepadState`
   (see the launcher's `GamepadReader::read()`). The runtime reads raylib, which numbers buttons its
   own way, so `ps2_pad.cpp` maps them with a `kRaylibButtonFor` table; axes are
   in the same order in both.

4. **Evaluate, then build the pad bytes.**

   ```cpp
   const ps2x::input::PadState pad = ps2x::input::evaluate(
       controls(), [](int key) { return IsKeyDown(key); }, gamepads);

   uint16_t buttons = 0xFFFF; // PS2 pad buttons are active low
   for (int i = 0; i < ps2x::input::kButtonActionCount; ++i)
       if (pad.isPressed(static_cast<ps2x::input::Action>(i)))
           buttons &= ~kActionMasks[i]; // the pad bit for each action, in Action order
   // pad.sticks[0..3] -> 0x80 + value * 127 for lx, ly, rx, ry
   ```

raylib's `IsKeyDown` takes raylib key codes, which are GLFW's, so key bindings pass straight
through. Keep the pad bit table in `Action` order; it is the only place the runtime ties an action
to the PS2 hardware.

## Using it in a front end

A front end edits the same file the runtime reads. The .hack//Infection and Drakengard launchers
(`launcher/` in each game's repository: `ControlsTab` and `GamepadReader`) are full examples in Qt. What matters for any
front end:

**Build the same two files.** The launcher adds `input_bindings.cpp` and `input_bindings.h` from
`ps2xRuntime` to its own executable, so it never keeps a copy that could drift.

**Edit, then save at once.** Load with `loadFile()` (or start from `defaults()` if there is no
file), change slots in the `Bindings`, and call `saveFile()` after each change. Point the front
end at the file the runtime will read: the runtime looks in its working directory, or at
`PS2X_CONTROLS`.

**Capture keys as GLFW key codes.** The runtime's keys come from GLFW, which on Windows picks the
key from the scancode (its `createKeyTables` in `win32_init.c`). A front end that is not GLFW
should do the same: map the native scancode to a GLFW key code with the same table, so a captured
key is exactly what the game will see, including left and right Shift and the keypad. The
launcher's `glfwKeyFromScancode()` is that table (the 0x100 bit marks extended keys, as in Qt's
`nativeScanCode()`), with a fallback from Qt key codes where there is no scancode. Refuse
`isReservedKey()` keys and ignore auto-repeat.

**Read controllers with the same GLFW.** Using the same library and version as the runtime
(GLFW 3.4) means the same gamepad mappings, so a button captured in the front end is the same
button in the game. Without a GLFW window, do not call `glfwPollEvents()` in an application
that has its own event loop: on Windows it would take that loop's thread messages. GLFW still
reads controller state without it, but it won't notice a controller connected later; the
launcher restarts GLFW (`glfwTerminate()`, then `glfwInit()`) when it needs to look again.

**Capture controller inputs as changes from rest.** When a capture starts, record each
controller's state as a baseline, then poll (the launcher polls every 16 ms) and take the first
input that changes from it:

- a button that is down now and was up at the start;
- a trigger past 0.5 that started below 0;
- a stick axis past 0.7 that started inside 0.3, bound in the direction it was pushed.

Anything already held when the capture began counts only once it has been released and pressed
again, so the click that started the capture, or a stick resting off centre, isn't taken. Give up
after a few seconds (the launcher waits 8) and let Escape cancel.

**Show labels, store ids.** Show `bindingLabel()` and `actionLabel()` to people; the file always
holds ids. To warn about a binding used twice, compare `Binding`s with `==`.

## Adding a bindable input

Everything about what can be bound is in `input_bindings.h` and `.cpp`; extend those, not the pad
code or a front end.

- **A new action:** add it to `Action` (buttons before `LeftStickUp`, so `kButtonActionCount`
  stays right), give it an id and label in `kActionNames` in the same position, add its defaults
  in `Bindings::defaults()`, and give it a pad bit in the runtime's `kActionMasks`. `pressed` is a
  32-bit mask, so there is room for 16 more button actions.
- **A new key name:** add it to `kKeyNames`. Letters, digits, F-keys and keypad digits are
  handled by code and don't need entries.
- **A new controller input:** add it to `GamepadButton` or `GamepadAxis` in GLFW's order, with a
  name in `kButtonNames` or `kAxisNames`, and fill it in wherever a `GamepadState` is built.

The test "every key and controller input has a name that reads back" checks that key names are
unique and that every named key, button and axis direction reads back as the same binding, so
run the tests after adding one.

## Tests

`ps2xTest/src/input_bindings_tests.cpp` covers: defaults surviving a save and load, an empty file,
a line replacing only its action, `none`, bad lines warning without stopping the parse, stick
settings, names reading back, `evaluate()` for keys, buttons and sticks, swapped and inverted
sticks, and the reserved keys.

The test build is off by default. To run it:

```bash
cmake -S . -B out/build -DPS2X_BUILD_TEST=ON
cmake --build out/build --target ps2x_tests
```

Run the test executable from the PS2Recomp root, then configure with `-DPS2X_BUILD_TEST=OFF`
again if your build normally leaves it off.
