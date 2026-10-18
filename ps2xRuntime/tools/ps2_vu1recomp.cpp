// ps2x_vu1recomp: translate captured VU1 code images to C++.
//
// Usage: ps2x_vu1recomp <dump dir> <output dir>
//
// The dump directory holds <hash>.bin (16 KiB VU1 micro memory) and <hash>.entries (hex MSCAL
// start PCs, one per line) written by setVu1ImageDumpDir(). For every image this writes
// vu1_<hash>.cpp with one function covering the pairs reachable from the entries, plus
// vu1_images.cpp with registerGeneratedVu1Images(). Each pair is emitted as the exact sequence
// VU1Interpreter::run performs (with immediate writeback), with the decoded usage folded into
// constants; instruction bodies still go through execUpper/execLower.

#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1_compiled.h"

#include <functional>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using A = Vu1CompiledAccess;
    constexpr uint32_t kCodeSize = PS2_VU1_CODE_SIZE;
    constexpr uint32_t kPcMask = kCodeSize - 1u;

    uint8_t laneForComponent(uint32_t component) { return static_cast<uint8_t>(1u << (3u - component)); }

    std::string hex(uint64_t value)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "0x%llXull", static_cast<unsigned long long>(value));
        return text;
    }

    std::string hex32(uint32_t value)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08Xu", value);
        return text;
    }

