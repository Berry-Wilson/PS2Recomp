#include "ps2_runtime.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"

#if defined(PS2X_HAS_HOST_DISPLAY)

#include "runtime/gs/gs_pcsx2_backend.h"
#include "runtime/host_display.h"
#include "imgui.h"

#endif
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_vu1_native.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"
#include "ps2_iop_host.h"
#include "ps2x/iop/iop_subsystem.h"

#include <iostream>
#include <stdexcept>
#include <fstream>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <sstream>
#include <filesystem>
#include <string>
#include <system_error>

namespace ps2_stubs
{
    void resetSifState();
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    // PS2X_VU0_SNAPSHOT_DIR + PS2X_VU0_SNAPSHOT_AT=<seconds>: from that time on, save VU0 state for
    // the next 12 microprogram calls, before and after, as vu0snap_<n>_<pre|post>.bin:
    // {u32 startPC, u32 fromVif0, vf[32][4] floats, u16 vi[16], 4KB data}.
    const std::chrono::steady_clock::time_point g_processStart = std::chrono::steady_clock::now();

    struct Vu0Snapshot
    {
        static Vu0Snapshot &instance()
        {
            static Vu0Snapshot s;
            return s;
        }
        bool armed()
        {
            if (dir.empty() || taken >= 12)
                return false;
            return std::chrono::steady_clock::now() >= from;
        }
        void save(bool post, uint32_t pc, bool fromVif0, const R5900Context *ctx, const uint8_t *data, const uint8_t *ram)
        {
            if (!post && taken == 0)
            {
                if (std::FILE *f = std::fopen((dir + "/ram.bin").c_str(), "wb"))
                {
                    std::fwrite(ram, 1, PS2_RAM_SIZE, f);
                    std::fclose(f);
                }
                if (std::FILE *f = std::fopen((dir + "/spr.bin").c_str(), "wb"))
                {
                    std::fwrite(ps2GetScratchpadHostPtr(), 1, PS2_SCRATCHPAD_SIZE, f);
                    std::fclose(f);
                }
            }
            char path[600];
            std::snprintf(path, sizeof(path), "%s/vu0snap_%02u_%s.bin", dir.c_str(), taken, post ? "post" : "pre");
            if (std::FILE *f = std::fopen(path, "wb"))
            {
                const uint32_t head[2] = {pc, fromVif0 ? 1u : 0u};
                std::fwrite(head, sizeof(head), 1, f);
                std::fwrite(ctx->vu0_vf, 16, 32, f);
                std::fwrite(ctx->vi, 2, 16, f);
                std::fwrite(data, 1, PS2_VU0_DATA_SIZE, f);
                std::fclose(f);
            }
            if (post)
                ++taken;
        }
        std::string dir;
        std::chrono::steady_clock::time_point from;
        uint32_t taken = 0;

    private:
        Vu0Snapshot()
        {
            if (const char *d = std::getenv("PS2X_VU0_SNAPSHOT_DIR"))
                dir = d;
            const char *at = std::getenv("PS2X_VU0_SNAPSHOT_AT");
            from = g_processStart +
                   std::chrono::milliseconds(static_cast<int64_t>((at ? std::atof(at) : 0.0) * 1000.0));
        }
    };
    thread_local bool t_vu0FromVif0 = false;
}

void ps2xSetGifCaptureRam(const uint8_t *ram, uint32_t size); // gs_frontend.cpp
void ps2xInstallWriteWatch(uint8_t *ram, uint32_t ramSize, uint8_t *scratchpad, uint32_t scratchpadSize,
                           std::function<uint32_t()> guestPc); // ps2_write_watch.cpp
void ps2xSetWriteWatchDescriber(std::function<std::string()> describe); // ps2_write_watch.cpp

namespace
{
    // PS2X_VU1_DUMP_DIR / PS2X_VU0_DUMP_DIR: dump each distinct micro memory image seen when a
    // microprogram starts and periodically log per-entry-point call counts, VU cycles and host time.
    class VuProfiler
    {
    public:
        static VuProfiler &vu1()
        {
            static VuProfiler s("PS2X_VU1_DUMP_DIR", "VU1", PS2_VU1_CODE_SIZE);
            return s;
        }
        static VuProfiler &vu0()
        {
            static VuProfiler s("PS2X_VU0_DUMP_DIR", "VU0", PS2_VU0_CODE_SIZE);
            return s;
        }

        bool enabled() const { return !m_dir.empty(); }

        void record(const uint8_t *code, uint64_t generation, uint32_t startPC,
                    uint64_t vuCycles, uint64_t hostNs)
        {
            if (generation != m_lastGeneration || !m_haveHash)
            {
                uint64_t h = 1469598103934665603ull;
                for (uint32_t i = 0; i < m_codeSize; ++i)
                    h = (h ^ code[i]) * 1099511628211ull;
                m_hash = h;
                m_haveHash = true;
                m_lastGeneration = generation;
                if (m_dumped.emplace(h, true).second)
                {
                    char path[512];
                    std::snprintf(path, sizeof(path), "%s/%s_%016llx.bin", m_dir.c_str(), m_fileTag.c_str(),
                                  static_cast<unsigned long long>(h));
                    if (FILE *f = std::fopen(path, "wb"))
                    {
                        std::fwrite(code, 1, m_codeSize, f);
                        std::fclose(f);
                    }
                }
            }

            Stat &s = m_stats[{m_hash, startPC}];
            ++s.calls;
            s.cycles += vuCycles;
            s.ns += hostNs;
            m_windowNs += hostNs;

            const auto now = std::chrono::steady_clock::now();
            if (now - m_lastReport >= std::chrono::seconds(5))
            {
                report(std::chrono::duration<double>(now - m_lastReport).count());
                m_lastReport = now;
                m_windowNs = 0;
            }
        }

    private:
        struct Key
        {
            uint64_t hash;
            uint32_t pc;
            bool operator==(const Key &o) const { return hash == o.hash && pc == o.pc; }
        };
        struct KeyHash
        {
            size_t operator()(const Key &k) const { return static_cast<size_t>(k.hash ^ (uint64_t(k.pc) * 0x9E3779B97F4A7C15ull)); }
        };
        struct Stat
        {
            uint64_t calls = 0;
            uint64_t cycles = 0;
            uint64_t ns = 0;
        };

        VuProfiler(const char *env, const char *tag, uint32_t codeSize)
            : m_tag(tag), m_fileTag(tag), m_codeSize(codeSize)
        {
            if (const char *dir = std::getenv(env))
                m_dir = dir;
            std::transform(m_fileTag.begin(), m_fileTag.end(), m_fileTag.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            m_lastReport = std::chrono::steady_clock::now();
        }

        void report(double windowSeconds)
        {
            std::fprintf(stderr, "[%sPROF] window %.1fs: %s host time %.1f%%\n", m_tag.c_str(), windowSeconds,
                         m_tag.c_str(), 100.0 * double(m_windowNs) / (windowSeconds * 1e9));
            for (const auto &[key, s] : m_stats)
            {
                std::fprintf(stderr, "[%sPROF]   code=%016llx pc=0x%04x calls=%llu cycles=%llu host_ms=%.1f\n",
                             m_tag.c_str(), static_cast<unsigned long long>(key.hash), key.pc,
                             static_cast<unsigned long long>(s.calls),
                             static_cast<unsigned long long>(s.cycles), double(s.ns) / 1e6);
            }
            std::fflush(stderr);
            m_stats.clear();
        }

        std::string m_dir;
        std::string m_tag;
        std::string m_fileTag;
        uint32_t m_codeSize;
        std::unordered_map<uint64_t, bool> m_dumped;
        std::unordered_map<Key, Stat, KeyHash> m_stats;
        std::chrono::steady_clock::time_point m_lastReport;
        uint64_t m_lastGeneration = ~0ull;
        uint64_t m_hash = 0;
        uint64_t m_windowNs = 0;
        bool m_haveHash = false;
    };

