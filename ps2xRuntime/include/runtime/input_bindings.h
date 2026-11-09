#pragma once

// Keyboard and controller bindings for the emulated PS2 pad: what can be bound, the defaults, a
// readable controls file, and the mapping from host input to pad state.
//
// This file has no dependencies beyond the standard library, so tools such as a launcher can
// build it too and capture bindings that mean exactly the same thing to the runtime. Key codes
// are GLFW's (GLFW_KEY_*, which raylib's KEY_* also use). Controller inputs are numbered like
// GLFW's standard gamepad layout (GLFW_GAMEPAD_BUTTON_*, GLFW_GAMEPAD_AXIS_*).

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::input
{
    // Everything that can be bound: each PS2 button, then the four directions of each stick.
    enum class Action : int
    {
        Up,
        Down,
        Left,
        Right,
        Cross,
        Circle,
        Square,
        Triangle,
        L1,
        R1,
        L2,
        R2,
        L3,
        R3,
        Start,
        Select,
        LeftStickUp,
        LeftStickDown,
        LeftStickLeft,
        LeftStickRight,
        RightStickUp,
        RightStickDown,
        RightStickLeft,
        RightStickRight,
        Count
    };
    constexpr int kActionCount = static_cast<int>(Action::Count);
    constexpr int kButtonActionCount = static_cast<int>(Action::LeftStickUp);

    // Standard gamepad buttons, by position: South is A on Xbox and Cross on PlayStation pads.
    enum class GamepadButton : int
    {
        South,
        East,
        West,
        North,
        LeftShoulder,
        RightShoulder,
        Back,
        Start,
        Guide,
        LeftStick,
        RightStick,
        DpadUp,
        DpadRight,
        DpadDown,
        DpadLeft,
        Count
    };
    constexpr int kGamepadButtonCount = static_cast<int>(GamepadButton::Count);

    // Stick axes run from -1 (left, up) to 1 (right, down). Triggers rest at -1 and reach 1.
    enum class GamepadAxis : int
    {
        LeftX,
        LeftY,
        RightX,
        RightY,
        LeftTrigger,
        RightTrigger,
        Count
    };
    constexpr int kGamepadAxisCount = static_cast<int>(GamepadAxis::Count);

    struct Binding
    {
        enum class Kind : uint8_t
        {
            None,
            Key,    // code: GLFW key code
            Button, // code: GamepadButton
            Axis    // code: GamepadAxis, direction: the way it must be pushed (+1 or -1)
        };
        Kind kind = Kind::None;
        int code = 0;
        int direction = 0;

        static Binding key(int glfwKey) { return {Kind::Key, glfwKey, 0}; }
        static Binding button(GamepadButton b) { return {Kind::Button, static_cast<int>(b), 0}; }
        static Binding axis(GamepadAxis a, int dir) { return {Kind::Axis, static_cast<int>(a), dir < 0 ? -1 : 1}; }

        bool isNone() const { return kind == Kind::None; }
        bool isKeyboard() const { return kind == Kind::Key; }
        bool isController() const { return kind == Kind::Button || kind == Kind::Axis; }
        bool operator==(const Binding &) const = default;
    };

    // Where a PS2 stick takes its controller input from.
    struct StickSettings
    {
        enum class Source : uint8_t
        {
            LeftStick,
            RightStick,
            None
        };
        Source source = Source::LeftStick;
        bool invertX = false;
        bool invertY = false;
        bool operator==(const StickSettings &) const = default;
    };

    constexpr int kKeyboardSlots = 2;
    constexpr int kControllerSlots = 2;

    struct Bindings
    {
        std::array<std::array<Binding, kKeyboardSlots>, kActionCount> keyboard{};
        std::array<std::array<Binding, kControllerSlots>, kActionCount> controller{};
        StickSettings leftStick{StickSettings::Source::LeftStick};
        StickSettings rightStick{StickSettings::Source::RightStick};

        static Bindings defaults();
        bool operator==(const Bindings &) const = default;
    };

    // Names. Ids are the words used in the controls file; labels are for people.
    const char *actionId(Action action);    // "cross", "left_stick_up"
    const char *actionLabel(Action action); // "Cross", "Left stick up"
    std::optional<Action> actionFromId(std::string_view id);

    std::string keyId(int glfwKey); // "Space", "LeftShift", "Keypad0"; empty if unknown
    std::optional<int> keyFromId(std::string_view id);
    const char *gamepadButtonId(GamepadButton button);
    const char *gamepadButtonLabel(GamepadButton button); // "South (A / Cross)"

    std::string bindingId(const Binding &binding); // "Space", "South", "LeftTrigger", "LeftX-"; empty for none
    std::string bindingLabel(const Binding &binding); // "Left Shift", "South (A / Cross)", "Left stick left"
    std::optional<Binding> bindingFromId(std::string_view id);

    // Keys the runtime keeps for itself (F1 debug panel, F5 super-sampling, F12 screenshot).
    bool isReservedKey(int glfwKey);

    // The controls file: an INI-style text with [keyboard], [controller] and [sticks] sections.
    // Anything missing keeps its default, so an empty file means the defaults.
    std::string serialize(const Bindings &bindings);
    Bindings parse(std::string_view text, std::vector<std::string> *warnings = nullptr);
    bool loadFile(const std::string &path, Bindings &out, std::vector<std::string> *warnings = nullptr);
    bool saveFile(const std::string &path, const Bindings &bindings);

    // Mapping host input to the pad.
    struct GamepadState
    {
        std::array<bool, kGamepadButtonCount> buttons{};
        std::array<float, kGamepadAxisCount> axes{0.0f, 0.0f, 0.0f, 0.0f, -1.0f, -1.0f};
    };

    struct PadState
    {
        uint32_t pressed = 0;                      // bit n set: button Action n is held
        std::array<float, 4> sticks{};             // left x, left y, right x, right y, each -1..1
        bool isPressed(Action action) const { return (pressed >> static_cast<int>(action)) & 1u; }
    };

    // How far an axis must move for an axis binding to count as pressed.
    float axisThreshold(GamepadAxis axis);
    bool isActive(const Binding &binding, const std::function<bool(int)> &keyDown, const GamepadState *gamepad);

    // Every keyboard and controller binding counts; each stick axis takes whichever source is
    // pushed furthest from centre.
    PadState evaluate(const Bindings &bindings, const std::function<bool(int)> &keyDown,
                      const std::vector<GamepadState> &gamepads);
}
