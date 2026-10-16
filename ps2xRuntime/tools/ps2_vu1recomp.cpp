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

    // Generated code is split into one function per 512-byte PC range (64 pairs): MSVC stops
    // inlining (and allocates registers poorly) inside one huge goto function. Jumps within a
    // chunk stay gotos; leaving a chunk returns to the image's dispatcher loop.
    constexpr uint32_t kChunkShift = 9u;
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
        o << "    { // " << names[kind] << (accTarget ? "A" : "") << (withFlags ? " inline (fast flags)\n" : " inline (flags unobserved)\n");
        if (source == Bc)
            o << "        const float b = vu1NormalizeOperand(st.vf[" << ft << "][" << (upper & 3u) << "]);\n";
        else if (source == Q)
            o << "        const float b = vu1NormalizeOperand(st.q);\n";
        else if (source == I)
            o << "        const float b = vu1NormalizeOperand(st.i);\n";
        o << "        float r[4] = {0.0f, 0.0f, 0.0f, 0.0f};";
        if (productSum)
            o << " float p[4] = {0.0f, 0.0f, 0.0f, 0.0f};";
        o << " bool inRange = true;";
        if (withFlags)
            o << " uint32_t mac = 0u; uint32_t productNegative = 0u;";
        o << "\n";
        for (uint32_t c = 0; c < 4u; ++c)
        {
            if ((dest & laneForComponent(c)) == 0u)
                continue;
            o << "        { const float s = vu1NormalizeOperand(st.vf[" << fs << "][" << c << "]);";
            const std::string t = source == Vt ? "vu1NormalizeOperand(st.vf[" + std::to_string(ft) + "][" + std::to_string(c) + "])" : "b";
            switch (kind)
            {
            case Add: o << " r[" << c << "] = s + " << t << "; inRange = inRange && " << (withFlags ? "vu1NormalNonMax" : "vu1NormalizeSingleRounding") << "(r[" << c << "]);"; break;
            case Sub: o << " r[" << c << "] = s - " << t << "; inRange = inRange && " << (withFlags ? "vu1NormalNonMax" : "vu1NormalizeSingleRounding") << "(r[" << c << "]);"; break;
            case Mul: o << " r[" << c << "] = s * " << t << "; inRange = inRange && " << (withFlags ? "vu1NormalNonMax" : "vu1NormalizeSingleRounding") << "(r[" << c << "]);"; break;
            case Madd:
                o << " p[" << c << "] = s * " << t << "; r[" << c << "] = vu1NormalizeOperand(st.acc[" << c << "]) + p[" << c
                  << "]; inRange = inRange && vu1ProductSumInRange(r[" << c << "], p[" << c << "])"
                  << (withFlags ? " && vu1NormalNonMax(p[" + std::to_string(c) + "]); productNegative |= std::signbit(p[" + std::to_string(c) + "]) ? 1u : 0u;" : ";");
                break;
            case Msub:
                o << " p[" << c << "] = s * " << t << "; r[" << c << "] = vu1NormalizeOperand(st.acc[" << c << "]) - p[" << c
                  << "]; inRange = inRange && vu1ProductSumInRange(r[" << c << "], p[" << c << "])"
                  << (withFlags ? " && vu1NormalNonMax(p[" + std::to_string(c) + "]); productNegative |= std::signbit(p[" + std::to_string(c) + "]) ? 1u : 0u;" : ";");
                break;
            }
            if (withFlags)
                o << " if (std::signbit(r[" << c << "])) mac |= " << (unsigned(laneForComponent(c)) << 4) << "u;";
            o << " }\n";
        }
        if (withFlags)
            o << "        if (!inRange) A::fmacExact(vu, " << hex32(upper) << ", r, " << dest
              << "u); else A::queueFmacFlags(vu, mac, mac != 0u ? 2u : 0u, productNegative != 0u ? 2u : 0u);\n";
        else
            o << "        if (!inRange) A::normalizeFmacValues(vu, " << hex32(upper) << ", r, " << dest << "u);\n";
        o << "       ";
        for (uint32_t c = 0; c < 4u; ++c)
            if ((dest & laneForComponent(c)) != 0u)
                o << (accTarget ? " st.acc[" : " st.vf[") << (accTarget ? "" : std::to_string(fd) + "][") << c << "] = r[" << c << "];";
        o << "\n    }\n";
        return true;
    }

    // Inline upper ops that never touch the flag pipelines (MAX/MINI forms, ITOF/FTOI), mirroring
    // execUpper: normalized operands, no result normalization, destination lanes only.
    bool emitInlineUpperSimple(std::ostream &o, uint32_t upper)
    {
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        const unsigned dest = (upper >> 21) & 0xFu;
        const unsigned ft = (upper >> 16) & 0x1Fu;
        const unsigned fs = (upper >> 11) & 0x1Fu;
        const unsigned fd = (upper >> 6) & 0x1Fu;
        auto lanes = [&](const std::function<std::string(uint32_t)> &value, unsigned dstReg)
        {
            o << "    { float r[4];";
            for (uint32_t c = 0; c < 4u; ++c)
                if ((dest & laneForComponent(c)) != 0u)
                    o << " r[" << c << "] = " << value(c) << ";";
            for (uint32_t c = 0; c < 4u; ++c)
                if ((dest & laneForComponent(c)) != 0u)
                    o << " st.vf[" << dstReg << "][" << c << "] = r[" << c << "];";
            o << " }\n";
        };
        auto vs = [&](uint32_t c) { return "vu1NormalizeOperand(st.vf[" + std::to_string(fs) + "][" + std::to_string(c) + "])"; };
        auto vt = [&](uint32_t c) { return "vu1NormalizeOperand(st.vf[" + std::to_string(ft) + "][" + std::to_string(c) + "])"; };
        auto minMax = [&](bool isMax, const std::function<std::string(uint32_t)> &other)
        {
            o << "    // " << (isMax ? "MAX" : "MINI") << " inline\n";
            lanes([&](uint32_t c)
                  {
                      const std::string a = "a" + std::to_string(c), b = "b" + std::to_string(c);
                      return "([](float " + a + ", float " + b + ") { return (" + a + (isMax ? " > " : " < ") + b + ") ? " + a +
                             " : " + b + "; })(" + vs(c) + ", " + other(c) + ")";
                  },
                  fd);
            return true;
        };
        if (op >= 0x10u && op <= 0x13u)
            return minMax(true, [&](uint32_t) { return vt(op & 3u); });
        if (op >= 0x14u && op <= 0x17u)
            return minMax(false, [&](uint32_t) { return vt(op & 3u); });
        if (op == 0x1Du)
            return minMax(true, [&](uint32_t) { return std::string("vu1NormalizeOperand(st.i)"); });
        if (op == 0x1Fu)
            return minMax(false, [&](uint32_t) { return std::string("vu1NormalizeOperand(st.i)"); });
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
            o << "    // ITOF inline\n";
            lanes([&](uint32_t c)
                  {
                      const std::string bits = "([&]() { int32_t v; std::memcpy(&v, &st.vf[" + std::to_string(fs) + "][" +
                                               std::to_string(c) + "], 4); return v; })()";
                      return sp == 0x10u ? "static_cast<float>(" + bits + ")"
                                         : "static_cast<float>(" + bits + ") / " + scales[sp & 3u];
                  },
                  ft);
            return true;
        }
        if (sp >= 0x14u && sp <= 0x17u)
        {
            o << "    // FTOI inline\n";
            lanes([&](uint32_t c)
                  {
                      return "([](int32_t v) { float f; std::memcpy(&f, &v, 4); return f; })(vu1FloatToInt(" + vs(c) + ", " +
                             scales[sp & 3u] + "))";
                  },
                  ft);
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
              << "], sizeof(words)); A::queueStore(vu, addr, words, " << dest << "u); }\n    }\n";
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
              << "] & 0xFFFF)); const uint32_t words[4] = {val, val, val, val}; A::queueStore(vu, addr, words, " << dest
              << "u); }\n    }\n";
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
        for (uint32_t index = 0; index < usage.vfReadCount; ++index)
        {
            const auto &access = usage.vfRead[index];
            if (access.reg == 0u)
                continue;
            for (uint32_t c = 0; c < 4u; ++c)
                if ((access.lanes & laneForComponent(c)) != 0u)
                    terms.push_back("A::vfReady(vu, " + std::to_string(access.reg) + ", " + std::to_string(c) + ")");
        }
        for (uint32_t reg = 1; reg < 16u; ++reg)
            if ((usage.viRead & (1u << reg)) != 0u)
                terms.push_back("A::viReady(vu, " + std::to_string(reg) + ")");
        for (uint32_t c = 0; c < 4u; ++c)
            if ((usage.accRead & laneForComponent(c)) != 0u)
                terms.push_back("A::accReady(vu, " + std::to_string(c) + ")");
    }

    uint32_t inlinedSimple = 0u;
    uint32_t inlinedFlags = 0u;
    uint32_t inlinedLower = 0u;

    void emitPair(std::ostream &o, const VU1Interpreter &vu, const uint8_t *code, uint32_t pc, const std::set<uint32_t> &compiled,
                  const ImageAnalysis &an, uint32_t &inlined)
    {
        const A::Decoded d = A::decode(vu, code, pc);
        o << label(pc) << ":\n{\n";
        o << "    if (cyc >= budgetEnd || A::stopRequested(vu)) { st.pc = " << pc << "u; return Vu1CompiledResult::Budget; }\n";
        o << "    A::commitReadyPipelines(vu);\n";
        if (d.upperUsage.reserved || d.lowerUsage.reserved)
        {
            o << "    st.pc = " << pc << "u;\n    return Vu1CompiledResult::Fallback; // reserved instruction: interpreter reports it\n}\n";
            return;
        }

        std::vector<std::string> terms;
        emitStallTerms(terms, d.upperUsage);
        emitStallTerms(terms, d.lowerUsage);
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
            o << "            if (readyCycle >= budgetEnd) { A::advanceTo(vu, budgetEnd); break; }\n";
            o << "            A::advanceTo(vu, readyCycle);\n            readyCycle = ready();\n        }\n";
            o << "        if (cyc >= budgetEnd) { st.pc = " << pc << "u; return Vu1CompiledResult::Budget; }\n    }\n";
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

        // markPairWrites with constants.
        if (lowerWrite.reg != 0u && d.suppressedLowerVf != lowerWrite.reg)
        {
            const unsigned latency = d.lowerUsage.vfLatency != 0u ? d.lowerUsage.vfLatency : d.lowerUsage.latency;
            for (uint32_t c = 0; c < 4u; ++c)
                if ((lowerWrite.lanes & laneForComponent(c)) != 0u)
                    o << "    A::vfReadyRef(vu, " << unsigned(lowerWrite.reg) << ", " << c << ") = cyc + " << latency << "u;\n";
        }
        if (upperWrite.reg != 0u)
        {
            const unsigned latency = d.upperUsage.vfLatency != 0u ? d.upperUsage.vfLatency : d.upperUsage.latency;
            for (uint32_t c = 0; c < 4u; ++c)
                if ((upperWrite.lanes & laneForComponent(c)) != 0u)
                    o << "    A::vfReadyRef(vu, " << unsigned(upperWrite.reg) << ", " << c << ") = cyc + " << latency << "u;\n";
        }
        {
            const unsigned latency = d.lowerUsage.viLatency != 0u ? d.lowerUsage.viLatency : d.lowerUsage.latency;
            for (uint32_t reg = 1; reg < 16u; ++reg)
                if ((d.lowerUsage.viWrite & (1u << reg)) != 0u)
                    o << "    A::viReadyRef(vu, " << reg << ") = cyc + " << latency << "u;\n";
        }
        for (uint32_t c = 0; c < 4u; ++c)
            if ((d.upperUsage.accWrite & laneForComponent(c)) != 0u)
                o << "    A::accReadyRef(vu, " << c << ") = cyc + " << A::kAccForwardLatency << "u;\n";
        if (writtenVi != 0u && d.lowerUsage.delaysNextBranchRead)
            o << "    A::recordViWriteForBranch(vu, " << unsigned(writtenVi) << ", oldVi);\n";

        o << "    st.vf[0][0] = 0.0f; st.vf[0][1] = 0.0f; st.vf[0][2] = 0.0f; st.vf[0][3] = 1.0f; st.vi[0] = 0;\n";
        const uint32_t next = (pc + 8u) & kPcMask;
        o << "    st.pc = " << next << "u;\n";
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

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: ps2x_vu1recomp <dump dir> <output dir>\n";
        return 1;
    }
    const std::filesystem::path dumpDir = argv[1];
    const std::filesystem::path outDir = argv[2];
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
        char fnName[64];
        std::snprintf(fnName, sizeof(fnName), "vu1img_%016llx", static_cast<unsigned long long>(hash));
        std::ofstream o(outDir / (std::string("vu1_") + (fnName + 7) + ".cpp"));
        o << "// Generated by ps2x_vu1recomp from " << name << ".bin: " << pairs.size() << " pairs, entries";
        for (uint32_t e : entries)
            o << " 0x" << std::hex << e << std::dec;
        o << ". Do not edit.\n";
        o << "#include \"runtime/ps2_vu1_compiled.h\"\n#include <algorithm>\n#include <cstring>\n\n";
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
                o << "    case " << pc << "u: goto " << label(pc) << ";\n";
            o << "    default: return (st.pc >> " << kChunkShift << ") == " << chunk
              << "u ? Vu1CompiledResult::Fallback : kContinue;\n    }\n}\n\n";
        }
        o << "Vu1CompiledResult " << fnName << "(VU1Interpreter &vu, uint64_t budgetEnd)\n{\n";
        o << "    VU1State &st = A::state(vu);\n    for (;;)\n    {\n        Vu1CompiledResult result;\n";
        o << "        switch (st.pc >> " << kChunkShift << ")\n        {\n";
        for (const auto &[chunk, chunkPairs] : chunks)
            o << "        case " << chunk << "u: result = " << fnName << "_c" << chunk << "(vu, budgetEnd); break;\n";
        o << "        default: return Vu1CompiledResult::Fallback;\n        }\n";
        o << "        if (result != kContinue)\n            return result;\n    }\n}\n";
        images.push_back(fnName);
        statusMasks.push_back(inlined != 0u ? 0x03Fu : 0xFFFu);
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
    reg << "}\n";
    std::cout << images.size() << " images\n";
    return 0;
}