    // VU microprograms run as translated C++ (vu1n) when a translation matches micro memory,
    // else in the interpreter. PS2X_VU1_NATIVE / PS2X_VU0_NATIVE = 0 forces the interpreter;
    // = verify runs both on every call (the interpreter's results are kept) and logs any divergence.
    enum class VuMode
    {
        Interpreter,
        Native,
        Verify
    };

    VuMode readVuMode(const char *env)
    {
        const char *e = std::getenv(env);
        if (!e)
            return VuMode::Native;
        if (std::strcmp(e, "verify") == 0)
            return VuMode::Verify;
        return e[0] == '0' ? VuMode::Interpreter : VuMode::Native;
    }

    // PS2X_VU_VERIFY_AFTER=<seconds>: verify mode runs translated code alone until then, so a slow
    // verified run can still reach a late scene.
    VuMode verifyGate(VuMode mode)
    {
        if (mode != VuMode::Verify)
            return mode;
        static const double after = []()
        {
            const char *e = std::getenv("PS2X_VU_VERIFY_AFTER");
            return e ? std::atof(e) : 0.0;
        }();
        static bool started = false;
        if (!started)
            started = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_processStart).count() >= after;
        return started ? VuMode::Verify : VuMode::Native;
    }

    VuMode vu1Mode()
    {
        static const VuMode mode = readVuMode("PS2X_VU1_NATIVE");
        return verifyGate(mode);
    }

    VuMode vu0Mode()
    {
        static const VuMode mode = readVuMode("PS2X_VU0_NATIVE");
        return verifyGate(mode);
    }

    struct Vu1KickCapture
    {
        std::vector<std::vector<uint8_t>> packets;
        PS2Memory *forward = nullptr;
    };

    void vu1CaptureKick(void *user, const uint8_t *packet, uint32_t bytes)
    {
        auto *capture = static_cast<Vu1KickCapture *>(user);
        capture->packets.emplace_back(packet, packet + bytes);
        if (capture->forward)
            capture->forward->submitGifPacket(GifPathId::Path1, packet, bytes);
    }

    void vu1SubmitKick(void *user, const uint8_t *packet, uint32_t bytes)
    {
        static_cast<PS2Memory *>(user)->submitGifPacket(GifPathId::Path1, packet, bytes);
    }

    // Returns a description of the differences between the translated and interpreted runs.
    std::string vuDiff(const VU1State &n, const VU1State &i, const uint8_t *nData, const uint8_t *iData,
                       uint32_t dataSize, const Vu1KickCapture &nKicks, const Vu1KickCapture &iKicks)
    {
        std::ostringstream out;
        int reported = 0;
        auto note = [&](const std::string &s)
        {
            if (reported++ < 8)
                out << ' ' << s;
        };
        char buf[160];
        for (int r = 1; r < 32; ++r)
            for (int l = 0; l < 4; ++l)
                if (std::memcmp(&n.vf[r][l], &i.vf[r][l], 4) != 0)
                {
                    uint32_t a, b;
                    std::memcpy(&a, &n.vf[r][l], 4);
                    std::memcpy(&b, &i.vf[r][l], 4);
                    std::snprintf(buf, sizeof(buf), "vf%d.%c=%08x/%08x", r, "xyzw"[l], a, b);
                    note(buf);
                }
        for (int r = 1; r < 16; ++r)
            if (n.vi[r] != i.vi[r])
            {
                std::snprintf(buf, sizeof(buf), "vi%d=%d/%d", r, n.vi[r], i.vi[r]);
                note(buf);
            }
        if (std::memcmp(n.acc, i.acc, sizeof(n.acc)) != 0)
            note("acc");
        if (std::memcmp(&n.q, &i.q, 4) != 0 || std::memcmp(&n.p, &i.p, 4) != 0 || std::memcmp(&n.i, &i.i, 4) != 0)
            note("q/p/i");
        if ((n.mac & 0xFFFFu) != (i.mac & 0xFFFFu) || n.clip != i.clip)
        {
            std::snprintf(buf, sizeof(buf), "mac=%04x/%04x clip=%06x/%06x", n.mac & 0xFFFFu, i.mac & 0xFFFFu, n.clip, i.clip);
            note(buf);
        }
        if (n.pc != i.pc)
        {
            std::snprintf(buf, sizeof(buf), "pc=%04x/%04x", n.pc, i.pc);
            note(buf);
        }
        for (uint32_t off = 0; off < dataSize; off += 16)
            if (std::memcmp(nData + off, iData + off, 16) != 0)
            {
                std::snprintf(buf, sizeof(buf), "data@%04x", off);
                note(buf);
                break;
            }
        if (nKicks.packets.size() != iKicks.packets.size())
        {
            std::snprintf(buf, sizeof(buf), "kicks=%zu/%zu", nKicks.packets.size(), iKicks.packets.size());
            note(buf);
        }
        else
            for (size_t k = 0; k < nKicks.packets.size(); ++k)
                if (nKicks.packets[k] != iKicks.packets[k])
                {
                    std::snprintf(buf, sizeof(buf), "kick%zu(%zu/%zu bytes)", k, nKicks.packets[k].size(), iKicks.packets[k].size());
                    note(buf);
                    break;
                }
        return out.str();
    }

