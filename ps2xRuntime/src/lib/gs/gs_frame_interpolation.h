#pragma once

#include <cstdint>
#include <vector>

// Frame interpolation on the GIF stream (experimental, PS2X_FRAME_INTERPOLATION=1).
//
// A game frame is the GIF packets it sent, in order. An in-between frame is the newer frame's packets
// with every vertex moved halfway back to where the same vertex was in the older frame. Vertices are
// paired by pairing their batches (one GIF tag and its data): a batch in the newer frame goes with
// the next unused batch of the older frame that has the same GIF tag, on the same path, drawn with
// the same texture. A batch with no partner, or whose vertices moved further than anything does in
// one frame, is drawn where the newer frame has it.
namespace ps2x_interp
{
    struct Packet
    {
        uint32_t path;
        uint32_t offset; // into Frame::data
        uint32_t size;
    };

    struct Frame
    {
        std::vector<uint8_t> data;
        std::vector<Packet> packets;

        void add(uint32_t path, const void *bytes, uint32_t size);
        bool empty() const { return packets.empty(); }
    };

    struct Stats
    {
        uint32_t batches = 0;  // batches with vertices in the newer frame
        uint32_t paired = 0;   // of those, found in the older frame
        uint32_t moved = 0;    // of those, changed
        uint32_t rejected = 0; // paired, but moved too far or differ in kind
        uint32_t frameFirst = 0xFFFFFFFFu; // FBP of the first and last FRAME write, for diagnostics
        uint32_t frameLast = 0xFFFFFFFFu;
    };

    // The packet writes the GS's FINISH register, which this game does once at the end of a frame.
    bool writesFinish(const void *bytes, uint32_t size);

    // out becomes the frame halfway between older and newer.
    void buildMiddle(const Frame &older, const Frame &newer, Frame &out, Stats &stats);
}
