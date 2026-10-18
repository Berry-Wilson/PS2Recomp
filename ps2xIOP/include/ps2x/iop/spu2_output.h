#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2x::iop
{
    // Sample rate of the SPU2's mixed output.
    inline constexpr unsigned kSpu2SampleRate = 48000u;

    // Pulls up to `frames` interleaved stereo 16-bit frames produced by the emulated SPU2. Safe to
    // call from an audio callback thread. Returns the number of frames written; callers pad the
    // rest (underrun) with silence.
    size_t readSpu2Frames(int16_t *destination, size_t frames);
}