    // PS2X_VU1_SNAPSHOT_DIR + PS2X_VU1_SNAPSHOT_AT=<seconds>: from that time on, save VU1 state for
    // the next 64 microprogram runs, before and after, as vu1snap_<n>_<pre|post>.bin:
    // {u32 startPC, u32 resume, u32 top, u32 itop, vf[32][4] floats, i32 vi[16], 16KB data}.
    // The code memory at the first snapshot is saved as vu1code.bin.
    struct Vu1Snapshot
    {
        static Vu1Snapshot &instance()
        {
            static Vu1Snapshot s;
            return s;
        }
        bool armed()
        {
            if (dir.empty() || taken >= limit)
                return false;
            return std::chrono::steady_clock::now() >= from;
        }
        void save(bool post, uint32_t pc, bool resume, uint32_t top, uint32_t itop, const VU1State &st,
                  const uint8_t *code, const uint8_t *data)
        {
            if (!post && taken == 0)
            {
                if (std::FILE *f = std::fopen((dir + "/vu1code.bin").c_str(), "wb"))
                {
                    std::fwrite(code, 1, PS2_VU1_CODE_SIZE, f);
                    std::fclose(f);
                }
            }
            // Micro memory changes between runs (MPG uploads): save each distinct version as
            // vu1code_<first run index>.bin.
            if (!post && (lastCode.empty() || std::memcmp(lastCode.data(), code, PS2_VU1_CODE_SIZE) != 0))
            {
                lastCode.assign(code, code + PS2_VU1_CODE_SIZE);
                char cpath[600];
                std::snprintf(cpath, sizeof(cpath), "%s/vu1code_%03u.bin", dir.c_str(), taken);
                if (std::FILE *f = std::fopen(cpath, "wb"))
                {
                    std::fwrite(code, 1, PS2_VU1_CODE_SIZE, f);
                    std::fclose(f);
                }
            }
            char path[600];
            std::snprintf(path, sizeof(path), "%s/vu1snap_%03u_%s.bin", dir.c_str(), taken, post ? "post" : "pre");
            if (std::FILE *f = std::fopen(path, "wb"))
            {
                const uint32_t head[4] = {pc, resume ? 1u : 0u, top, itop};
                std::fwrite(head, sizeof(head), 1, f);
                std::fwrite(st.vf, 16, 32, f);
                std::fwrite(st.vi, 4, 16, f);
                std::fwrite(data, 1, PS2_VU1_DATA_SIZE, f);
                std::fclose(f);
            }
            if (post)
                ++taken;
        }
        std::string dir;
        std::chrono::steady_clock::time_point from;
        uint32_t taken = 0;
        uint32_t limit = 64;
        std::vector<uint8_t> lastCode;

    private:
        Vu1Snapshot()
        {
            if (const char *d = std::getenv("PS2X_VU1_SNAPSHOT_DIR"))
                dir = d;
            if (const char *n = std::getenv("PS2X_VU1_SNAPSHOT_COUNT")) // default 64
                limit = static_cast<uint32_t>(std::max(1, std::atoi(n)));
            const char *at = std::getenv("PS2X_VU1_SNAPSHOT_AT");
            from = g_processStart +
                   std::chrono::milliseconds(static_cast<int64_t>((at ? std::atof(at) : 0.0) * 1000.0));
        }
    };

    void runVu1ProgramImpl(VU1Interpreter &vu1, PS2Memory &memory, GS &gs, bool resume,
                           uint32_t startPC, uint32_t top, uint32_t itop);

    void runVu1Program(VU1Interpreter &vu1, PS2Memory &memory, GS &gs, bool resume,
                       uint32_t startPC, uint32_t top, uint32_t itop)
    {
        Vu1Snapshot &snap = Vu1Snapshot::instance();
        if (!snap.armed())
        {
            runVu1ProgramImpl(vu1, memory, gs, resume, startPC, top, itop);
            return;
        }
        const uint32_t pc = resume ? vu1.state().pc : startPC;
        snap.save(false, pc, resume, top, itop, vu1.state(), memory.getVU1Code(), memory.getVU1Data());
        runVu1ProgramImpl(vu1, memory, gs, resume, startPC, top, itop);
        snap.save(true, pc, resume, top, itop, vu1.state(), memory.getVU1Code(), memory.getVU1Data());
    }

    void runVu1ProgramImpl(VU1Interpreter &vu1, PS2Memory &memory, GS &gs, bool resume,
                           uint32_t startPC, uint32_t top, uint32_t itop)
    {
        uint8_t *code = memory.getVU1Code();
        uint8_t *data = memory.getVU1Data();
        const uint64_t generation = memory.getVU1CodeGeneration();
        if (resume)
            startPC = vu1.state().pc;
        auto interpret = [&]
        {
            vu1n::invalidateRegisters(vu1n::Unit::VU1);
            if (resume)
                vu1.resume(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, &memory, top, itop, 65536);
            else
                vu1.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, &memory, startPC, top, itop, 65536);
        };
        auto logUntranslated = [&]
        {
            static std::unordered_map<uint64_t, bool> seen;
            const uint64_t key = (generation << 16) | startPC;
            if (seen.emplace(key, true).second && seen.size() <= 64)
                std::fprintf(stderr, "[VU1N] no translation for entry pc=0x%04x (code generation %llu); interpreting\n",
                             startPC, static_cast<unsigned long long>(generation));
        };

        const VuMode mode = vu1Mode();
        if (mode == VuMode::Interpreter)
        {
            interpret();
            return;
        }

        if (mode == VuMode::Native)
        {
            // PS2X_VU_STATS=1: host time per VU cycle of translated VU1 runs, every 5 seconds.
            static const bool s_stats = std::getenv("PS2X_VU_STATS") != nullptr;
            const auto t0 = s_stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const uint64_t cyc0 = vu1.state().cycles;
            const vu1n::RunResult r = vu1n::run(vu1n::Unit::VU1, vu1.state(), code, generation, data, startPC, top, itop,
                                                65536, vu1SubmitKick, &memory);
            if (s_stats)
            {
                static uint64_t s_ns = 0, s_cycles = 0, s_calls = 0;
                static auto s_last = std::chrono::steady_clock::now();
                const auto t1 = std::chrono::steady_clock::now();
                s_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
                s_cycles += vu1.state().cycles - cyc0;
                ++s_calls;
                if (t1 - s_last >= std::chrono::seconds(5))
                {
                    std::fprintf(stderr, "[VU1STATS] calls=%llu cycles=%llu host=%.1fms %.2f ns/cycle\n",
                                 static_cast<unsigned long long>(s_calls), static_cast<unsigned long long>(s_cycles),
                                 s_ns / 1e6, s_cycles ? static_cast<double>(s_ns) / s_cycles : 0.0);
                    s_ns = s_cycles = s_calls = 0;
                    s_last = t1;
                }
            }
            if (r == vu1n::RunResult::NotTranslated)
            {
                logUntranslated();
                interpret();
            }
            else if (r == vu1n::RunResult::Bailed)
            {
                vu1n::invalidateRegisters(vu1n::Unit::VU1);
                vu1.resetPipelines();
                vu1.resume(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, &memory, top, itop, 65536);
            }
            return;
        }

        static uint64_t verified = 0, mismatches = 0, skipped = 0;
        static auto lastReport = std::chrono::steady_clock::now();
        VU1State nativeState = vu1.state();
        std::vector<uint8_t> nativeData(data, data + PS2_VU1_DATA_SIZE);
        Vu1KickCapture nativeKicks;
        const vu1n::RunResult r = vu1n::run(vu1n::Unit::VU1, nativeState, code, generation, nativeData.data(), startPC,
                                            top, itop, 65536, vu1CaptureKick, &nativeKicks);
        Vu1KickCapture interpKicks;
        interpKicks.forward = &memory;
        vu1.setXgkickSink(vu1CaptureKick, &interpKicks);
        interpret();
        vu1.setXgkickSink(nullptr, nullptr);

