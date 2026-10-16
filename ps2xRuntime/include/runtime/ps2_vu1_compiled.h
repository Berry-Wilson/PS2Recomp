#pragma once

// Ahead-of-time compiled VU1 microprograms.
//
// A "code image" is the full VU1 micro memory (16 KiB) at the time a program runs; it is
// identified by a 64-bit FNV-1a hash. Images are captured at runtime (setVu1ImageDumpDir),
// translated offline to C++ (one block per instruction pair, mirroring
// VU1Interpreter::run with the decoded usage folded to constants) and registered with
// registerVu1CompiledImage. VU1Interpreter::run executes a registered image instead of
// interpreting it, falling back to the interpreter for any PC the translation does not cover.
// Compiled images assume immediate writeback (setVu1ImmediateWriteback) and are skipped
// otherwise.

#include "runtime/ps2_vu1.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

class GS;
class PS2Memory;

enum class Vu1CompiledResult : int
{
    Budget = 0,   // cycle budget exhausted or stop requested; m_state.pc is the next pair
    Ended = 1,    // E/D/T-bit end reached; run() flushes the pipelines
    Fallback = 2, // reached a PC outside the translation; interpret from m_state.pc
};

using Vu1CompiledFn = Vu1CompiledResult (*)(VU1Interpreter &vu, uint64_t budgetEnd);

// statusVerifyMask: status bits verify mode compares at program end. The translator clears
// the sticky bits (0xFC0) when it proved them unobservable and skipped flag updates.
void registerVu1CompiledImage(uint64_t imageHash, Vu1CompiledFn fn, uint32_t statusVerifyMask = 0xFFFu);
// Writes every newly seen VU1 code image (<hash>.bin) and MSCAL entry PCs (<hash>.entries)
// to the directory; empty disables dumping.
void setVu1ImageDumpDir(const char *directory);
// Off switch for comparisons (compiled images are used when registered).
void setVu1CompiledImagesEnabled(bool enabled);
// Debug: after each compiled run, re-run the same program on a copy of the interpreter from
// the same starting state (XGKICK output suppressed) and report register/memory/cycle
// mismatches on stderr.
void setVu1CompiledVerify(bool enabled);
uint64_t hashVu1CodeImage(const uint8_t *code, uint32_t size);

// Access used by generated code and the translator (friend of VU1Interpreter).
// Operand normalization, identical to VU1Interpreter::normalizeOperand.
inline float vu1NormalizeOperand(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u)
        bits &= 0x80000000u;
    else if (exponent == 0xFFu)
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Applies the interpreter's FMAC result normalization to a single-rounding result (add/sub/mul
// of normalized operands under round-toward-zero) when it can be decided from the float alone:
// normal values below FLT_MAX are unchanged, zero/denormal results become a zero with the
// result's sign (the exact result has the same sign and is zero or below FLT_MIN). Returns
// false (value untouched) when the exact recomputation is needed.
inline bool vu1NormalizeSingleRounding(float &result)
{
    uint32_t bits;
    std::memcpy(&bits, &result, sizeof(bits));
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u)
    {
        bits &= 0x80000000u;
        std::memcpy(&result, &bits, sizeof(result));
        return true;
    }
    return exponent != 0xFFu && (bits & 0x7FFFFFFFu) != 0x7F7FFFFFu;
}

// Same for multiply-accumulate results (acc +- product): the exact acc +- product the
// interpreter recomputes is within 25% of the float result when there is no heavy
// cancellation (|r| >= |p| * 2^-20) and both are well inside the float range, so its
// normalization cannot change the value.
inline bool vu1ProductSumInRange(float result, float product)
{
    uint32_t rbits, pbits;
    std::memcpy(&rbits, &result, sizeof(rbits));
    std::memcpy(&pbits, &product, sizeof(pbits));
    const uint32_t rexp = (rbits >> 23) & 0xFFu;
    const uint32_t pexp = (pbits >> 23) & 0xFFu;
    return rexp >= 5u && rexp <= 0xFCu && pexp <= 0xFCu &&
           std::fabs(result) >= std::fabs(product) * 9.5367431640625e-7f;
}

// FTOIn conversion, identical to the interpreter's (saturating, truncating).
inline int32_t vu1FloatToInt(float value, float scale)
{
    const double scaled = static_cast<double>(value) * static_cast<double>(scale);
    if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
        return std::numeric_limits<int32_t>::max();
    if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(scaled);
}

class Vu1CompiledAccess
{
public:
    // Value normalization of an FMAC result vector exactly as the interpreter does it (flags
    // are computed and discarded); used when a fast range check fails.
    static void normalizeFmacValues(VU1Interpreter &vu, uint32_t upper, float *result, uint8_t dest)
    {
        vu.m_currentUpperInstruction = upper;
        uint8_t laneFlags[4];
        vu.normalizeFmacResult(result, dest, laneFlags);
    }
    using Decoded = VU1Interpreter::DecodedInstructionPair;
    using Usage = VU1Interpreter::InstructionUsage;

