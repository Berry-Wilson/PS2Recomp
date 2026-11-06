#include "iop_spu2.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRamMask = IopSpu2::kRamWords - 1u;
        constexpr size_t kSinkFrames = 128u;

        // Core registers, offset from the core's base (core 1 is 0x400 above core 0).
        constexpr uint32_t kPmon = 0x180;
        constexpr uint32_t kNon = 0x184;
        constexpr uint32_t kVmixL = 0x188;
        constexpr uint32_t kVmixEL = 0x18C;
        constexpr uint32_t kVmixR = 0x190;
        constexpr uint32_t kVmixER = 0x194;
        constexpr uint32_t kMmix = 0x198;
        constexpr uint32_t kAttr = 0x19A;
        constexpr uint32_t kIrqaHi = 0x19C;
        constexpr uint32_t kIrqaLo = 0x19E;
        constexpr uint32_t kKonLo = 0x1A0;
        constexpr uint32_t kKonHi = 0x1A2;
        constexpr uint32_t kKofLo = 0x1A4;
        constexpr uint32_t kKofHi = 0x1A6;
        constexpr uint32_t kTsaHi = 0x1A8;
        constexpr uint32_t kTsaLo = 0x1AA;
        constexpr uint32_t kData = 0x1AC;
        constexpr uint32_t kAdmas = 0x1B0;
        constexpr uint32_t kVoiceAddresses = 0x1C0;
        constexpr uint32_t kEsaHi = 0x2E0;
        constexpr uint32_t kEsaLo = 0x2E2;
        constexpr uint32_t kEeaHi = 0x33C;
        constexpr uint32_t kEndxLo = 0x340;
        constexpr uint32_t kEndxHi = 0x342;
        constexpr uint32_t kStatx = 0x344;

        // Reverb work area offsets, each a hi/lo register pair.
        constexpr uint32_t kApf1Size = 0x2E4;
        constexpr uint32_t kApf2Size = 0x2E8;
        constexpr uint32_t kSameLDst = 0x2EC;
        constexpr uint32_t kSameRDst = 0x2F0;
        constexpr uint32_t kComb1L = 0x2F4;
        constexpr uint32_t kComb1R = 0x2F8;
        constexpr uint32_t kComb2L = 0x2FC;
        constexpr uint32_t kComb2R = 0x300;
        constexpr uint32_t kSameLSrc = 0x304;
        constexpr uint32_t kSameRSrc = 0x308;
        constexpr uint32_t kDiffLDst = 0x30C;
        constexpr uint32_t kDiffRDst = 0x310;
        constexpr uint32_t kComb3L = 0x314;
        constexpr uint32_t kComb3R = 0x318;
        constexpr uint32_t kComb4L = 0x31C;
        constexpr uint32_t kComb4R = 0x320;
        constexpr uint32_t kDiffLSrc = 0x324;
        constexpr uint32_t kDiffRSrc = 0x328;
        constexpr uint32_t kApf1LDst = 0x32C;
        constexpr uint32_t kApf1RDst = 0x330;
        constexpr uint32_t kApf2LDst = 0x334;
        constexpr uint32_t kApf2RDst = 0x338;

        // Per-core block at 0x760 (core 1 follows at +0x28), as indexes into Core::misc.
        enum Misc
        {
            kMvolL,
            kMvolR,
            kEvolL,
            kEvolR,
            kAvolL,
            kAvolR,
            kBvolL,
            kBvolR,
            kMvolXL,
            kMvolXR,
            kIirVol,
            kCombVol1,
            kCombVol2,
            kCombVol3,
            kCombVol4,
            kWallVol,
            kApfVol1,
            kApfVol2,
            kInCoefL,
            kInCoefR,
        };

        constexpr uint32_t kMiscBase = 0x760;
        constexpr uint32_t kMiscStride = 0x28;
        constexpr uint32_t kMiscEnd = kMiscBase + 2u * kMiscStride;
        constexpr uint32_t kIrqInfo = 0x7C2;

        constexpr int32_t kAdpcmPositive[5] = {0, 60, 115, 98, 122};
        constexpr int32_t kAdpcmNegative[5] = {0, 0, -52, -55, -60};

        int32_t clamp16(int32_t value)
        {
            return std::clamp(value, -0x8000, 0x7FFF);
        }

        int32_t scale(int32_t value, int32_t volume)
        {
            return (value * volume) >> 15;
        }

        // One step of the envelope and volume sweep generator. Returns the number of samples to wait
        // before the next step in wait.
        int32_t envelopeStep(int32_t level, bool exponential, bool decrease, int shift, int rawStep, uint32_t &wait)
        {
            int32_t step = decrease ? -8 + rawStep : 7 - rawStep;
            uint32_t cycles = 1u << std::max(0, shift - 11);
            step = shift < 11 ? step * (1 << (11 - shift)) : step;
            if (exponential && !decrease && level > 0x6000)
                cycles *= 4u;
            if (exponential && decrease)
                step = (step * level) >> 15;
            wait = cycles;
            return std::clamp(level + step, 0, 0x7FFF);
        }
    }

    void IopSpu2::Slide::write(uint16_t newReg)
    {
        reg = newReg;
        wait = 0;
        if ((newReg & 0x8000u) == 0u)
            value = static_cast<int16_t>(static_cast<uint16_t>(newReg << 1u));
    }

    void IopSpu2::Slide::step()
    {
        if ((reg & 0x8000u) == 0u)
            return;
        if (wait > 1u)
        {
            --wait;
            return;
        }
        const bool exponential = (reg & 0x4000u) != 0u;
        const bool decrease = (reg & 0x2000u) != 0u;
        const bool negative = (reg & 0x1000u) != 0u;
        const int32_t magnitude = envelopeStep(std::abs(value) > 0x7FFF ? 0x7FFF : std::abs(value),
                                               exponential, decrease,
                                               (reg >> 2u) & 0x1Fu, reg & 3u, wait);
        value = negative ? -magnitude : magnitude;
    }

    IopSpu2::IopSpu2()
        : m_ram(kRamWords)
    {
        m_buffer.reserve(kSinkFrames * 2u);
        reset();
    }

    void IopSpu2::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
        for (Core &core : m_cores)
            core = Core{};
        m_outputPos = 0;
        m_irqInfo = 0;
        m_interrupt = false;
        m_buffer.clear();
    }

    uint32_t IopSpu2::regAddress(const Core &core, uint32_t reg) const
    {
        return ((static_cast<uint32_t>(core.regs[reg / 2u]) << 16u) | core.regs[reg / 2u + 1u]) & kRamMask;
    }

    bool IopSpu2::takeInterrupt()
    {
        const bool result = m_interrupt;
        m_interrupt = false;
        return result;
    }

    void IopSpu2::checkIrq(uint32_t first, uint32_t words)
    {
        for (int i = 0; i < 2; ++i)
        {
            Core &core = m_cores[i];
            const uint16_t bit = static_cast<uint16_t>(4u << i);
            if ((core.regs[kAttr / 2u] & 0x40u) == 0u || (m_irqInfo & bit) != 0u)
                continue;
            if (((core.irqa - first) & kRamMask) < words)
            {
                m_irqInfo |= bit;
                m_interrupt = true;
            }
        }
    }

    uint16_t IopSpu2::read16(uint32_t physicalAddress)
    {
        const uint32_t offset = (physicalAddress - kRegisterBase) & 0x7FFu;
        if (offset < kMiscBase)
            return readCoreRegister(static_cast<int>(offset >> 10u), offset & 0x3FFu);
        if (offset < kMiscEnd)
        {
            const int coreIndex = offset >= kMiscBase + kMiscStride ? 1 : 0;
            const uint32_t index = (offset - kMiscBase - static_cast<uint32_t>(coreIndex) * kMiscStride) / 2u;
            const Core &core = m_cores[coreIndex];
            if (index == kMvolXL)
                return static_cast<uint16_t>(core.masterL.value);
            if (index == kMvolXR)
                return static_cast<uint16_t>(core.masterR.value);
            return core.misc[index];
        }
        return offset == kIrqInfo ? m_irqInfo : uint16_t{0};
    }

    void IopSpu2::write16(uint32_t physicalAddress, uint16_t value)
    {
        const uint32_t offset = (physicalAddress - kRegisterBase) & 0x7FFu;
        if (offset < kMiscBase)
        {
            writeCoreRegister(static_cast<int>(offset >> 10u), offset & 0x3FFu, value);
        }
        else if (offset < kMiscEnd)
        {
            const int coreIndex = offset >= kMiscBase + kMiscStride ? 1 : 0;
            const uint32_t index = (offset - kMiscBase - static_cast<uint32_t>(coreIndex) * kMiscStride) / 2u;
            Core &core = m_cores[coreIndex];
            core.misc[index] = value;
            if (index == kMvolL)
                core.masterL.write(value);
            else if (index == kMvolR)
                core.masterR.write(value);
        }
        else if (offset == kIrqInfo)
        {
            m_irqInfo = value;
        }
    }

    uint16_t IopSpu2::readCoreRegister(int coreIndex, uint32_t reg)
    {
        Core &core = m_cores[coreIndex];
        if (reg < kPmon)
        {
            const Voice &voice = core.voices[reg >> 4u];
            switch (reg & 0xFu)
            {
            case 0x0: return voice.volL.reg;
            case 0x2: return voice.volR.reg;
            case 0x4: return voice.pitch;
            case 0x6: return voice.adsr1;
            case 0x8: return voice.adsr2;
            case 0xA: return static_cast<uint16_t>(voice.env);
            case 0xC: return static_cast<uint16_t>(voice.volL.value);
            default: return static_cast<uint16_t>(voice.volR.value);
            }
        }
        if (reg >= kVoiceAddresses && reg < kEsaHi)
        {
            const uint32_t index = (reg - kVoiceAddresses) / 12u;
            const Voice &voice = core.voices[index];
            switch ((reg - kVoiceAddresses) % 12u)
            {
            case 0x0: return static_cast<uint16_t>(voice.ssa >> 16u);
            case 0x2: return static_cast<uint16_t>(voice.ssa);
            case 0x4: return static_cast<uint16_t>(voice.lsa >> 16u);
            case 0x6: return static_cast<uint16_t>(voice.lsa);
            case 0x8: return static_cast<uint16_t>(voice.nax >> 16u);
            default: return static_cast<uint16_t>(voice.nax);
            }
        }
        switch (reg)
        {
        case kTsaHi:
            return static_cast<uint16_t>(core.tsa >> 16u);
        case kTsaLo:
            return static_cast<uint16_t>(core.tsa);
        case kIrqaHi:
            return static_cast<uint16_t>(core.irqa >> 16u);
        case kIrqaLo:
            return static_cast<uint16_t>(core.irqa);
        case kEndxLo:
            return static_cast<uint16_t>(core.endx);
        case kEndxHi:
            return static_cast<uint16_t>(core.endx >> 16u);
        case kData:
        {
            const uint16_t value = m_ram[core.tsa];
            checkIrq(core.tsa, 1u);
            core.tsa = (core.tsa + 1u) & kRamMask;
            return value;
        }
        default:
            return core.regs[reg / 2u];
        }
    }

    void IopSpu2::writeCoreRegister(int coreIndex, uint32_t reg, uint16_t value)
    {
        Core &core = m_cores[coreIndex];
        if (reg < kPmon)
        {
            Voice &voice = core.voices[reg >> 4u];
            switch (reg & 0xFu)
            {
            case 0x0: voice.volL.write(value); break;
            case 0x2: voice.volR.write(value); break;
            case 0x4: voice.pitch = value; break;
            case 0x6: voice.adsr1 = value; break;
            case 0x8: voice.adsr2 = value; break;
            case 0xA: voice.env = value & 0x7FFFu; break;
            default: break;
            }
            return;
        }
        if (reg >= kVoiceAddresses && reg < kEsaHi)
        {
            const uint32_t index = (reg - kVoiceAddresses) / 12u;
            Voice &voice = core.voices[index];
            const auto setHigh = [value](uint32_t &address) { address = (address & 0xFFFFu) | ((value & 0xFu) << 16u); };
            const auto setLow = [value](uint32_t &address) { address = (address & 0xF0000u) | value; };
            switch ((reg - kVoiceAddresses) % 12u)
            {
            case 0x0: setHigh(voice.ssa); break;
            case 0x2: setLow(voice.ssa); break;
            case 0x4: setHigh(voice.lsa); voice.loopWritten = true; break;
            case 0x6: setLow(voice.lsa); voice.loopWritten = true; break;
            case 0x8: setHigh(voice.nax); break;
            default: setLow(voice.nax); break;
            }
            return;
        }

        core.regs[reg / 2u] = value;
        switch (reg)
        {
        case kKonLo:
            keyOn(core, value);
            break;
        case kKonHi:
            keyOn(core, (value & 0xFFu) << 16u);
            break;
        case kKofLo:
            keyOff(core, value);
            break;
        case kKofHi:
            keyOff(core, (value & 0xFFu) << 16u);
            break;
        case kTsaHi:
            core.tsa = (core.tsa & 0xFFFFu) | ((value & 0xFu) << 16u);
            break;
        case kTsaLo:
            core.tsa = (core.tsa & 0xF0000u) | value;
            break;
        case kIrqaHi:
            core.irqa = (core.irqa & 0xFFFFu) | ((value & 0xFu) << 16u);
            break;
        case kIrqaLo:
            core.irqa = (core.irqa & 0xF0000u) | value;
            break;
        case kData:
            m_ram[core.tsa] = value;
            checkIrq(core.tsa, 1u);
            core.tsa = (core.tsa + 1u) & kRamMask;
            break;
        case kAttr:
            if ((value & 0x40u) == 0u)
                m_irqInfo &= static_cast<uint16_t>(~(4u << coreIndex));
            break;
        case kEsaHi:
        case kEsaLo:
        case kEeaHi:
            core.reverbPos = 0;
            break;
        case kEndxLo:
            core.endx &= 0xFF0000u;
            break;
        case kEndxHi:
            core.endx &= 0x00FFFFu;
            break;
        case kAdmas:
            if (value == 0u)
                stopAutoDma(coreIndex);
            break;
        default:
            break;
        }
    }

    void IopSpu2::keyOn(Core &core, uint32_t mask)
    {
        core.endx &= ~mask;
        core.active |= mask;
        for (int i = 0; mask != 0u; ++i, mask >>= 1u)
        {
            if ((mask & 1u) == 0u)
                continue;
            Voice &voice = core.voices[i];
            voice.phase = Phase::Attack;
            voice.env = 0;
            voice.envWait = 0;
            voice.nax = voice.ssa;
            voice.loopWritten = false;
            voice.counter = 0;
            voice.adpcm1 = voice.adpcm2 = 0;
            voice.blockIndex = 28;
            voice.blockFlags = 0;
            voice.blockAddress = ((voice.ssa & ~7u) - 8u) & kRamMask;
            voice.history = {};
            voice.lastOut = 0;
        }
    }

    void IopSpu2::keyOff(Core &core, uint32_t mask)
    {
        for (int i = 0; mask != 0u; ++i, mask >>= 1u)
        {
            Voice &voice = core.voices[i];
            if ((mask & 1u) != 0u && voice.phase != Phase::Off)
            {
                voice.phase = Phase::Release;
                voice.envWait = 0;
            }
        }
    }

    void IopSpu2::dma(int coreIndex, uint8_t *iopRam, uint32_t iopRamSize, uint32_t address, uint32_t words32, bool toSpu)
    {
        Core &core = m_cores[coreIndex];
        uint32_t source = address & (iopRamSize - 1u);
        const uint32_t words = words32 * 2u;
        checkIrq(core.tsa, words);
        for (uint32_t i = 0; i < words && source + 1u < iopRamSize; ++i, source += 2u)
        {
            if (toSpu)
                std::memcpy(&m_ram[core.tsa], iopRam + source, 2u);
            else
                std::memcpy(iopRam + source, &m_ram[core.tsa], 2u);
            core.tsa = (core.tsa + 1u) & kRamMask;
        }
        core.regs[kStatx / 2u] |= 0x80u;
    }

    bool IopSpu2::autoDmaEnabled(int coreIndex) const
    {
        return m_cores[coreIndex].regs[kAdmas / 2u] != 0u;
    }

    bool IopSpu2::takeAutoDmaDone(int coreIndex)
    {
        AutoDma &adma = m_cores[coreIndex].adma;
        const bool done = adma.done;
        adma.done = false;
        return done;
    }

    void IopSpu2::startAutoDma(int coreIndex, const uint8_t *iopRam, uint32_t iopRamSize, uint32_t address, uint32_t bytes)
    {
        Core &core = m_cores[coreIndex];
        AutoDma &adma = core.adma;
        const uint32_t start = address & (iopRamSize - 1u);
        if (adma.active)
        {
            adma.nextAddress = start;
            adma.nextBytes = bytes;
        }
        else
        {
            adma = AutoDma{};
            adma.ram = iopRam;
            adma.ramSize = iopRamSize;
            adma.address = start;
            adma.bytes = bytes;
            adma.active = bytes != 0u;
            adma.done = bytes <= 0x400u;
        }
        core.regs[kStatx / 2u] |= 0x80u;
    }

    void IopSpu2::stopAutoDma(int coreIndex)
    {
        AutoDma &adma = m_cores[coreIndex].adma;
        adma.active = false;
        adma.done = false;
        adma.nextBytes = 0u;
    }

    void IopSpu2::readInput(Core &core, int32_t &left, int32_t &right)
    {
        AutoDma &adma = core.adma;
        left = right = 0;
        if (!adma.active)
            return;
        const auto sampleAt = [&adma](uint32_t offset) -> int32_t {
            if (offset + 1u >= adma.bytes || adma.address + offset + 1u >= adma.ramSize)
                return 0;
            int16_t value;
            std::memcpy(&value, adma.ram + adma.address + offset, sizeof(value));
            return value;
        };
        left = sampleAt(adma.sample * 2u);
        right = sampleAt(0x200u + adma.sample * 2u);
        if (++adma.sample == 256u)
        {
            adma.sample = 0;
            adma.address += 0x400u;
            adma.bytes -= std::min<uint32_t>(adma.bytes, 0x400u);
            if (adma.bytes == 0u)
            {
                adma.address = adma.nextAddress;
                adma.bytes = adma.nextBytes;
                adma.nextBytes = 0u;
                adma.active = adma.bytes != 0u;
                adma.done = adma.done || (adma.active && adma.bytes <= 0x400u);
            }
            else if (adma.bytes <= 0x400u)
            {
                adma.done = true; // its last block is now playing
            }
        }
    }

    void IopSpu2::fetchBlock(int coreIndex, int voiceIndex)
    {
        Core &core = m_cores[coreIndex];
        Voice &voice = core.voices[voiceIndex];

        uint32_t address = (voice.blockAddress + 8u) & kRamMask;
        if ((voice.blockFlags & 1u) != 0u)
        {
            core.endx |= 1u << voiceIndex;
            address = voice.lsa & kRamMask & ~7u;
            if ((voice.blockFlags & 2u) == 0u)
            {
                voice.phase = Phase::Off;
                voice.env = 0;
                core.active &= ~(1u << voiceIndex);
                voice.block = {};
                voice.blockIndex = 0;
                voice.blockFlags = 0;
                voice.blockAddress = address;
                voice.nax = address;
                return;
            }
        }

        checkIrq(address, 8u);
        const uint16_t header = m_ram[address];
        const int shift = std::min<int>(header & 0xFu, 12);
        const int filter = std::min<int>((header >> 4u) & 0xFu, 4);
        const uint8_t flags = static_cast<uint8_t>(header >> 8u);

        int index = 0;
        for (uint32_t w = 1u; w < 8u; ++w)
        {
            uint16_t word = m_ram[(address + w) & kRamMask];
            for (int n = 0; n < 4; ++n, word >>= 4u)
            {
                const int32_t nibble = static_cast<int16_t>(static_cast<uint16_t>((word & 0xFu) << 12u));
                int32_t sample = (nibble >> shift) +
                                 ((voice.adpcm1 * kAdpcmPositive[filter] + voice.adpcm2 * kAdpcmNegative[filter]) >> 6);
                sample = clamp16(sample);
                voice.adpcm2 = voice.adpcm1;
                voice.adpcm1 = sample;
                voice.block[index++] = static_cast<int16_t>(sample);
            }
        }

        if ((flags & 4u) != 0u && !voice.loopWritten)
            voice.lsa = address;
        voice.blockAddress = address;
        voice.blockFlags = flags;
        voice.blockIndex = 0;
    }

    void IopSpu2::stepEnvelope(Voice &voice)
    {
        if (voice.envWait > 1u)
        {
            --voice.envWait;
            return;
        }
        switch (voice.phase)
        {
        case Phase::Attack:
            voice.env = envelopeStep(voice.env, (voice.adsr1 & 0x8000u) != 0u, false,
                                     (voice.adsr1 >> 10u) & 0x1Fu, (voice.adsr1 >> 8u) & 3u, voice.envWait);
            if (voice.env >= 0x7FFF)
                voice.phase = Phase::Decay;
            break;
        case Phase::Decay:
        {
            voice.env = envelopeStep(voice.env, true, true, (voice.adsr1 >> 4u) & 0xFu, 0, voice.envWait);
            const int32_t sustainLevel = std::min<int32_t>(((voice.adsr1 & 0xFu) + 1) * 0x800, 0x7FFF);
            if (voice.env <= sustainLevel)
                voice.phase = Phase::Sustain;
            break;
        }
        case Phase::Sustain:
            voice.env = envelopeStep(voice.env, (voice.adsr2 & 0x8000u) != 0u, (voice.adsr2 & 0x4000u) != 0u,
                                     (voice.adsr2 >> 8u) & 0x1Fu, (voice.adsr2 >> 6u) & 3u, voice.envWait);
            break;
        case Phase::Release:
            voice.env = envelopeStep(voice.env, (voice.adsr2 & 0x20u) != 0u, true, voice.adsr2 & 0x1Fu, 0, voice.envWait);
            break;
        case Phase::Off:
            break;
        }
    }

    void IopSpu2::stepNoise(Core &core)
    {
        const uint32_t clock = (core.regs[kAttr / 2u] >> 8u) & 0x3Fu;
        const int shift = static_cast<int>(clock >> 2u);
        const int step = static_cast<int>(clock & 3u) + 4;
        if (core.noiseWait > 1u)
        {
            --core.noiseWait;
            return;
        }
        core.noiseWait = 1u;
        core.noiseSign -= step;
        if (core.noiseSign < 0)
        {
            const uint32_t n = core.noise;
            const uint32_t bit = ((n >> 15u) ^ (n >> 12u) ^ (n >> 11u) ^ (n >> 10u) ^ 1u) & 1u;
            core.noise = static_cast<uint16_t>((n << 1u) | bit);
            core.noiseSign += 0x20000 >> shift;
        }
    }

    int32_t IopSpu2::voiceSample(int coreIndex, int voiceIndex, int32_t previousOut)
    {
        Core &core = m_cores[coreIndex];
        Voice &voice = core.voices[voiceIndex];

        uint32_t pitch = voice.pitch;
        if ((((static_cast<uint32_t>(core.regs[kPmon / 2u + 1u]) << 16u) | core.regs[kPmon / 2u]) >> voiceIndex) & 1u)
            pitch = (pitch * static_cast<uint32_t>(previousOut + 0x8000)) >> 15u;

        voice.counter += pitch;
        while (voice.counter >= 0x1000u)
        {
            voice.counter -= 0x1000u;
            if (voice.blockIndex >= 28u)
            {
                fetchBlock(coreIndex, voiceIndex);
                if (voice.phase == Phase::Off)
                    return 0;
            }
            voice.history[0] = voice.history[1];
            voice.history[1] = voice.history[2];
            voice.history[2] = voice.history[3];
            voice.history[3] = voice.block[voice.blockIndex++];
            voice.nax = (voice.blockAddress + 1u + voice.blockIndex / 4u) & kRamMask;
        }

        int32_t sample;
        if ((((static_cast<uint32_t>(core.regs[kNon / 2u + 1u]) << 16u) | core.regs[kNon / 2u]) >> voiceIndex) & 1u)
        {
            sample = static_cast<int16_t>(core.noise);
        }
        else
        {
            // Catmull-Rom interpolation between history[1] and history[2].
            const float f = static_cast<float>(voice.counter) / 4096.0f;
            const float a = voice.history[0], b = voice.history[1], c = voice.history[2], d = voice.history[3];
            const float value = b + 0.5f * f * (c - a + f * (2.0f * a - 5.0f * b + 4.0f * c - d + f * (3.0f * (b - c) + d - a)));
            sample = clamp16(static_cast<int32_t>(value));
        }

        stepEnvelope(voice);
        if (voice.phase == Phase::Release && voice.env <= 0)
        {
            voice.phase = Phase::Off;
            voice.env = 0;
            core.active &= ~(1u << voiceIndex);
            return 0;
        }
        return scale(sample, voice.env);
    }

    void IopSpu2::runReverb(Core &core, int32_t inL, int32_t inR, int32_t &outL, int32_t &outR)
    {
        const uint32_t start = regAddress(core, kEsaHi);
        const uint32_t end = ((static_cast<uint32_t>(core.regs[kEeaHi / 2u]) << 16u) | 0xFFFFu) & kRamMask;
        if (end <= start)
        {
            outL = outR = 0;
            return;
        }
        const uint32_t size = end - start + 1u;

        const auto at = [&](uint32_t offset) -> uint16_t & {
            return m_ram[start + (core.reverbPos + offset) % size];
        };
        const auto rd = [&](uint32_t offset) -> int32_t { return static_cast<int16_t>(at(offset)); };
        const auto wr = [&](uint32_t offset, int32_t value) { at(offset) = static_cast<uint16_t>(clamp16(value)); };
        const auto reg = [&](uint32_t r) { return regAddress(core, r); };
        const auto vol = [&](Misc index) -> int32_t { return static_cast<int16_t>(core.misc[index]); };

        const int32_t lin = scale(inL, vol(kInCoefL));
        const int32_t rin = scale(inR, vol(kInCoefR));
        const int32_t wall = vol(kWallVol);
        const int32_t iir = vol(kIirVol);

        const auto reflect = [&](uint32_t dst, uint32_t src, int32_t in) {
            const uint32_t d = reg(dst);
            const int32_t previous = rd(d + size - 1u);
            wr(d, scale(in + scale(rd(reg(src)), wall) - previous, iir) + previous);
        };
        reflect(kSameLDst, kSameLSrc, lin);
        reflect(kSameRDst, kSameRSrc, rin);
        reflect(kDiffLDst, kDiffRSrc, lin);
        reflect(kDiffRDst, kDiffLSrc, rin);

        const auto comb = [&](uint32_t r1, uint32_t r2, uint32_t r3, uint32_t r4) {
            return clamp16(scale(rd(reg(r1)), vol(kCombVol1)) + scale(rd(reg(r2)), vol(kCombVol2)) +
                           scale(rd(reg(r3)), vol(kCombVol3)) + scale(rd(reg(r4)), vol(kCombVol4)));
        };
        int32_t left = comb(kComb1L, kComb2L, kComb3L, kComb4L);
        int32_t right = comb(kComb1R, kComb2R, kComb3R, kComb4R);

        const auto allPass = [&](int32_t value, uint32_t dst, uint32_t sizeReg, int32_t volume) {
            const uint32_t d = reg(dst);
            const int32_t delayed = rd(d + size - reg(sizeReg) % size);
            const int32_t fed = clamp16(value - scale(delayed, volume));
            wr(d, fed);
            return clamp16(scale(fed, volume) + delayed);
        };
        left = allPass(left, kApf1LDst, kApf1Size, vol(kApfVol1));
        right = allPass(right, kApf1RDst, kApf1Size, vol(kApfVol1));
        left = allPass(left, kApf2LDst, kApf2Size, vol(kApfVol2));
        right = allPass(right, kApf2RDst, kApf2Size, vol(kApfVol2));

        core.reverbPos = (core.reverbPos + 1u) % size;
        outL = left;
        outR = right;
    }

    void IopSpu2::mixCore(int coreIndex, int32_t extL, int32_t extR, int32_t &outL, int32_t &outR)
    {
        Core &core = m_cores[coreIndex];
        const auto mask = [&](uint32_t reg) {
            return (static_cast<uint32_t>(core.regs[reg / 2u + 1u]) << 16u) | core.regs[reg / 2u];
        };
        const uint32_t vmixL = mask(kVmixL), vmixR = mask(kVmixR), vmixEL = mask(kVmixEL), vmixER = mask(kVmixER);

        int32_t dryL = 0, dryR = 0, wetL = 0, wetR = 0;
        int32_t previous = 0;
        stepNoise(core);
        for (int i = 0; i < 24; ++i)
        {
            Voice &voice = core.voices[i];
            const uint32_t bit = 1u << i;
            if ((core.active & bit) == 0u)
            {
                previous = 0;
                continue;
            }
            const int32_t out = voiceSample(coreIndex, i, previous);
            voice.volL.step();
            voice.volR.step();
            voice.lastOut = out;
            previous = out;
            const int32_t l = scale(out, voice.volL.value);
            const int32_t r = scale(out, voice.volR.value);
            if (vmixL & bit) dryL += l;
            if (vmixR & bit) dryR += r;
            if (vmixEL & bit) wetL += l;
            if (vmixER & bit) wetR += r;
        }
        dryL = clamp16(dryL);
        dryR = clamp16(dryR);
        wetL = clamp16(wetL);
        wetR = clamp16(wetR);

        // The chip writes each core's voice mix to the output areas of sound RAM.
        const uint32_t base = coreIndex == 0 ? 0x1000u : 0x1800u;
        m_ram[base + m_outputPos] = static_cast<uint16_t>(dryL);
        m_ram[base + 0x200u + m_outputPos] = static_cast<uint16_t>(dryR);
        m_ram[base + 0x400u + m_outputPos] = static_cast<uint16_t>(wetL);
        m_ram[base + 0x600u + m_outputPos] = static_cast<uint16_t>(wetR);

        const uint16_t mmix = core.regs[kMmix / 2u];
        int32_t mixL = (mmix & 0x800u) ? dryL : 0;
        int32_t mixR = (mmix & 0x400u) ? dryR : 0;
        if (mmix & 0x008u) mixL += extL;
        if (mmix & 0x004u) mixR += extR;

        int32_t inputL = 0, inputR = 0;
        readInput(core, inputL, inputR);
        inputL = scale(inputL, static_cast<int16_t>(core.misc[kBvolL]));
        inputR = scale(inputR, static_cast<int16_t>(core.misc[kBvolR]));
        if (mmix & 0x080u) mixL += inputL;
        if (mmix & 0x040u) mixR += inputR;

        // Reverb runs at half the sample rate, on the average of the two samples it covers.
        const bool effects = (core.regs[kAttr / 2u] & 0x80u) != 0u;
        int32_t reverbL = 0, reverbR = 0;
        if (effects)
        {
            int32_t sendL = (mmix & 0x200u) ? wetL : 0;
            int32_t sendR = (mmix & 0x100u) ? wetR : 0;
            if (mmix & 0x002u) sendL += extL;
            if (mmix & 0x001u) sendR += extR;
            if (mmix & 0x020u) sendL += inputL;
            if (mmix & 0x010u) sendR += inputR;
            sendL = clamp16(sendL);
            sendR = clamp16(sendR);
            if (core.reverbOdd)
            {
                core.reverbPrevOutL = core.reverbOutL;
                core.reverbPrevOutR = core.reverbOutR;
                runReverb(core, (core.reverbInL + sendL) / 2, (core.reverbInR + sendR) / 2,
                          core.reverbOutL, core.reverbOutR);
                reverbL = core.reverbOutL;
                reverbR = core.reverbOutR;
            }
            else
            {
                core.reverbInL = sendL;
                core.reverbInR = sendR;
                reverbL = (core.reverbPrevOutL + core.reverbOutL) / 2;
                reverbR = (core.reverbPrevOutR + core.reverbOutR) / 2;
            }
            core.reverbOdd = !core.reverbOdd;
        }
        mixL += scale(reverbL, static_cast<int16_t>(core.misc[kEvolL]));
        mixR += scale(reverbR, static_cast<int16_t>(core.misc[kEvolR]));

        core.masterL.step();
        core.masterR.step();
        outL = scale(clamp16(mixL), core.masterL.value);
        outR = scale(clamp16(mixR), core.masterR.value);
    }

    void IopSpu2::tick()
    {
        int32_t core0L = 0, core0R = 0;
        mixCore(0, 0, 0, core0L, core0R);
        const Core &core1 = m_cores[1];
        const int32_t extL = scale(core0L, static_cast<int16_t>(core1.misc[kAvolL]));
        const int32_t extR = scale(core0R, static_cast<int16_t>(core1.misc[kAvolR]));
        int32_t outL = 0, outR = 0;
        mixCore(1, extL, extR, outL, outR);
        m_outputPos = (m_outputPos + 1u) & 0x1FFu;

        m_buffer.push_back(static_cast<int16_t>(clamp16(outL)));
        m_buffer.push_back(static_cast<int16_t>(clamp16(outR)));
        if (m_buffer.size() >= kSinkFrames * 2u)
            flush();
    }

    void IopSpu2::flush()
    {
        if (m_sink && !m_buffer.empty())
            m_sink(m_buffer.data(), m_buffer.size() / 2u);
        m_buffer.clear();
    }
}
