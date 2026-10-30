#include "runtime/ps2_vu1_native.h"

#include <algorithm>
#include <cstdio>
#include <vector>
#include <xmmintrin.h>

namespace vu1n
{
    namespace
    {
        std::vector<const BlockDesc *> &registry(Unit unit)
        {
            static std::vector<const BlockDesc *> blocks[2];
            return blocks[static_cast<int>(unit)];
        }

        struct Table
        {
            BlockFn fns[kDataSize / 8u]{};
            const uint8_t *code = nullptr;
            uint64_t generation = 0;
            bool valid = false;
        };

        Table &table(Unit unit)
        {
            static Table t[2];
            return t[static_cast<int>(unit)];
        }

        void rebuild(Unit unit, Table &t, const uint8_t *code, uint64_t generation)
        {
            std::fill(std::begin(t.fns), std::end(t.fns), nullptr);
            for (const BlockDesc *block : registry(unit))
            {
                BlockFn &slot = t.fns[block->pc >> 3];
                if (slot == nullptr && std::memcmp(code + block->pc, block->words, block->pairs * 8u) == 0)
                    slot = block->fn;
            }
            t.code = code;
            t.generation = generation;
            t.valid = true;
        }

        Ctx &context()
        {
            static Ctx *c = new Ctx();
            return *c;
        }

        void flush(Ctx &c)
        {
            if (c.qPending)
            {
                c.q = c.qVal;
                c.qPending = false;
            }
            c.cyc = ~0u;
            commitP(c);
            if (c.kickActive)
                kickSubmit(c);
        }

        void logOnce(Unit unit, const char *what, uint32_t pc)
        {
            static uint8_t seen[2][2][kDataSize / 8u]{};
            const int kind = what[0] == 'b' ? 0 : 1;
            if (seen[static_cast<int>(unit)][kind][(pc & kPcMask) >> 3]++ == 0)
                std::fprintf(stderr, "[VU%dN] %s at pc=0x%04x; continuing in the interpreter\n",
                             unit == Unit::VU1 ? 1 : 0, what, pc);
        }
    }

    void registerBlocks(Unit unit, const BlockDesc *blocks, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
            registry(unit).push_back(&blocks[i]);
        table(unit).valid = false;
    }

    void kickStorePatch(Ctx &c, uint32_t addr, const uint32_t *words, int sseLaneMask)
    {
        const uint32_t offset = (addr - c.kickSrc) & (kDataSize - 1u);
        if (offset >= c.kickBytes)
            return;
        // PATH1 reads qword i at kickIssue + 1 + 2i; a store issued at cycle s is visible from s + 1.
        if (c.cyc > c.kickIssue + 2u * (offset / 16u))
            return;
        uint32_t *dst = reinterpret_cast<uint32_t *>(c.kickBuf + offset);
        for (int lane = 0; lane < 4; ++lane)
            if ((sseLaneMask >> lane) & 1)
                dst[lane] = words[lane];
    }

    void kickSubmit(Ctx &c)
    {
        c.kickActive = false;
        if (c.kickSink)
            c.kickSink(c.kickUser, c.kickBuf, c.kickBytes);
    }

    void xgkick(Ctx &c, int32_t vi)
    {
        if (c.kickActive)
            kickSubmit(c);

        // Same packet walk as VU1Interpreter::progressXgkick, done at issue time.
        const uint32_t src = (static_cast<uint32_t>(static_cast<uint16_t>(vi)) * 16u) % kDataSize;
        uint32_t copied = 0u;
        uint32_t tagEnd = 0u;
        bool eop = false;
        for (;;)
        {
            if (copied > sizeof(c.kickBuf) - 16u)
            {
                std::fprintf(stderr, "[VU1N] XGKICK packet from 0x%04x exceeds 64KB; dropped\n", src);
                return;
            }
            const uint32_t qwordOffset = copied;
            for (uint32_t i = 0; i < 16u; ++i)
                c.kickBuf[copied + i] = c.mem[(src + copied + i) % kDataSize];
            copied += 16u;

            if (tagEnd == 0u)
            {
                uint64_t tagLo = 0;
                std::memcpy(&tagLo, c.kickBuf + qwordOffset, sizeof(tagLo));
                const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
                uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                uint64_t tagBytes = 16u;
                if (format == 0u)
                    tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
                else if (format == 1u)
                    tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
                else
                    tagBytes += static_cast<uint64_t>(nloop) * 16u;
                if (tagBytes > sizeof(c.kickBuf) - qwordOffset)
                {
                    std::fprintf(stderr, "[VU1N] XGKICK packet from 0x%04x exceeds 64KB; dropped\n", src);
                    return;
                }
                tagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
                eop = ((tagLo >> 15) & 1u) != 0u;
            }
            if (copied >= tagEnd)
            {
                if (eop)
                    break;
                tagEnd = 0u;
            }
        }

        c.kickActive = true;
        c.kickIssue = c.cyc;
        c.kickSrc = src;
        c.kickBytes = copied;
        c.kickDone = c.cyc + 2u * (copied / 16u) - 1u;
    }