    static VU1State &state(VU1Interpreter &vu) { return vu.m_state; }
    static uint64_t &cycle(VU1Interpreter &vu) { return vu.m_cycle; }
    static bool stopRequested(const VU1Interpreter &vu) { return vu.m_stopRequested; }

    static void commitReadyPipelines(VU1Interpreter &vu)
    {
        if (vu.m_cycle >= vu.m_nextCommitCycle)
            vu.commitReadyPipelinesNow();
    }
    static void advanceTo(VU1Interpreter &vu, uint64_t target) { vu.advanceTo(target); }
    // Same steps as VU1Interpreter::advanceOneCycle, inlined.
    static void advanceOneCycle(VU1Interpreter &vu)
    {
        ++vu.m_cycle;
        vu.m_state.cycles = vu.m_cycle;
        commitReadyPipelines(vu);
        if (vu.m_xgkick.active)
            vu.progressXgkick();
    }
    static uint64_t pairReadyCycle(const VU1Interpreter &vu, const Decoded &decoded) { return vu.calculatePairReadyCycle(decoded); }
    static void markPairWrites(VU1Interpreter &vu, const Decoded &decoded) { vu.markPairWrites(decoded); }
    static void recordViWriteForBranch(VU1Interpreter &vu, uint8_t reg, int32_t oldValue) { vu.recordViWriteForBranch(reg, oldValue); }
    static void clearViBranchBackup(VU1Interpreter &vu) { vu.m_viBranchBackupValid = false; }
    static uint32_t microAddressMask(const VU1Interpreter &vu) { return vu.microAddressMask(); }
    static float normalizeOperand(const VU1Interpreter &vu, float value) { return vu.normalizeOperand(value); }

    static void execUpper(VU1Interpreter &vu, uint32_t instr) { vu.execUpper(instr); }
    static void execLower(VU1Interpreter &vu, uint32_t instr, uint32_t upper)
    {
        vu.execLower(instr, vu.m_activeVuData, vu.m_activeVuDataSize, *vu.m_activeGs, vu.m_activeMemory, upper);
    }

    // Inline lower instructions (LQ/SQ/ILW/ISW, branches) use these.
    static uint8_t *vuData(VU1Interpreter &vu) { return vu.m_activeVuData; }
    static uint32_t vuDataSize(const VU1Interpreter &vu) { return vu.m_activeVuDataSize; }
    static void queueStore(VU1Interpreter &vu, uint32_t address, const uint32_t words[4], uint8_t laneMask)
    {
        vu.queueStore(address, words, laneMask);
    }
    static int32_t readBranchVi(const VU1Interpreter &vu, uint8_t reg) { return vu.readBranchVi(reg); }

    static uint64_t vfReady(const VU1Interpreter &vu, uint32_t reg, uint32_t component) { return vu.m_vfReady[reg][component]; }
    static uint64_t &vfReadyRef(VU1Interpreter &vu, uint32_t reg, uint32_t component) { return vu.m_vfReady[reg][component]; }
    static uint64_t viReady(const VU1Interpreter &vu, uint32_t reg) { return vu.m_viReady[reg]; }
    static uint64_t &viReadyRef(VU1Interpreter &vu, uint32_t reg) { return vu.m_viReady[reg]; }
    static uint64_t accReady(const VU1Interpreter &vu, uint32_t component) { return vu.m_accReady[component]; }
    static uint64_t &accReadyRef(VU1Interpreter &vu, uint32_t component) { return vu.m_accReady[component]; }
    static bool fdivValid(const VU1Interpreter &vu) { return vu.m_fdiv.valid; }
    static uint64_t fdivReady(const VU1Interpreter &vu) { return vu.m_fdiv.readyCycle; }
    static uint64_t efuResourceReady(const VU1Interpreter &vu) { return vu.m_efuResourceReady; }
    static uint64_t efuMaxPendingReady(const VU1Interpreter &vu)
    {
        uint64_t ready = 0u;
        for (const auto &entry : vu.m_efu)
            if (entry.valid && entry.readyCycle > ready)
                ready = entry.readyCycle;
        return ready;
    }
    static bool xgkickActive(const VU1Interpreter &vu) { return vu.m_xgkick.active; }
    static bool &pendingHaltD(VU1Interpreter &vu) { return vu.m_pendingHaltD; }
    static bool &pendingHaltT(VU1Interpreter &vu) { return vu.m_pendingHaltT; }

    // Translator side: decode a pair exactly as the interpreter does.
    static Decoded decode(const VU1Interpreter &vu, const uint8_t *code, uint32_t pc) { return vu.decodeInstructionPair(code, pc); }
    static constexpr uint8_t kPipelineFdiv = VU1Interpreter::PipelineFdiv;
    static constexpr uint8_t kPipelineEfu = VU1Interpreter::PipelineEfu;
    static constexpr uint8_t kPipelineBranch = VU1Interpreter::PipelineBranch;
    static constexpr uint8_t kPipelineXgkick = VU1Interpreter::PipelineXgkick;
    static constexpr uint32_t kAccForwardLatency = VU1Interpreter::kAccForwardLatency;
};
