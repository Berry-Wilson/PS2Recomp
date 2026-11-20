#include "runtime/ps2_pad.h"
#include "runtime/input_bindings.h"
#include "ps2_host_backend.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;

    // Pad bit for each button Action, in ps2x::input::Action order.
    constexpr uint16_t kActionMasks[ps2x::input::kButtonActionCount] = {
        PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE,
        PAD_L1, PAD_R1, PAD_L2, PAD_R2, PAD_L3, PAD_R3, PAD_START, PAD_SELECT};

    // PS2X_CONTROLS=<file> picks the controls file; otherwise controls.ini in the working directory
    // is used if it exists, and the built-in defaults if not (docs/input-bindings.md).
    const ps2x::input::Bindings &controls()
    {
        static const ps2x::input::Bindings s_bindings = []()
        {
            ps2x::input::Bindings bindings = ps2x::input::Bindings::defaults();
            const char *env = std::getenv("PS2X_CONTROLS");
            const std::string path = env && env[0] ? env : "controls.ini";
            std::vector<std::string> warnings;
            if (ps2x::input::loadFile(path, bindings, &warnings))
            {
                std::fprintf(stderr, "[pad] controls from %s\n", path.c_str());
                for (const std::string &w : warnings)
                    std::fprintf(stderr, "[pad] %s: %s\n", path.c_str(), w.c_str());
            }
            else if (env && env[0])
                std::fprintf(stderr, "[pad] cannot read %s, using the default controls\n", path.c_str());
            return bindings;
        }();
        return s_bindings;
    }

    // raylib numbers gamepad buttons its own way; ps2x::input uses GLFW's standard order.
    constexpr int kRaylibButtonFor[ps2x::input::kGamepadButtonCount] = {
        GAMEPAD_BUTTON_RIGHT_FACE_DOWN,  // South
        GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, // East
        GAMEPAD_BUTTON_RIGHT_FACE_LEFT,  // West
        GAMEPAD_BUTTON_RIGHT_FACE_UP,    // North
        GAMEPAD_BUTTON_LEFT_TRIGGER_1,   // LeftShoulder
        GAMEPAD_BUTTON_RIGHT_TRIGGER_1,  // RightShoulder
        GAMEPAD_BUTTON_MIDDLE_LEFT,      // Back
        GAMEPAD_BUTTON_MIDDLE_RIGHT,     // Start
        GAMEPAD_BUTTON_MIDDLE,           // Guide
        GAMEPAD_BUTTON_LEFT_THUMB,       // LeftStick
        GAMEPAD_BUTTON_RIGHT_THUMB,      // RightStick
        GAMEPAD_BUTTON_LEFT_FACE_UP,     // DpadUp
        GAMEPAD_BUTTON_LEFT_FACE_RIGHT,  // DpadRight
        GAMEPAD_BUTTON_LEFT_FACE_DOWN,   // DpadDown
        GAMEPAD_BUTTON_LEFT_FACE_LEFT,   // DpadLeft
    };
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    // Every connected gamepad and the keyboard count together, so an idle or wrongly mapped pad in
    // slot 0 cannot shut out the others.
    constexpr int kMaxGamepads = 4;
    static bool s_padConnected[kMaxGamepads] = {};
    std::vector<ps2x::input::GamepadState> gamepads;
    for (int pad = 0; pad < kMaxGamepads; ++pad)
    {
        const bool available = IsGamepadAvailable(pad);
        if (available != s_padConnected[pad])
        {
            s_padConnected[pad] = available;
            if (available)
                std::fprintf(stderr, "[pad] gamepad %d connected: %s\n", pad, GetGamepadName(pad));
            else
                std::fprintf(stderr, "[pad] gamepad %d disconnected\n", pad);
        }
        if (!available)
            continue;
        ps2x::input::GamepadState state;
        for (int b = 0; b < ps2x::input::kGamepadButtonCount; ++b)
            state.buttons[b] = IsGamepadButtonDown(pad, kRaylibButtonFor[b]);
        for (int a = 0; a < ps2x::input::kGamepadAxisCount; ++a)
            state.axes[a] = GetGamepadAxisMovement(pad, a); // same order as GAMEPAD_AXIS_*
        gamepads.push_back(state);
    }

    // raylib's KEY_* values are GLFW's key codes, which the bindings use.
    const ps2x::input::PadState pad = ps2x::input::evaluate(
        controls(), [](int key) { return IsKeyDown(key); }, gamepads);

    uint16_t btns = 0xFFFFu;
    for (int i = 0; i < ps2x::input::kButtonActionCount; ++i)
    {
        if (pad.isPressed(static_cast<ps2x::input::Action>(i)))
            btns &= static_cast<uint16_t>(~kActionMasks[i]);
    }
    const float *axes = pad.sticks.data(); // lx, ly, rx, ry in [-1, 1]
    auto stickByte = [](float value) -> uint8_t
    {
        value = std::fmax(-1.0f, std::fmin(1.0f, value));
        return static_cast<uint8_t>(kPadStickCenter + static_cast<int>(value * 127.0f));
    };
    data[6] = stickByte(axes[0]);
    data[7] = stickByte(axes[1]);
    data[4] = stickByte(axes[2]);
    data[5] = stickByte(axes[3]);

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
