#include "iop_spu2.h"

#include "ps2x/iop/spu2_output.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2x::iop
{
    namespace
    {
        // Single-producer (IOP thread) / single-consumer (audio callback) ring of stereo frames.
        constexpr size_t kRingFrames = 16384u;
        constexpr size_t kMaxLatencyFrames = 9600u;   // 200 ms: drop older audio beyond this
        constexpr size_t kTrimLatencyFrames = 4800u;  // keep 100 ms after a drop
        int16_t g_ring[kRingFrames * 2];
        std::atomic<size_t> g_ringWrite{0};
        std::atomic<size_t> g_ringRead{0};

        // Optional capture of the mixed output (DHP1_SPU2_WAV=<path>) for offline inspection.
        struct WavCapture
        {
            std::FILE *file = nullptr;
            uint32_t frames = 0;
            WavCapture()
            {
                const char *path = std::getenv("DHP1_SPU2_WAV");
                if (!path || !*path)
                    return;
                file = std::fopen(path, "wb");
                if (file)
                    writeHeader();
            }
            ~WavCapture()
            {
                if (!file)
                    return;
                std::fseek(file, 0, SEEK_SET);
                writeHeader();
                std::fclose(file);
            }
            void writeHeader()
            {
                const uint32_t dataBytes = frames * 4u;
                const uint32_t riffBytes = 36u + dataBytes;
                const uint32_t rate = 48000u, byteRate = 48000u * 4u, fmtBytes = 16u;
                const uint16_t pcm = 1u, channels = 2u, align = 4u, bits = 16u;
                std::fwrite("RIFF", 1, 4, file);
                std::fwrite(&riffBytes, 4, 1, file);
                std::fwrite("WAVEfmt ", 1, 8, file);
                std::fwrite(&fmtBytes, 4, 1, file);
                std::fwrite(&pcm, 2, 1, file);
                std::fwrite(&channels, 2, 1, file);
                std::fwrite(&rate, 4, 1, file);
                std::fwrite(&byteRate, 4, 1, file);
                std::fwrite(&align, 2, 1, file);
                std::fwrite(&bits, 2, 1, file);
                std::fwrite("data", 1, 4, file);
                std::fwrite(&dataBytes, 4, 1, file);
            }
            void write(int16_t left, int16_t right)
            {
                if (!file)
                    return;
                const int16_t frame[2] = {left, right};
                std::fwrite(frame, sizeof(frame), 1, file);
                if ((++frames % 48000u) == 0u)
                {
                    // Keep the header valid even if the process is killed.
                    const long position = std::ftell(file);
                    std::fseek(file, 0, SEEK_SET);
                    writeHeader();
                    std::fseek(file, position, SEEK_SET);
                    std::fflush(file);
                }
            }
        };
        WavCapture g_wavCapture;

        void pushFrame(int16_t left, int16_t right)
        {
            g_wavCapture.write(left, right);
            const size_t write = g_ringWrite.load(std::memory_order_relaxed);
            const size_t read = g_ringRead.load(std::memory_order_acquire);
            if (write - read >= kRingFrames)
                return; // nobody is consuming (no audio device): drop
            const size_t slot = (write % kRingFrames) * 2u;
            g_ring[slot] = left;
            g_ring[slot + 1u] = right;
            g_ringWrite.store(write + 1u, std::memory_order_release);
        }
    }

    size_t readSpu2Frames(int16_t *destination, size_t frames)
    {
        size_t read = g_ringRead.load(std::memory_order_relaxed);
        const size_t write = g_ringWrite.load(std::memory_order_acquire);
        if (write - read > kMaxLatencyFrames)
            read = write - kTrimLatencyFrames;
        const size_t available = std::min(frames, write - read);
        for (size_t i = 0; i < available; ++i)
        {
            const size_t slot = ((read + i) % kRingFrames) * 2u;
            destination[i * 2u] = g_ring[slot];
            destination[i * 2u + 1u] = g_ring[slot + 1u];
        }
        g_ringRead.store(read + available, std::memory_order_release);
        return available;
    }
}

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kAddressMask = IopSpu2::RamHalfwords - 1u;
        constexpr uint32_t kCoreStride = 0x400u;
        constexpr uint32_t kVoiceAddrBase = 0x1C0u;
        constexpr uint32_t kSharedVolBase = 0x760u;
        constexpr uint32_t kSharedVolStride = 0x28u;
        constexpr uint32_t kIrqInfo = 0x7C2u;

        // Core-local register offsets.
        constexpr uint32_t kPmon = 0x180u, kNon = 0x184u, kVmixl = 0x188u, kVmixel = 0x18Cu,
                           kVmixr = 0x190u, kVmixer = 0x194u, kMmix = 0x198u, kAttr = 0x19Au,
                           kIrqa = 0x19Cu, kKon = 0x1A0u, kKoff = 0x1A4u, kTsa = 0x1A8u,
                           kData = 0x1ACu, kAdmas = 0x1B0u, kEndx = 0x340u, kStatx = 0x344u;

        constexpr int32_t kFilterPos[5] = {0, 60, 115, 98, 122};
        constexpr int32_t kFilterNeg[5] = {0, 0, -52, -55, -60};

        int32_t clamp16(int32_t value)
        {
            return std::clamp(value, -32768, 32767);
        }

        bool logEnabled()
        {
            static const bool enabled = std::getenv("DHP1_SPU2_LOG") != nullptr || std::getenv("PS2X_SPU2_LOG") != nullptr;
            return enabled;
        }
    }

    void IopSpu2::Envelope::reset(uint8_t newRate, bool newDecreasing, bool newExponential)
    {
        rate = static_cast<uint8_t>(newRate & 0x7Fu);
        decreasing = newDecreasing;
        exponential = newExponential;
        counter = 0;
    }

    int32_t IopSpu2::Envelope::tick(int32_t level)
    {
        // PSX/SPU2 envelope: rate = (shift << 2) | step. Each tick adds 'increment' to a 15-bit
        // counter; on overflow the level moves by 'step'.
        const int32_t shift = rate >> 2;
        const int32_t baseStep = 7 - (rate & 3);
        int32_t step = decreasing ? -(baseStep + 1) : baseStep;
        if (shift < 11)
            step *= 1 << (11 - shift);
        uint32_t increment = shift > 11 ? (shift - 11 >= 16 ? 0u : (0x8000u >> (shift - 11))) : 0x8000u;
        if (exponential)
        {
            if (decreasing)
                step = (step * level) >> 15;
            else if (level >= 0x6000)
            {
                if (rate < 40)
                    step >>= 2;
                else if (rate >= 44)
                    increment >>= 2;
                else
                {
                    step >>= 1;
                    increment >>= 1;
                }
            }
        }
        counter += increment;
        if (counter < 0x8000u)
            return level;
        counter = 0;
        return std::clamp(level + step, 0, 0x7FFF);
    }

    IopSpu2::IopSpu2()
        : m_ram(RamHalfwords)
    {
        reset();
    }

    void IopSpu2::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
        m_regs.fill(0);
        for (Core &core : m_cores)
            core = Core{};
        m_cycle = 0;
        m_cycleValid = false;
        m_irq = false;
    }

    uint32_t IopSpu2::addressPair(uint32_t hiOffset) const
    {
        return ((static_cast<uint32_t>(regValue(hiOffset) & 0xFu) << 16) | regValue(hiOffset + 2u)) & kAddressMask;
    }

    void IopSpu2::setAddressPair(uint32_t hiOffset, uint32_t address)
    {
        reg(hiOffset) = static_cast<uint16_t>((address >> 16) & 0xFu);
        reg(hiOffset + 2u) = static_cast<uint16_t>(address & 0xFFFFu);
    }

    uint32_t IopSpu2::voiceMask(int core, uint32_t lowOffset) const
    {
        const uint32_t base = static_cast<uint32_t>(core) * kCoreStride + lowOffset;
        return (static_cast<uint32_t>(regValue(base)) | (static_cast<uint32_t>(regValue(base + 2u) & 0xFFu) << 16)) & 0xFFFFFFu;
    }

    void IopSpu2::checkIrq(uint32_t address, uint32_t count)
    {
        for (int c = 0; c < 2; ++c)
        {
            const uint32_t base = static_cast<uint32_t>(c) * kCoreStride;
            if ((regValue(base + kAttr) & 0x40u) == 0u)
                continue;
            const uint32_t irqa = addressPair(base + kIrqa);
            if (((irqa - address) & kAddressMask) < count)
            {
                reg(kIrqInfo) = static_cast<uint16_t>(reg(kIrqInfo) | (4u << c));
                m_irq = true;
                ++m_statIrq;
            }
        }
    }

    bool IopSpu2::takeIrq()
    {
        const bool irq = m_irq;
        m_irq = false;
        return irq;
    }

    void IopSpu2::setDmaReady(int core, bool ready)
    {
        uint16_t &statx = m_cores[core & 1].statx;
        statx = ready ? static_cast<uint16_t>(statx | 0x80u) : static_cast<uint16_t>(statx & ~0x80u);
    }

    uint16_t IopSpu2::read16(uint32_t address)
    {
        const uint32_t offset = (address - RegisterBase) & 0x7FFu;
        if (offset == kIrqInfo)
        {
            const uint16_t value = reg(kIrqInfo);
            reg(kIrqInfo) = 0;
            return value;
        }
        if (offset >= kSharedVolBase)
            return regValue(offset);

        const int c = offset >= kCoreStride ? 1 : 0;
        const uint32_t local = offset & (kCoreStride - 1u);
        Core &core = m_cores[c];
        if (local < 0x180u)
        {
            const Voice &voice = core.voices[local >> 4];
            switch (local & 0xFu)
            {
            case 0xA:
                return static_cast<uint16_t>(voice.level);
            case 0xC:
                return static_cast<uint16_t>(voice.volX[0]);
            case 0xE:
                return static_cast<uint16_t>(voice.volX[1]);
            default:
                return regValue(offset);
            }
        }
        if (local >= kVoiceAddrBase && local < kVoiceAddrBase + 24u * 0xCu)
        {
            const uint32_t rel = local - kVoiceAddrBase;
            const Voice &voice = core.voices[rel / 0xCu];
            if (rel % 0xCu == 8u)
                return static_cast<uint16_t>((voice.blockAddr >> 16) & 0xFu);
            if (rel % 0xCu == 0xAu)
                return static_cast<uint16_t>(voice.blockAddr & 0xFFFFu);
            return regValue(offset);
        }
        switch (local)
        {
        case kEndx:
            return static_cast<uint16_t>(core.endx & 0xFFFFu);
        case kEndx + 2u:
            return static_cast<uint16_t>(core.endx >> 16);
        case kStatx:
            return core.statx;
        case kKon:
        case kKon + 2u:
        case kKoff:
        case kKoff + 2u:
            return 0;
        default:
            return regValue(offset);
        }
    }

    void IopSpu2::write16(uint32_t address, uint16_t value)
    {
        const uint32_t offset = (address - RegisterBase) & 0x7FFu;
        if (offset >= kSharedVolBase)
        {
            reg(offset) = value;
            return;
        }
        const int c = offset >= kCoreStride ? 1 : 0;
        const uint32_t local = offset & (kCoreStride - 1u);
        Core &core = m_cores[c];

        if (local >= kVoiceAddrBase && local < kVoiceAddrBase + 24u * 0xCu)
        {
            reg(offset) = value;
            const uint32_t rel = local - kVoiceAddrBase;
            Voice &voice = core.voices[rel / 0xCu];
            const uint32_t pairHi = offset - ((rel % 0xCu) & 2u);
            switch (rel % 0xCu)
            {
            case 4u:
            case 6u:
                voice.loopAddr = addressPair(pairHi);
                if (voice.phase != AdsrPhase::Off)
                    voice.loopLocked = true;
                break;
            case 8u:
            case 0xAu:
                voice.blockAddr = addressPair(pairHi);
                voice.decodedPos = 28;
                voice.firstBlock = true;
                break;
            default:
                break;
            }
            return;
        }

        switch (local)
        {
        case kKon:
            keyOn(c, value);
            return;
        case kKon + 2u:
            keyOn(c, static_cast<uint32_t>(value & 0xFFu) << 16);
            return;
        case kKoff:
            keyOff(c, value);
            return;
        case kKoff + 2u:
            keyOff(c, static_cast<uint32_t>(value & 0xFFu) << 16);
            return;
        case kTsa:
        case kTsa + 2u:
            reg(offset) = value;
            core.tsa = addressPair(static_cast<uint32_t>(c) * kCoreStride + kTsa);
            return;
        case kData:
            m_ram[core.tsa] = value;
            checkIrq(core.tsa, 1u);
            core.tsa = (core.tsa + 1u) & kAddressMask;
            setAddressPair(static_cast<uint32_t>(c) * kCoreStride + kTsa, core.tsa);
            return;
        case kAdmas:
            reg(offset) = value;
            if ((value & (1u << c)) == 0u)
            {
                core.memIn.clear();
                core.memInPos = 0;
            }
            return;
        case kEndx:
            core.endx &= 0xFF0000u;
            return;
        case kEndx + 2u:
            core.endx &= 0xFFFFu;
            return;
        case kStatx:
            return;
        default:
            reg(offset) = value;
            if (local < 0x180u)
            {
                Voice &voice = core.voices[local >> 4];
                if ((local & 0xFu) == 0xAu)
                    voice.level = value & 0x7FFF;
            }
            return;
        }
    }

    void IopSpu2::keyOn(int c, uint32_t mask)
    {
        Core &core = m_cores[c];
        const uint32_t base = static_cast<uint32_t>(c) * kCoreStride;
        for (int v = 0; v < 24; ++v)
        {
            if ((mask & (1u << v)) == 0u)
                continue;
            Voice &voice = core.voices[v];
            const uint32_t addrRegs = base + kVoiceAddrBase + static_cast<uint32_t>(v) * 0xCu;
            voice.blockAddr = addressPair(addrRegs);
            voice.loopAddr = addressPair(addrRegs + 4u);
            voice.loopLocked = false;
            voice.decodedPos = 28;
            voice.firstBlock = true;
            voice.hist1 = voice.hist2 = 0;
            voice.window.fill(0);
            voice.counter = 0;
            voice.phase = AdsrPhase::Attack;
            voice.level = 0;
            const uint16_t adsr1 = regValue(base + static_cast<uint32_t>(v) * 0x10u + 6u);
            voice.adsr.reset(static_cast<uint8_t>((adsr1 >> 8) & 0x7Fu), false, (adsr1 & 0x8000u) != 0u);
            core.endx &= ~(1u << v);
            ++m_statKeyOn;
        }
    }

    void IopSpu2::keyOff(int c, uint32_t mask)
    {
        Core &core = m_cores[c];
        const uint32_t base = static_cast<uint32_t>(c) * kCoreStride;
        for (int v = 0; v < 24; ++v)
        {
            if ((mask & (1u << v)) == 0u)
                continue;
            Voice &voice = core.voices[v];
            if (voice.phase == AdsrPhase::Off)
                continue;
            const uint16_t adsr2 = regValue(base + static_cast<uint32_t>(v) * 0x10u + 8u);
            voice.phase = AdsrPhase::Release;
            voice.adsr.reset(static_cast<uint8_t>((adsr2 & 0x1Fu) << 2), true, (adsr2 & 0x20u) != 0u);
        }
    }

    void IopSpu2::decodeBlock(int c, int v)
    {
        Voice &voice = m_cores[c].voices[v];
        const uint32_t addr = voice.blockAddr & ~7u;
        checkIrq(addr, 8u);
        const uint16_t header = m_ram[addr];
        int shift = header & 0xF;
        if (shift > 12)
            shift = 9;
        const int filter = std::min<int>((header >> 4) & 0x7, 4);
        const uint8_t flags = static_cast<uint8_t>(header >> 8);
        if ((flags & 4u) != 0u && !voice.loopLocked)
            voice.loopAddr = addr;

        for (int i = 0; i < 28; ++i)
        {
            const uint16_t word = m_ram[(addr + 1u + static_cast<uint32_t>(i >> 2)) & kAddressMask];
            const int32_t nibble = (word >> ((i & 3) * 4)) & 0xF;
            int32_t sample = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12)) >> shift;
            sample += (voice.hist1 * kFilterPos[filter] + voice.hist2 * kFilterNeg[filter] + 32) >> 6;
            sample = clamp16(sample);
            voice.hist2 = voice.hist1;
            voice.hist1 = sample;
            voice.decoded[i] = static_cast<int16_t>(sample);
        }
        voice.decodedFlags = flags;
        voice.decodedPos = 0;
    }

    int16_t IopSpu2::nextSample(int c, int v)
    {
        Voice &voice = m_cores[c].voices[v];
        if (voice.decodedPos >= 28)
        {
            if (!voice.firstBlock)
            {
                const uint8_t flags = voice.decodedFlags;
                if ((flags & 1u) != 0u)
                {
                    m_cores[c].endx |= 1u << v;
                    voice.blockAddr = voice.loopAddr;
                    if ((flags & 2u) == 0u)
                    {
                        voice.phase = AdsrPhase::Off;
                        voice.level = 0;
                    }
                }
                else
                {
                    voice.blockAddr = (voice.blockAddr + 8u) & kAddressMask;
                }
            }
            voice.firstBlock = false;
            decodeBlock(c, v);
        }
        return voice.decoded[voice.decodedPos++];
    }

    void IopSpu2::stepAdsr(Voice &voice, int c, int v)
    {
        const uint32_t regs = static_cast<uint32_t>(c) * kCoreStride + static_cast<uint32_t>(v) * 0x10u;
        const uint16_t adsr1 = regValue(regs + 6u);
        const uint16_t adsr2 = regValue(regs + 8u);
        switch (voice.phase)
        {
        case AdsrPhase::Attack:
            voice.level = voice.adsr.tick(voice.level);
            if (voice.level >= 0x7FFF)
            {
                voice.level = 0x7FFF;
                voice.phase = AdsrPhase::Decay;
                voice.adsr.reset(static_cast<uint8_t>(((adsr1 >> 4) & 0xFu) << 2), true, true);
            }
            break;
        case AdsrPhase::Decay:
        {
            voice.level = voice.adsr.tick(voice.level);
            const int32_t sustain = std::min<int32_t>(((adsr1 & 0xF) + 1) * 0x800, 0x7FFF);
            if (voice.level <= sustain)
            {
                voice.level = std::max(voice.level, 0);
                voice.phase = AdsrPhase::Sustain;
                voice.adsr.reset(static_cast<uint8_t>((adsr2 >> 6) & 0x7Fu), (adsr2 & 0x4000u) != 0u, (adsr2 & 0x8000u) != 0u);
            }
            break;
        }
        case AdsrPhase::Sustain:
            voice.level = voice.adsr.tick(voice.level);
            break;
        case AdsrPhase::Release:
            voice.level = voice.adsr.tick(voice.level);
            if (voice.level <= 0)
            {
                voice.level = 0;
                voice.phase = AdsrPhase::Off;
            }
            break;
        case AdsrPhase::Off:
            break;
        }
    }

    int32_t IopSpu2::applyVolume(uint16_t value, int32_t &current, Envelope &envelope, uint16_t &lastReg)
    {
        if ((value & 0x8000u) == 0u)
        {
            current = static_cast<int16_t>(static_cast<uint16_t>(value << 1));
            lastReg = value;
            return current;
        }
        // Sweep: bit 14 exponential, bit 13 decreasing, bit 12 inverted phase, bits 6-0 rate.
        if (value != lastReg)
        {
            envelope.reset(static_cast<uint8_t>(value & 0x7Fu), (value & 0x2000u) != 0u, (value & 0x4000u) != 0u);
            lastReg = value;
        }
        const int32_t magnitude = envelope.tick(std::abs(current));
        current = (value & 0x1000u) != 0u ? -magnitude : magnitude;
        return current;
    }

    void IopSpu2::mixSample(int32_t &finalL, int32_t &finalR)
    {
        for (int c = 0; c < 2; ++c)
        {
            Core &core = m_cores[c];
            const uint32_t base = static_cast<uint32_t>(c) * kCoreStride;
            const uint32_t pmon = voiceMask(c, kPmon);
            const uint32_t non = voiceMask(c, kNon);
            const uint32_t vmixl = voiceMask(c, kVmixl);
            const uint32_t vmixr = voiceMask(c, kVmixr);
            const uint16_t attr = regValue(base + kAttr);

            // Noise generator (shared by the core's voices with NON set).
            const uint32_t noiseClock = (attr >> 8) & 0x3Fu;
            const uint32_t noiseShift = noiseClock >> 2;
            core.noiseCounter += (noiseClock & 3u) + 4u;
            const uint32_t noiseThreshold = 0x20000u >> noiseShift;
            while (core.noiseCounter >= noiseThreshold)
            {
                core.noiseCounter -= noiseThreshold;
                const int32_t l = core.noiseLevel;
                const int32_t parity = ((l >> 15) ^ (l >> 12) ^ (l >> 11) ^ (l >> 10) ^ 1) & 1;
                core.noiseLevel = ((l << 1) | parity) & 0xFFFF;
            }

            int32_t dryL = 0, dryR = 0;
            int32_t previousOut = 0;
            for (int v = 0; v < 24; ++v)
            {
                Voice &voice = core.voices[v];
                const uint32_t bit = 1u << v;
                if (voice.phase == AdsrPhase::Off && voice.level == 0)
                {
                    voice.lastOut = 0;
                    previousOut = 0;
                    continue;
                }
                const uint32_t regs = base + static_cast<uint32_t>(v) * 0x10u;
                int32_t pitch = std::min<int32_t>(regValue(regs + 4u), 0x3FFF);
                if ((pmon & bit) != 0u && v > 0)
                    pitch = std::clamp((pitch * (previousOut + 0x8000)) >> 15, 0, 0x3FFF);

                voice.counter += static_cast<uint32_t>(pitch);
                while (voice.counter >= 0x1000u)
                {
                    voice.counter -= 0x1000u;
                    voice.window[0] = voice.window[1];
                    voice.window[1] = voice.window[2];
                    voice.window[2] = voice.window[3];
                    voice.window[3] = nextSample(c, v);
                }

                int32_t sample;
                if ((non & bit) != 0u)
                {
                    sample = static_cast<int16_t>(static_cast<uint16_t>(core.noiseLevel));
                }
                else
                {
                    // 4-point cubic (Catmull-Rom) between window[1] and window[2].
                    const float t = static_cast<float>(voice.counter) * (1.0f / 4096.0f);
                    const float p0 = voice.window[0], p1 = voice.window[1], p2 = voice.window[2], p3 = voice.window[3];
                    const float value = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
                    sample = clamp16(static_cast<int32_t>(value));
                }

                const int32_t out = (sample * voice.level) >> 15;
                stepAdsr(voice, c, v);
                voice.lastOut = out;
                previousOut = out;

                const int32_t volL = applyVolume(regValue(regs + 0u), voice.volX[0], voice.sweep[0], voice.sweepReg[0]);
                const int32_t volR = applyVolume(regValue(regs + 2u), voice.volX[1], voice.sweep[1], voice.sweepReg[1]);
                if ((vmixl & bit) != 0u)
                    dryL += (out * volL) >> 15;
                if ((vmixr & bit) != 0u)
                    dryR += (out * volR) >> 15;
            }

            // ADMA PCM input ("memory in"): one frame per sample while auto-DMA is enabled.
            int32_t inL = 0, inR = 0;
            if ((regValue(base + kAdmas) & (1u << c)) != 0u && !core.memIn.empty())
            {
                const auto frame = core.memIn.front();
                core.memIn.pop_front();
                const uint32_t inputBase = 0x2000u + static_cast<uint32_t>(c) * 0x400u;
                checkIrq(inputBase + core.memInPos, 1u);
                checkIrq(inputBase + 0x200u + core.memInPos, 1u);
                core.memInPos = (core.memInPos + 1u) & 0x1FFu;
                const uint32_t shared = kSharedVolBase + static_cast<uint32_t>(c) * kSharedVolStride;
                // BVOL (like AVOL and EVOL) is a plain signed 16-bit volume, not the fixed/sweep format.
                inL = (frame[0] * static_cast<int16_t>(regValue(shared + 0xCu))) >> 15;
                inR = (frame[1] * static_cast<int16_t>(regValue(shared + 0xEu))) >> 15;
            }

            // Core 1's external input is core 0's output, scaled by core 1's AVOL.
            int32_t extL = 0, extR = 0;
            if (c == 1)
            {
                const uint32_t shared = kSharedVolBase + kSharedVolStride;
                extL = (m_cores[0].out[0] * static_cast<int16_t>(regValue(shared + 8u))) >> 15;
                extR = (m_cores[0].out[1] * static_cast<int16_t>(regValue(shared + 0xAu))) >> 15;
            }

            // MMIX: bit 11/10 voice dry L/R, 7/6 memory-in dry L/R, 3/2 external dry L/R.
            const uint16_t mmix = regValue(base + kMmix);
            int32_t outL = ((mmix & 0x800u) ? dryL : 0) + ((mmix & 0x80u) ? inL : 0) + ((mmix & 0x8u) ? extL : 0);
            int32_t outR = ((mmix & 0x400u) ? dryR : 0) + ((mmix & 0x40u) ? inR : 0) + ((mmix & 0x4u) ? extR : 0);
            outL = clamp16(outL);
            outR = clamp16(outR);

            const uint32_t shared = kSharedVolBase + static_cast<uint32_t>(c) * kSharedVolStride;
            const int32_t mvolL = applyVolume(regValue(shared + 0u), core.mvolX[0], core.mvolSweep[0], core.mvolSweepReg[0]);
            const int32_t mvolR = applyVolume(regValue(shared + 2u), core.mvolX[1], core.mvolSweep[1], core.mvolSweepReg[1]);
            core.out[0] = clamp16((outL * mvolL) >> 15);
            core.out[1] = clamp16((outR * mvolR) >> 15);
        }
        finalL = m_cores[1].out[0];
        finalR = m_cores[1].out[1];
    }

    void IopSpu2::advanceTo(uint64_t iopCycle)
    {
        if (!m_cycleValid)
        {
            m_cycle = iopCycle;
            m_cycleValid = true;
            return;
        }
        if (iopCycle <= m_cycle)
            return;
        uint64_t samples = (iopCycle - m_cycle) / IopCyclesPerSample;
        // Never try to catch up more than a second at once (e.g. after a long stall).
        if (samples > 48000u)
        {
            m_cycle = iopCycle - 48000u * IopCyclesPerSample;
            samples = 48000u;
        }
        for (uint64_t i = 0; i < samples; ++i)
        {
            int32_t left = 0, right = 0;
            mixSample(left, right);
            ps2x::iop::pushFrame(static_cast<int16_t>(left), static_cast<int16_t>(right));
            m_statPeak = std::max({m_statPeak, std::abs(left), std::abs(right)});
            if (++m_statSamples % 48000u == 0u && logEnabled())
            {
                int active = 0;
                for (const Core &core : m_cores)
                    for (const Voice &voice : core.voices)
                        active += voice.phase != AdsrPhase::Off ? 1 : 0;
                std::fprintf(stderr,
                             "[spu2] t=%llus keyon=%llu active=%d dma=%llu admaFrames=%llu irq=%llu peak=%d "
                             "mmix=%04x/%04x attr=%04x/%04x admas=%x/%x mvol=%04x,%04x/%04x,%04x\n",
                             static_cast<unsigned long long>(m_statSamples / 48000u),
                             static_cast<unsigned long long>(m_statKeyOn), active,
                             static_cast<unsigned long long>(m_statDma),
                             static_cast<unsigned long long>(m_statAdmaFrames),
                             static_cast<unsigned long long>(m_statIrq), m_statPeak,
                             regValue(kMmix), regValue(kCoreStride + kMmix), regValue(kAttr), regValue(kCoreStride + kAttr),
                             regValue(kAdmas), regValue(kCoreStride + kAdmas),
                             regValue(kSharedVolBase), regValue(kSharedVolBase + 2u),
                             regValue(kSharedVolBase + kSharedVolStride), regValue(kSharedVolBase + kSharedVolStride + 2u));
                m_statPeak = 0;
            }
        }
        m_cycle += samples * IopCyclesPerSample;
    }

    uint64_t IopSpu2::startDma(int c, uint8_t *iopRam, uint32_t iopRamSize, uint32_t madr, uint32_t bytes, bool toSpu)
    {
        c &= 1;
        Core &core = m_cores[c];
        madr &= 0x1FFFFFu;
        if (madr >= iopRamSize)
            return 64u;
        bytes = std::min(bytes, iopRamSize - madr);
        ++m_statDma;

        const uint32_t base = static_cast<uint32_t>(c) * kCoreStride;
        if (toSpu && (regValue(base + kAdmas) & (1u << c)) != 0u)
        {
            // Auto-DMA: 1 KB blocks of 256 left samples followed by 256 right samples.
            const uint32_t blocks = bytes / 1024u;
            for (uint32_t b = 0; b < blocks; ++b)
            {
                const uint8_t *block = iopRam + madr + b * 1024u;
                for (uint32_t i = 0; i < 256u; ++i)
                {
                    int16_t left, right;
                    std::memcpy(&left, block + i * 2u, sizeof(left));
                    std::memcpy(&right, block + 512u + i * 2u, sizeof(right));
                    core.memIn.push_back({left, right});
                }
            }
            m_statAdmaFrames += static_cast<uint64_t>(blocks) * 256u;
            // The SPU2 double-buffers 512 frames; the transfer ends once its data is buffered.
            const uint64_t queued = core.memIn.size();
            return queued > 512u ? (queued - 512u) * IopCyclesPerSample : 64u;
        }

        const uint32_t halfwords = bytes / 2u;
        if (toSpu)
        {
            for (uint32_t i = 0; i < halfwords; ++i)
            {
                uint16_t value;
                std::memcpy(&value, iopRam + madr + i * 2u, sizeof(value));
                m_ram[(core.tsa + i) & kAddressMask] = value;
            }
        }
        else
        {
            for (uint32_t i = 0; i < halfwords; ++i)
            {
                const uint16_t value = m_ram[(core.tsa + i) & kAddressMask];
                std::memcpy(iopRam + madr + i * 2u, &value, sizeof(value));
            }
        }
        checkIrq(core.tsa, halfwords);
        core.tsa = (core.tsa + halfwords) & kAddressMask;
        setAddressPair(base + kTsa, core.tsa);
        return std::max<uint64_t>(halfwords, 64u);
    }
}
