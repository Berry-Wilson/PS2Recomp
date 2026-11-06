#pragma once

#include <cstdint>

// Lets game code adjust GIF packets before they reach the GS, for example to change screen layout.
class GifRewriter
{
public:
    virtual ~GifRewriter() = default;
    // Called for every packet. Packets on paths this returns false for are not copied or rewritten.
    virtual bool wantsPacket(uint32_t path) = 0;
    // A private copy of the packet: rewrite it in place.
    virtual void rewrite(uint32_t path, uint8_t *data, uint32_t sizeBytes) = 0;
    // Called before each frame is presented. May run alongside wantsPacket and rewrite.
    virtual void endFrame() = 0;
};

// The rewriter must outlive the GS. nullptr, the default, turns rewriting off.
void ps2xSetGifRewriter(GifRewriter *rewriter);
