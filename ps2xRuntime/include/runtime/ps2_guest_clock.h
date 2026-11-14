#pragma once

#include <cstdint>
#include <ctime>

// The PS2's real-time clock as the game sees it. It starts at the PC's time when the runtime
// starts and then advances with the game's vsyncs, not with the PC's clock, so what a game does
// with it (waits for the seconds to change, seeds from it) is the same on every run of the same
// input. A recording stores its start time ("# clock <seconds>") and a replay starts from it.

// The clock's starting value, as seconds since the Unix epoch.
std::time_t ps2xGuestClockStart();
// The clock after `vsyncTick` vsyncs (60 a second).
std::time_t ps2xGuestClockNow(uint64_t vsyncTick);
