#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ps2x::iop::detail
{
    // The SPU2 sound chip: two cores of 24 ADPCM voices with ADSR envelopes, reverb and 2 MB of sound
    // RAM. Registers are 16 bits wide and sound RAM is addressed in 16-bit words. The chip makes one
    // stereo sample every 768 IOP cycles (48 kHz); core 0's output feeds core 1, and core 1's output
    // is what you hear.
    //
    // Not emulated: the SPDIF output.
    class IopSpu2
    {
    public:
        static constexpr uint32_t kCyclesPerSample = 768u;
        static constexpr uint32_t kRamWords = 0x100000u;
        static constexpr uint32_t kRegisterBase = 0x1F900000u;

        using AudioSink = std::function<void(const int16_t *stereo, size_t frames)>;

        IopSpu2();

        void reset();
        void setSink(AudioSink sink) { m_sink = std::move(sink); }

        [[nodiscard]] uint16_t read16(uint32_t physicalAddress);
        void write16(uint32_t physicalAddress, uint16_t value);

        // Copies a DMA transfer between IOP RAM and sound RAM, at the core's transfer address.
        void dma(int core, uint8_t *iopRam, uint32_t iopRamSize, uint32_t address, uint32_t words32, bool toSpu);

        // AutoDMA: with a core's ADMAS register set, a DMA transfer feeds that core's stereo input as
        // the chip plays it (blocks of 0x200 bytes left then 0x200 bytes right), rather than copying
        // to sound RAM. The chip buffers ahead of what it plays, so a transfer finishes as its last
        // block starts playing, and the driver's next transfer follows that block without a gap.
        [[nodiscard]] bool autoDmaEnabled(int core) const;
        [[nodiscard]] bool autoDmaActive(int core) const { return m_cores[core].adma.active; }
        [[nodiscard]] uint32_t autoDmaAddress(int core) const
        {
            const AutoDma &adma = m_cores[core].adma;
            return adma.nextBytes != 0u ? adma.nextAddress : adma.address;
        }
        [[nodiscard]] bool takeAutoDmaDone(int core);
        void startAutoDma(int core, const uint8_t *iopRam, uint32_t iopRamSize, uint32_t address, uint32_t bytes);
        void stopAutoDma(int core);

        void tick(); // one 48 kHz sample
        void flush(); // hands buffered samples to the sink

        // True once if an IRQ address was hit since the last call.
        [[nodiscard]] bool takeInterrupt();

    private:
        enum class Phase : uint8_t
        {
            Off,
            Attack,
            Decay,
            Sustain,
            Release,
        };

        struct Slide
        {
            uint16_t reg = 0;
            int32_t value = 0; // -0x8000..0x7FFF
            uint32_t wait = 0;

            void write(uint16_t reg);
            void step();
        };

        struct Voice
        {
            Slide volL, volR;
            uint16_t pitch = 0;
            uint16_t adsr1 = 0, adsr2 = 0;
            uint32_t ssa = 0, lsa = 0, nax = 0;
            bool loopWritten = false;

            Phase phase = Phase::Off;
            int32_t env = 0;
            uint32_t envWait = 0;

            uint32_t counter = 0; // position between samples, 12-bit fraction
            int32_t adpcm1 = 0, adpcm2 = 0;
            std::array<int16_t, 28> block{};
            uint32_t blockIndex = 28;
            uint32_t blockAddress = 0;
            uint8_t blockFlags = 0;
            std::array<int16_t, 4> history{};
            int32_t lastOut = 0;
        };

        struct AutoDma
        {
            const uint8_t *ram = nullptr;
            uint32_t ramSize = 0;
            uint32_t address = 0;
            uint32_t bytes = 0;
            uint32_t sample = 0;
            uint32_t nextAddress = 0;
            uint32_t nextBytes = 0;
            bool active = false;
            bool done = false;
        };

        struct Core
        {
            AutoDma adma;
            std::array<Voice, 24> voices{};
            std::array<uint16_t, 0x200> regs{};  // by register offset / 2, below 0x400
            std::array<uint16_t, 0x14> misc{};   // volumes and reverb coefficients, by offset from 0x760 / 2
            Slide masterL, masterR;
            uint32_t tsa = 0, irqa = 0;
            uint32_t endx = 0;
            uint32_t active = 0;
            uint32_t reverbPos = 0;
            int32_t reverbInL = 0, reverbInR = 0;
            int32_t reverbOutL = 0, reverbOutR = 0;
            int32_t reverbPrevOutL = 0, reverbPrevOutR = 0;
            bool reverbOdd = false;
            uint32_t noiseWait = 0;
            uint16_t noise = 1;
            int32_t noiseSign = 1;
        };

        [[nodiscard]] uint32_t regAddress(const Core &core, uint32_t reg) const;
        void writeCoreRegister(int coreIndex, uint32_t reg, uint16_t value);
        [[nodiscard]] uint16_t readCoreRegister(int coreIndex, uint32_t reg);
        void keyOn(Core &core, uint32_t mask);
        void keyOff(Core &core, uint32_t mask);
        void checkIrq(uint32_t first, uint32_t words);
        void fetchBlock(int coreIndex, int voiceIndex);
        [[nodiscard]] int32_t voiceSample(int coreIndex, int voiceIndex, int32_t previousOut);
        void stepEnvelope(Voice &voice);
        void stepNoise(Core &core);
        void readInput(Core &core, int32_t &left, int32_t &right);
        void mixCore(int coreIndex, int32_t extL, int32_t extR, int32_t &outL, int32_t &outR);
        void runReverb(Core &core, int32_t inL, int32_t inR, int32_t &outL, int32_t &outR);

        std::vector<uint16_t> m_ram;
        std::array<Core, 2> m_cores{};
        uint32_t m_outputPos = 0;
        uint16_t m_irqInfo = 0;
        bool m_interrupt = false;
        std::vector<int16_t> m_buffer;
        AudioSink m_sink;
    };
}
