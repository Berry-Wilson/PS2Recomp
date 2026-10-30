// PS2X_WATCH=<guest hex addr>:<hex length>[,...]: logs every distinct guest PC that writes to the
// given EE RAM or scratchpad (0x70000000+) ranges, from PS2X_WATCH_AFTER seconds on (default 0).
// PS2X_WATCH_VALUES=1 logs every write (up to 6000) with the quadword it left behind instead. The host pages backing them are made read-only; a write faults, the guest
// PC is recorded, and the store is single-stepped with the page writable before re-protecting it.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace
{
    struct Range
    {
        uint32_t guestStart;
        uint32_t guestEnd;
        uintptr_t hostStart; // host address of guestStart
    };

    std::chrono::steady_clock::time_point g_logFrom;
    const std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();
    std::function<uint32_t()> g_guestPc;
    std::function<std::string()> g_describe; // extra guest state for PS2X_WATCH_VALUES lines
    thread_local std::string t_valueDesc;
    std::vector<Range> g_ranges;
    std::vector<std::pair<uintptr_t, uintptr_t>> g_pages; // host page spans to protect
    std::mutex g_seenMutex;
    std::set<std::pair<uint32_t, uint32_t>> g_seen; // (guest pc, guest address / 16)
    thread_local void *t_reprotect = nullptr;
    bool g_values = false;
    bool g_badFloatOnly = false; // PS2X_WATCH_VALUES=inf: only log stores that leave an Inf/NaN word
    uint32_t g_minExponent = 0xFF; // PS2X_WATCH_VALUES=big: also log floats with |x| >= 2^20
    float g_jump = 0.0f; // PS2X_WATCH_VALUES=jump[:delta]: log stores that move a float word by more than delta
    uint32_t g_valueLines = 0;
    thread_local uintptr_t t_valueHost = 0;
    thread_local uint32_t t_valueGuest = 0;
    thread_local uint32_t t_valuePc = 0;
    thread_local std::string t_hostStack;
    thread_local uint32_t t_before[4];

    // Symbolized host call stack of the faulting store (PS2X_WATCH_VALUES modes), so writes made by
    // runtime code (DMA, VU, syscalls) rather than recompiled guest code can be told apart.
    std::string hostStack(const CONTEXT *ctx)
    {
        static bool init = false;
        HANDLE proc = GetCurrentProcess();
        if (!init)
        {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(proc, nullptr, TRUE);
            init = true;
        }
        CONTEXT c = *ctx;
        STACKFRAME64 f{};
        f.AddrPC.Offset = c.Rip;
        f.AddrPC.Mode = AddrModeFlat;
        f.AddrFrame.Offset = c.Rbp;
        f.AddrFrame.Mode = AddrModeFlat;
        f.AddrStack.Offset = c.Rsp;
        f.AddrStack.Mode = AddrModeFlat;
        std::string out;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        for (int i = 0; i < 8; ++i)
        {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &f, &c, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || f.AddrPC.Offset == 0)
                break;
            auto *si = reinterpret_cast<SYMBOL_INFO *>(buf);
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            DWORD64 disp = 0;
            out += i ? " < " : "";
            out += SymFromAddr(proc, f.AddrPC.Offset, &disp, si) ? si->Name : "?";
        }
        return out;
    }

    void protectPage(void *page, DWORD prot)
    {
        DWORD old = 0;
        VirtualProtect(page, 0x1000, prot, &old);
    }

    LONG CALLBACK watchHandler(EXCEPTION_POINTERS *info)
    {
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_SINGLE_STEP && t_reprotect)
        {
            if (t_valueHost)
            {
                const uint32_t *q = reinterpret_cast<const uint32_t *>(t_valueHost & ~static_cast<uintptr_t>(0xF));
                // A wide store may change several words: log when any word of the quadword changed to a
                // filtered value.
                bool hit = false;
                for (int w = 0; w < 4; ++w)
                {
                    hit |= q[w] != t_before[w] && ((q[w] >> 23) & 0xFFu) >= g_minExponent;
                    if (g_jump > 0.0f && q[w] != t_before[w])
                    {
                        float now, was;
                        std::memcpy(&now, &q[w], 4);
                        std::memcpy(&was, &t_before[w], 4);
                        hit |= !(std::fabs(now - was) <= g_jump);
                    }
                }
                if (g_badFloatOnly && !hit)
                {
                    t_valueHost = 0;
                    protectPage(t_reprotect, PAGE_READONLY);
                    t_reprotect = nullptr;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                ++g_valueLines;
                std::fprintf(stderr, "[watchv] t=%.2f pc=0x%08x addr=0x%08x qw=%08x %08x %08x %08x\n",
                             std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count(),
                             t_valuePc, t_valueGuest & ~0xFu, q[0], q[1], q[2], q[3]);
                if (g_jump > 0.0f)
                {
                    float f[8];
                    std::memcpy(f, t_before, 16);
                    std::memcpy(f + 4, q, 16);
                    std::fprintf(stderr, "[watchv]   (%g %g %g %g) -> (%g %g %g %g)\n", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
                }
                if (!t_valueDesc.empty())
                    std::fprintf(stderr, "[watchv]   %s\n", t_valueDesc.c_str());
                if (!t_hostStack.empty())
                    std::fprintf(stderr, "[watchv]   host: %s\n", t_hostStack.c_str());
                t_valueHost = 0;
            }
            protectPage(t_reprotect, PAGE_READONLY);
            t_reprotect = nullptr;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (code != EXCEPTION_ACCESS_VIOLATION || info->ExceptionRecord->ExceptionInformation[0] != 1)
            return EXCEPTION_CONTINUE_SEARCH;

        const uintptr_t fault = static_cast<uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
        bool ours = false;
        for (const auto &[lo, hi] : g_pages)
            ours |= fault >= lo && fault < hi;
        if (!ours)
            return EXCEPTION_CONTINUE_SEARCH;

        for (const Range &r : g_ranges)
        {
            const uint32_t guestAddr = r.guestStart + static_cast<uint32_t>(fault - r.hostStart);
            if (fault >= r.hostStart && guestAddr < r.guestEnd && std::chrono::steady_clock::now() >= g_logFrom)
            {
                const uint32_t pc = g_guestPc ? g_guestPc() : 0u;
                std::lock_guard<std::mutex> lock(g_seenMutex);
                if (g_values)
                {
                    if (g_valueLines <= 6000)
                    {
                        t_valueHost = fault;
                        std::memcpy(t_before, reinterpret_cast<const void *>(fault & ~static_cast<uintptr_t>(0xF)), 16);
                        t_valueGuest = guestAddr;
                        t_valuePc = pc;
                        t_valueDesc = g_describe ? g_describe() : std::string();
                        t_hostStack = g_valueLines < 400 ? hostStack(info->ContextRecord) : std::string();
                    }
                }
                else if (g_seen.emplace(pc, guestAddr >> 4).second && g_seen.size() <= 4000)
                    std::fprintf(stderr, "[watch] guest pc=0x%08x wrote 0x%08x (host rip=%p)\n", pc, guestAddr,
                                 reinterpret_cast<void *>(info->ContextRecord->Rip));
                break;
            }
        }

        void *page = reinterpret_cast<void *>(fault & ~static_cast<uintptr_t>(0xFFF));
        protectPage(page, PAGE_READWRITE);
        t_reprotect = page;
        info->ContextRecord->EFlags |= 0x100; // trap after the store
        return EXCEPTION_CONTINUE_EXECUTION;
    }
}

void ps2xSetWriteWatchDescriber(std::function<std::string()> describe)
{
    g_describe = std::move(describe);
}

// PS2X_PEEK=<guest hex addr>:<hex length> prints those EE RAM quadwords as floats every PS2X_PEEK_MS
// milliseconds (default 100) from PS2X_WATCH_AFTER seconds on: a sampled view of values the game
// rewrites in place several times per frame.
static void startPeek(uint8_t *ram, uint32_t ramSize)
{
    const char *spec = std::getenv("PS2X_PEEK");
    if (!spec || !*spec || !ram)
        return;
    const char *colon = std::strchr(spec, ':');
    const uint32_t start = static_cast<uint32_t>(std::strtoul(spec, nullptr, 16)) & 0x01FFFFF0u;
    const uint32_t len = colon ? static_cast<uint32_t>(std::strtoul(colon + 1, nullptr, 16)) : 0x10u;
    if (start >= ramSize)
        return;
    const uint32_t end = std::min(ramSize, start + len);
    const char *ms = std::getenv("PS2X_PEEK_MS");
    const auto period = std::chrono::milliseconds(ms ? std::atoi(ms) : 100);
    const char *after = std::getenv("PS2X_WATCH_AFTER");
    const auto from = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(static_cast<int64_t>((after ? std::atof(after) : 0.0) * 1000.0));
    std::thread([=]()
                {
                    std::this_thread::sleep_until(from);
                    for (;;)
                    {
                        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
                        for (uint32_t a = start; a < end; a += 16)
                        {
                            float f[4];
                            std::memcpy(f, ram + a, 16);
                            std::fprintf(stderr, "[peek] t=%.2f 0x%08x %g %g %g %g\n", t, a, f[0], f[1], f[2], f[3]);
                        }
                        std::this_thread::sleep_for(period);
                    } })
        .detach();
}

void ps2xInstallWriteWatch(uint8_t *ram, uint32_t ramSize, uint8_t *scratchpad, uint32_t scratchpadSize,
                           std::function<uint32_t()> guestPc)
{
    startPeek(ram, ramSize);
    const char *spec = std::getenv("PS2X_WATCH");
    if (!spec || !*spec || !ram)
        return;
    std::string s = spec;
    size_t at = 0;
    while (at < s.size())
    {
        const size_t comma = s.find(',', at);
        const std::string item = s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        at = comma == std::string::npos ? s.size() : comma + 1;
        const size_t colon = item.find(':');
        if (colon == std::string::npos)
            continue;
        const uint32_t start = static_cast<uint32_t>(std::strtoul(item.substr(0, colon).c_str(), nullptr, 16));
        const uint32_t len = static_cast<uint32_t>(std::strtoul(item.substr(colon + 1).c_str(), nullptr, 16));
        if (len == 0)
            continue;
        if (start >= 0x70000000u && start < 0x70000000u + scratchpadSize && scratchpad)
        {
            const uint32_t off = start - 0x70000000u;
            g_ranges.push_back({start, start + std::min(len, scratchpadSize - off), reinterpret_cast<uintptr_t>(scratchpad) + off});
        }
        else if ((start & 0x01FFFFFFu) < ramSize)
        {
            const uint32_t off = start & 0x01FFFFFFu;
            g_ranges.push_back({off, off + std::min(len, ramSize - off), reinterpret_cast<uintptr_t>(ram) + off});
        }
    }
    const char *values = std::getenv("PS2X_WATCH_VALUES");
    g_values = values && (values[0] == '1' || values[0] == 'i' || values[0] == 'b' || values[0] == 'j');
    g_badFloatOnly = values && (values[0] == 'i' || values[0] == 'b' || values[0] == 'j');
    if (values && values[0] == 'b')
        g_minExponent = 127 + 20;
    if (values && values[0] == 'j')
    {
        const char *delta = std::strchr(values, ':');
        g_jump = delta ? static_cast<float>(std::atof(delta + 1)) : 100.0f;
    }
    const char *after = std::getenv("PS2X_WATCH_AFTER");
    g_logFrom = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(static_cast<int64_t>((after ? std::atof(after) : 0.0) * 1000.0));
    if (g_ranges.empty())
        return;

    g_guestPc = std::move(guestPc);
    AddVectoredExceptionHandler(1, watchHandler);
    for (const Range &r : g_ranges)
    {
        const uintptr_t lo = r.hostStart & ~static_cast<uintptr_t>(0xFFF);
        const uintptr_t hi = (r.hostStart + (r.guestEnd - r.guestStart) + 0xFFF) & ~static_cast<uintptr_t>(0xFFF);
        g_pages.emplace_back(lo, hi);
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void *>(lo), hi - lo, PAGE_READONLY, &old);
        std::fprintf(stderr, "[watch] watching guest 0x%08x-0x%08x\n", r.guestStart, r.guestEnd);
    }
}
#else
void ps2xInstallWriteWatch(uint8_t *, uint32_t, uint8_t *, uint32_t, std::function<uint32_t()>) {}
void ps2xSetWriteWatchDescriber(std::function<std::string()>) {}
#endif
