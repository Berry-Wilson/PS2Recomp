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
// Relaxed-timing images (ps2x_vu1recomp default) do not model VF/VI/ACC stall cycles, so their
// cycle counts legitimately differ from the interpreter's; verify then skips the cycle check.
void setVu1CompiledVerifyIgnoresCycles(bool ignore);
uint64_t hashVu1CodeImage(const uint8_t *code, uint32_t size);

// Generated code calls these per lane; MSVC otherwise leaves some of them out of line.
#if defined(_MSC_VER)
#define PS2X_VU1_FORCEINLINE __forceinline
#else
#define PS2X_VU1_FORCEINLINE inline __attribute__((always_inline))
#endif

// Access used by generated code and the translator (friend of VU1Interpreter).
// Operand normalization, identical to VU1Interpreter::normalizeOperand.
PS2X_VU1_FORCEINLINE float vu1NormalizeOperand(float value)
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
PS2X_VU1_FORCEINLINE bool vu1NormalizeSingleRounding(float &result)
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
PS2X_VU1_FORCEINLINE bool vu1ProductSumInRange(float result, float product)
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
// True for a normal float below FLT_MAX in magnitude: the interpreter's FMAC fast path keeps
// such a result unchanged and reports only its sign.
PS2X_VU1_FORCEINLINE bool vu1NormalNonMax(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    return exponent != 0u && exponent != 0xFFu && (bits & 0x7FFFFFFFu) != 0x7F7FFFFFu;
}

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
    PS2X_VU1_FORCEINLINE static void normalizeFmacValues(VU1Interpreter &vu, uint32_t upper, float *result, uint8_t dest)
    {
        vu.m_currentUpperInstruction = upper;
        uint8_t laneFlags[4];
        vu.normalizeFmacResult(result, dest, laneFlags);
    }
    using Decoded = VU1Interpreter::DecodedInstructionPair;
    using Usage = VU1Interpreter::InstructionUsage;

    PS2X_VU1_FORCEINLINE static VU1State &state(VU1Interpreter &vu) { return vu.m_state; }
    PS2X_VU1_FORCEINLINE static uint64_t &cycle(VU1Interpreter &vu) { return vu.m_cycle; }
    PS2X_VU1_FORCEINLINE static bool stopRequested(const VU1Interpreter &vu) { return vu.m_stopRequested; }

    PS2X_VU1_FORCEINLINE static void commitReadyPipelines(VU1Interpreter &vu)
    {
        if (vu.m_cycle < vu.m_nextCommitCycle)
            return;
        // Compiled code mostly has only FMAC flag writes in flight (stores are written at once,
        // VF/VI/ACC use immediate writeback): commit those inline, exactly as
        // commitReadyPipelinesNow does, instead of calling it every few cycles.
        if ((vu.m_storeValidMask | vu.m_vfWriteValidMask | vu.m_viWriteValidMask | vu.m_accWriteValidMask) != 0u ||
            vu.m_fdiv.valid || vu.m_efu[0].valid || vu.m_efu[1].valid)
        {
            vu.commitReadyPipelinesNow();
            return;
        }
        uint64_t next = ~0ull;
        for (uint32_t mask = vu.m_flagValidMask; mask != 0u; mask &= mask - 1u)
        {
            uint32_t slot = 0u;
            while (((mask >> slot) & 1u) == 0u)
                ++slot;
            auto &entry = vu.m_flagPipeline[slot];
            if (entry.readyCycle > vu.m_cycle)
            {
                if (entry.readyCycle < next)
                    next = entry.readyCycle;
                continue;
            }
            if (entry.writesMac)
                vu.m_state.mac = entry.mac;
            if (entry.writesStatus)
            {
                const uint32_t current = entry.status & 0xFu;
                vu.m_state.status = (vu.m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
            }
            if (entry.writesSticky)
                vu.m_state.status = (vu.m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
            if (entry.writesClip)
                vu.m_state.clip = entry.clip;
            entry.valid = false;
            vu.m_flagValidMask &= ~(1u << slot);
        }
        vu.m_nextCommitCycle = next;
    }
    PS2X_VU1_FORCEINLINE static void advanceTo(VU1Interpreter &vu, uint64_t target) { vu.advanceTo(target); }
    // Same steps as VU1Interpreter::advanceOneCycle, inlined.
    PS2X_VU1_FORCEINLINE static void advanceOneCycle(VU1Interpreter &vu)
    {
        ++vu.m_cycle;
        vu.m_state.cycles = vu.m_cycle;
        commitReadyPipelines(vu);
        if (vu.m_xgkick.active)
            vu.progressXgkick();
    }
    PS2X_VU1_FORCEINLINE static uint64_t pairReadyCycle(const VU1Interpreter &vu, const Decoded &decoded) { return vu.calculatePairReadyCycle(decoded); }
    PS2X_VU1_FORCEINLINE static void markPairWrites(VU1Interpreter &vu, const Decoded &decoded) { vu.markPairWrites(decoded); }
    PS2X_VU1_FORCEINLINE static void recordViWriteForBranch(VU1Interpreter &vu, uint8_t reg, int32_t oldValue) { vu.recordViWriteForBranch(reg, oldValue); }
    PS2X_VU1_FORCEINLINE static void clearViBranchBackup(VU1Interpreter &vu) { vu.m_viBranchBackupValid = false; }
    PS2X_VU1_FORCEINLINE static uint32_t microAddressMask(const VU1Interpreter &vu) { return vu.microAddressMask(); }
    PS2X_VU1_FORCEINLINE static float normalizeOperand(const VU1Interpreter &vu, float value) { return vu.normalizeOperand(value); }

    PS2X_VU1_FORCEINLINE static void execUpper(VU1Interpreter &vu, uint32_t instr) { vu.execUpper(instr); }
    PS2X_VU1_FORCEINLINE static void execLower(VU1Interpreter &vu, uint32_t instr, uint32_t upper)
    {
        vu.execLower(instr, vu.m_activeVuData, vu.m_activeVuDataSize, *vu.m_activeGs, vu.m_activeMemory, upper);
    }

    // Flag-observing FMAC ops inlined by the translator. Fast path: every lane is a normal
    // in-range value, so the MAC flags are the lane sign bits (same as updateFmacFlags).
    PS2X_VU1_FORCEINLINE static void queueFmacFlags(VU1Interpreter &vu, uint32_t mac, uint32_t status, uint32_t extraSticky)
    {
        const uint32_t freeMask = ~vu.m_flagValidMask & ((1u << VU1Interpreter::kMaxFlagEntries) - 1u);
        if (freeMask == 0u)
        {
            const uint8_t laneFlags[4] = {0u, 0u, 0u, 0u};
            vu.updateFmacFlags(laneFlags, 0xFu, extraSticky); // full queue: let the interpreter report it
            return;
        }
        uint32_t slot = 0u;
        while ((freeMask & (1u << slot)) == 0u)
            ++slot;
        vu.m_flagValidMask |= 1u << slot;
        auto &entry = vu.m_flagPipeline[slot];
        entry = {};
        entry.valid = true;
        entry.issueCycle = vu.m_cycle;
        entry.readyCycle = vu.m_cycle + VU1Interpreter::kFmacLatency;
        if (entry.readyCycle < vu.m_nextCommitCycle)
            vu.m_nextCommitCycle = entry.readyCycle;
        entry.mac = mac;
        entry.status = status;
        entry.extraSticky = extraSticky;
        entry.writesMac = true;
        entry.writesStatus = true;
    }
    // Slow path: the interpreter's exact normalization and flags for `upper` (operands must
    // still hold their pre-instruction values); `result` is updated in place.
    PS2X_VU1_FORCEINLINE static void fmacExact(VU1Interpreter &vu, uint32_t upper, float *result, uint8_t dest)
    {
        vu.m_currentUpperInstruction = upper;
        uint8_t laneFlags[4]{};
        vu.normalizeFmacResult(result, dest, laneFlags);
        vu.updateFmacFlags(laneFlags, dest, vu.calculateFmacProductSticky(dest));
    }

    // Inline lower instructions (LQ/SQ/ILW/ISW, branches) use these.
    PS2X_VU1_FORCEINLINE static uint8_t *vuData(VU1Interpreter &vu) { return vu.m_activeVuData; }
    PS2X_VU1_FORCEINLINE static uint32_t vuDataSize(const VU1Interpreter &vu) { return vu.m_activeVuDataSize; }
    PS2X_VU1_FORCEINLINE static void queueStore(VU1Interpreter &vu, uint32_t address, const uint32_t words[4], uint8_t laneMask)
    {
        vu.queueStore(address, words, laneMask);
    }
    PS2X_VU1_FORCEINLINE static int32_t readBranchVi(const VU1Interpreter &vu, uint8_t reg) { return vu.readBranchVi(reg); }

    PS2X_VU1_FORCEINLINE static uint64_t vfReady(const VU1Interpreter &vu, uint32_t reg, uint32_t component) { return vu.m_vfReady[reg][component]; }
    PS2X_VU1_FORCEINLINE static uint64_t &vfReadyRef(VU1Interpreter &vu, uint32_t reg, uint32_t component) { return vu.m_vfReady[reg][component]; }
    PS2X_VU1_FORCEINLINE static uint64_t viReady(const VU1Interpreter &vu, uint32_t reg) { return vu.m_viReady[reg]; }
    PS2X_VU1_FORCEINLINE static uint64_t &viReadyRef(VU1Interpreter &vu, uint32_t reg) { return vu.m_viReady[reg]; }
    PS2X_VU1_FORCEINLINE static uint64_t accReady(const VU1Interpreter &vu, uint32_t component) { return vu.m_accReady[component]; }
    PS2X_VU1_FORCEINLINE static uint64_t &accReadyRef(VU1Interpreter &vu, uint32_t component) { return vu.m_accReady[component]; }
    PS2X_VU1_FORCEINLINE static bool fdivValid(const VU1Interpreter &vu) { return vu.m_fdiv.valid; }
    PS2X_VU1_FORCEINLINE static uint64_t fdivReady(const VU1Interpreter &vu) { return vu.m_fdiv.readyCycle; }
    PS2X_VU1_FORCEINLINE static uint64_t efuResourceReady(const VU1Interpreter &vu) { return vu.m_efuResourceReady; }
    PS2X_VU1_FORCEINLINE static uint64_t efuMaxPendingReady(const VU1Interpreter &vu)
    {
        uint64_t ready = 0u;
        for (const auto &entry : vu.m_efu)
            if (entry.valid && entry.readyCycle > ready)
                ready = entry.readyCycle;
        return ready;
    }
    // Cycle at which every pending flag (MAC/status/clip) write has landed (relaxed timing).
    PS2X_VU1_FORCEINLINE static uint64_t flagPipelineReady(const VU1Interpreter &vu)
    {
        uint64_t ready = 0u;
        for (uint32_t mask = vu.m_flagValidMask; mask != 0u; mask &= mask - 1u)
        {
            uint32_t slot = 0u;
            while (((mask >> slot) & 1u) == 0u)
                ++slot;
            if (vu.m_flagPipeline[slot].readyCycle > ready)
                ready = vu.m_flagPipeline[slot].readyCycle;
        }
        return ready;
    }
    PS2X_VU1_FORCEINLINE static bool xgkickActive(const VU1Interpreter &vu) { return vu.m_xgkick.active; }
    PS2X_VU1_FORCEINLINE static bool &pendingHaltD(VU1Interpreter &vu) { return vu.m_pendingHaltD; }
    PS2X_VU1_FORCEINLINE static bool &pendingHaltT(VU1Interpreter &vu) { return vu.m_pendingHaltT; }

    // Translator side: decode a pair exactly as the interpreter does.
    PS2X_VU1_FORCEINLINE static Decoded decode(const VU1Interpreter &vu, const uint8_t *code, uint32_t pc) { return vu.decodeInstructionPair(code, pc); }
    static constexpr uint8_t kPipelineFdiv = VU1Interpreter::PipelineFdiv;
    static constexpr uint8_t kPipelineEfu = VU1Interpreter::PipelineEfu;
    static constexpr uint8_t kPipelineBranch = VU1Interpreter::PipelineBranch;
    static constexpr uint8_t kPipelineXgkick = VU1Interpreter::PipelineXgkick;
    static constexpr uint32_t kAccForwardLatency = VU1Interpreter::kAccForwardLatency;
};