    RunResult run(Unit unit, VU1State &state, const uint8_t *code, uint64_t codeGeneration, uint8_t *data,
                  uint32_t startPc, uint32_t top, uint32_t itop, uint32_t maxCycles, KickSink sink, void *sinkUser)
    {
        Table &t = table(unit);
        if (!t.valid || t.code != code || t.generation != codeGeneration)
            rebuild(unit, t, code, codeGeneration);

        const uint32_t pcMask = unit == Unit::VU1 ? kPcMask : 0x0FFFu;
        startPc &= pcMask;
        BlockFn fn = t.fns[startPc >> 3];
        if (fn == nullptr || (startPc & 7u) != 0u)
            return RunResult::NotTranslated;

        Ctx &c = context();
        std::memcpy(c.vf, state.vf, sizeof(c.vf));
        c.vf[0][0] = c.vf[0][1] = c.vf[0][2] = 0.0f;
        c.vf[0][3] = 1.0f;
        std::memcpy(c.acc, state.acc, sizeof(c.acc));
        std::memcpy(c.vi, state.vi, sizeof(c.vi));
        c.vi[0] = 0;
        c.q = state.q;
        c.p = state.p;
        c.i = state.i;
        c.mem = data;
        c.top = top;
        c.cyc = 0u;
        c.endPc = 0u;
        std::memset(c.vfReady, 0, sizeof(c.vfReady));
        std::memset(c.viReady, 0, sizeof(c.viReady));
        c.macHead = c.macCount = 0u;
        c.macBase = state.mac;
        c.clipHead = c.clipCount = 0u;
        c.clipBase = state.clip;
        c.workingClip = state.clip;
        c.qPending = false;
        c.pPending[0] = c.pPending[1] = false;
        c.efuReady = 0u;
        c.kickActive = false;
        c.kickDone = 0u;
        c.kickSink = sink;
        c.kickUser = sinkUser;
        c.bkReg = 0u;
        c.bkVal = 0;

        // Match the interpreter's FE_TOWARDZERO; leave DAZ/FTZ as they are.
        const unsigned int savedCsr = _mm_getcsr();
        _mm_setcsr((savedCsr & ~0x6000u) | 0x6000u);

        RunResult result = RunResult::Done;
        bool ended = true;
        uint32_t pc = startPc;
        for (;;)
        {
            const uint32_t ret = fn(c);
            if (ret == kEnd)
            {
                pc = c.endPc & pcMask;
                break;
            }
            pc = ret & pcMask;
            if ((ret & kBailFlag) != 0u)
            {
                logOnce(unit, "block not translatable", pc);
                result = RunResult::Bailed;
                break;
            }
            fn = t.fns[pc >> 3];
            if (fn == nullptr)
            {
                logOnce(unit, "no translated block", pc);
                result = RunResult::Bailed;
                break;
            }
            if (c.cyc >= maxCycles)
            {
                static bool warned[2] = {};
                if (!warned[static_cast<int>(unit)])
                {
                    warned[static_cast<int>(unit)] = true;
                    std::fprintf(stderr, "[VU%dN] program from 0x%04x ran past its %u-cycle budget; stopping at 0x%04x\n",
                                 unit == Unit::VU1 ? 1 : 0, startPc, maxCycles, pc);
                }
                ended = false;
                break;
            }
        }

        const uint32_t cycles = c.cyc;
        if (ended)
            flush(c);
        _mm_setcsr(savedCsr);

        std::memcpy(state.vf, c.vf, sizeof(c.vf));
        std::memcpy(state.acc, c.acc, sizeof(c.acc));
        std::memcpy(state.vi, c.vi, sizeof(c.vi));
        state.q = c.q;
        state.p = c.p;
        state.i = c.i;
        state.mac = c.macCount ? c.macVal[c.macHead] : c.macBase;
        state.clip = c.clipCount ? c.clipVal[c.clipHead] : c.clipBase;
        state.pc = pc;
        state.top = top;
        state.itop = itop;
        state.ebit = false;
        state.haltAfterDelaySlot = false;
        state.stoppedByD = false;
        state.stoppedByT = false;
        state.branchPending = false;
        state.cycles += cycles;
        return result;
    }
}