        if (r != vu1n::RunResult::Done)
        {
            if (r == vu1n::RunResult::NotTranslated)
                logUntranslated();
            ++skipped;
        }
        else
        {
            ++verified;
            const std::string diff = vuDiff(nativeState, vu1.state(), nativeData.data(), data, PS2_VU1_DATA_SIZE,
                                            nativeKicks, interpKicks);
            if (!diff.empty() && ++mismatches <= 40)
                std::fprintf(stderr, "[VU1VERIFY] mismatch entry=0x%04x gen=%llu native/interp:%s\n", startPC,
                             static_cast<unsigned long long>(generation), diff.c_str());
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport >= std::chrono::seconds(5))
        {
            lastReport = now;
            std::fprintf(stderr, "[VU1VERIFY] verified=%llu mismatches=%llu skipped=%llu\n",
                         static_cast<unsigned long long>(verified), static_cast<unsigned long long>(mismatches),
                         static_cast<unsigned long long>(skipped));
        }
    }

    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

#if defined(PS2X_HAS_HOST_DISPLAY)
using HostFrameTarget = HostDisplay;
#else
using HostFrameTarget = Texture2D;
#endif

// Set by F12 on the main thread. UploadFrame clears it and has the frame saved: by PCSX2's GS as a
// PNG at its internal resolution, or with raylib from the CPU frame.
std::atomic<bool> g_ps2xScreenshotRequested{false};

// Set by F5 on the main thread; PCSX2's GS steps to the next internal resolution at its next vsync
// and stores the new scale here, which is shown on screen for a moment.
std::atomic<bool> g_ps2xUpscaleCycleRequested{false};
std::atomic<int> g_ps2xUpscaleRate{1};
// Finished-frame presentation. A game override that knows when the game has finished a frame calls
// PS2Runtime::gs().latchHostPresentationFrame() at that moment and stores the vsync tick here (plus
// one, so 0 means never). Some games draw into the buffer that is shown and start the next frame
// before the vertical blank, so the frame is only complete right after it is finished. While such
// frames keep coming, UploadFrame does not latch on its own at each vsync; it does again once none
// has come for kFinishedFrameTimeoutTicks (a loading screen that never finishes a frame, say).
std::atomic<uint64_t> g_ps2xFinishedFrameTick{0};
static constexpr uint64_t kFinishedFrameTimeoutTicks = 30;

// Writes tightly packed RGBA rows as a 24-bit bottom-up BMP.
static bool WriteFrameBmp(const char *path, const uint8_t *rgba, uint32_t width, uint32_t height)
{
    std::FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    const uint32_t rowBytes = (width * 3u + 3u) & ~3u;
    const uint32_t imgBytes = rowBytes * height;
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&hdr](size_t off, uint32_t v)
    { hdr[off] = v & 0xFF; hdr[off + 1] = (v >> 8) & 0xFF; hdr[off + 2] = (v >> 16) & 0xFF; hdr[off + 3] = (v >> 24) & 0xFF; };
    put32(2, 54u + imgBytes);
    put32(10, 54u);
    put32(14, 40u);
    put32(18, width);
    put32(22, height);
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, imgBytes);
    bool ok = std::fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    std::vector<uint8_t> row(rowBytes, 0u);
    for (uint32_t y = 0; ok && y < height; ++y)
    {
        const uint8_t *src = rgba + static_cast<size_t>(height - 1u - y) * width * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            row[x * 3u + 0u] = src[x * 4u + 2u];
            row[x * 3u + 1u] = src[x * 4u + 1u];
            row[x * 3u + 2u] = src[x * 4u + 0u];
        }
        ok = std::fwrite(row.data(), 1, rowBytes, f) == rowBytes;
    }
    return std::fclose(f) == 0 && ok;
}

// F12 saves screenshots/screenshotNNN.png in the working directory. Like raylib's own F12, numbering
// restarts at 000 each run, overwriting older shots.
static std::filesystem::path NextScreenshotPath()
{
    static int s_counter = 0;
    std::error_code ec;
    std::filesystem::create_directories("screenshots", ec);
    char name[32];
    std::snprintf(name, sizeof(name), "screenshot%03d.png", s_counter++);
    return std::filesystem::absolute(std::filesystem::path("screenshots") / name, ec);
}

// Optional headless frame capture: PS2X_DUMP_FRAMES_DIR=<dir>, PS2X_DUMP_FRAMES_EVERY=<vsync ticks,
// default 60>, PS2X_DUMP_FRAMES_AFTER=<seconds since start>, PS2X_DUMP_FRAMES_COUNT=<n>.
struct FrameDumpSettings
{
    const char *dir = std::getenv("PS2X_DUMP_FRAMES_DIR");
    uint64_t every = []()
    {
        const char *e = std::getenv("PS2X_DUMP_FRAMES_EVERY");
        const long long v = e ? std::atoll(e) : 60;
        return static_cast<uint64_t>(v > 0 ? v : 60);
    }();
    double after = []()
    {
        const char *e = std::getenv("PS2X_DUMP_FRAMES_AFTER");
        return e ? std::atof(e) : 0.0;
    }();
    uint64_t left = []()
    {
        const char *e = std::getenv("PS2X_DUMP_FRAMES_COUNT");
        return e ? static_cast<uint64_t>(std::atoll(e)) : ~0ull;
    }();

    bool due(uint64_t frameIndex) const
    {
        return dir && left > 0 && (frameIndex % every) == 0u &&
               std::chrono::duration<double>(std::chrono::steady_clock::now() - g_processStart).count() >= after;
    }
};

static FrameDumpSettings &FrameDumps()
{
    static FrameDumpSettings s_settings;
    return s_settings;
}

static void SaveScreenshot(const uint8_t *rgba, uint32_t width, uint32_t height)
{
    std::error_code ec;
    const std::filesystem::path path = NextScreenshotPath();
    std::vector<uint8_t> opaque(rgba, rgba + static_cast<size_t>(width) * height * 4u);
    for (size_t i = 3; i < opaque.size(); i += 4)
        opaque[i] = 0xFF;
    Image image{};
    image.data = opaque.data();
    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    image.mipmaps = 1;
    image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    if (ExportImage(image, path.string().c_str()))
        std::cerr << "[ps2xRuntime] saved screenshot " << std::filesystem::absolute(path, ec).string() << " ("
                  << width << "x" << height << ")" << std::endl;
    else
        std::cerr << "[ps2xRuntime] could not write screenshot " << path.string() << std::endl;
}

static void UploadFrame(HostFrameTarget &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight)
{
    if (hostinput::IsKeyPressed(KEY_F12))
        g_ps2xScreenshotRequested.store(true, std::memory_order_release);
    if (hostinput::IsKeyPressed(KEY_F5))
        g_ps2xUpscaleCycleRequested.store(true, std::memory_order_release);

    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
#if !defined(PS2X_HAS_HOST_DISPLAY)
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);
#endif

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    const bool needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick;
    const uint64_t finishedFrameTick = g_ps2xFinishedFrameTick.load(std::memory_order_acquire);
    const bool gamePresents = finishedFrameTick != 0u && currentTick + 1u < finishedFrameTick + kFinishedFrameTimeoutTicks;
    if (needsLatch)
    {
        if (!gamePresents)
            rt->gs().latchHostPresentationFrame();
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }
    else if (s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }

    static uint64_t s_hostFrameCounter = 0;
    const uint64_t frameIndex = (currentTick > 0) ? currentTick : s_hostFrameCounter++;
    {
        static const uint64_t s_maxTicks = []()
        {
            const char *m = std::getenv("PS2X_MAX_TICKS");
            return m ? static_cast<uint64_t>(std::atoll(m)) : 0ull;
        }();
        if (s_maxTicks > 0 && frameIndex >= s_maxTicks)
        {
            std::cerr << "[ps2xRuntime] Reached max ticks (" << frameIndex << "), requesting stop." << std::endl;
            rt->requestStop();
        }
    }

