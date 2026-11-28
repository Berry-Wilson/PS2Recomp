#pragma once

#include <chrono>

// The refreshes of the display the game window is on, so the game's frames can be lined up with them.
//
// With vsync (PS2X_VSYNC, on by default) each picture is shown at the display's next refresh. When the
// display refreshes at 60 or 120 Hz (within 2%, 59.94 included), the guest's 60 Hz
// vertical blank is also moved onto the display's refreshes: every frame then reaches the screen the
// same time after a refresh, and none lands so close to one that a millisecond decides whether it is
// shown at that refresh or the next. The game then runs at the display's rate, 59.94 instead of 60 on
// such a screen. At other rates (144, 165, 240, 280 Hz) the guest keeps its own 60 Hz clock.
namespace ps2x::display_clock
{
    // PS2X_VSYNC: anything but "0" (the default is on).
    bool vsyncEnabled();

    // Starts following the display under `nativeWindow` (an HWND); it follows the window to another
    // display. Does nothing when vsync is off, or on platforms without support.
    void start(void *nativeWindow);
    void stop();

    // The time the guest's next vertical blank should happen, given the time its own 60 Hz clock
    // picked (`wanted`). When the display is followed, that is just after the display refresh nearest
    // to `wanted`, and never before `now`. Otherwise `wanted` itself.
    std::chrono::steady_clock::time_point align(std::chrono::steady_clock::time_point wanted,
                                                std::chrono::steady_clock::time_point now);
}