const char *const kSimdPreamble = R"VU1SIMD(// SSE helpers for inlined FMAC ops: the interpreter's per-lane operand normalization and
// fast-path range checks, four lanes at a time. Masks are movemask-style (bit c = component c).
static inline __m128 vu1VSel(__m128 mask, __m128 a, __m128 b) { return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b)); }
static inline __m128 vu1LaneMask(int bits)
{
    return _mm_castsi128_ps(_mm_set_epi32((bits & 8) ? -1 : 0, (bits & 4) ? -1 : 0, (bits & 2) ? -1 : 0, (bits & 1) ? -1 : 0));
}
static inline __m128i vu1VExp(__m128 v) { return _mm_and_si128(_mm_srli_epi32(_mm_castps_si128(v), 23), _mm_set1_epi32(0xFF)); }
// vu1NormalizeOperand per lane: exponent 0 -> signed zero, exponent 255 -> signed FLT_MAX.
static inline __m128 vu1VNorm(__m128 v)
{
    const __m128i bits = _mm_castps_si128(v);
    const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
    const __m128i exp = vu1VExp(v);
    const __m128i zero = _mm_cmpeq_epi32(exp, _mm_setzero_si128());
    const __m128i inf = _mm_cmpeq_epi32(exp, _mm_set1_epi32(0xFF));
    __m128i out = _mm_or_si128(_mm_and_si128(zero, sign), _mm_andnot_si128(zero, bits));
    out = _mm_or_si128(_mm_and_si128(inf, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF))), _mm_andnot_si128(inf, out));
    return _mm_castsi128_ps(out);
}
// vu1NormalNonMax per lane.
static inline int vu1VNormalNonMax(__m128 v)
{
    const __m128i exp = vu1VExp(v);
    const __m128i bad = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi32(exp, _mm_setzero_si128()), _mm_cmpeq_epi32(exp, _mm_set1_epi32(0xFF))),
                                     _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(v), _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F7FFFFF)));
    return ~_mm_movemask_ps(_mm_castsi128_ps(bad)) & 0xF;
}
// vu1NormalizeSingleRounding per lane: ok unless exponent 255 or |v| == FLT_MAX (zero/denormal
// lanes are ok and become signed zeros, see vu1VZeroDenormals).
static inline int vu1VSingleRoundingOk(__m128 v)
{
    const __m128i exp = vu1VExp(v);
    const __m128i bad = _mm_or_si128(_mm_cmpeq_epi32(exp, _mm_set1_epi32(0xFF)),
                                     _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(v), _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F7FFFFF)));
    return ~_mm_movemask_ps(_mm_castsi128_ps(bad)) & 0xF;
}
static inline __m128 vu1VZeroDenormals(__m128 v)
{
    const __m128i bits = _mm_castps_si128(v);
    const __m128i zero = _mm_cmpeq_epi32(vu1VExp(v), _mm_setzero_si128());
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(zero, _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)))),
                                         _mm_andnot_si128(zero, bits)));
}
// vu1ProductSumInRange per lane (and, for flag-observing ops, the product must be normal too).
static inline int vu1VProductSumOk(__m128 r, __m128 p, bool productNormal)
{
    const __m128i rexp = vu1VExp(r);
    const __m128i pexp = vu1VExp(p);
    __m128i ok = _mm_and_si128(_mm_cmpgt_epi32(rexp, _mm_set1_epi32(4)), _mm_cmplt_epi32(rexp, _mm_set1_epi32(0xFD)));
    ok = _mm_and_si128(ok, _mm_cmplt_epi32(pexp, _mm_set1_epi32(0xFD)));
    const __m128 absMask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    ok = _mm_and_si128(ok, _mm_castps_si128(_mm_cmpge_ps(_mm_and_ps(r, absMask), _mm_mul_ps(_mm_and_ps(p, absMask), _mm_set1_ps(9.5367431640625e-7f)))));
    int bits = _mm_movemask_ps(_mm_castsi128_ps(ok));
    if (productNormal)
        bits &= vu1VNormalNonMax(p);
    return bits;
}
// MAC sign bits (lane c -> (8 >> c) << 4) for a movemask of the result's sign bits.
static const uint32_t vu1MacSignBits[16] = {0x00u, 0x80u, 0x40u, 0xC0u, 0x20u, 0xA0u, 0x60u, 0xE0u,
                                            0x10u, 0x90u, 0x50u, 0xD0u, 0x30u, 0xB0u, 0x70u, 0xF0u};

)VU1SIMD";

    // Generated code is split into one function per 256-byte PC range (32 pairs): MSVC stops
    // inlining (and allocates registers poorly) inside one huge goto function. Jumps within a
    // chunk stay gotos; leaving a chunk returns to the image's dispatcher loop.
    constexpr uint32_t kChunkShift = 8u;

    // Relaxed timing (opt-in --relaxed-timing; default is the interpreter's exact cycle model): with
    // immediate writeback, VF/VI/ACC stall cycles only change cycle counts, so they are not
    // modelled. Stores are written at once (they became visible to the next pair anyway).
    // Because fewer cycles pass, instructions reading Q/P wait for the FDIV/EFU unit and flag
    // readers wait for pending flag writes, as if the stalls had covered the latency.
    // NOT safe in general: dhp1's town renders broken geometry with it (its VU1 code depends on
    // the stall timing somewhere), and it gave no measurable speedup there.
    bool g_exactTiming = true;

    // Static stall elimination. A "block" is a straight-line run of pairs entered only at its
    // first pair (an MSCAL entry, a branch target, a link return, a chunk's first pair, or a pair
    // not reached by fallthrough). Compiled code is only ever entered at block starts (the
    // dispatcher lists nothing else and Budget is only returned there), so within a block every
    // pair advances the cycle by at least one. A VF/VI/ACC write with latency L at block pair w is
    // therefore ready by pair w + L, and any write from before the block is ready once the pair
    // index reaches the image's maximum write latency. Reads proven ready skip the runtime check.
    std::set<uint32_t> g_blockStarts;
    uint32_t g_maxVfLatency = 0u, g_maxViLatency = 0u, g_maxAccLatency = 0u;
    struct BlockTracker
    {
        uint32_t index = 0u;
        uint32_t vf[32][4]{};
        uint32_t vi[16]{};
        uint32_t acc[4]{};
        void start()
        {
            index = 0u;
            for (auto &reg : vf)
                for (uint32_t &lane : reg)
                    lane = g_maxVfLatency;
            for (uint32_t &reg : vi)
                reg = g_maxViLatency;
            for (uint32_t &lane : acc)
                lane = g_maxAccLatency;
        }
    } g_block;
    uint32_t g_stallChecksRemoved = 0u, g_stallChecksKept = 0u;

    bool upperReadsQ(uint32_t upper)
    {
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        if (op < 0x3Cu)
            return op == 0x1Cu || op == 0x20u || op == 0x21u || op == 0x24u || op == 0x25u;
        const uint8_t sp = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        return sp == 0x1Cu || sp == 0x20u || sp == 0x21u || sp == 0x24u || sp == 0x25u;
    }
    bool lowerReadsP(uint32_t lower)
    {
        return (lower & 0x80000000u) != 0u && (lower & 0x3Cu) == 0x3Cu &&
               (((lower >> 6) & 0x1Fu) << 2 | (lower & 3u)) == 0x64u; // MFP
    }
    uint32_t chunkOf(uint32_t pc) { return pc >> kChunkShift; }

    std::string label(uint32_t pc)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "L_%04X", pc);
        return text;
    }

    bool isUpperNop(uint32_t upper)
    {
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        if (op < 0x3Cu)
            return false;
        const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        return special == 0x2Fu || special == 0x30u;
    }

    bool isLowerNop(uint32_t lower) { return lower == 0x00000000u || lower == 0x8000033Cu; }

    struct Flow
    {
        bool staticBranch = false;   // B, BAL, IBxx
        bool conditional = false;    // IBxx
        bool link = false;           // BAL, JALR
        bool jump = false;           // JR, JALR (dynamic target)
        uint32_t target = 0u;
    };

    Flow lowerFlow(uint32_t pc, uint32_t lower)
    {
        Flow flow;
        if ((lower & 0x80000000u) != 0u)
            return flow;
        const uint32_t op = (lower >> 25) & 0x7Fu;
        const int32_t imm11 = static_cast<int32_t>(lower << 21) >> 21;
        const uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(pc) + 8 + imm11 * 8) & kPcMask;
        switch (op)
        {
        case 0x20: flow.staticBranch = true; flow.target = target; break;
        case 0x21: flow.staticBranch = true; flow.link = true; flow.target = target; break;
        case 0x24: flow.jump = true; break;
        case 0x25: flow.jump = true; flow.link = true; break;
        case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            flow.staticBranch = true; flow.conditional = true; flow.target = target; break;
        default: break;
        }
        return flow;
    }

    // Pairs reachable from the entries. A pair whose fallthrough is not followed is a delay
    // slot of an unconditional transfer or the pair after an E-bit.
    std::set<uint32_t> reachablePairs(const VU1Interpreter &vu, const uint8_t *code, const std::set<uint32_t> &entries)
    {
        std::set<uint32_t> followed; // pairs whose successors were added
        std::set<uint32_t> seen;     // pairs to compile
        std::deque<std::pair<uint32_t, bool>> work; // (pc, follow fallthrough)
        for (uint32_t entry : entries)
            work.push_back({entry & kPcMask & ~7u, true});
        while (!work.empty())
        {
            auto [pc, follow] = work.front();
            work.pop_front();
            seen.insert(pc);
            if (!follow || !followed.insert(pc).second)
                continue;
            const A::Decoded d = A::decode(vu, code, pc);
            if (d.upperUsage.reserved || d.lowerUsage.reserved)
                continue;
            const uint32_t next = (pc + 8u) & kPcMask;
            const uint32_t afterSlot = (pc + 16u) & kPcMask;
            const Flow flow = lowerFlow(pc, d.lower);
            if (d.eBit)
            {
                work.push_back({next, false});
                continue;
            }
            if (flow.staticBranch)
            {
                work.push_back({flow.target, true});
                if (flow.conditional)
                    work.push_back({next, true});
                else
                {
                    work.push_back({next, false});
                    if (flow.link)
                        work.push_back({afterSlot, true});
                }
                continue;
            }
            if (flow.jump)
            {
                work.push_back({next, false});
                if (flow.link)
                    work.push_back({afterSlot, true});
                continue;
            }
            work.push_back({next, true});
        }
        return seen;
    }

    // --- FMAC flag liveness ---------------------------------------------------------------
    // Upper ops that queue MAC/status flags (applyFmacDest/applyFmacDestAcc in execUpper).
    bool isFlagWriter(uint32_t upper)
    {
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        if (op < 0x3Cu)
        {
            if (op >= 0x30u)
                return false;
            return !((op >= 0x10u && op <= 0x17u) || op == 0x1Du || op == 0x1Fu || op == 0x2Bu || op == 0x2Fu);
        }
        const uint8_t sp = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        return sp <= 0x0Fu || (sp >= 0x18u && sp <= 0x1Cu) || sp == 0x1Eu || (sp >= 0x20u && sp <= 0x2Au) ||
               (sp >= 0x2Cu && sp <= 0x2Eu);
    }

    uint32_t lowerPrimaryOp(uint32_t lower) { return (lower & 0x80000000u) != 0u ? 0xFFu : (lower >> 25) & 0x7Fu; }
    // FMEQ/FMAND/FMOR read MAC; FSEQ/FSAND/FSOR read status.
    bool isFlagReader(uint32_t lower)
    {
        const uint32_t op = lowerPrimaryOp(lower);
        return op == 0x18u || op == 0x1Au || op == 0x1Bu || op == 0x14u || op == 0x16u || op == 0x17u;
    }
    bool readsStickyStatus(uint32_t lower)
    {
        const uint32_t op = lowerPrimaryOp(lower);
        const uint32_t imm12 = (((lower >> 21) & 1u) << 11) | (lower & 0x7FFu);
        return op == 0x14u || op == 0x17u || (op == 0x16u && (imm12 & 0xFC0u) != 0u);
    }

    enum Pending : uint8_t { PendNone, PendUncond, PendCond, PendDynamic, PendEnd };
    struct FlowState
    {
        uint32_t pc;
        uint8_t pending;
        uint32_t target;
        auto key() const { return (static_cast<uint64_t>(pc) << 40) | (static_cast<uint64_t>(pending) << 32) | target; }
    };

    struct ImageAnalysis
    {
        std::map<uint32_t, A::Decoded> pairs;
        std::set<uint64_t> states; // reachable FlowState keys
        std::set<uint32_t> deadFlagWriters;
        bool stickyObservable = false;
        uint32_t writers = 0;
    };

    // Successors of executing pair `state.pc` entered with `state.pending`. Sets pathEnd when
    // execution may leave the analysed code (program end, unknown jump, halt bits, untranslated PC).
    void successors(const ImageAnalysis &an, const FlowState &state, std::vector<FlowState> &out, bool &pathEnd)
    {
        const auto it = an.pairs.find(state.pc);
        if (it == an.pairs.end())
        {
            pathEnd = true;
            return;
        }
        const A::Decoded &d = it->second;
        const uint32_t next = (state.pc + 8u) & kPcMask;
        auto push = [&](uint32_t pc, uint8_t pending, uint32_t target)
        {
            if (an.pairs.count(pc) == 0u)
                pathEnd = true;
            else
                out.push_back({pc, pending, target});
        };
        if (d.dBit || d.tBit || d.upperUsage.reserved || d.lowerUsage.reserved)
            pathEnd = true;
        switch (state.pending)
        {
        case PendEnd:
        case PendDynamic:
            pathEnd = true;
            return;
        case PendUncond:
        case PendCond:
            if (lowerFlow(state.pc, d.lower).staticBranch || lowerFlow(state.pc, d.lower).jump || d.eBit)
                pathEnd = true; // branch in a delay slot: not modelled
            push(state.target, PendNone, 0u);
            if (state.pending == PendCond)
                push(next, PendNone, 0u);
            return;
        default:
            break;
        }
        const Flow flow = lowerFlow(state.pc, d.lower);
        if (d.eBit)
            push(next, PendEnd, 0u);
        else if (flow.staticBranch)
            push(next, flow.conditional ? PendCond : PendUncond, flow.target);
        else if (flow.jump)
            push(next, PendDynamic, 0u);
        else
            push(next, PendNone, 0u);
    }

    // A flag write is visible 4 cycles after issue. Pair-distance >= 4 guarantees >= 4 cycles,
    // so a writer W is unobservable if on every path another writer W' comes at least 4 pairs
    // before any flag reader, and before the path leaves the analysed code.
    bool flagsLive(const ImageAnalysis &an, uint32_t writerPc)
    {
        constexpr uint8_t kNoKiller = 0xFFu;
        std::set<std::pair<uint64_t, uint8_t>> visited;
        std::vector<std::pair<FlowState, uint8_t>> work;
        for (uint64_t key : an.states)
        {
            if (static_cast<uint32_t>(key >> 40) != writerPc)
                continue;
            const FlowState start{writerPc, static_cast<uint8_t>((key >> 32) & 0xFFu), static_cast<uint32_t>(key & 0xFFFFFFFFu)};
            std::vector<FlowState> next;
            bool pathEnd = false;
            successors(an, start, next, pathEnd);
            if (pathEnd)
                return true;
            for (const auto &n : next)
                work.push_back({n, kNoKiller});
        }
        while (!work.empty())
        {
            auto [state, killerDistance] = work.back();
            work.pop_back();
            if (killerDistance != kNoKiller && killerDistance >= 4u)
                continue;
            if (!visited.insert({state.key(), killerDistance}).second)
                continue;
            const A::Decoded &d = an.pairs.at(state.pc);
            if (isFlagReader(d.lower))
                return true;
            uint8_t k = killerDistance;
            if (k == kNoKiller && isFlagWriter(d.upper))
                k = 0u;
            std::vector<FlowState> next;
            bool pathEnd = false;
            successors(an, state, next, pathEnd);
            if (pathEnd)
                return true;
            for (const auto &n : next)
                work.push_back({n, k == kNoKiller ? kNoKiller : static_cast<uint8_t>(k + 1u)});
        }
        return false;
    }

    ImageAnalysis analyse(const VU1Interpreter &vu, const uint8_t *code, const std::set<uint32_t> &compiled, const std::set<uint32_t> &entries)
    {
        ImageAnalysis an;
        for (uint32_t pc : compiled)
            an.pairs.emplace(pc, A::decode(vu, code, pc));
        std::vector<FlowState> work;
        for (uint32_t entry : entries)
            if (an.pairs.count(entry & kPcMask & ~7u) != 0u)
                work.push_back({entry & kPcMask & ~7u, PendNone, 0u});
        while (!work.empty())
        {
            const FlowState state = work.back();
            work.pop_back();
            if (!an.states.insert(state.key()).second)
                continue;
            std::vector<FlowState> next;
            bool pathEnd = false;
            successors(an, state, next, pathEnd);
            for (const auto &n : next)
                work.push_back(n);
        }
        // Sticky status bits accumulate across instructions and programs, so a writer's sticky
        // contribution is only provably unobserved when every sticky reader is "protected": no
        // path from a program entry reaches it without passing an FSSET (which resets the
        // sticky bits; FSSET also cancels same-cycle FMAC status writes). Then only writers
        // between an FSSET and the next FSSET can feed a reader, and they keep their flags.
        bool anyStickyReader = false;
        for (const auto &[pc, d] : an.pairs)
            if (readsStickyStatus(d.lower))
                anyStickyReader = true;
        std::set<uint32_t> stickyLiveWriters;
        if (anyStickyReader)
        {
            auto statesAt = [&](uint32_t pc, std::vector<FlowState> &out)
            {
                for (uint64_t key : an.states)
                    if (static_cast<uint32_t>(key >> 40) == pc)
                        out.push_back({pc, static_cast<uint8_t>((key >> 32) & 0xFFu), static_cast<uint32_t>(key & 0xFFFFFFFFu)});
            };
            // Forward walk that stops at FSSET pairs; returns true if it reaches a sticky reader.
            auto walk = [&](std::vector<FlowState> work, std::set<uint32_t> *writers)
            {
                bool readerReached = false;
                std::set<uint64_t> seen;
                while (!work.empty())
                {
                    const FlowState state = work.back();
                    work.pop_back();
                    if (!seen.insert(state.key()).second)
                        continue;
                    const A::Decoded &d = an.pairs.at(state.pc);
                    if (lowerPrimaryOp(d.lower) == 0x15u) // FSSET
                        continue;
                    if (readsStickyStatus(d.lower))
                        readerReached = true;
                    if (writers && isFlagWriter(d.upper))
                        writers->insert(state.pc);
                    std::vector<FlowState> next;
                    bool pathEnd = false;
                    successors(an, state, next, pathEnd);
                    for (const auto &n : next)
                        work.push_back(n);
                }
                return readerReached;
            };
            std::vector<FlowState> fromEntries;
            for (uint32_t entry : entries)
                if (an.pairs.count(entry & kPcMask & ~7u) != 0u)
                    fromEntries.push_back({entry & kPcMask & ~7u, PendNone, 0u});
            if (walk(fromEntries, nullptr))
            {
                an.stickyObservable = true;
            }
            else
            {
                for (const auto &[pc, d] : an.pairs)
                {
                    if (lowerPrimaryOp(d.lower) != 0x15u)
                        continue;
                    std::vector<FlowState> starts, after;
                    statesAt(pc, starts);
                    for (const auto &s : starts)
                    {
                        bool pathEnd = false;
                        successors(an, s, after, pathEnd);
                    }
                    walk(after, &stickyLiveWriters);
                }
            }
        }
        for (const auto &[pc, d] : an.pairs)
        {
            if (!isFlagWriter(d.upper))
                continue;
            ++an.writers;
            if (!an.stickyObservable && stickyLiveWriters.count(pc) == 0u && !flagsLive(an, pc))
                an.deadFlagWriters.insert(pc);
        }
        return an;
    }

    // Inline arithmetic for an FMAC op whose flags are unobservable. Mirrors execUpper's
    // expression for each form and its result normalization; returns false if not handled.
    // withFlags: the op's MAC/status flags are observed. Emits the interpreter's fast path
    // (every lane a normal in-range value: flags are the lane signs, a product-sum's product
    // sticky is its sign) and calls the interpreter's exact routine otherwise.
    bool emitInlineFmac(std::ostream &o, uint32_t upper, bool withFlags = false)
    {
        enum Kind { Add, Sub, Mul, Madd, Msub };
        enum Source { Bc, Q, I, Vt };
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        const bool accTarget = op >= 0x3Cu;
        const uint8_t code = accTarget ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : op;
        Kind kind;
        Source source;
        if (code <= 0x03u) { kind = Add; source = Bc; }
        else if (code <= 0x07u) { kind = Sub; source = Bc; }
        else if (code <= 0x0Bu) { kind = Madd; source = Bc; }
        else if (code <= 0x0Fu) { kind = Msub; source = Bc; }
        else if (code >= 0x18u && code <= 0x1Bu) { kind = Mul; source = Bc; }
        else
        {
            switch (code)
            {
            case 0x1C: kind = Mul; source = Q; break;
            case 0x1E: kind = Mul; source = I; break;
            case 0x20: kind = Add; source = Q; break;
            case 0x21: kind = Madd; source = Q; break;
            case 0x22: kind = Add; source = I; break;
            case 0x23: kind = Madd; source = I; break;
            case 0x24: kind = Sub; source = Q; break;
            case 0x25: kind = Msub; source = Q; break;
            case 0x26: kind = Sub; source = I; break;
            case 0x27: kind = Msub; source = I; break;
            case 0x28: kind = Add; source = Vt; break;
            case 0x29: kind = Madd; source = Vt; break;
            case 0x2A: kind = Mul; source = Vt; break;
            case 0x2C: kind = Sub; source = Vt; break;
            case 0x2D: kind = Msub; source = Vt; break;
            default: return false;
            }
        }
        const unsigned dest = (upper >> 21) & 0xFu;
        const unsigned ft = (upper >> 16) & 0x1Fu;
        const unsigned fs = (upper >> 11) & 0x1Fu;
        const unsigned fd = (upper >> 6) & 0x1Fu;
        const bool productSum = kind == Madd || kind == Msub;
        static const char *names[] = {"ADD", "SUB", "MUL", "MADD", "MSUB"};
        o << "    { // " << names[kind] << (accTarget ? "A" : "") << (withFlags ? " inline SSE (fast flags)\n" : " inline SSE (flags unobserved)\n");
        // Operands, normalized as the interpreter does, four lanes at a time. Packed SSE add/sub/mul
        // round each lane exactly like the scalar float code (same MXCSR rounding mode).
        o << "        const __m128 s = vu1VNorm(_mm_loadu_ps(st.vf[" << fs << "]));\n";
        if (source == Vt)
            o << "        const __m128 t = vu1VNorm(_mm_loadu_ps(st.vf[" << ft << "]));\n";
        else if (source == Bc)
            o << "        const __m128 t = _mm_set1_ps(vu1NormalizeOperand(st.vf[" << ft << "][" << (upper & 3u) << "]));\n";
        else
            o << "        const __m128 t = _mm_set1_ps(vu1NormalizeOperand(st." << (source == Q ? "q" : "i") << "));\n";
        const char *opName = kind == Add ? "_mm_add_ps" : kind == Sub ? "_mm_sub_ps" : "_mm_mul_ps";
        // Lane c of the vector is component c; dest bit (8 >> c) selects it.
        const unsigned laneBits = ((dest & 8u) ? 1u : 0u) | ((dest & 4u) ? 2u : 0u) | ((dest & 2u) ? 4u : 0u) | ((dest & 1u) ? 8u : 0u);
        o << "        const int laneBits = " << laneBits << ";\n";
        if (productSum)
        {
            o << "        const __m128 p = _mm_mul_ps(s, t);\n";
            o << "        const __m128 a = vu1VNorm(_mm_loadu_ps(st.acc));\n";
            o << "        __m128 r = " << (kind == Madd ? "_mm_add_ps(a, p)" : "_mm_sub_ps(a, p)") << ";\n";
            o << "        bool inRange = (vu1VProductSumOk(r, p" << (withFlags ? ", true" : ", false") << ") & laneBits) == laneBits;\n";
        }
        else
        {
            o << "        __m128 r = " << opName << "(s, t);\n";
            if (withFlags)
                o << "        bool inRange = (vu1VNormalNonMax(r) & laneBits) == laneBits;\n";
            else
                o << "        bool inRange = (vu1VSingleRoundingOk(r) & laneBits) == laneBits; r = vu1VZeroDenormals(r);\n";
        }
        o << "        if (!inRange) { alignas(16) float rv[4]; _mm_store_ps(rv, r); ";
        if (withFlags)
            o << "A::fmacExact(vu, " << hex32(upper) << ", rv, " << dest << "u); r = _mm_load_ps(rv); }\n";
        else
            o << "A::normalizeFmacValues(vu, " << hex32(upper) << ", rv, " << dest << "u); r = _mm_load_ps(rv); }\n";
        if (withFlags)
        {
            o << "        else { const uint32_t mac = vu1MacSignBits[_mm_movemask_ps(r) & laneBits];";
            if (productSum)
                o << " const uint32_t productSticky = (_mm_movemask_ps(p) & laneBits) != 0 ? 2u : 0u;";
            else
                o << " const uint32_t productSticky = 0u;";
            o << " A::queueFmacFlags(vu, mac, mac != 0u ? 2u : 0u, productSticky); }\n";
        }
        const std::string target = accTarget ? "st.acc" : "st.vf[" + std::to_string(fd) + "]";
        if (laneBits == 0xFu)
            o << "        _mm_storeu_ps(" << target << ", r);\n";
        else
            o << "        _mm_storeu_ps(" << target << ", vu1VSel(vu1LaneMask(" << laneBits << "), r, _mm_loadu_ps(" << target << ")));\n";
        o << "    }\n";
        return true;
    }

    // Inline upper ops that never touch the flag pipelines (MAX/MINI forms, ITOF/FTOI), mirroring
    // execUpper: normalized operands, no result normalization, destination lanes only.
    bool emitInlineUpperSimple(std::ostream &o, uint32_t upper)
    {
        // SSE versions of the interpreter's expressions: _mm_max_ps/_mm_min_ps are exactly
        // (a > b) ? a : b and (a < b) ? a : b; int<->float conversions use the same MXCSR rounding
        // as the scalar casts; FTOI saturates positive overflow like vuFloatToInt (the scaled
        // operand is normalized, so it is never NaN, and scaling by a power of two is exact).
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        const unsigned dest = (upper >> 21) & 0xFu;
        const unsigned ft = (upper >> 16) & 0x1Fu;
        const unsigned fs = (upper >> 11) & 0x1Fu;
        const unsigned fd = (upper >> 6) & 0x1Fu;
        const unsigned laneBits = ((dest & 8u) ? 1u : 0u) | ((dest & 4u) ? 2u : 0u) | ((dest & 2u) ? 4u : 0u) | ((dest & 1u) ? 8u : 0u);
        auto store = [&](unsigned reg, const std::string &value)
        {
            const std::string target = "st.vf[" + std::to_string(reg) + "]";
            if (laneBits == 0xFu)
                o << "        _mm_storeu_ps(" << target << ", " << value << ");\n";
            else
                o << "        _mm_storeu_ps(" << target << ", vu1VSel(vu1LaneMask(" << laneBits << "), " << value << ", _mm_loadu_ps(" << target << ")));\n";
        };
        const std::string vs = "vu1VNorm(_mm_loadu_ps(st.vf[" + std::to_string(fs) + "]))";
        const std::string vt = "vu1VNorm(_mm_loadu_ps(st.vf[" + std::to_string(ft) + "]))";
        auto minMax = [&](bool isMax, const std::string &other)
        {
            o << "    { // " << (isMax ? "MAX" : "MINI") << " inline SSE\n";
            store(fd, std::string(isMax ? "_mm_max_ps(" : "_mm_min_ps(") + vs + ", " + other + ")");
            o << "    }\n";
            return true;
        };
        const std::string bc = "_mm_set1_ps(vu1NormalizeOperand(st.vf[" + std::to_string(ft) + "][" + std::to_string(op & 3u) + "]))";
        const std::string iReg = "_mm_set1_ps(vu1NormalizeOperand(st.i))";
        if (op >= 0x10u && op <= 0x13u)
            return minMax(true, bc);
        if (op >= 0x14u && op <= 0x17u)
            return minMax(false, bc);
        if (op == 0x1Du)
            return minMax(true, iReg);
        if (op == 0x1Fu)
            return minMax(false, iReg);
        if (op == 0x2Bu)
            return minMax(true, vt);
        if (op == 0x2Fu)
            return minMax(false, vt);
        if (op < 0x3Cu)
            return false;
        const uint8_t sp = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        static const char *const scales[] = {"1.0f", "16.0f", "4096.0f", "32768.0f"};
        if (sp >= 0x10u && sp <= 0x13u)
        {
            o << "    { // ITOF inline SSE\n";
            o << "        const __m128 f = _mm_cvtepi32_ps(_mm_castps_si128(_mm_loadu_ps(st.vf[" << fs << "])));\n";
            store(ft, sp == 0x10u ? std::string("f") : std::string("_mm_div_ps(f, _mm_set1_ps(") + scales[sp & 3u] + "))");
            o << "    }\n";
            return true;
        }
        if (sp >= 0x14u && sp <= 0x17u)
        {
            o << "    { // FTOI inline SSE\n";
            o << "        const __m128 scaled = _mm_mul_ps(" << vs << ", _mm_set1_ps(" << scales[sp & 3u] << "));\n";
            o << "        const __m128i trunc = _mm_cvttps_epi32(scaled);\n";
            o << "        const __m128 positiveOverflow = _mm_cmpge_ps(scaled, _mm_set1_ps(2147483648.0f));\n";
            o << "        const __m128 ints = vu1VSel(positiveOverflow, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)), _mm_castsi128_ps(trunc));\n";
            store(ft, "ints");
            o << "    }\n";
            return true;
        }
        return false;
    }

    // Inline the hot lower ops, mirroring execLower case by case (same address wrap, same
    // store queue, same branch-VI backup reads). Returns false for anything else.
    bool emitInlineLower(std::ostream &o, uint32_t pc, uint32_t lower)
    {
        const uint32_t opHi = (lower >> 25) & 0x7Fu;
        const unsigned dest = (lower >> 21) & 0xFu;
        const unsigned lit = (lower >> 16) & 0x1Fu, lis = (lower >> 11) & 0x1Fu;
        const unsigned vit = (lower >> 16) & 0xFu, vis = (lower >> 11) & 0xFu, vid = (lower >> 6) & 0xFu;
        const int imm11 = static_cast<int16_t>(static_cast<int32_t>(lower << 21) >> 21);
        auto address = [&](unsigned baseVi)
        {
            o << "        const uint32_t ds = A::vuDataSize(vu);\n";
            o << "        const uint32_t addr = (static_cast<uint32_t>(static_cast<int32_t>(st.vi[" << baseVi << "] + (" << imm11
              << "))) * 16u) & (ds - 1u);\n";
        };
        auto viWrite = [&](unsigned reg, const std::string &expr)
        {
            if (reg != 0u)
                o << "    st.vi[" << reg << "] = " << expr << ";\n";
        };
        switch (opHi)
        {
        case 0x00: // LQ
            o << "    { // LQ inline\n";
            address(vis);
            o << "        if (addr + 16u <= ds) { float tmp[4]; std::memcpy(tmp, A::vuData(vu) + addr, 16);";
            for (uint32_t c = 0; c < 4u; ++c)
                if ((dest & laneForComponent(c)) != 0u)
                    o << " st.vf[" << lit << "][" << c << "] = tmp[" << c << "];";
            o << " }\n    }\n";
            return true;
        case 0x01: // SQ
            o << "    { // SQ inline\n";
            address(vit);
            o << "        if (addr + 16u <= ds) { uint32_t words[4]; std::memcpy(words, st.vf[" << lis
              << "], sizeof(words));";
            // Written at once: a queued store commits at the start of the next cycle, before
            // anything else (XGKICK, the next pair's loads) reads data memory, so the result is
            // identical without the store queue.
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((dest & laneForComponent(c)) != 0u)
                        o << " std::memcpy(A::vuData(vu) + addr + " << c * 4u << "u, &words[" << c << "], 4);";
            o << " }\n    }\n";
            return true;
        case 0x04: // ILW
        {
            const unsigned comp = (dest & 8u) ? 0u : (dest & 4u) ? 1u : (dest & 2u) ? 2u : 3u;
            o << "    { // ILW inline\n";
            address(vis);
            o << "        if (addr + 16u <= ds) { uint32_t v; std::memcpy(&v, A::vuData(vu) + addr + " << comp * 4u << "u, 4);";
            if (vit != 0u)
                o << " st.vi[" << vit << "] = static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu));";
            else
                o << " (void)v;";
            o << " }\n    }\n";
            return true;
        }
        case 0x05: // ISW
            o << "    { // ISW inline\n";
            address(vis);
            o << "        if (addr + 16u <= ds) { const uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>(st.vi[" << vit
              << "] & 0xFFFF));";
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((dest & laneForComponent(c)) != 0u)
                        o << " std::memcpy(A::vuData(vu) + addr + " << c * 4u << "u, &val, 4);";
            o << " }\n    }\n";
            return true;
        case 0x08: // IADDIU
        case 0x09: // ISUBIU
        {
            const int imm = static_cast<int16_t>((lower & 0x7FFu) | ((lower >> 10) & 0x7800u));
            viWrite(vit, "static_cast<int16_t>(st.vi[" + std::to_string(vis) + "] " + (opHi == 0x08u ? "+" : "-") + " (" +
                             std::to_string(imm) + "))");
            return true;
        }
        case 0x1A: // FMAND
            viWrite(vit, "static_cast<int32_t>(st.mac & static_cast<uint32_t>(static_cast<uint16_t>(st.vi[" + std::to_string(vis) +
                             "])))");
            return true;
        case 0x28: // IBEQ
        case 0x29: // IBNE
        {
            const uint32_t target = (pc + 8u + static_cast<uint32_t>(imm11 * 8)) & kPcMask;
            o << "    if (static_cast<int16_t>(A::readBranchVi(vu, " << vis << ")) " << (opHi == 0x28u ? "==" : "!=")
              << " static_cast<int16_t>(A::readBranchVi(vu, " << vit << "))) { st.branchPending = true; st.branchTarget = "
              << target << "u & pcMask; st.branchDelay = 1; }\n";
            return true;
        }
        case 0x40:
        {
            const uint32_t funct = lower & 0x3Fu;
            const std::string s = "st.vi[" + std::to_string(vis) + "]", t = "st.vi[" + std::to_string(vit) + "]";
            switch (funct)
            {
            case 0x30: viWrite(vid, "static_cast<int16_t>(" + s + " + " + t + ")"); return true; // IADD
            case 0x31: viWrite(vid, "static_cast<int16_t>(" + s + " - " + t + ")"); return true; // ISUB
            case 0x32: // IADDI
            {
                const int imm5 = static_cast<int16_t>(static_cast<int32_t>(((lower >> 6) & 0x1Fu) << 27) >> 27);
                viWrite(vit, "static_cast<int16_t>(" + s + " + (" + std::to_string(imm5) + "))");
                return true;
            }
            case 0x34: viWrite(vid, s + " & " + t); return true; // IAND
            case 0x35: viWrite(vid, s + " | " + t); return true; // IOR
            default: return false;
            }
        }
        default:
            return false;
        }
    }

    void emitStallTerms(std::vector<std::string> &terms, const A::Usage &usage)
    {
        const uint32_t j = g_block.index;
        auto keep = [&](uint32_t readyIndex, std::string term)
        {
            if (readyIndex <= j)
            {
                ++g_stallChecksRemoved;
                return;
            }
            ++g_stallChecksKept;
            terms.push_back(std::move(term));
        };
        for (uint32_t index = 0; index < usage.vfReadCount; ++index)
        {
            const auto &access = usage.vfRead[index];
            if (access.reg == 0u)
                continue;
            for (uint32_t c = 0; c < 4u; ++c)
                if ((access.lanes & laneForComponent(c)) != 0u)
                    keep(g_block.vf[access.reg & 31u][c], "A::vfReady(vu, " + std::to_string(access.reg) + ", " + std::to_string(c) + ")");
        }
        for (uint32_t reg = 1; reg < 16u; ++reg)
            if ((usage.viRead & (1u << reg)) != 0u)
                keep(g_block.vi[reg], "A::viReady(vu, " + std::to_string(reg) + ")");
        for (uint32_t c = 0; c < 4u; ++c)
            if ((usage.accRead & laneForComponent(c)) != 0u)
                keep(g_block.acc[c], "A::accReady(vu, " + std::to_string(c) + ")");
    }

    uint32_t inlinedSimple = 0u;
    uint32_t inlinedFlags = 0u;
    uint32_t inlinedLower = 0u;

    void emitPairBody(std::ostream &o, const VU1Interpreter &vu, const uint8_t *code, uint32_t pc, const std::set<uint32_t> &compiled,
                      const ImageAnalysis &an, uint32_t &inlined)
    {
        const A::Decoded d = A::decode(vu, code, pc);
        const bool blockStart = g_blockStarts.count(pc) != 0u;
        if (blockStart)
            g_block.start();
        o << label(pc) << ":\n{\n";
        if (blockStart)
            o << "    if (cyc >= budgetEnd || A::stopRequested(vu)) { st.pc = " << pc << "u; return Vu1CompiledResult::Budget; }\n";
        o << "    A::commitReadyPipelines(vu);\n";
        if (d.upperUsage.reserved || d.lowerUsage.reserved)
        {
            o << "    st.pc = " << pc << "u;\n    return Vu1CompiledResult::Fallback; // reserved instruction: interpreter reports it\n}\n";
            return;
        }

        std::vector<std::string> terms;
        if (g_exactTiming)
        {
            emitStallTerms(terms, d.upperUsage);
            emitStallTerms(terms, d.lowerUsage);
        }
        else
        {
            if (upperReadsQ(d.upper))
                terms.push_back("(A::fdivValid(vu) ? A::fdivReady(vu) : 0u)");
            if (lowerReadsP(d.lower))
                terms.push_back("A::efuMaxPendingReady(vu)");
            const uint32_t lowerOp = lowerPrimaryOp(d.lower);
            const bool clipReader = lowerOp == 0x10u || lowerOp == 0x12u || lowerOp == 0x13u || lowerOp == 0x1Cu;
            if (isFlagReader(d.lower) || clipReader)
                terms.push_back("A::flagPipelineReady(vu)");
        }
        std::sort(terms.begin(), terms.end());
        terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
        if (d.lowerUsage.pipeline == A::kPipelineFdiv || d.lowerUsage.waitQ)
            terms.push_back("(A::fdivValid(vu) ? A::fdivReady(vu) : 0u)");
        if (d.lowerUsage.pipeline == A::kPipelineEfu)
            terms.push_back("A::efuResourceReady(vu)");
        if (d.lowerUsage.waitP)
            terms.push_back("A::efuMaxPendingReady(vu)");
        if (d.lowerUsage.pipeline == A::kPipelineXgkick)
            terms.push_back("(A::xgkickActive(vu) ? cyc + 1u : 0u)");
        if (!terms.empty())
        {
            o << "    {\n        auto ready = [&]() { uint64_t r = cyc;";
            for (const auto &term : terms)
                o << " r = std::max<uint64_t>(r, " << term << ");";
            o << " return r; };\n";
            o << "        uint64_t readyCycle = ready();\n";
            o << "        while (readyCycle > cyc)\n        {\n";
            if (blockStart)
                o << "            if (readyCycle >= budgetEnd) { A::advanceTo(vu, budgetEnd); break; }\n";
            o << "            A::advanceTo(vu, readyCycle);\n            readyCycle = ready();\n        }\n";
            if (blockStart)
                o << "        if (cyc >= budgetEnd) { st.pc = " << pc << "u; return Vu1CompiledResult::Budget; }\n";
            o << "    }\n";
        }

        uint8_t writtenVi = 0u;
        for (uint32_t reg = 1; reg < 16u; ++reg)
            if ((d.lowerUsage.viWrite & (1u << reg)) != 0u)
            {
                writtenVi = static_cast<uint8_t>(reg);
                break;
            }
        const auto upperWrite = d.upperUsage.vfWrite;
        const auto lowerWrite = d.lowerUsage.vfWrite;
        const bool hasUpperWrite = upperWrite.reg != 0u;
        const bool hasLowerWrite = lowerWrite.reg != 0u && d.suppressedLowerVf != lowerWrite.reg;
        const bool hasDistinctLowerWrite = hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg);

        // Instruction bodies write exactly their destination lanes, so the save/restore that
        // limits writeback to the decoded lanes only matters when upper and lower write the
        // same register (verify mode checks this against the interpreter).
        const bool laneExactWriteback = hasUpperWrite && lowerWrite.reg == upperWrite.reg;
        if (writtenVi != 0u)
            o << "    const int32_t oldVi = st.vi[" << unsigned(writtenVi) << "];\n";
        if (hasUpperWrite && laneExactWriteback)
            o << "    float oldUpperVf[4]; std::memcpy(oldUpperVf, st.vf[" << unsigned(upperWrite.reg) << "], sizeof(oldUpperVf));\n";
        if (hasDistinctLowerWrite && laneExactWriteback)
            o << "    float oldLowerVf[4]; std::memcpy(oldLowerVf, st.vf[" << unsigned(lowerWrite.reg) << "], sizeof(oldLowerVf));\n";

        const bool upperNop = isUpperNop(d.upper);
        const bool lowerNop = isLowerNop(d.lower);
        auto emitUpper = [&]()
        {
            if (an.deadFlagWriters.count(pc) != 0u && emitInlineFmac(o, d.upper))
                ++inlined;
            else if (an.deadFlagWriters.count(pc) == 0u && isFlagWriter(d.upper) && emitInlineFmac(o, d.upper, true))
                ++inlinedFlags;
            else if (emitInlineUpperSimple(o, d.upper))
                ++inlinedSimple;
            else
                o << "    A::execUpper(vu, " << hex32(d.upper) << ");\n";
        };
        if (d.iBit)
        {
            if (!upperNop)
                emitUpper();
            o << "    { float immediate; const uint32_t bits = " << hex32(d.lower)
              << "; std::memcpy(&immediate, &bits, sizeof(immediate)); st.i = A::normalizeOperand(vu, immediate); }\n";
        }
        else if (d.upperVfShadowReg != 0u)
        {
            const unsigned r = d.upperVfShadowReg;
            o << "    {\n        float oldVf[4]; float upperVf[4];\n";
            o << "        std::memcpy(oldVf, st.vf[" << r << "], sizeof(oldVf));\n";
            o << "        A::execUpper(vu, " << hex32(d.upper) << ");\n";
            o << "        std::memcpy(upperVf, st.vf[" << r << "], sizeof(upperVf));\n";
            o << "        std::memcpy(st.vf[" << r << "], oldVf, sizeof(oldVf));\n";
            o << "        A::execLower(vu, " << hex32(d.lower) << ", " << hex32(d.upper) << ");\n";
            o << "        std::memcpy(st.vf[" << r << "], upperVf, sizeof(upperVf));\n    }\n";
        }
        else
        {
            if (!upperNop)
                emitUpper();
            if (!lowerNop)
            {
                if (emitInlineLower(o, pc, d.lower))
                    ++inlinedLower;
                else
                    o << "    A::execLower(vu, " << hex32(d.lower) << ", " << hex32(d.upper) << ");\n";
            }
        }
        o << "    A::clearViBranchBackup(vu);\n";

        // Immediate writeback (mirrors run()).
        if (hasUpperWrite && laneExactWriteback)
        {
            const unsigned r = upperWrite.reg;
            o << "    { float v[4]; std::memcpy(v, st.vf[" << r << "], sizeof(v)); std::memcpy(st.vf[" << r << "], oldUpperVf, sizeof(v));";
            if (hasDistinctLowerWrite)
                o << " float lv[4]; std::memcpy(lv, st.vf[" << unsigned(lowerWrite.reg) << "], sizeof(lv)); std::memcpy(st.vf["
                  << unsigned(lowerWrite.reg) << "], oldLowerVf, sizeof(lv));";
            for (uint32_t c = 0; c < 4u; ++c)
                if ((upperWrite.lanes & laneForComponent(c)) != 0u)
                    o << " st.vf[" << r << "][" << c << "] = v[" << c << "];";
            if (hasDistinctLowerWrite)
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((lowerWrite.lanes & laneForComponent(c)) != 0u)
                        o << " st.vf[" << unsigned(lowerWrite.reg) << "][" << c << "] = lv[" << c << "];";
            o << " }\n";
        }
        else if (hasDistinctLowerWrite && laneExactWriteback)
        {
            const unsigned r = lowerWrite.reg;
            o << "    { float lv[4]; std::memcpy(lv, st.vf[" << r << "], sizeof(lv)); std::memcpy(st.vf[" << r << "], oldLowerVf, sizeof(lv));";
            for (uint32_t c = 0; c < 4u; ++c)
                if ((lowerWrite.lanes & laneForComponent(c)) != 0u)
                    o << " st.vf[" << r << "][" << c << "] = lv[" << c << "];";
            o << " }\n";
        }
        if (writtenVi != 0u)
            o << "    st.vi[" << unsigned(writtenVi) << "] = static_cast<int16_t>(st.vi[" << unsigned(writtenVi) << "]);\n";

        // markPairWrites with constants (exact timing only).
        if (g_exactTiming && lowerWrite.reg != 0u && d.suppressedLowerVf != lowerWrite.reg)
        {
            const unsigned latency = d.lowerUsage.vfLatency != 0u ? d.lowerUsage.vfLatency : d.lowerUsage.latency;
            for (uint32_t c = 0; c < 4u; ++c)
                if ((lowerWrite.lanes & laneForComponent(c)) != 0u)
                    o << "    A::vfReadyRef(vu, " << unsigned(lowerWrite.reg) << ", " << c << ") = cyc + " << latency << "u;\n";
        }
        if (g_exactTiming && upperWrite.reg != 0u)
        {
            const unsigned latency = d.upperUsage.vfLatency != 0u ? d.upperUsage.vfLatency : d.upperUsage.latency;
            for (uint32_t c = 0; c < 4u; ++c)
                if ((upperWrite.lanes & laneForComponent(c)) != 0u)
                    o << "    A::vfReadyRef(vu, " << unsigned(upperWrite.reg) << ", " << c << ") = cyc + " << latency << "u;\n";
        }
        if (g_exactTiming)
        {
            const unsigned latency = d.lowerUsage.viLatency != 0u ? d.lowerUsage.viLatency : d.lowerUsage.latency;
            for (uint32_t reg = 1; reg < 16u; ++reg)
                if ((d.lowerUsage.viWrite & (1u << reg)) != 0u)
                    o << "    A::viReadyRef(vu, " << reg << ") = cyc + " << latency << "u;\n";
        }
        for (uint32_t c = 0; g_exactTiming && c < 4u; ++c)
            if ((d.upperUsage.accWrite & laneForComponent(c)) != 0u)
                o << "    A::accReadyRef(vu, " << c << ") = cyc + " << A::kAccForwardLatency << "u;\n";
        if (writtenVi != 0u && d.lowerUsage.delaysNextBranchRead)
            o << "    A::recordViWriteForBranch(vu, " << unsigned(writtenVi) << ", oldVi);\n";
        {
            const uint32_t w = g_block.index;
            if (lowerWrite.reg != 0u && d.suppressedLowerVf != lowerWrite.reg)
            {
                const unsigned latency = d.lowerUsage.vfLatency != 0u ? d.lowerUsage.vfLatency : d.lowerUsage.latency;
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((lowerWrite.lanes & laneForComponent(c)) != 0u)
                        g_block.vf[lowerWrite.reg & 31u][c] = w + latency;
            }
            if (upperWrite.reg != 0u)
            {
                const unsigned latency = d.upperUsage.vfLatency != 0u ? d.upperUsage.vfLatency : d.upperUsage.latency;
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((upperWrite.lanes & laneForComponent(c)) != 0u)
                        g_block.vf[upperWrite.reg & 31u][c] = w + latency;
            }
            const unsigned viLatency = d.lowerUsage.viLatency != 0u ? d.lowerUsage.viLatency : d.lowerUsage.latency;
            for (uint32_t reg = 1; reg < 16u; ++reg)
                if ((d.lowerUsage.viWrite & (1u << reg)) != 0u)
                    g_block.vi[reg] = w + viLatency;
            for (uint32_t c = 0; c < 4u; ++c)
                if ((d.upperUsage.accWrite & laneForComponent(c)) != 0u)
                    g_block.acc[c] = w + A::kAccForwardLatency;
            ++g_block.index;
        }

        o << "    st.vf[0][0] = 0.0f; st.vf[0][1] = 0.0f; st.vf[0][2] = 0.0f; st.vf[0][3] = 1.0f; st.vi[0] = 0;\n";
        const uint32_t next = (pc + 8u) & kPcMask;
        o << "    st.pc = " << next << "u;\n";
        // Only branch pairs and delay slots can see a pending branch (entry with one pending
        // elsewhere falls back to the interpreter, see the image function).
        const Flow pairFlow = lowerFlow(pc, d.lower);
        // A delay slot is always the pair right after a branch (whatever the flow analysis models).
        bool delaySlot = false;
        if (pc >= 8u)
        {
            const uint32_t prev = pc - 8u;
            const A::Decoded pd = A::decode(vu, code, prev);
            const Flow prevFlow = lowerFlow(prev, pd.lower);
            delaySlot = prevFlow.staticBranch || prevFlow.jump;
        }
        if (pairFlow.staticBranch || pairFlow.jump || delaySlot)
            o << "    if (st.branchPending)\n    {\n        if (st.branchDelay == 0u) { st.pc = st.branchTarget & pcMask; st.branchPending = false; }\n        else --st.branchDelay;\n    }\n";

        const bool dBit = d.dBit;
        const bool tBit = d.tBit;
        const bool haltBranchPossible = (dBit || tBit) && d.lowerUsage.pipeline == A::kPipelineBranch;
        o << "    bool ended = false;\n";
        if (dBit || tBit)
        {
            o << "    const bool dHalt = " << (dBit ? "st.dBitEnabled" : "false") << ";\n";
            o << "    const bool tHalt = " << (tBit ? "st.tBitEnabled" : "false") << ";\n";
        }
        o << "    if (st.haltAfterDelaySlot) { st.stoppedByD = A::pendingHaltD(vu); st.stoppedByT = A::pendingHaltT(vu); ended = true; }\n";
        o << "    else if (st.ebit) ended = true;\n";
        if ((dBit || tBit) && !haltBranchPossible)
            o << "    else if (dHalt || tHalt) { st.stoppedByD = dHalt; st.stoppedByT = tHalt; ended = true; }\n";
        if (d.eBit)
            o << "    else st.ebit = true;\n";
        else if (haltBranchPossible)
            o << "    else if (dHalt || tHalt) { st.haltAfterDelaySlot = true; A::pendingHaltD(vu) = dHalt; A::pendingHaltT(vu) = tHalt; }\n";
        o << "    A::advanceOneCycle(vu);\n";
        o << "    if (ended) return Vu1CompiledResult::Ended;\n";
        if (compiled.count(next) != 0u && chunkOf(next) == chunkOf(pc))
            o << "    if (st.pc == " << next << "u) goto " << label(next) << ";\n";
        o << "    goto dispatch;\n}\n";
    }
}