#if defined(PS2X_HAS_HOST_DISPLAY)
    // With PCSX2's GS the picture never comes back to the CPU: PCSX2 saves it itself, as a PNG at
    // its internal resolution, when it presents the next frame.
    {
        if (g_ps2xScreenshotRequested.exchange(false, std::memory_order_acq_rel))
        {
            const std::filesystem::path path = NextScreenshotPath();
            ps2xPcsx2QueueSnapshot(path.string().c_str());
            std::cerr << "[ps2xRuntime] saving screenshot " << path.string() << std::endl;
        }
        FrameDumpSettings &dumps = FrameDumps();
        if (dumps.due(frameIndex))
        {
            char path[1024];
            std::snprintf(path, sizeof(path), "%s/frame_%06llu.png", dumps.dir,
                          static_cast<unsigned long long>(frameIndex));
            ps2xPcsx2QueueSnapshot(std::filesystem::absolute(path).string().c_str());
            --dumps.left;
        }
    }
#endif

    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource))
    {
#if !defined(PS2X_HAS_HOST_DISPLAY)
        Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, MAGENTA);
        UpdateTexture(tex, blank.data);
        UnloadImage(blank);
#endif
        // With PCSX2's GS this is the normal path: it draws the frame into the window itself.
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;

#if !defined(PS2X_HAS_HOST_DISPLAY)
    std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
#endif
    {
        FrameDumpSettings &dumps = FrameDumps();
        if (dumps.due(frameIndex) && !s_scratch.empty() && width != 0u && height != 0u &&
            s_scratch.size() >= static_cast<size_t>(width) * height * 4u)
        {
            char path[1024];
            std::snprintf(path, sizeof(path), "%s/frame_%06llu.bmp", dumps.dir,
                          static_cast<unsigned long long>(frameIndex));
            if (WriteFrameBmp(path, s_scratch.data(), width, height))
                --dumps.left;
        }
    }
    if (g_ps2xScreenshotRequested.load(std::memory_order_acquire) && width != 0u && height != 0u &&
        s_scratch.size() >= static_cast<size_t>(width) * height * 4u)
    {
        g_ps2xScreenshotRequested.store(false, std::memory_order_release);
        SaveScreenshot(s_scratch.data(), width, height);
    }
#if !defined(PS2X_HAS_HOST_DISPLAY)
    if (!s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH);
        const uint32_t copyHeight = std::min<uint32_t>(height, FB_HEIGHT);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(FB_WIDTH) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
        }
    }

    UpdateTexture(tex, s_uploadBuffer.data());
#endif
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
}

PS2Runtime::PS2Runtime()
{
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);

    m_eeScheduler = std::make_unique<EeScheduler>(*this);

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_asyncCallbackStackTop = PS2_RAM_SIZE;
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        m_memory.stopVif1Worker(); // it runs VU1 programs on m_vu1 and feeds m_gs
        m_iopSubsystem.reset();
        m_iopHost.reset();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            m_audioBackend.setAudioReady(false);
            CloseAudioDevice();
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

#if defined(PS2X_HAS_HOST_DISPLAY)
        // PCSX2's GS draws into a child of the window, so it has to go first.
        m_gs.setRasterBackend(nullptr);
        HostDisplay::destroy();
#else
        if (IsWindowReady())
        {
            CloseWindow();
        }
#endif

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModule(std::string_view path, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModule(path, arguments, argumentSize);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModuleBuffer(guestAddress, arguments, argumentSize);
}

bool PS2Runtime::stopIopModule(int32_t moduleId, int32_t *result)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->stopModule(moduleId, result);
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

bool PS2Runtime::canBindIopRpc(uint32_t sid) const noexcept
{
    return m_iopSubsystem->canBindRpc(sid);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    auto scope = m_iopHost->enterCall(ctx, rdram);
    request.callToken = scope.token();
    return m_iopSubsystem->handleRpc(request);
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::advanceIopEeCycles(uint64_t eeCycles) noexcept
{
    m_iopSubsystem->runEeCycles(eeCycles);
}

void PS2Runtime::resetIop()
{
    m_iopSubsystem->reset();
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

uint32_t PS2Runtime::allocateIopMemory(uint32_t size, uint32_t alignment)
{
    return m_iopSubsystem ? m_iopSubsystem->allocateMemory(size, alignment) : 0u;
}

bool PS2Runtime::freeIopMemory(uint32_t address)
{
    return m_iopSubsystem && m_iopSubsystem->freeMemory(address);
}

bool PS2Runtime::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->readMemory(address, destination, size);
}

bool PS2Runtime::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->writeMemory(address, source, size);
}

bool PS2Runtime::zeroIopMemory(uint32_t address, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->zeroMemory(address, size);
}

bool PS2Runtime::isIopMemoryRange(uint32_t address, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->isMemoryRange(address, size);
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());

#if defined(PS2X_HAS_HOST_DISPLAY)
    // PCSX2's GS (ps2x-gs.dll) draws the picture into the window. PS2X_GS_BACKEND=cpu or software
    // picks its software renderer.
    m_gs.setRasterBackend(std::make_unique<GSPcsx2Backend>());
