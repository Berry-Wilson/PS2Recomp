#include <cstdlib>
#include <cstdio>
// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
#include <cstring>

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;

    uint32_t pendingGifImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadBytes = 0u;
            if (flg == 0u) // PACKED
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16ull;
            }
            else if (flg == 1u) // REGLIST, padded to a quadword
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 8ull;
                payloadBytes = (payloadBytes + 15ull) & ~15ull;
            }
            else if (flg == kGifFmtImage)
            {
                payloadBytes = static_cast<uint64_t>(nloop) * 16ull;
                const uint64_t availableBytes = sizeBytes - offset;
                if (payloadBytes > availableBytes)
                {
                    return static_cast<uint32_t>((payloadBytes - availableBytes) / 16ull);
                }
            }
            else
            {
                return 0u;
            }

            if (payloadBytes > static_cast<uint64_t>(sizeBytes - offset))
                return 0u;
            offset += static_cast<uint32_t>(payloadBytes);
        }

        return 0u;
    }

    // Quadwords still owed to a VIF1 DIRECT/DIRECTHL whose payload ran past the end of the data
    // handed to processVIF1Data. Those bytes arrive raw at the start of the next call. Kept
    // separate from PS2Memory::m_vif1PendingPath2ImageQwc, which is GIF-level state (an IMAGE
    // GIFtag still waiting for data) and must only be fed from later DIRECT payloads.
    uint32_t s_vif1DirectRemainQwc = 0u;
    bool s_vif1DirectRemainHl = false;
}

void resetVif1DirectContinuation()
{
    s_vif1DirectRemainQwc = 0u;
    s_vif1DirectRemainHl = false;
}

