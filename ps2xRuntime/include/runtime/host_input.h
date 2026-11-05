#pragma once

// Keyboard and gamepad state for the guest pad code, using raylib's key, button and axis numbers.
// With parallel-gs the game window belongs to the Vulkan swapchain, so raylib never opens a window
// and this state comes from GLFW instead. The main thread polls; the game thread reads.

#if defined(PS2X_HAS_PARALLEL_GS)

struct GLFWwindow;

namespace hostinput
{
    void attach(GLFWwindow *window); // installs the key callback; call before ImGui's GLFW backend
    void detach();
    void pollGamepads();

    bool IsWindowReady();
    bool IsKeyDown(int key);
    bool IsKeyPressed(int key); // true once per press; main thread only
    bool IsGamepadAvailable(int gamepad);
    const char *GetGamepadName(int gamepad);
    bool IsGamepadButtonDown(int gamepad, int button);
    float GetGamepadAxisMovement(int gamepad, int axis);
    double GetTime();
}

#else

namespace hostinput
{
    inline bool IsWindowReady() { return ::IsWindowReady(); }
    inline bool IsKeyDown(int key) { return ::IsKeyDown(key); }
    inline bool IsKeyPressed(int key) { return ::IsKeyPressed(key); }
    inline bool IsGamepadAvailable(int gamepad) { return ::IsGamepadAvailable(gamepad); }
    inline const char *GetGamepadName(int gamepad) { return ::GetGamepadName(gamepad); }
    inline bool IsGamepadButtonDown(int gamepad, int button) { return ::IsGamepadButtonDown(gamepad, button); }
    inline float GetGamepadAxisMovement(int gamepad, int axis) { return ::GetGamepadAxisMovement(gamepad, axis); }
    inline double GetTime() { return ::GetTime(); }
}

#endif