#endif
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size, uint32_t path)
                                    { m_gs.processGIFPacket(data, size, path); });
    m_memory.setGifArbiter(&m_gifArbiter);
    // On the VIF1 worker the EE context is off limits: the async gate below only lets transfers
    // through while D/T breaks are disabled, and has already cleared the VPU_STAT stop bits.
    m_memory.setVif1AsyncGate([this]()
                              {
                                  R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                  if (!cpuContext)
                                      cpuContext = &m_cpuContext;
                                  if ((cpuContext->vu0_fbrst & ((1u << 10) | (1u << 11))) != 0u)
                                      return false;
                                  cpuContext->vu0_vpu_stat &= ~0x0600u;
                                  return true; });
    m_memory.setGifPrivilegedEffects([this](const uint8_t *data, uint32_t size)
                                     {
                                         if (!m_gs.handlesRawGif())
                                             return false;
                                         if (data)
                                             m_gs.applyGifPrivilegedEffects(data, size);
                                         return true; });
    m_memory.setGsFinishNotify([this]()
                               {
                                   if (m_eeScheduler)
                                       m_eeScheduler->postEvent(EeEvent{EeEventType::GsInterrupt}); });
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     const bool onWorker = PS2Memory::onVif1Worker();
                                     R5900Context *cpuContext = nullptr;
                                     if (!onWorker)
                                     {
                                         cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                         if (!cpuContext)
                                             cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         cpuContext && (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         cpuContext && (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     VuProfiler &prof = VuProfiler::vu1();
                                     const auto t0 = std::chrono::steady_clock::now();
                                     const uint64_t cycles0 = m_vu1.state().cycles;
                                     runVu1Program(m_vu1, m_memory, m_gs, false, startPC, top, itop);
                                     if (prof.enabled())
                                     {
                                         const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                             std::chrono::steady_clock::now() - t0)
                                                             .count();
                                         prof.record(m_memory.getVU1Code(), m_memory.getVU1CodeGeneration(),
                                                     startPC, m_vu1.state().cycles - cycles0, static_cast<uint64_t>(ns));
                                     }
                                     if (cpuContext)
                                         cpuContext->vu0_vpu_stat =
                                             (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                             (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                             (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    ps2xSetGifCaptureRam(m_memory.getRDRAM(), PS2_RAM_SIZE);
    ps2xInstallWriteWatch(m_memory.getRDRAM(), PS2_RAM_SIZE, m_memory.getScratchpad(), PS2_SCRATCHPAD_SIZE, [this]() -> uint32_t
                          {
                              R5900Context *c = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                              return c ? c->pc : 0u; });
    ps2xSetWriteWatchDescriber([this]() -> std::string
                               {
                                   R5900Context *c = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                   if (!c)
                                       return {};
                                   char buf[400];
                                   std::snprintf(buf, sizeof(buf), "a0=%08x a1=%08x a2=%08x a3=%08x v0=%08x v1=%08x sp=%08x ra=%08x "
                                                                   "s0=%08x s1=%08x s2=%08x s3=%08x s4=%08x s5=%08x s6=%08x s7=%08x",
                                                 GPR_U32(c, 4), GPR_U32(c, 5), GPR_U32(c, 6), GPR_U32(c, 7),
                                                 GPR_U32(c, 2), GPR_U32(c, 3), GPR_U32(c, 29), GPR_U32(c, 31),
                                                 GPR_U32(c, 16), GPR_U32(c, 17), GPR_U32(c, 18), GPR_U32(c, 19),
                                                 GPR_U32(c, 20), GPR_U32(c, 21), GPR_U32(c, 22), GPR_U32(c, 23));
                                   return buf; });
    m_memory.setVu0MicroCallback([this](bool resume, uint32_t startPC, uint32_t itop)
                                 {
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     // VIF0 latches ITOPS into VU0's ITOP when it starts the program.
                                     cpuContext->vu0_itop = itop;
                                     t_vu0FromVif0 = true;
                                     executeVU0Microprogram(m_memory.getRDRAM(), cpuContext,
                                                            resume ? cpuContext->vu0_tpc : startPC);
                                     t_vu0FromVif0 = false; });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 {
                                     R5900Context *cpuContext = nullptr;
                                     if (!PS2Memory::onVif1Worker())
                                     {
                                         cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                         if (!cpuContext)
                                             cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         cpuContext && (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         cpuContext && (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     runVu1Program(m_vu1, m_memory, m_gs, true, 0u, top, itop);
                                     if (cpuContext)
                                         cpuContext->vu0_vpu_stat =
                                             (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                             (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                             (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();
    vu1n::invalidateRegisters(vu1n::Unit::VU1);

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

#if defined(PS2X_HAS_HOST_DISPLAY)
        // Before the core subsystems: PCSX2's GS draws into this window.
        if (!HostDisplay::create(title, HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT))
        {
            std::cerr << "Failed to create the game window" << std::endl;
            return false;
        }
#endif
        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#elif defined(PS2X_HAS_HOST_DISPLAY)
        InitAudioDevice(); // raylib is only the audio backend here; it never opens a window
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
#else
        SetConfigFlags(FLAG_WINDOW_RESIZABLE);
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
        InitAudioDevice();
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
#endif
#if !defined(PS2X_HAS_HOST_DISPLAY)
        SetTargetFPS(60);
        SetExitKey(KEY_NULL); // raylib closes the window on Escape by default; Escape does nothing here
#endif
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);
#if defined(PS2X_HAS_HOST_DISPLAY)
    ps2xSetGameElfName(std::filesystem::path(elfPath).filename().string().c_str());
#endif

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
        m_asyncCallbackStackFloor = std::min(std::max(hardLimit, suggestedHeapBase), PS2_RAM_SIZE);
        m_asyncCallbackStackTop = PS2_RAM_SIZE;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string romError;
    if (!m_romDevice.configure(identity, &romError))
    {
        std::cerr << "[ROM0] failed to configure profile: " << romError << std::endl;
        return false;
    }

    m_iopSubsystem->reset();

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }
    // PS2X_MEMCARD_DIR=<folder>: where memory card 1's saves live, instead of mc0 beside the game.
    if (const char *folder = std::getenv("PS2X_MEMCARD_DIR"); folder && folder[0])
    {
        paths.mcRoot = folder;
    }

    setIoPaths(paths);
}

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) && g_ps2RecompiledFunctionTable[slot] != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);

    // PS2X_CALLPROF=<start seconds>,<window seconds>: count guest calls per call site and target over
    // the window, then print the busiest ones with per-vsync rates.
    struct CallProfile
    {
        double start = -1.0, window = 0.0;
        bool done = false;
        uint64_t firstTick = 0;
        std::unordered_map<uint64_t, uint64_t> counts; // (source << 32) | target
        CallProfile()
        {
            if (const char *e = std::getenv("PS2X_CALLPROF"))
                std::sscanf(e, "%lf,%lf", &start, &window);
        }
    };
    static CallProfile s_prof;
    if (s_prof.start >= 0.0 && !s_prof.done && isCall)
    {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_processStart).count();
        if (t >= s_prof.start)
        {
            const uint64_t tick = m_memory.gs().vsyncTick.load(std::memory_order_relaxed);
            if (s_prof.counts.empty())
                s_prof.firstTick = tick;
            ++s_prof.counts[(static_cast<uint64_t>(sourcePc) << 32) | targetPc];
            // PS2X_CALLPROF_ARGS=<hex target>: also log the arguments of the first 200 calls to it.
            static const uint32_t s_argTarget = []() -> uint32_t
            {
                const char *e = std::getenv("PS2X_CALLPROF_ARGS");
                return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0u;
            }();
            static uint32_t s_argLines = 0;
            if (targetPc == s_argTarget && s_argLines++ < 200)
                std::fprintf(stderr, "[callargs] vs=%llu from 0x%08x a0=%08x a1=%08x a2=%08x a3=%08x\n",
                             static_cast<unsigned long long>(tick), sourcePc, GPR_U32(ctx, 4), GPR_U32(ctx, 5),
                             GPR_U32(ctx, 6), GPR_U32(ctx, 7));
            if (targetPc == s_argTarget && s_argLines <= 40)
            {
                // The 4x4 float matrix a1 points at (RAM only).
                const uint32_t a1 = GPR_U32(ctx, 5) & 0x01FFFFF0u;
                float m[16];
                std::memcpy(m, rdram + a1, sizeof(m));
                for (int r = 0; r < 4; ++r)
                    std::fprintf(stderr, "[callargs]   a1[%d] %g %g %g %g\n", r, m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
            }
            if (t >= s_prof.start + s_prof.window)
            {
                s_prof.done = true;
                const uint64_t frames = std::max<uint64_t>(1u, tick - s_prof.firstTick);
                std::vector<std::pair<uint64_t, uint64_t>> rows(s_prof.counts.begin(), s_prof.counts.end());
                std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b)
                          { return a.second > b.second; });
                std::fprintf(stderr, "[callprof] %llu vsyncs, %zu call sites\n",
                             static_cast<unsigned long long>(frames), rows.size());
                for (const auto &[key, n] : rows)
                    std::fprintf(stderr, "[callprof] 0x%08x -> 0x%08x %llu (%.2f/vsync)\n",
                                 static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key),
                                 static_cast<unsigned long long>(n), static_cast<double>(n) / frames);
            }
        }
    }

    // Account cycles for inter-function transfers without aborting active C++ call frames.
    if (m_eeScheduler)
    {
        m_eeScheduler->accountCycles(EeScheduler::kGuestDispatchCycles);
    }

    if (!isCall)
    {
        if (kind == GuestBranchKind::DirectJump || kind == GuestBranchKind::IndirectJump)
        {
            if (hasFunction(targetPc))
            {
                RecompiledFunction targetFn = lookupFunction(targetPc);
                targetFn(rdram, ctx, this);
                return false;
            }
        }

        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        return false;
    }

    if (!hasFunction(targetPc))
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            // if you need the app to keep open to open debug pannel change this to false
            return false;
        }

        return false;
    }

    RecompiledFunction targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;

    // PS2X_TRACE_FN=<hex target>[,...] (+PS2X_TRACE_FN_AFTER=<seconds>): logs the 64-bit argument
    // registers and f12/f13 on entry and v0/v1/f0 on return for the first 400 calls to those targets.
    struct FnTrace
    {
        std::vector<uint32_t> targets;
        double after = 0.0;
        uint32_t lines = 0;
        uint32_t fromLo = 0u, fromHi = 0xFFFFFFFFu; // PS2X_TRACE_FN_FROM=<hex lo>-<hex hi>: call sites to trace
        FnTrace()
        {
            if (const char *e = std::getenv("PS2X_TRACE_FN"))
                for (const char *p = e; p && *p;)
                {
                    targets.push_back(static_cast<uint32_t>(std::strtoul(p, nullptr, 16)));
                    p = std::strchr(p, ',');
                    if (p)
                        ++p;
                }
            if (const char *a = std::getenv("PS2X_TRACE_FN_AFTER"))
                after = std::atof(a);
            if (const char *r = std::getenv("PS2X_TRACE_FN_FROM"))
                std::sscanf(r, "%x-%x", &fromLo, &fromHi);
        }
    };
    static FnTrace s_trace;
    const bool traced = !s_trace.targets.empty() && s_trace.lines < 400 &&
                        std::find(s_trace.targets.begin(), s_trace.targets.end(), targetPc) != s_trace.targets.end() &&
                        sourcePc >= s_trace.fromLo && sourcePc <= s_trace.fromHi &&
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_processStart).count() >= s_trace.after;
    if (traced)
    {
        ++s_trace.lines;
        std::fprintf(stderr, "[fn] > 0x%08x from 0x%08x a0=%016llx a1=%016llx a2=%016llx a3=%016llx f12=%.9g f13=%.9g\n",
                     targetPc, sourcePc, static_cast<unsigned long long>(GPR_U64(ctx, 4)),
                     static_cast<unsigned long long>(GPR_U64(ctx, 5)), static_cast<unsigned long long>(GPR_U64(ctx, 6)),
                     static_cast<unsigned long long>(GPR_U64(ctx, 7)), ctx->f[12], ctx->f[13]);
    }
    targetFn(rdram, ctx, this);
    if (traced)
        std::fprintf(stderr, "[fn] < 0x%08x v0=%016llx v1=%016llx f0=%.9g\n", targetPc,
                     static_cast<unsigned long long>(GPR_U64(ctx, 2)), static_cast<unsigned long long>(GPR_U64(ctx, 3)),
                     ctx->f[0]);

    if (isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }

    if (ctx->pc == entryPc)
    {
        ctx->pc = fallthroughPc;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }
    Vu0Snapshot &snap = Vu0Snapshot::instance();
    const bool snapshot = snap.armed();
    if (snapshot)
        snap.save(false, startPC, t_vu0FromVif0, ctx, vu0Data, m_memory.getRDRAM());
    struct SnapshotPost
    {
        Vu0Snapshot &snap;
        bool active;
        uint32_t pc;
        R5900Context *ctx;
        const uint8_t *data;
        const uint8_t *ram;
        ~SnapshotPost()
        {
            if (active)
                snap.save(true, pc, t_vu0FromVif0, ctx, data, ram);
        }
    } snapshotPost{snap, snapshot, startPC, ctx, vu0Data, m_memory.getRDRAM()};

    VuProfiler &prof = VuProfiler::vu0();
    const auto t0 = std::chrono::steady_clock::now();
    constexpr uint32_t kVu0Budget = 4096;
    const uint64_t generation = m_memory.getVU0CodeGeneration();
    // No m_vu0.reset(): copyVu0ContextToState rebuilds the whole state, and the interpreter paths
    // reset their own pipelines (execute() / resetPipelines()), so the translated path skips that work.
    copyVu0ContextToState(ctx, m_vu0.state());
    auto interpret = [&]
    {
        m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE, vu0Data, PS2_VU0_DATA_SIZE, m_gs, &m_memory,
                      startPC, 0u, ctx->vu0_itop, kVu0Budget);
    };
    auto logUntranslated = [&]
    {
        static std::unordered_map<uint64_t, bool> seen;
        if (seen.emplace((generation << 16) | startPC, true).second && seen.size() <= 64)
            std::fprintf(stderr, "[VU0N] no translation for entry pc=0x%04x (code generation %llu); interpreting\n",
                         startPC, static_cast<unsigned long long>(generation));
    };

    const VuMode mode = vu0Mode();
    if (mode == VuMode::Interpreter)
    {
        interpret();
    }
    else if (mode == VuMode::Native)
    {
        VU1State &state = m_vu0.state();
        const vu1n::RunResult r = vu1n::run(vu1n::Unit::VU0, state, vu0Code, generation, vu0Data, startPC, 0u,
                                            ctx->vu0_itop, kVu0Budget, nullptr, nullptr);
        if (r == vu1n::RunResult::NotTranslated)
        {
            logUntranslated();
            interpret();
        }
        else if (r == vu1n::RunResult::Bailed)
        {
            const uint32_t used = static_cast<uint32_t>(std::min<uint64_t>(state.cycles, kVu0Budget));
            m_vu0.resetPipelines();
            m_vu0.resume(vu0Code, PS2_VU0_CODE_SIZE, vu0Data, PS2_VU0_DATA_SIZE, m_gs, &m_memory, 0u,
                         ctx->vu0_itop, kVu0Budget - used);
        }
    }
    else
    {
        static uint64_t verified = 0, mismatches = 0, skipped = 0;
        static auto lastReport = std::chrono::steady_clock::now();
        VU1State nativeState = m_vu0.state();
        std::vector<uint8_t> nativeData(vu0Data, vu0Data + PS2_VU0_DATA_SIZE);
        const vu1n::RunResult r = vu1n::run(vu1n::Unit::VU0, nativeState, vu0Code, generation, nativeData.data(),
                                            startPC, 0u, ctx->vu0_itop, kVu0Budget, nullptr, nullptr);
        interpret();
        if (r != vu1n::RunResult::Done)
        {
            if (r == vu1n::RunResult::NotTranslated)
                logUntranslated();
            ++skipped;
        }
        else
        {
            ++verified;
            const Vu1KickCapture noKicks;
            const std::string diff = vuDiff(nativeState, m_vu0.state(), nativeData.data(), vu0Data, PS2_VU0_DATA_SIZE,
                                            noKicks, noKicks);
            if (!diff.empty() && ++mismatches <= 40)
                std::fprintf(stderr, "[VU0VERIFY] mismatch entry=0x%04x gen=%llu native/interp:%s\n", startPC,
                             static_cast<unsigned long long>(generation), diff.c_str());
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport >= std::chrono::seconds(5))
        {
            lastReport = now;
            std::fprintf(stderr, "[VU0VERIFY] verified=%llu mismatches=%llu skipped=%llu\n",
                         static_cast<unsigned long long>(verified), static_cast<unsigned long long>(mismatches),
                         static_cast<unsigned long long>(skipped));
        }
    }
    copyVu0StateToContext(m_vu0.state(), ctx);
    if (prof.enabled())
    {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        prof.record(vu0Code, m_memory.getVU0CodeGeneration(), startPC, m_vu0.state().cycles,
                    static_cast<uint64_t>(ns));
    }
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::raisePendingGsInterrupts()
{
    // GS FINISH sets CSR bit 1; when IMR.FINISHMSK (bit 9) is clear the GS asserts
    // INTC cause 0 (INTC_GS). Games that chain display lists from that handler
    // (e.g. Drakengard's render device) stall without it.
    constexpr uint64_t kCsrFinish = 0x2ull;
    constexpr uint64_t kImrFinishMask = 0x200ull;
    GSRegisters &gs = m_memory.gs();
    if ((gs.csr.load() & kCsrFinish) == 0ull)
    {
        m_gsFinishIrqRaised = false;
        return;
    }
    if (m_gsFinishIrqRaised || (gs.imr & kImrFinishMask) != 0ull)
    {
        return;
    }
    m_gsFinishIrqRaised = true;
    eeScheduler().dispatchIrq(false, 0u);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
    raisePendingGsInterrupts();
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    if (m_eeScheduler)
    {
        m_eeScheduler->accountCycles(cycles);
    }
    return false;
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             });
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    resetIop();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

