#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace ps2x::iop::detail
{
    // SPU2 sound processor: two cores of 24 voices sharing 2 MB of sound RAM, mixed to 48 kHz
    // stereo. Register map and behaviour follow PCSX2's SPU2 and the PSX SPU (ADPCM, ADSR) that the
    // SPU2 cores extend. Reverb is not implemented yet: the wet (effects) send is dropped.
    class IopSpu2
    {
    public:
        static constexpr uint32_t RegisterBase = 0x1F900000u;
        static constexpr uint32_t RamHalfwords = 0x100000u; // 2 MB
        static constexpr uint32_t IopCyclesPerSample = 768u; // 36.864 MHz / 48 kHz
        static constexpr int Irq = 9;

        IopSpu2();
        void reset();

        [[nodiscard]] uint16_t read16(uint32_t address);
        void write16(uint32_t address, uint16_t value);

        // An SPU2 DMA (IOP channel 4 = core 0, channel 7 = core 1) was started. Copies the data
        // and returns how many IOP cycles until the transfer counts as complete. Auto-DMA (ADMA)
        // transfers feed the core's PCM input and complete as playback consumes them.
        uint64_t startDma(int core, uint8_t *iopRam, uint32_t iopRamSize, uint32_t madr, uint32_t bytes, bool toSpu);
        // Clears STATX's DMA-ready bit while a transfer runs and sets it when it ends.
        void setDmaReady(int core, bool ready);

        // Generates samples up to the given IOP cycle.
        void advanceTo(uint64_t iopCycle);
        // True once per raised SPU2 interrupt (IRQA hit with the core's IRQ enabled).
        [[nodiscard]] bool takeIrq();

    private:
        struct Envelope
        {
            uint8_t rate = 0;
            bool decreasing = false;
            bool exponential = false;
            uint32_t counter = 0;

            void reset(uint8_t newRate, bool newDecreasing, bool newExponential);
            [[nodiscard]] int32_t tick(int32_t level);
        };

        enum class AdsrPhase : uint8_t
        {
            Off,
            Attack,
            Decay,
            Sustain,
            Release
        };

        struct Voice
        {
            uint32_t blockAddr = 0;       // halfword address of the ADPCM block being played
            uint32_t loopAddr = 0;        // LSAX
            bool loopLocked = false;      // LSAX written while the voice was on: ignore loop-start flags
            std::array<int16_t, 28> decoded{};
            uint8_t decodedFlags = 0;
            int decodedPos = 28;          // 28 = need the next block
            bool firstBlock = true;
            int32_t hist1 = 0;
            int32_t hist2 = 0;
            std::array<int16_t, 4> window{}; // last four decoded samples for interpolation
            uint32_t counter = 0;            // 4.12 pitch phase
            AdsrPhase phase = AdsrPhase::Off;
            int32_t level = 0;
            Envelope adsr;
            int32_t lastOut = 0;            // output after envelope (pitch modulation source)
            int32_t volX[2] = {0, 0};
            Envelope sweep[2];
            uint16_t sweepReg[2] = {0, 0};
        };

        struct Core
        {
            std::array<Voice, 24> voices;
            uint32_t endx = 0;
            uint16_t statx = 0; // DMA-ready (0x80) is not set at reset: LIBSD init waits for STATX == 0
            uint32_t tsa = 0;
            bool irqPending = false;
            uint32_t noiseCounter = 0;
            int32_t noiseLevel = 1;
            int32_t mvolX[2] = {0, 0};
            Envelope mvolSweep[2];
            uint16_t mvolSweepReg[2] = {0, 0};
            uint32_t memInPos = 0;        // ADMA input buffer read position (0..511)
            int32_t out[2] = {0, 0};      // last mixed core output (core 0 feeds core 1's external input)
            // ADMA PCM input, interleaved stereo frames.
            std::deque<std::array<int16_t, 2>> memIn;
        };

        [[nodiscard]] uint16_t &reg(uint32_t offset) { return m_regs[(offset & 0x7FFu) >> 1]; }
        [[nodiscard]] uint16_t regValue(uint32_t offset) const { return m_regs[(offset & 0x7FFu) >> 1]; }
        [[nodiscard]] uint32_t addressPair(uint32_t hiOffset) const;
        void setAddressPair(uint32_t hiOffset, uint32_t address);
        [[nodiscard]] uint32_t voiceMask(int core, uint32_t lowOffset) const;

        void keyOn(int core, uint32_t mask);
        void keyOff(int core, uint32_t mask);
        void checkIrq(uint32_t address, uint32_t count);
        void decodeBlock(int core, int voiceIndex);
        [[nodiscard]] int16_t nextSample(int core, int voiceIndex);
        void stepAdsr(Voice &voice, int core, int voiceIndex);
        [[nodiscard]] static int32_t applyVolume(uint16_t regValue, int32_t &current, Envelope &envelope, uint16_t &lastReg);
        void mixSample(int32_t &outL, int32_t &outR);

        std::vector<uint16_t> m_ram;
        std::array<uint16_t, 0x400> m_regs{};
        std::array<Core, 2> m_cores;
        uint64_t m_cycle = 0;
        // Debug counters (DHP1_SPU2_LOG / PS2X_SPU2_LOG).
        uint64_t m_statKeyOn = 0, m_statDma = 0, m_statAdmaFrames = 0, m_statIrq = 0, m_statSamples = 0;
        int32_t m_statPeak = 0;
        bool m_cycleValid = false;
        bool m_irq = false;
    };
}