namespace
{
    // VF0/VI0 are constants; the per-pair reset is only needed when the pair may write register
    // 0 (an interpreter fallback, or inline code naming st.vf[0]/st.vi[0]).
    void emitPair(std::ostream &out, const VU1Interpreter &vu, const uint8_t *code, uint32_t pc, const std::set<uint32_t> &compiled,
                  const ImageAnalysis &an, uint32_t &inlined)
    {
        std::ostringstream body;
        emitPairBody(body, vu, code, pc, compiled, an, inlined);
        std::string text = body.str();
        const std::string reset = "    st.vf[0][0] = 0.0f; st.vf[0][1] = 0.0f; st.vf[0][2] = 0.0f; st.vf[0][3] = 1.0f; st.vi[0] = 0;\n";
        const size_t at = text.find(reset);
        if (at != std::string::npos)
        {
            std::string rest = text;
            rest.erase(at, reset.size());
            const bool mayWriteZero = rest.find("A::execUpper") != std::string::npos || rest.find("A::execLower") != std::string::npos ||
                                      rest.find("st.vf[0]") != std::string::npos || rest.find("st.vi[0]") != std::string::npos;
            if (!mayWriteZero)
                text = rest;
        }
        out << text;
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: ps2x_vu1recomp <dump dir> <output dir>\n";
        return 1;
    }
    const std::filesystem::path dumpDir = argv[1];
    const std::filesystem::path outDir = argv[2];
    for (int i = 3; i < argc; ++i)
        if (std::string(argv[i]) == "--relaxed-timing")
            g_exactTiming = false;
    std::filesystem::create_directories(outDir);

