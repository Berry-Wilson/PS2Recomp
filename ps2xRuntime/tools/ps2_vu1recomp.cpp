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

    void emitPair(std::ostream &o, const VU1Interpreter &vu, const uint8_t *code, uint32_t pc, const std::set<uint32_t> &compiled)
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
        if (d.iBit)
        {
            if (!upperNop)
                o << "    A::execUpper(vu, " << hex32(d.upper) << ");\n";
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
                o << "    A::execUpper(vu, " << hex32(d.upper) << ");\n";
            if (!lowerNop)
                o << "    A::execLower(vu, " << hex32(d.lower) << ", " << hex32(d.upper) << ");\n";
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
        if (compiled.count(next) != 0u)
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
        char fnName[64];
        std::snprintf(fnName, sizeof(fnName), "vu1img_%016llx", static_cast<unsigned long long>(hash));
        std::ofstream o(outDir / (std::string("vu1_") + (fnName + 7) + ".cpp"));
        o << "// Generated by ps2x_vu1recomp from " << name << ".bin: " << pairs.size() << " pairs, entries";
        for (uint32_t e : entries)
            o << " 0x" << std::hex << e << std::dec;
        o << ". Do not edit.\n";
        o << "#include \"runtime/ps2_vu1_compiled.h\"\n#include <algorithm>\n#include <cstring>\n\n";
        o << "using A = Vu1CompiledAccess;\n\n";
        o << "Vu1CompiledResult " << fnName << "(VU1Interpreter &vu, uint64_t budgetEnd)\n{\n";
        o << "VU1State &st = A::state(vu);\nuint64_t &cyc = A::cycle(vu);\nconst uint32_t pcMask = A::microAddressMask(vu);\n";
        o << "goto dispatch;\n";
        for (uint32_t pc : pairs)
            emitPair(o, vu, code.data(), pc, pairs);
        o << "dispatch:\n    switch (st.pc)\n    {\n";
        for (uint32_t pc : pairs)
            o << "    case " << pc << "u: goto " << label(pc) << ";\n";
        o << "    default: return Vu1CompiledResult::Fallback;\n    }\n}\n";
        images.push_back(fnName);
        std::cout << name << ": " << pairs.size() << " pairs from " << entries.size() << " entries\n";
    }

    std::ofstream reg(outDir / "vu1_images.cpp");
    reg << "// Generated by ps2x_vu1recomp. Do not edit.\n#include \"runtime/ps2_vu1_compiled.h\"\n\n";
    for (const auto &fn : images)
        reg << "Vu1CompiledResult " << fn << "(VU1Interpreter &vu, uint64_t budgetEnd);\n";
    reg << "\nvoid registerGeneratedVu1Images()\n{\n";
    for (const auto &fn : images)
        reg << "    registerVu1CompiledImage(0x" << fn.substr(7) << "ull, &" << fn << ");\n";
    reg << "}\n";
    std::cout << images.size() << " images\n";
    return 0;
}
