#include "raylib.h"
#include "runtime/host_input.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <array>
#include <atomic>
#include <string>

// Same semantics as raylib's desktop GLFW platform (rcore_desktop_glfw.c), which is what the pad
// code ran on before, except that Escape does nothing: key state from the key callback, and gamepads polled
// through glfwGetGamepadState with L2/R2 reported as buttons once their axis passes 0.1.
namespace
{
    constexpr int kKeyCount = GLFW_KEY_LAST + 1;
    constexpr int kMaxGamepads = 4;
    constexpr int kButtonCount = 32;
    constexpr int kAxisCount = GLFW_GAMEPAD_AXIS_LAST + 1;

    struct GamepadState
    {
        std::atomic<bool> ready{false};
        std::array<std::atomic<bool>, kButtonCount> buttons{};
        std::array<std::atomic<float>, kAxisCount> axes{};
        std::string name;
    };

    std::atomic<bool> s_attached{false};
    std::array<std::atomic<bool>, kKeyCount> s_keyDown{};
    std::array<std::atomic<bool>, kKeyCount> s_keyPressed{};
    std::array<GamepadState, kMaxGamepads> s_gamepads{};

    void keyCallback(GLFWwindow *, int key, int, int action, int)
    {
        if (key < 0 || key >= kKeyCount)
            return;
        if (action == GLFW_RELEASE)
        {
            s_keyDown[key].store(false, std::memory_order_relaxed);
        }
        else if (action == GLFW_PRESS)
        {
            s_keyDown[key].store(true, std::memory_order_relaxed);
            s_keyPressed[key].store(true, std::memory_order_relaxed);
        }
    }

    int raylibButton(int glfwButton)
    {
        switch (glfwButton)
        {
        case GLFW_GAMEPAD_BUTTON_Y: return GAMEPAD_BUTTON_RIGHT_FACE_UP;
        case GLFW_GAMEPAD_BUTTON_B: return GAMEPAD_BUTTON_RIGHT_FACE_RIGHT;
        case GLFW_GAMEPAD_BUTTON_A: return GAMEPAD_BUTTON_RIGHT_FACE_DOWN;
        case GLFW_GAMEPAD_BUTTON_X: return GAMEPAD_BUTTON_RIGHT_FACE_LEFT;
        case GLFW_GAMEPAD_BUTTON_LEFT_BUMPER: return GAMEPAD_BUTTON_LEFT_TRIGGER_1;
        case GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER: return GAMEPAD_BUTTON_RIGHT_TRIGGER_1;
        case GLFW_GAMEPAD_BUTTON_BACK: return GAMEPAD_BUTTON_MIDDLE_LEFT;
        case GLFW_GAMEPAD_BUTTON_GUIDE: return GAMEPAD_BUTTON_MIDDLE;
        case GLFW_GAMEPAD_BUTTON_START: return GAMEPAD_BUTTON_MIDDLE_RIGHT;
        case GLFW_GAMEPAD_BUTTON_DPAD_UP: return GAMEPAD_BUTTON_LEFT_FACE_UP;
        case GLFW_GAMEPAD_BUTTON_DPAD_RIGHT: return GAMEPAD_BUTTON_LEFT_FACE_RIGHT;
        case GLFW_GAMEPAD_BUTTON_DPAD_DOWN: return GAMEPAD_BUTTON_LEFT_FACE_DOWN;
        case GLFW_GAMEPAD_BUTTON_DPAD_LEFT: return GAMEPAD_BUTTON_LEFT_FACE_LEFT;
        case GLFW_GAMEPAD_BUTTON_LEFT_THUMB: return GAMEPAD_BUTTON_LEFT_THUMB;
        case GLFW_GAMEPAD_BUTTON_RIGHT_THUMB: return GAMEPAD_BUTTON_RIGHT_THUMB;
        default: return -1;
        }
    }
}

namespace hostinput
{
    void attach(GLFWwindow *window)
    {
        glfwSetKeyCallback(window, keyCallback);
        s_attached.store(true, std::memory_order_release);
    }

    void detach()
    {
        s_attached.store(false, std::memory_order_release);
    }

    void pollGamepads()
    {
        for (int pad = 0; pad < kMaxGamepads; ++pad)
        {
            GamepadState &out = s_gamepads[pad];
            const bool present = glfwJoystickPresent(pad) == GLFW_TRUE;
            if (present && !out.ready.load(std::memory_order_relaxed))
            {
                const char *name = glfwGetJoystickName(pad);
                out.name = name ? name : "";
            }
            if (present)
            {
                GLFWgamepadstate state{};
                glfwGetGamepadState(pad, &state);
                for (int k = 0; k <= GLFW_GAMEPAD_BUTTON_LAST; ++k)
                {
                    const int button = raylibButton(k);
                    if (button >= 0)
                        out.buttons[button].store(state.buttons[k] == GLFW_PRESS, std::memory_order_relaxed);
                }
                for (int k = 0; k < kAxisCount; ++k)
                    out.axes[k].store(state.axes[k], std::memory_order_relaxed);
                out.buttons[GAMEPAD_BUTTON_LEFT_TRIGGER_2].store(state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0.1f, std::memory_order_relaxed);
                out.buttons[GAMEPAD_BUTTON_RIGHT_TRIGGER_2].store(state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.1f, std::memory_order_relaxed);
            }
            out.ready.store(present, std::memory_order_release);
        }
    }

    bool IsWindowReady()
    {
        return s_attached.load(std::memory_order_acquire);
    }

    bool IsKeyDown(int key)
    {
        return key >= 0 && key < kKeyCount && s_keyDown[key].load(std::memory_order_relaxed);
    }

    bool IsKeyPressed(int key)
    {
        return key >= 0 && key < kKeyCount && s_keyPressed[key].exchange(false, std::memory_order_relaxed);
    }

    bool IsGamepadAvailable(int gamepad)
    {
        return gamepad >= 0 && gamepad < kMaxGamepads && s_gamepads[gamepad].ready.load(std::memory_order_acquire);
    }

    const char *GetGamepadName(int gamepad)
    {
        return IsGamepadAvailable(gamepad) ? s_gamepads[gamepad].name.c_str() : "";
    }

    bool IsGamepadButtonDown(int gamepad, int button)
    {
        return IsGamepadAvailable(gamepad) && button >= 0 && button < kButtonCount &&
               s_gamepads[gamepad].buttons[button].load(std::memory_order_relaxed);
    }

    float GetGamepadAxisMovement(int gamepad, int axis)
    {
        const float idle = (axis == GAMEPAD_AXIS_LEFT_TRIGGER || axis == GAMEPAD_AXIS_RIGHT_TRIGGER) ? -1.0f : 0.0f;
        if (!IsGamepadAvailable(gamepad) || axis < 0 || axis >= kAxisCount)
            return idle;
        return s_gamepads[gamepad].axes[axis].load(std::memory_order_relaxed);
    }

    double GetTime()
    {
        return glfwGetTime();
    }
}