#if defined(PS2X_HAS_HOST_DISPLAY)
    HostDisplay &display = *HostDisplay::get();
    uint64_t lastPresentedTick = std::numeric_limits<uint64_t>::max();
#else
    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);
#endif

    std::atomic<bool> gameThreadFinished{false};

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        try
        {
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    uint64_t tick = 0;
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);

            }
        });
#if defined(PS2X_HAS_HOST_DISPLAY)
        // Each guest VSync is one frame. The timeout keeps the window and debug panel alive if
        // the game stops producing them.
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(16);
            while (eeScheduler().currentVSyncTick() == lastPresentedTick &&
                   std::chrono::steady_clock::now() < deadline &&
                   !isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            lastPresentedTick = eeScheduler().currentVSyncTick();
        }

        display.pollEvents();
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        UploadFrame(display, this, presentWidth, presentHeight);

        {
            // Guest VSyncs per second (60 = full speed NTSC), averaged over half-second windows.
            static double s_rateStart = hostinput::GetTime();
            static uint64_t s_rateStartTick = m_memory.gs().vsyncTick.load(std::memory_order_acquire);
            static double s_vsyncsPerSecond = 0.0;
            const double now = hostinput::GetTime();
            const uint64_t tickNow = m_memory.gs().vsyncTick.load(std::memory_order_acquire);
            if (now - s_rateStart >= 0.5)
            {
                s_vsyncsPerSecond = static_cast<double>(tickNow - s_rateStartTick) / (now - s_rateStart);
                s_rateStart = now;
                s_rateStartTick = tickNow;
            }
            char text[32];
            std::snprintf(text, sizeof(text), "VSync/s: %.1f", s_vsyncsPerSecond);
            const uint32_t rateColor = s_vsyncsPerSecond >= 59.0 ? IM_COL32(0, 228, 48, 255) : IM_COL32(253, 249, 0, 255);

            // After F5 steps PCSX2's internal resolution, show the new one for two seconds.
            static int s_shownRate = g_ps2xUpscaleRate.load(std::memory_order_relaxed);
            static double s_rateShownUntil = 0.0;
            const int rate = g_ps2xUpscaleRate.load(std::memory_order_relaxed);
            if (rate != s_shownRate)
            {
                s_shownRate = rate;
                s_rateShownUntil = now + 2.0;
            }
            char notice[40] = "";
            if (now < s_rateShownUntil)
                std::snprintf(notice, sizeof(notice), "Internal resolution: %dx", rate);
            // PCSX2 draws the picture, so these lines go to its overlay.
            ps2xPcsx2SetOverlay(0u, text, rateColor);
            ps2xPcsx2SetOverlay(1u, notice, IM_COL32(255, 255, 255, 255));
        }
        // The debug panel (F1) is not drawn here: PCSX2 owns the picture and its ImGui lives in the
        // DLL, so the panel has nothing to draw into.

        if (display.shouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
#else
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        UploadFrame(frameTex, this, presentWidth, presentHeight);

        BeginDrawing();
        ClearBackground(BLACK);
        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        const float scale = std::min(screenWidth / srcWidth, screenHeight / srcHeight);
        const float dstWidth = srcWidth * scale;
        const float dstHeight = srcHeight * scale;
        const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
        const Rectangle dstRect{
            (screenWidth - dstWidth) * 0.5f,
            (screenHeight - dstHeight) * 0.5f,
            dstWidth,
            dstHeight};
        DrawTexturePro(frameTex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        {
            // Guest VSyncs per second (60 = full speed NTSC), averaged over half-second windows.
            static double s_rateStart = GetTime();
            static uint64_t s_rateStartTick = m_memory.gs().vsyncTick.load(std::memory_order_acquire);
            static double s_vsyncsPerSecond = 0.0;
            const double now = GetTime();
            const uint64_t tickNow = m_memory.gs().vsyncTick.load(std::memory_order_acquire);
            if (now - s_rateStart >= 0.5)
            {
                s_vsyncsPerSecond = static_cast<double>(tickNow - s_rateStartTick) / (now - s_rateStart);
                s_rateStart = now;
                s_rateStartTick = tickNow;
            }
            const char *text = TextFormat("VSync/s: %.1f", s_vsyncsPerSecond);
            DrawText(text, 9, 9, 20, BLACK);
            DrawText(text, 8, 8, 20, s_vsyncsPerSecond >= 59.0 ? GREEN : YELLOW);
        }
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        EndDrawing();

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
#endif
    }

    requestStop();
    if (gameThread.joinable())
    {
        gameThread.join();
    }

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
#if defined(PS2X_HAS_HOST_DISPLAY)
    display.hide(); // closed by ~PS2Runtime, after the GS backend that draws into it
#else
    UnloadTexture(frameTex);
    CloseWindow();
#endif

    m_audioBackend.setAudioReady(false); // main leaves through _Exit, so ~PS2Runtime may not run

    RUNTIME_LOG("[run] exiting loop");
}
