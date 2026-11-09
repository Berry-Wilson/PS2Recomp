#include "runtime/input_bindings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace ps2x::input
{
    namespace
    {
        struct ActionName
        {
            const char *id;
            const char *label;
        };

        constexpr ActionName kActionNames[kActionCount] = {
            {"up", "Up"},
            {"down", "Down"},
            {"left", "Left"},
            {"right", "Right"},
            {"cross", "Cross"},
            {"circle", "Circle"},
            {"square", "Square"},
            {"triangle", "Triangle"},
            {"l1", "L1"},
            {"r1", "R1"},
            {"l2", "L2"},
            {"r2", "R2"},
            {"l3", "L3"},
            {"r3", "R3"},
            {"start", "Start"},
            {"select", "Select"},
            {"left_stick_up", "Left stick up"},
            {"left_stick_down", "Left stick down"},
            {"left_stick_left", "Left stick left"},
            {"left_stick_right", "Left stick right"},
            {"right_stick_up", "Right stick up"},
            {"right_stick_down", "Right stick down"},
            {"right_stick_left", "Right stick left"},
            {"right_stick_right", "Right stick right"},
        };

        struct KeyName
        {
            int code;
            const char *id;
        };

        // GLFW key codes. Letters and digits are their ASCII codes and are handled separately.
        constexpr KeyName kKeyNames[] = {
            {32, "Space"},
            {39, "Apostrophe"},
            {44, "Comma"},
            {45, "Minus"},
            {46, "Period"},
            {47, "Slash"},
            {59, "Semicolon"},
            {61, "Equal"},
            {91, "LeftBracket"},
            {92, "Backslash"},
            {93, "RightBracket"},
            {96, "GraveAccent"},
            {256, "Escape"},
            {257, "Enter"},
            {258, "Tab"},
            {259, "Backspace"},
            {260, "Insert"},
            {261, "Delete"},
            {262, "Right"},
            {263, "Left"},
            {264, "Down"},
            {265, "Up"},
            {266, "PageUp"},
            {267, "PageDown"},
            {268, "Home"},
            {269, "End"},
            {280, "CapsLock"},
            {281, "ScrollLock"},
            {282, "NumLock"},
            {283, "PrintScreen"},
            {284, "Pause"},
            {330, "KeypadDecimal"},
            {331, "KeypadDivide"},
            {332, "KeypadMultiply"},
            {333, "KeypadSubtract"},
            {334, "KeypadAdd"},
            {335, "KeypadEnter"},
            {336, "KeypadEqual"},
            {340, "LeftShift"},
            {341, "LeftControl"},
            {342, "LeftAlt"},
            {343, "LeftSuper"},
            {344, "RightShift"},
            {345, "RightControl"},
            {346, "RightAlt"},
            {347, "RightSuper"},
            {348, "Menu"},
        };
        constexpr int kKeyF1 = 290;
        constexpr int kKeyF25 = 314;
        constexpr int kKeyKeypad0 = 320;
        constexpr int kKeyKeypad9 = 329;

        struct ButtonName
        {
            const char *id;
            const char *label;
        };

        constexpr ButtonName kButtonNames[kGamepadButtonCount] = {
            {"South", "South (A / Cross)"},
            {"East", "East (B / Circle)"},
            {"West", "West (X / Square)"},
            {"North", "North (Y / Triangle)"},
            {"LeftShoulder", "Left shoulder (LB / L1)"},
            {"RightShoulder", "Right shoulder (RB / R1)"},
            {"Back", "Back (View / Share)"},
            {"Start", "Start (Menu / Options)"},
            {"Guide", "Guide (Xbox / PS)"},
            {"LeftStickPress", "Left stick press (LS / L3)"},
            {"RightStickPress", "Right stick press (RS / R3)"},
            {"DpadUp", "D-pad up"},
            {"DpadRight", "D-pad right"},
            {"DpadDown", "D-pad down"},
            {"DpadLeft", "D-pad left"},
        };

        struct AxisName
        {
            GamepadAxis axis;
            int direction;
            const char *id;
            const char *label;
        };

        constexpr AxisName kAxisNames[] = {
            {GamepadAxis::LeftX, -1, "LeftX-", "Left stick left"},
            {GamepadAxis::LeftX, 1, "LeftX+", "Left stick right"},
            {GamepadAxis::LeftY, -1, "LeftY-", "Left stick up"},
            {GamepadAxis::LeftY, 1, "LeftY+", "Left stick down"},
            {GamepadAxis::RightX, -1, "RightX-", "Right stick left"},
            {GamepadAxis::RightX, 1, "RightX+", "Right stick right"},
            {GamepadAxis::RightY, -1, "RightY-", "Right stick up"},
            {GamepadAxis::RightY, 1, "RightY+", "Right stick down"},
            {GamepadAxis::LeftTrigger, 1, "LeftTrigger", "Left trigger (LT / L2)"},
            {GamepadAxis::RightTrigger, 1, "RightTrigger", "Right trigger (RT / R2)"},
        };

        bool equalsIgnoreCase(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            }
            return true;
        }

        std::string_view trim(std::string_view s)
        {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.remove_suffix(1);
            return s;
        }

        // "LeftShift" -> "Left Shift", "Keypad0" -> "Keypad 0", "F12" stays.
        std::string spaced(std::string_view id)
        {
            std::string out;
            for (size_t i = 0; i < id.size(); ++i)
            {
                const unsigned char c = static_cast<unsigned char>(id[i]);
                const bool afterLower = i > 0 && std::islower(static_cast<unsigned char>(id[i - 1]));
                if (afterLower && (std::isupper(c) || std::isdigit(c)))
                    out.push_back(' ');
                out.push_back(static_cast<char>(c));
            }
            return out;
        }

        std::optional<bool> parseBool(std::string_view value)
        {
            if (equalsIgnoreCase(value, "yes") || equalsIgnoreCase(value, "true") || value == "1" ||
                equalsIgnoreCase(value, "on"))
                return true;
            if (equalsIgnoreCase(value, "no") || equalsIgnoreCase(value, "false") || value == "0" ||
                equalsIgnoreCase(value, "off"))
                return false;
            return std::nullopt;
        }

        const char *sourceId(StickSettings::Source source)
        {
            switch (source)
            {
            case StickSettings::Source::LeftStick:
                return "left";
            case StickSettings::Source::RightStick:
                return "right";
            default:
                return "none";
            }
        }
    }

    Bindings Bindings::defaults()
    {
        Bindings b;
        auto key = [&b](Action a, int first, int second = 0)
        {
            b.keyboard[static_cast<int>(a)][0] = Binding::key(first);
            if (second)
                b.keyboard[static_cast<int>(a)][1] = Binding::key(second);
        };
        auto pad = [&b](Action a, Binding binding)
        { b.controller[static_cast<int>(a)][0] = binding; };

        // Keyboard: D-pad on the arrows, left stick on WASD, right stick on IJKL.
        key(Action::Up, 265);
        key(Action::Down, 264);
        key(Action::Left, 263);
        key(Action::Right, 262);
        key(Action::Cross, 'X', 32);      // X, Space
        key(Action::Circle, 'C');
        key(Action::Square, 'Z', 320);    // Z, Keypad 0
        key(Action::Triangle, 'V', 321);  // V, Keypad 1
        key(Action::L1, 'Q');
        key(Action::R1, 'E');
        key(Action::L2, 340);             // Left Shift
        key(Action::R2, 344);             // Right Shift
        key(Action::L3, 341);             // Left Control
        key(Action::R3, 345);             // Right Control
        key(Action::Start, 257);          // Enter
        key(Action::Select, 258);         // Tab
        key(Action::LeftStickUp, 'W');
        key(Action::LeftStickDown, 'S');
        key(Action::LeftStickLeft, 'A');
        key(Action::LeftStickRight, 'D');
        key(Action::RightStickUp, 'I');
        key(Action::RightStickDown, 'K');
        key(Action::RightStickLeft, 'J');
        key(Action::RightStickRight, 'L');

        // Controller: same positions as a PS2 pad; the sticks come from StickSettings.
        pad(Action::Up, Binding::button(GamepadButton::DpadUp));
        pad(Action::Down, Binding::button(GamepadButton::DpadDown));
        pad(Action::Left, Binding::button(GamepadButton::DpadLeft));
        pad(Action::Right, Binding::button(GamepadButton::DpadRight));
        pad(Action::Cross, Binding::button(GamepadButton::South));
        pad(Action::Circle, Binding::button(GamepadButton::East));
        pad(Action::Square, Binding::button(GamepadButton::West));
        pad(Action::Triangle, Binding::button(GamepadButton::North));
        pad(Action::L1, Binding::button(GamepadButton::LeftShoulder));
        pad(Action::R1, Binding::button(GamepadButton::RightShoulder));
        pad(Action::L2, Binding::axis(GamepadAxis::LeftTrigger, 1));
        pad(Action::R2, Binding::axis(GamepadAxis::RightTrigger, 1));
        pad(Action::L3, Binding::button(GamepadButton::LeftStick));
        pad(Action::R3, Binding::button(GamepadButton::RightStick));
        pad(Action::Start, Binding::button(GamepadButton::Start));
        pad(Action::Select, Binding::button(GamepadButton::Back));
        return b;
    }

    const char *actionId(Action action)
    {
        const int i = static_cast<int>(action);
        return i >= 0 && i < kActionCount ? kActionNames[i].id : "";
    }

    const char *actionLabel(Action action)
    {
        const int i = static_cast<int>(action);
        return i >= 0 && i < kActionCount ? kActionNames[i].label : "";
    }

    std::optional<Action> actionFromId(std::string_view id)
    {
        for (int i = 0; i < kActionCount; ++i)
        {
            if (equalsIgnoreCase(id, kActionNames[i].id))
                return static_cast<Action>(i);
        }
        return std::nullopt;
    }

    std::string keyId(int glfwKey)
    {
        if ((glfwKey >= '0' && glfwKey <= '9') || (glfwKey >= 'A' && glfwKey <= 'Z'))
            return std::string(1, static_cast<char>(glfwKey));
        if (glfwKey >= kKeyF1 && glfwKey <= kKeyF25)
            return "F" + std::to_string(glfwKey - kKeyF1 + 1);
        if (glfwKey >= kKeyKeypad0 && glfwKey <= kKeyKeypad9)
            return "Keypad" + std::to_string(glfwKey - kKeyKeypad0);
        for (const KeyName &k : kKeyNames)
        {
            if (k.code == glfwKey)
                return k.id;
        }
        return {};
    }

    std::optional<int> keyFromId(std::string_view id)
    {
        if (id.size() == 1)
        {
            const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(id[0])));
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))
                return static_cast<int>(c);
        }
        if (id.size() >= 2 && (id[0] == 'F' || id[0] == 'f') &&
            std::all_of(id.begin() + 1, id.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
        {
            const int n = std::stoi(std::string(id.substr(1)));
            if (n >= 1 && n <= 25)
                return kKeyF1 + n - 1;
        }
        if (id.size() == 7 && equalsIgnoreCase(id.substr(0, 6), "Keypad") &&
            std::isdigit(static_cast<unsigned char>(id[6])))
            return kKeyKeypad0 + (id[6] - '0');
        for (const KeyName &k : kKeyNames)
        {
            if (equalsIgnoreCase(id, k.id))
                return k.code;
        }
        return std::nullopt;
    }

    const char *gamepadButtonId(GamepadButton button)
    {
        const int i = static_cast<int>(button);
        return i >= 0 && i < kGamepadButtonCount ? kButtonNames[i].id : "";
    }

    const char *gamepadButtonLabel(GamepadButton button)
    {
        const int i = static_cast<int>(button);
        return i >= 0 && i < kGamepadButtonCount ? kButtonNames[i].label : "";
    }

    std::string bindingId(const Binding &binding)
    {
        switch (binding.kind)
        {
        case Binding::Kind::Key:
            return keyId(binding.code);
        case Binding::Kind::Button:
            return gamepadButtonId(static_cast<GamepadButton>(binding.code));
        case Binding::Kind::Axis:
            for (const AxisName &a : kAxisNames)
            {
                if (static_cast<int>(a.axis) == binding.code && a.direction == binding.direction)
                    return a.id;
            }
            return {};
        default:
            return {};
        }
    }

    std::string bindingLabel(const Binding &binding)
    {
        switch (binding.kind)
        {
        case Binding::Kind::Key:
        {
            const std::string id = keyId(binding.code);
            return id.empty() ? "Key " + std::to_string(binding.code) : spaced(id);
        }
        case Binding::Kind::Button:
            return gamepadButtonLabel(static_cast<GamepadButton>(binding.code));
        case Binding::Kind::Axis:
            for (const AxisName &a : kAxisNames)
            {
                if (static_cast<int>(a.axis) == binding.code && a.direction == binding.direction)
                    return a.label;
            }
            return {};
        default:
            return {};
        }
    }

    std::optional<Binding> bindingFromId(std::string_view id)
    {
        id = trim(id);
        if (id.empty() || equalsIgnoreCase(id, "none"))
            return Binding{};
        for (int i = 0; i < kGamepadButtonCount; ++i)
        {
            if (equalsIgnoreCase(id, kButtonNames[i].id))
                return Binding::button(static_cast<GamepadButton>(i));
        }
        for (const AxisName &a : kAxisNames)
        {
            if (equalsIgnoreCase(id, a.id))
                return Binding::axis(a.axis, a.direction);
        }
        if (const auto key = keyFromId(id))
            return Binding::key(*key);
        return std::nullopt;
    }

    bool isReservedKey(int glfwKey)
    {
        return glfwKey == kKeyF1 || glfwKey == kKeyF1 + 4 || glfwKey == kKeyF1 + 11;
    }

    std::string serialize(const Bindings &bindings)
    {
        std::ostringstream out;
        out << "# PS2 pad controls. Each line is a pad button or stick direction, then up to two\n"
               "# bindings separated by commas, or none. Missing lines keep their defaults.\n";
        auto section = [&](const char *name, auto &slots)
        {
            out << "\n[" << name << "]\n";
            for (int i = 0; i < kActionCount; ++i)
            {
                std::string value;
                for (const Binding &b : slots[i])
                {
                    if (b.isNone())
                        continue;
                    if (!value.empty())
                        value += ", ";
                    value += bindingId(b);
                }
                out << actionId(static_cast<Action>(i)) << " = " << (value.empty() ? "none" : value) << "\n";
            }
        };
        section("keyboard", bindings.keyboard);
        section("controller", bindings.controller);

        out << "\n# Where each PS2 stick reads the controller from: left, right or none.\n[sticks]\n";
        auto stick = [&](const char *name, const StickSettings &s)
        {
            out << name << " = " << sourceId(s.source) << "\n";
            out << name << "_invert_x = " << (s.invertX ? "yes" : "no") << "\n";
            out << name << "_invert_y = " << (s.invertY ? "yes" : "no") << "\n";
        };
        stick("left_stick", bindings.leftStick);
        stick("right_stick", bindings.rightStick);
        return out.str();
    }

    Bindings parse(std::string_view text, std::vector<std::string> *warnings)
    {
        Bindings bindings = Bindings::defaults();
        enum class Section
        {
            None,
            Keyboard,
            Controller,
            Sticks
        } section = Section::None;

        auto warn = [warnings](int line, const std::string &message)
        {
            if (warnings)
                warnings->push_back("line " + std::to_string(line) + ": " + message);
        };

        int lineNumber = 0;
        size_t pos = 0;
        while (pos <= text.size())
        {
            const size_t end = std::min(text.find('\n', pos), text.size());
            std::string_view line = trim(text.substr(pos, end - pos));
            pos = end + 1;
            ++lineNumber;
            if (line.empty() || line[0] == '#' || line[0] == ';')
            {
                if (end == text.size())
                    break;
                continue;
            }
            if (line.front() == '[' && line.back() == ']')
            {
                const std::string_view name = trim(line.substr(1, line.size() - 2));
                if (equalsIgnoreCase(name, "keyboard"))
                    section = Section::Keyboard;
                else if (equalsIgnoreCase(name, "controller"))
                    section = Section::Controller;
                else if (equalsIgnoreCase(name, "sticks"))
                    section = Section::Sticks;
                else
                {
                    section = Section::None;
                    warn(lineNumber, "unknown section [" + std::string(name) + "]");
                }
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string_view::npos)
            {
                warn(lineNumber, "expected name = value");
                continue;
            }
            const std::string_view name = trim(line.substr(0, eq));
            const std::string_view value = trim(line.substr(eq + 1));

            if (section == Section::Sticks)
            {
                StickSettings *stick = nullptr;
                std::string_view field = name;
                if (field.substr(0, 10) == "left_stick")
                {
                    stick = &bindings.leftStick;
                    field.remove_prefix(10);
                }
                else if (field.substr(0, 11) == "right_stick")
                {
                    stick = &bindings.rightStick;
                    field.remove_prefix(11);
                }
                if (!stick)
                {
                    warn(lineNumber, "unknown stick setting " + std::string(name));
                    continue;
                }
                if (field.empty())
                {
                    if (equalsIgnoreCase(value, "left"))
                        stick->source = StickSettings::Source::LeftStick;
                    else if (equalsIgnoreCase(value, "right"))
                        stick->source = StickSettings::Source::RightStick;
                    else if (equalsIgnoreCase(value, "none"))
                        stick->source = StickSettings::Source::None;
                    else
                        warn(lineNumber, "stick source must be left, right or none");
                }
                else if (field == "_invert_x" || field == "_invert_y")
                {
                    const auto flag = parseBool(value);
                    if (!flag)
                        warn(lineNumber, "expected yes or no");
                    else if (field == "_invert_x")
                        stick->invertX = *flag;
                    else
                        stick->invertY = *flag;
                }
                else
                    warn(lineNumber, "unknown stick setting " + std::string(name));
                continue;
            }

            if (section == Section::None)
            {
                warn(lineNumber, "setting outside a section");
                continue;
            }
            const auto action = actionFromId(name);
            if (!action)
            {
                warn(lineNumber, "unknown pad input " + std::string(name));
                continue;
            }
            const bool keyboard = section == Section::Keyboard;
            std::vector<Binding> parsed;
            size_t start = 0;
            while (start <= value.size())
            {
                const size_t comma = std::min(value.find(',', start), value.size());
                const std::string_view token = trim(value.substr(start, comma - start));
                start = comma + 1;
                const auto binding = bindingFromId(token);
                if (!binding)
                    warn(lineNumber, "unknown binding " + std::string(token));
                else if (!binding->isNone() && binding->isKeyboard() != keyboard)
                    warn(lineNumber, std::string(token) + (keyboard ? " is not a key" : " is not a controller input"));
                else if (!binding->isNone())
                    parsed.push_back(*binding);
                if (comma == value.size())
                    break;
            }
            const size_t slots = keyboard ? kKeyboardSlots : kControllerSlots;
            if (parsed.size() > slots)
                warn(lineNumber, "only the first " + std::to_string(slots) + " bindings are used");
            for (size_t i = 0; i < slots; ++i)
            {
                const Binding b = i < parsed.size() ? parsed[i] : Binding{};
                if (keyboard)
                    bindings.keyboard[static_cast<int>(*action)][i] = b;
                else
                    bindings.controller[static_cast<int>(*action)][i] = b;
            }
        }
        return bindings;
    }

    bool loadFile(const std::string &path, Bindings &out, std::vector<std::string> *warnings)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;
        std::ostringstream text;
        text << file.rdbuf();
        out = parse(text.str(), warnings);
        return true;
    }

    bool saveFile(const std::string &path, const Bindings &bindings)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file << serialize(bindings);
        return static_cast<bool>(file);
    }

    float axisThreshold(GamepadAxis axis)
    {
        // Triggers rest at -1; 0.1 is just past half way, as GLFW's own trigger buttons use.
        return axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger ? 0.1f : 0.5f;
    }

    bool isActive(const Binding &binding, const std::function<bool(int)> &keyDown, const GamepadState *gamepad)
    {
        switch (binding.kind)
        {
        case Binding::Kind::Key:
            return keyDown && keyDown(binding.code);
        case Binding::Kind::Button:
            return gamepad && binding.code >= 0 && binding.code < kGamepadButtonCount && gamepad->buttons[binding.code];
        case Binding::Kind::Axis:
            return gamepad && binding.code >= 0 && binding.code < kGamepadAxisCount &&
                   gamepad->axes[binding.code] * static_cast<float>(binding.direction) >
                       axisThreshold(static_cast<GamepadAxis>(binding.code));
        default:
            return false;
        }
    }

    PadState evaluate(const Bindings &bindings, const std::function<bool(int)> &keyDown,
                      const std::vector<GamepadState> &gamepads)
    {
        PadState state;
        std::array<bool, kActionCount> active{};
        for (int i = 0; i < kActionCount; ++i)
        {
            for (const Binding &b : bindings.keyboard[i])
                active[i] = active[i] || isActive(b, keyDown, nullptr);
            for (const GamepadState &pad : gamepads)
            {
                for (const Binding &b : bindings.controller[i])
                    active[i] = active[i] || isActive(b, nullptr, &pad);
            }
        }
        for (int i = 0; i < kButtonActionCount; ++i)
        {
            if (active[i])
                state.pressed |= 1u << i;
        }

        auto merge = [&state](int index, float value)
        {
            if (std::fabs(value) > std::fabs(state.sticks[index]))
                state.sticks[index] = value;
        };
        auto digital = [&active](Action negative, Action positive)
        {
            return (active[static_cast<int>(positive)] ? 1.0f : 0.0f) - (active[static_cast<int>(negative)] ? 1.0f : 0.0f);
        };
        merge(0, digital(Action::LeftStickLeft, Action::LeftStickRight));
        merge(1, digital(Action::LeftStickUp, Action::LeftStickDown));
        merge(2, digital(Action::RightStickLeft, Action::RightStickRight));
        merge(3, digital(Action::RightStickUp, Action::RightStickDown));

        auto stick = [&](const StickSettings &settings, int xIndex)
        {
            if (settings.source == StickSettings::Source::None)
                return;
            const bool left = settings.source == StickSettings::Source::LeftStick;
            const int axisX = static_cast<int>(left ? GamepadAxis::LeftX : GamepadAxis::RightX);
            const int axisY = static_cast<int>(left ? GamepadAxis::LeftY : GamepadAxis::RightY);
            for (const GamepadState &pad : gamepads)
            {
                merge(xIndex, settings.invertX ? -pad.axes[axisX] : pad.axes[axisX]);
                merge(xIndex + 1, settings.invertY ? -pad.axes[axisY] : pad.axes[axisY]);
            }
        };
        stick(bindings.leftStick, 0);
        stick(bindings.rightStick, 2);

        for (float &v : state.sticks)
            v = std::clamp(v, -1.0f, 1.0f);
        return state;
    }
}