static void feedVif1Path2(PS2Memory &mem, const uint8_t *data, uint32_t qwCount, bool directHl)
{
    // An IMAGE transfer started by an earlier DIRECT continues with this payload: the GIF keeps
    // its NLOOP state per path across VIF DIRECT commands, so the data is passed on raw and the
    // GS side (CPU decoder or PCSX2's GS) continues the transfer.
    uint32_t consumedQw = 0u;
    if (mem.m_vif1PendingPath2ImageQwc != 0u && qwCount != 0u)
    {
        const uint32_t chunkQw = std::min<uint32_t>(mem.m_vif1PendingPath2ImageQwc, qwCount);
        mem.submitGifPacket(GifPathId::Path2, data, chunkQw * 16u, true, mem.m_vif1PendingPath2DirectHl || directHl);
        mem.m_vif1PendingPath2ImageQwc -= chunkQw;
        if (mem.m_vif1PendingPath2ImageQwc == 0u)
            mem.m_vif1PendingPath2DirectHl = false;
        consumedQw = chunkQw;
    }

    if (qwCount > consumedQw)
    {
        const uint8_t *packet = data + consumedQw * 16u;
        const uint32_t packetBytes = (qwCount - consumedQw) * 16u;
        mem.submitGifPacket(GifPathId::Path2, packet, packetBytes, true, directHl);
        const uint32_t pendingImageQw = pendingGifImageQwc(packet, packetBytes);
        if (pendingImageQw != 0u)
        {
            mem.m_vif1PendingPath2ImageQwc = pendingImageQw;
            mem.m_vif1PendingPath2DirectHl = directHl;
        }
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    // Hardware fills the lanes V2/V3 lack: V2 writes x,y,x,y; V3 W gets the next element.
                    if (components == 2)
                    {
                        lanes[2] = lanes[0];
                        lanes[3] = lanes[1];
                    }
                    else if (components == 3 && srcVec + 16u <= data + sizeBytes)
                    {
                        std::memcpy(&lanes[3], srcVec + 12u, sizeof(uint32_t));
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}


namespace
{
    // Templated VIF UNPACK for CL >= WL (every write consumes one source vector), any mode, any
    // write mask. Same semantics as the general loop in processVIF1Data: V2 writes x,y,x,y; V3's W
    // takes the next source element (or keeps the lane at the end of the data); masks select data
    // (plus ROW in offset/difference mode), ROW, COL[min(cycle,3)] or keep the lane.
    template <uint32_t VN, uint32_t VL>
    inline uint32_t vifElement(const uint8_t *e, bool zeroExtend)
    {
        if constexpr (VL == 0u)
        {
            uint32_t w;
            std::memcpy(&w, e, sizeof(w));
            return w;
        }
        else if constexpr (VL == 1u)
        {
            uint16_t h;
            std::memcpy(&h, e, sizeof(h));
            return zeroExtend ? static_cast<uint32_t>(h) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(h)));
        }
        else
        {
            return zeroExtend ? static_cast<uint32_t>(*e) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(*e)));
        }
    }

    template <uint32_t VN, uint32_t VL>
    void vifUnpackFast(uint8_t *vuData, const uint8_t *src, const uint8_t *dataEnd, uint32_t writeCount, uint32_t vuAddr,
                       uint32_t cl, uint32_t wl, uint32_t mode, bool maskEnable, uint32_t mask, uint32_t (&row)[4],
                       const uint32_t (&col)[4], bool zeroExtend)
    {
        constexpr uint32_t elemBytes = 4u >> VL;
        constexpr uint32_t bytesPerVector = (VN + 1u) * elemBytes;
        // spec[cycle][field]: 0 data, 1 row, 2 col, 3 keep.
        uint8_t spec[4][4]{};
        bool anyKeep = false;
        if (maskEnable)
            for (uint32_t c = 0; c < 4u; ++c)
                for (uint32_t f = 0; f < 4u; ++f)
                {
                    spec[c][f] = static_cast<uint8_t>((mask >> ((c * 4u + f) * 2u)) & 3u);
                    anyKeep |= spec[c][f] == 3u;
                }
        const bool plain = !maskEnable && mode == 0u;
        uint32_t cyclePos = 0u;
        uint32_t blockBase = vuAddr;
        for (uint32_t w = 0; w < writeCount; ++w)
        {
            const uint32_t destOff = ((blockBase + cyclePos) & 0x3FFu) * 16u;
            const uint8_t *v = src + w * bytesPerVector;
            uint32_t out[4];
            if constexpr (VN == 0u)
            {
                out[0] = out[1] = out[2] = out[3] = vifElement<VN, VL>(v, zeroExtend);
            }
            else if constexpr (VN == 1u)
            {
                out[0] = out[2] = vifElement<VN, VL>(v, zeroExtend);
                out[1] = out[3] = vifElement<VN, VL>(v + elemBytes, zeroExtend);
            }
            else if constexpr (VN == 2u)
            {
                out[0] = vifElement<VN, VL>(v, zeroExtend);
                out[1] = vifElement<VN, VL>(v + elemBytes, zeroExtend);
                out[2] = vifElement<VN, VL>(v + 2u * elemBytes, zeroExtend);
                const uint8_t *next = v + 3u * elemBytes;
                if (next + elemBytes <= dataEnd)
                    out[3] = vifElement<VN, VL>(next, zeroExtend);
                else
                    std::memcpy(&out[3], vuData + destOff + 12u, sizeof(uint32_t));
            }
            else
            {
                out[0] = vifElement<VN, VL>(v, zeroExtend);
                out[1] = vifElement<VN, VL>(v + elemBytes, zeroExtend);
                out[2] = vifElement<VN, VL>(v + 2u * elemBytes, zeroExtend);
                out[3] = vifElement<VN, VL>(v + 3u * elemBytes, zeroExtend);
            }

            if (plain)
            {
                std::memcpy(vuData + destOff, out, sizeof(out));
            }
            else
            {
                uint32_t lanes[4];
                const uint32_t c = cyclePos > 3u ? 3u : cyclePos;
                if (anyKeep)
                    std::memcpy(lanes, vuData + destOff, sizeof(lanes));
                for (uint32_t f = 0; f < 4u; ++f)
                {
                    switch (spec[c][f])
                    {
                    case 0u:
                    {
                        uint32_t value = out[f];
                        if (mode == 1u || mode == 2u)
                        {
                            value += row[f];
                            if (mode == 2u)
                                row[f] = value;
                        }
                        lanes[f] = value;
                        break;
                    }
                    case 1u:
                        lanes[f] = row[f];
                        break;
                    case 2u:
                        lanes[f] = col[c];
                        break;
                    default:
                        break; // keep
                    }
                }
                std::memcpy(vuData + destOff, lanes, sizeof(lanes));
            }

            if (++cyclePos == wl)
            {
                cyclePos = 0u;
                blockBase += cl;
            }
        }
    }

    using VifUnpackFn = void (*)(uint8_t *, const uint8_t *, const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t,
                                 uint32_t, bool, uint32_t, uint32_t (&)[4], const uint32_t (&)[4], bool);
    constexpr VifUnpackFn kVifUnpackFast[4][3] = {
        {vifUnpackFast<0, 0>, vifUnpackFast<0, 1>, vifUnpackFast<0, 2>},
        {vifUnpackFast<1, 0>, vifUnpackFast<1, 1>, vifUnpackFast<1, 2>},
        {vifUnpackFast<2, 0>, vifUnpackFast<2, 1>, vifUnpackFast<2, 2>},
        {vifUnpackFast<3, 0>, vifUnpackFast<3, 1>, vifUnpackFast<3, 2>},
    };
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;

    while (pos + 4 <= sizeBytes)
    {
        if (s_vif1DirectRemainQwc != 0u)
        {
            // Rest of a DIRECT/DIRECTHL payload that did not fit in the previous call.
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
            {
                break;
            }

            const uint32_t chunkQw = std::min<uint32_t>(s_vif1DirectRemainQwc, availableQw);
            feedVif1Path2(*this, data + pos, chunkQw, s_vif1DirectRemainHl);
            pos += chunkQw * 16u;
            s_vif1DirectRemainQwc -= chunkQw;
            if (s_vif1DirectRemainQwc == 0u)
                s_vif1DirectRemainHl = false;
            continue;
        }

        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
            vif1_regs.stat |= (1u << 11); // INT

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;

            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, runTop, runItop);
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscntCallback)
                m_vu1MscntCallback(runTop, runItop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            uint32_t destAddr = (uint32_t)imm * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    markVU1CodeModified();
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            const bool directHl = (opcode == VIF_DIRECTHL);
            if (truncated)
            {
                s_vif1DirectRemainQwc = qwCount - availableQw;
                s_vif1DirectRemainHl = directHl;
                qwCount = availableQw;
            }

            if (qwCount > 0)
                feedVif1Path2(*this, data + pos, qwCount, directHl);

            pos += qwCount * 16;
            if (truncated)
            {
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const bool zeroExtend = (imm & 0x4000u) != 0u;
            // Templated path for CL >= WL with any mode/mask (dhp1's interleaved vertex unpacks).
            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes && cl >= wl && vl != 3u)
            {
                kVifUnpackFast[vn][vl](m_vu1Data, data + pos, data + sizeBytes, writeVectorCount, vuAddr, cl, wl,
                                       vif1_regs.mode & 3u, maskEnable, vif1_regs.mask, vif1_regs.row, vif1_regs.col, zeroExtend);
                pos += totalBytes;
                if (pos > sizeBytes)
                    break;
                continue;
            }

            // Fast path for the common case: no write mask, no row add/offset, one source vector per
            // write (CL == WL). Every field then takes the decoded value and the writes are
            // contiguous, so this is the general loop below without its per-field dispatch.
            const bool v45 = vl == 3u && vn == 3u;
            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes && !maskEnable && cl == wl &&
                ((vif1_regs.mode & 3u) == 0u || v45) && (vl != 3u || v45))
            {
                const uint8_t *srcBase = data + pos;
                const uint8_t *dataEnd = data + sizeBytes;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t destOff = ((vuAddr + writeIndex) & 0x3FFu) * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                        continue;
                    const uint8_t *v = srcBase + writeIndex * bytesPerVector;
                    uint32_t out[4];
                    if (v45)
                    {
                        uint16_t packed;
                        std::memcpy(&packed, v, sizeof(packed));
                        out[0] = packed & 0x1Fu;
                        out[1] = (packed >> 5) & 0x1Fu;
                        out[2] = (packed >> 10) & 0x1Fu;
                        out[3] = (packed >> 15) & 0x01u;
                    }
                    else
                    {
                        const uint32_t elemBytes = static_cast<uint32_t>(bitsPerComponent) / 8u;
                        auto element = [&](const uint8_t *e) -> uint32_t
                        {
                            if (vl == 0u)
                            {
                                uint32_t w;
                                std::memcpy(&w, e, sizeof(w));
                                return w;
                            }
                            if (vl == 1u)
                            {
                                uint16_t h;
                                std::memcpy(&h, e, sizeof(h));
                                return zeroExtend ? static_cast<uint32_t>(h)
                                                  : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(h)));
                            }
                            return zeroExtend ? static_cast<uint32_t>(*e)
                                              : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(*e)));
                        };
                        if (components == 1)
                        {
                            out[0] = out[1] = out[2] = out[3] = element(v);
                        }
                        else if (components == 2)
                        {
                            out[0] = out[2] = element(v);
                            out[1] = out[3] = element(v + elemBytes);
                        }
                        else if (components == 3)
                        {
                            out[0] = element(v);
                            out[1] = element(v + elemBytes);
                            out[2] = element(v + 2u * elemBytes);
                            const uint8_t *next = v + 3u * elemBytes;
                            if (next + elemBytes <= dataEnd)
                                out[3] = element(next);
                            else
                                std::memcpy(&out[3], m_vu1Data + destOff + 12u, sizeof(uint32_t));
                        }
                        else
                        {
                            out[0] = element(v);
                            out[1] = element(v + elemBytes);
                            out[2] = element(v + 2u * elemBytes);
                            out[3] = element(v + 3u * elemBytes);
                        }
                    }
                    std::memcpy(m_vu1Data + destOff, out, sizeof(out));
                }
                pos += totalBytes;
                if (pos > sizeBytes)
                    break;
                continue;
            }

            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: packed color-like format in a single 16-bit value.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = packed & 0x1Fu;
                        decompressed[1] = (packed >> 5) & 0x1Fu;
                        decompressed[2] = (packed >> 10) & 0x1Fu;
                        decompressed[3] = (packed >> 15) & 0x01u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // Lanes a V2/V3 unpack has no data for are still written on hardware (PCSX2
                    // Vif_Unpack.cpp): V2 writes x,y,x,y, and V3 uses the V4 path, so W receives the
                    // next source element. Games rely on this, e.g. adding STROW 1.0f to the
                    // duplicated U in offset mode to get Q = 1.0.
                    if (handledFormat && vl != 3u)
                    {
                        if (components == 2)
                        {
                            decompressed[2] = decompressed[0];
                            decompressed[3] = decompressed[1];
                        }
                        else if (components == 3)
                        {
                            const uint32_t elemBytes = static_cast<uint32_t>(bitsPerComponent) / 8u;
                            const uint8_t *next = srcVec + 3u * elemBytes;
                            if (next + elemBytes <= data + sizeBytes)
                            {
                                if (vl == 0u)
                                    std::memcpy(&decompressed[3], next, sizeof(uint32_t));
                                else if (vl == 1u)
                                {
                                    uint16_t raw = 0;
                                    std::memcpy(&raw, next, sizeof(raw));
                                    decompressed[3] = extend16(raw);
                                }
                                else
                                    decompressed[3] = extend8(*next);
                            }
                        }
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                }
            }
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            continue;
        }
    }
}
