#include "MiniTest.h"
#include "runtime/input_bindings.h"

#include <set>
#include <string>

using namespace ps2x::input;

void register_input_bindings_tests()
{
    MiniTest::Case("InputBindings", [](TestCase &tc)
    {
        tc.Run("defaults survive a save and load", [](TestCase &t)
        {
            const Bindings defaults = Bindings::defaults();
            std::vector<std::string> warnings;
            const Bindings loaded = parse(serialize(defaults), &warnings);
            t.IsTrue(loaded == defaults, "parsed defaults should equal the defaults");
            t.Equals(warnings.size(), size_t(0), "no warnings for the written file");
        });

        tc.Run("an empty file gives the defaults", [](TestCase &t)
        {
            t.IsTrue(parse("") == Bindings::defaults(), "empty text means defaults");
        });

        tc.Run("a line replaces only that action's slots", [](TestCase &t)
        {
            const Bindings b = parse("[keyboard]\ncross = Keypad5\n[controller]\ncircle = South, RightTrigger\n");
            const Bindings d = Bindings::defaults();
            const int cross = static_cast<int>(Action::Cross);
            const int circle = static_cast<int>(Action::Circle);
            t.IsTrue(b.keyboard[cross][0] == Binding::key(325), "cross is Keypad 5");
            t.IsTrue(b.keyboard[cross][1].isNone(), "the second cross key is cleared");
            t.IsTrue(b.controller[circle][0] == Binding::button(GamepadButton::South), "circle is South");
            t.IsTrue(b.controller[circle][1] == Binding::axis(GamepadAxis::RightTrigger, 1), "and the right trigger");
            t.IsTrue(b.keyboard[static_cast<int>(Action::Square)] == d.keyboard[static_cast<int>(Action::Square)],
                     "other actions keep their defaults");
        });

        tc.Run("none clears an action", [](TestCase &t)
        {
            const Bindings b = parse("[controller]\nselect = none\n");
            const auto &slots = b.controller[static_cast<int>(Action::Select)];
            t.IsTrue(slots[0].isNone() && slots[1].isNone(), "select has no controller binding");
        });

        tc.Run("bad lines warn and are skipped", [](TestCase &t)
        {
            std::vector<std::string> warnings;
            const Bindings b = parse("[keyboard]\ncross = South\njump = X\n[controller]\nl1 = Space\n[colours]\n", &warnings);
            t.Equals(warnings.size(), size_t(4), "controller input on keyboard, unknown action, key on controller, unknown section");
            t.IsTrue(b.keyboard[static_cast<int>(Action::Cross)][0].isNone(), "the rejected binding leaves the slot empty");
        });

        tc.Run("stick settings parse", [](TestCase &t)
        {
            const Bindings b = parse("[sticks]\nleft_stick = right\nleft_stick_invert_y = yes\nright_stick = none\n");
            t.IsTrue(b.leftStick.source == StickSettings::Source::RightStick, "left stick reads the right stick");
            t.IsTrue(b.leftStick.invertY && !b.leftStick.invertX, "only Y inverted");
            t.IsTrue(b.rightStick.source == StickSettings::Source::None, "right stick reads nothing");
        });

        tc.Run("every key and controller input has a name that reads back", [](TestCase &t)
        {
            std::set<std::string> seen;
            for (int key = 0; key < 349; ++key)
            {
                const std::string id = keyId(key);
                if (id.empty())
                    continue;
                t.IsTrue(seen.insert(id).second, "key names are unique: " + id);
                const auto back = keyFromId(id);
                t.IsTrue(back && *back == key, "key name reads back: " + id);
            }
            for (int i = 0; i < kGamepadButtonCount; ++i)
            {
                const Binding b = Binding::button(static_cast<GamepadButton>(i));
                const auto back = bindingFromId(bindingId(b));
                t.IsTrue(back && *back == b, "button reads back: " + bindingId(b));
            }
            for (int i = 0; i < kGamepadAxisCount; ++i)
            {
                for (int dir : {-1, 1})
                {
                    const Binding b = Binding::axis(static_cast<GamepadAxis>(i), dir);
                    const std::string id = bindingId(b);
                    if (id.empty())
                        continue; // triggers only go one way
                    const auto back = bindingFromId(id);
                    t.IsTrue(back && *back == b, "axis reads back: " + id);
                }
            }
            t.Equals(bindingLabel(Binding::key(340)), std::string("Left Shift"), "labels add spaces");
            t.Equals(bindingLabel(Binding::key(320)), std::string("Keypad 0"), "keypad label");
            t.Equals(bindingLabel(Binding::key(301)), std::string("F12"), "function key label");
        });

        tc.Run("evaluate maps keys, buttons and sticks", [](TestCase &t)
        {
            const Bindings d = Bindings::defaults();
            std::set<int> down = {'X', 'W'};
            auto keyDown = [&down](int key) { return down.count(key) != 0; };

            GamepadState pad;
            pad.buttons[static_cast<int>(GamepadButton::East)] = true;
            pad.axes[static_cast<int>(GamepadAxis::RightTrigger)] = 0.5f;
            pad.axes[static_cast<int>(GamepadAxis::LeftX)] = 0.25f;
            pad.axes[static_cast<int>(GamepadAxis::RightY)] = -0.75f;

            const PadState s = evaluate(d, keyDown, {pad});
            t.IsTrue(s.isPressed(Action::Cross), "X key is Cross");
            t.IsTrue(s.isPressed(Action::Circle), "East is Circle");
            t.IsTrue(s.isPressed(Action::R2), "right trigger past the threshold is R2");
            t.IsFalse(s.isPressed(Action::L2), "resting left trigger is not L2");
            t.Equals(s.sticks[0], 0.25f, "left stick x from the controller");
            t.Equals(s.sticks[1], -1.0f, "W pushes the left stick up fully");
            t.Equals(s.sticks[3], -0.75f, "right stick y from the controller");
        });

        tc.Run("stick settings swap and invert", [](TestCase &t)
        {
            Bindings b = Bindings::defaults();
            b.leftStick = {StickSettings::Source::RightStick, true, false};
            b.rightStick = {StickSettings::Source::None, false, false};
            GamepadState pad;
            pad.axes[static_cast<int>(GamepadAxis::LeftX)] = 0.9f;
            pad.axes[static_cast<int>(GamepadAxis::RightX)] = 0.5f;
            const PadState s = evaluate(b, [](int) { return false; }, {pad});
            t.Equals(s.sticks[0], -0.5f, "PS2 left stick reads the right stick, inverted");
            t.Equals(s.sticks[2], 0.0f, "PS2 right stick reads nothing");
        });

        tc.Run("function keys the runtime uses are reserved", [](TestCase &t)
        {
            t.IsTrue(isReservedKey(290) && isReservedKey(294) && isReservedKey(301), "F1, F5 and F12");
            t.IsFalse(isReservedKey(291), "F2 is free");
        });
    });
}