    VU1Interpreter vu(VU1Interpreter::Unit::VU1);
    std::vector<std::string> images;
    std::vector<uint32_t> statusMasks;
    for (const auto &entry : std::filesystem::directory_iterator(dumpDir))
    {
        if (entry.path().extension() != ".bin")
            continue;
        const std::string name = entry.path().stem().string();
        std::vector<uint8_t> code(kCodeSize, 0u);
        {
            std::ifstream in(entry.path(), std::ios::binary);
            in.read(reinterpret_cast<char *>(code.data()), kCodeSize);
            if (in.gcount() != static_cast<std::streamsize>(kCodeSize))
            {
                std::cerr << "skipping " << entry.path() << ": not a 16 KiB image\n";
                continue;
            }
        }
        const uint64_t hash = hashVu1CodeImage(code.data(), kCodeSize);
        std::set<uint32_t> entries;
        {
            std::ifstream in(dumpDir / (name + ".entries"));
            std::string line;
            while (std::getline(in, line))
                if (!line.empty())
                    entries.insert(static_cast<uint32_t>(std::stoul(line, nullptr, 16)));
        }
        if (entries.empty())
            continue;

        const std::set<uint32_t> pairs = reachablePairs(vu, code.data(), entries);
        const ImageAnalysis analysis = analyse(vu, code.data(), pairs, entries);
        {
            g_blockStarts.clear();
            g_maxVfLatency = g_maxViLatency = 0u;
            g_maxAccLatency = A::kAccForwardLatency;
            std::set<uint32_t> noFallthrough; // pairs not reached by sequential flow
            for (uint32_t pc : pairs)
            {
                const A::Decoded d = A::decode(vu, code.data(), pc);
                // Only pairs that actually write a register count (EFU ops report a long latency but
                // write P, not VF/VI).
                if (d.lowerUsage.vfWrite.reg != 0u && d.suppressedLowerVf != d.lowerUsage.vfWrite.reg)
                    g_maxVfLatency = std::max<uint32_t>(g_maxVfLatency, d.lowerUsage.vfLatency != 0u ? d.lowerUsage.vfLatency : d.lowerUsage.latency);
                if (d.upperUsage.vfWrite.reg != 0u)
                    g_maxVfLatency = std::max<uint32_t>(g_maxVfLatency, d.upperUsage.vfLatency != 0u ? d.upperUsage.vfLatency : d.upperUsage.latency);
                if ((d.lowerUsage.viWrite & 0xFFFEu) != 0u)
                    g_maxViLatency = std::max<uint32_t>(g_maxViLatency, d.lowerUsage.viLatency != 0u ? d.lowerUsage.viLatency : d.lowerUsage.latency);
                const Flow flow = lowerFlow(pc, d.lower);
                const uint32_t afterSlot = (pc + 16u) & kPcMask;
                if (flow.staticBranch)
                    g_blockStarts.insert(flow.target & kPcMask & ~7u);
                if (flow.link)
                    g_blockStarts.insert(afterSlot);
                if (d.eBit || ((flow.staticBranch || flow.jump) && !flow.conditional))
                    noFallthrough.insert(afterSlot);
            }
            for (uint32_t entry : entries)
                g_blockStarts.insert(entry & kPcMask & ~7u);
            for (uint32_t pc : pairs)
            {
                const uint32_t previous = (pc - 8u) & kPcMask;
                if (pairs.count(previous) == 0u || chunkOf(previous) != chunkOf(pc) || noFallthrough.count(pc) != 0u)
                    g_blockStarts.insert(pc);
            }
            g_stallChecksRemoved = g_stallChecksKept = 0u;
        }
        char fnName[64];
        std::snprintf(fnName, sizeof(fnName), "vu1img_%016llx", static_cast<unsigned long long>(hash));
        std::ofstream o(outDir / (std::string("vu1_") + (fnName + 7) + ".cpp"));
        o << "// Generated by ps2x_vu1recomp from " << name << ".bin: " << pairs.size() << " pairs, entries";
        for (uint32_t e : entries)
            o << " 0x" << std::hex << e << std::dec;
        o << ". Do not edit.\n";
        o << "#include \"runtime/ps2_vu1_compiled.h\"\n#include <algorithm>\n#include <cstring>\n#include <emmintrin.h>\n\n";
        o << kSimdPreamble;
        o << "using A = Vu1CompiledAccess;\n";
        o << "// Returned by a chunk when execution continues in another chunk.\n";
        o << "static constexpr Vu1CompiledResult kContinue = static_cast<Vu1CompiledResult>(3);\n\n";
        std::map<uint32_t, std::vector<uint32_t>> chunks;
        for (uint32_t pc : pairs)
            chunks[chunkOf(pc)].push_back(pc);
        uint32_t inlined = 0u;
        for (const auto &[chunk, chunkPairs] : chunks)
        {
            o << "static Vu1CompiledResult " << fnName << "_c" << chunk << "(VU1Interpreter &vu, uint64_t budgetEnd)\n{\n";
            o << "VU1State &st = A::state(vu);\nuint64_t &cyc = A::cycle(vu);\nconst uint32_t pcMask = A::microAddressMask(vu);\n";
            o << "goto dispatch;\n";
            for (uint32_t pc : chunkPairs)
                emitPair(o, vu, code.data(), pc, pairs, analysis, inlined);
            o << "dispatch:\n    switch (st.pc)\n    {\n";
            for (uint32_t pc : chunkPairs)
                if (g_blockStarts.count(pc) != 0u)
                    o << "    case " << pc << "u: goto " << label(pc) << ";\n";
            o << "    default: return (st.pc >> " << kChunkShift << ") == " << chunk
              << "u ? Vu1CompiledResult::Fallback : kContinue;\n    }\n}\n\n";
        }
        o << "Vu1CompiledResult " << fnName << "(VU1Interpreter &vu, uint64_t budgetEnd)\n{\n";
        o << "    VU1State &st = A::state(vu);\n";
        o << "    if (st.branchPending)\n        return Vu1CompiledResult::Fallback; // resumed mid-branch: the interpreter takes this run\n";
        o << "    for (;;)\n    {\n        Vu1CompiledResult result;\n";
        o << "        switch (st.pc >> " << kChunkShift << ")\n        {\n";
        for (const auto &[chunk, chunkPairs] : chunks)
            o << "        case " << chunk << "u: result = " << fnName << "_c" << chunk << "(vu, budgetEnd); break;\n";
        o << "        default: return Vu1CompiledResult::Fallback;\n        }\n";
        o << "        if (result != kContinue)\n            return result;\n    }\n}\n";
        images.push_back(fnName);
        statusMasks.push_back(inlined != 0u ? 0x03Fu : 0xFFFu);
        std::cout << name << ": " << g_blockStarts.size() << " block starts, stall checks removed " << g_stallChecksRemoved
                  << " kept " << g_stallChecksKept << " (max latency vf " << g_maxVfLatency << " vi " << g_maxViLatency << ")\n";
        std::cout << name << ": " << pairs.size() << " pairs from " << entries.size() << " entries; flag writers "
                  << analysis.writers << ", unobserved " << analysis.deadFlagWriters.size() << ", inlined " << inlined << " (+" << inlinedFlags << " with flags, " << inlinedSimple << " simple upper, " << inlinedLower << " lower)"
                  << (analysis.stickyObservable ? " (sticky status observable: no flag elimination)" : "") << "\n";
    }

    std::ofstream reg(outDir / "vu1_images.cpp");
    reg << "// Generated by ps2x_vu1recomp. Do not edit.\n#include \"runtime/ps2_vu1_compiled.h\"\n\n";
    for (const auto &fn : images)
        reg << "Vu1CompiledResult " << fn << "(VU1Interpreter &vu, uint64_t budgetEnd);\n";
    reg << "\nvoid registerGeneratedVu1Images()\n{\n";
    for (size_t i = 0; i < images.size(); ++i)
        reg << "    registerVu1CompiledImage(0x" << images[i].substr(7) << "ull, &" << images[i] << ", 0x" << std::hex
            << statusMasks[i] << std::dec << "u);\n";
    if (!g_exactTiming)
        reg << "    setVu1CompiledVerifyIgnoresCycles(true); // relaxed timing\n";
    reg << "}\n";
    std::cout << images.size() << " images\n";
    return 0;
}
