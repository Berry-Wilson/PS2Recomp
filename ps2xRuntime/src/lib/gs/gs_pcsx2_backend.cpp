#include "runtime/gs/gs_pcsx2_backend.h"
#include "runtime/ps2_memory.h"

#include "../../../pcsx2-gs/ps2x_gs_api.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
// windows.h clashes with this runtime's headers (and raylib's), so only what is used is declared.
extern "C" __declspec(dllimport) void *__stdcall LoadLibraryExW(const wchar_t *, void *, unsigned long);
extern "C" __declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void *, wchar_t *, unsigned long);
extern "C" __declspec(dllimport) unsigned long __stdcall GetLastError();
extern "C" __declspec(dllimport) int __stdcall SetConsoleCtrlHandler(int(__stdcall *)(unsigned long), int);
constexpr unsigned long kLoadWithAlteredSearchPath = 0x00000008;
#endif

namespace
{
    std::atomic<Ps2xGsSetOverlayFn> g_setOverlay{nullptr};
    std::atomic<Ps2xGsSnapshotFn> g_snapshot{nullptr};
    std::atomic<bool> g_active{false};
    std::atomic<Ps2xGsCloseFn> g_close{nullptr};

    // A process ended from outside (console close, Ctrl+C) runs exit() without shutting the
    // runtime down; PCSX2's GS must be closed before the DLL's own statics are destroyed.
    void closeAtExit()
    {
        if (Ps2xGsCloseFn fn = g_close.exchange(nullptr, std::memory_order_acq_rel))
            fn();
    }

    // "SLUS_202.67" -> "SLUS-20267", the form PCSX2's GameIndex.yaml uses.
    std::string serialFromElfName(const std::string &path)
    {
        const std::string name = std::filesystem::path(path).filename().string();
        std::string serial;
        for (char c : name)
        {
            if (c == '_')
                serial.push_back('-');
            else if (c == ';')
                break;
            else if (c != '.')
                serial.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
        return serial.size() == 10 && serial[4] == '-' ? serial : std::string();
    }

    float envFloat(const char *name, float fallback)
    {
        const char *value = std::getenv(name);
        return value && value[0] ? static_cast<float>(std::atof(value)) : fallback;
    }

    int envRenderer()
    {
        const char *value = std::getenv("PS2X_PCSX2_RENDERER");
        if (!value || !value[0])
            return PS2XGS_RENDERER_AUTO;
        const std::string v(value);
        if (v == "dx11" || v == "d3d11")
            return PS2XGS_RENDERER_DX11;
        if (v == "dx12" || v == "d3d12")
            return PS2XGS_RENDERER_DX12;
        if (v == "vulkan" || v == "vk")
            return PS2XGS_RENDERER_VULKAN;
        if (v == "opengl" || v == "gl")
            return PS2XGS_RENDERER_OPENGL;
        if (v == "software" || v == "sw")
            return PS2XGS_RENDERER_SOFTWARE;
        std::cerr << "[pcsx2-gs] ignoring PS2X_PCSX2_RENDERER=" << value << " (dx11, dx12, vulkan, opengl or software)" << std::endl;
        return PS2XGS_RENDERER_AUTO;
    }

    // Privileged register offsets from 0x12000000, as ps2xgs_regs lays them out.
    constexpr size_t kPmode = 0x00, kSmode1 = 0x10, kSmode2 = 0x20, kDispfb1 = 0x70, kDisplay1 = 0x80, kDispfb2 = 0x90,
                     kDisplay2 = 0xA0, kBgcolor = 0xE0;
    // NTSC, interlaced, as the BIOS's SetGsCrt programs it: PCSX2 tells the video mode from SMODE1,
    // which this runtime does not set.
    constexpr uint64_t kSmode1Ntsc = 0x740834504ull;

    constexpr uint8_t kRegSignal = 0x60, kRegFinish = 0x61, kRegLabel = 0x62;

    uint64_t word(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }

    class Pcsx2Backend final : public GSRasterBackend
    {
    public:
        explicit Pcsx2Backend(GSRegisters &registers) : m_priv(registers) {}

        ~Pcsx2Backend() override
        {
            g_active.store(false, std::memory_order_release);
            g_setOverlay.store(nullptr, std::memory_order_release);
            g_snapshot.store(nullptr, std::memory_order_release);
            closeAtExit();
        }

        bool start(void *parentWindow, const char *gameElfName)
        {
            if (!load())
                return false;
            std::string serial;
            if (const char *value = std::getenv("PS2X_GAME_SERIAL"); value && value[0])
                serial = value;
            else if (gameElfName)
                serial = serialFromElfName(gameElfName);
            const float upscale = envFloat("PS2X_PCSX2_UPSCALE", 2.0f);
            // PCSX2's resources (shaders, fonts, GameIndex.yaml) and its cache and logs live next to the DLL.
            std::string resources = (m_dllDir / "resources").string();
            if (const char *value = std::getenv("PS2X_PCSX2_RESOURCES"); value && value[0])
                resources = value;
            const std::string dataDir = (m_dllDir / "gs-data").string();

            Ps2xGsConfig config{};
            config.size = sizeof(config);
            config.parent_window = parentWindow;
            config.renderer = envRenderer();
            config.upscale = upscale;
            config.widescreen = 0;
            config.sharpness = static_cast<int>(envFloat("PS2X_PCSX2_SHARPNESS", 0.0f));
            config.vsync = 0; // the runtime paces frames itself
            config.osd = envFloat("PS2X_PCSX2_OSD", 0.0f) != 0.0f ? 1 : 0;
            config.data_dir = dataDir.c_str();
            config.resources_dir = resources.c_str();
            config.serial = serial.empty() ? nullptr : serial.c_str();
            if (!m_openFn(&config))
            {
                std::cerr << "[pcsx2-gs] PCSX2's GS did not start" << std::endl;
                return false;
            }
            m_open = true;
            std::cerr << "[pcsx2-gs] PCSX2 GS open, internal resolution " << upscale << "x"
                      << (serial.empty() ? std::string() : ", game " + serial) << std::endl;
            g_snapshot.store(m_snapshot, std::memory_order_release);
            g_close.store(m_close, std::memory_order_release);
            static const bool s_atExitRegistered = []
            {
                std::atexit(closeAtExit);
#if defined(_WIN32)
                // Ctrl+C or closing the console window ends the process with ExitProcess, which skips
                // atexit: close PCSX2's GS first, then let the default handler exit.
                SetConsoleCtrlHandler([](unsigned long) -> int
                {
                    closeAtExit();
                    return 0;
                }, 1);
#endif
                return true;
            }();
            (void)s_atExitRegistered;
            g_active.store(true, std::memory_order_release);
            return true;
        }

        void Initialize(uint8_t *, uint32_t) override {}
        void Reset() override {}

        bool UsesRawCommands() const override { return true; }

        void ProcessGIF(uint32_t path, const uint8_t *data, uint32_t size) override
        {
            if (!data || size < 16u)
                return;
            path = (path >= 1u && path <= 3u) ? path : 3u;
            size &= ~15u;
            applyPrivilegedEffects(path, data, size);
            m_gif(path, data, size);
        }

        // A register write outside any GIF packet becomes a one-register A+D packet on PATH2, so it
        // cannot land in the middle of a PATH3 image transfer.
        void WriteRegisterRaw(uint8_t addr, uint64_t value) override
        {
            if (addr >= 128u)
                return;
            // Compatibility aliases used by native stubs, not GIF registers.
            switch (addr)
            {
            case 0x59: m_priv.dispfb1 = value; return;
            case 0x5a: m_priv.display1 = value; return;
            case 0x5b: m_priv.dispfb2 = value; return;
            case 0x5c: m_priv.display2 = value; return;
            case 0x5f: m_priv.bgcolor = value; return;
            default: break;
            }
            applyRegister(addr, value);
            uint64_t packet[4];
            packet[0] = 1ull | (1ull << 15) | (1ull << 60); // NLOOP 1, EOP, PACKED, NREG 1
            packet[1] = 0xEull;                             // A+D
            packet[2] = value;
            packet[3] = addr;
            m_gif(2u, packet, sizeof(packet));
        }

        void Submit(const GSPrimitiveBatch &) override {}
        void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override {}
        void BeginTransfer(const GSTransferCommand &) override {}

        // Image data written through HWREG after the transfer registers were set the same way.
        void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
        {
            if (!data || !sizeBytes)
                return;
            const uint32_t qwords = (sizeBytes + 15u) / 16u;
            std::vector<uint8_t> packet(16u + qwords * 16u, 0u);
            const uint64_t tag = static_cast<uint64_t>(qwords & 0x7FFFu) | (1ull << 15) | (2ull << 58); // IMAGE, EOP
            std::memcpy(packet.data(), &tag, sizeof(tag));
            std::memcpy(packet.data() + 16, data, sizeBytes);
            m_gif(2u, packet.data(), static_cast<unsigned>(packet.size()));
        }

        void Flush() override {}
        void TextureFlush() override {}
        void Sync(GSSyncReason) override {}

        PresentationFrame Present(const GSPresentationRequest &request) override
        {
            uint8_t *regs = static_cast<uint8_t *>(m_regs());
            auto put = [regs](size_t offset, uint64_t value) { std::memcpy(regs + offset, &value, sizeof(value)); };
            put(kPmode, request.pmode);
            put(kSmode1, kSmode1Ntsc);
            put(kSmode2, request.smode2);
            put(kDispfb1, request.dispfb1);
            put(kDisplay1, request.display1);
            put(kDispfb2, request.dispfb2);
            put(kDisplay2, request.display2);
            put(kBgcolor, request.bgcolor);
            m_vsync(m_field);
            m_field ^= 1u;

            PresentationFrame frame{};
            frame.presentedToDisplay = true;
            frame.width = 640u;
            frame.height = 448u;
            return frame;
        }

        bool ClearFramebuffer(const GSContext &, uint32_t) override { return false; }
        // PCSX2's GS has no readback path through the DLL; local-to-host transfers read zeros.
        uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override { return 0u; }
        uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override { return 0u; }
        void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void SnapshotVram(std::vector<uint8_t> &out) const override { out.clear(); }
        GSTransferSnapshot GetTransferSnapshot() const override { return {}; }

    private:
        bool load()
        {
#if defined(_WIN32)
            std::filesystem::path dll;
            if (const char *value = std::getenv("PS2X_PCSX2_GS_DLL"); value && value[0])
                dll = value;
            else
            {
                wchar_t exe[1024] = {};
                GetModuleFileNameW(nullptr, exe, 1024);
                const std::filesystem::path dir = std::filesystem::path(exe).parent_path();
                dll = dir / "pcsx2-gs" / "ps2x-gs.dll";
            }
            // Its own dependencies (SDL3, FreeType, ...) sit next to it.
            dll = std::filesystem::absolute(dll);
            m_dllDir = dll.parent_path();
            m_module = LoadLibraryExW(dll.wstring().c_str(), nullptr, kLoadWithAlteredSearchPath);
            if (!m_module)
            {
                std::cerr << "[pcsx2-gs] cannot load " << dll.string() << " (error " << GetLastError() << ")" << std::endl;
                return false;
            }
            auto get = [this](const char *name) { return GetProcAddress(m_module, name); };
            m_openFn = reinterpret_cast<Ps2xGsOpenFn>(get("ps2xgs_open"));
            m_gif = reinterpret_cast<Ps2xGsGifFn>(get("ps2xgs_gif"));
            m_regs = reinterpret_cast<Ps2xGsRegsFn>(get("ps2xgs_regs"));
            m_vsync = reinterpret_cast<Ps2xGsVsyncFn>(get("ps2xgs_vsync"));
            m_snapshot = reinterpret_cast<Ps2xGsSnapshotFn>(get("ps2xgs_snapshot"));
            m_close = reinterpret_cast<Ps2xGsCloseFn>(get("ps2xgs_close"));
            if (!m_openFn || !m_gif || !m_regs || !m_vsync || !m_snapshot || !m_close)
            {
                std::cerr << "[pcsx2-gs] " << dll.string() << " is missing functions" << std::endl;
                return false;
            }
            g_setOverlay.store(reinterpret_cast<Ps2xGsSetOverlayFn>(get("ps2xgs_set_overlay")), std::memory_order_release);
            std::cerr << "[pcsx2-gs] loaded " << dll.string() << std::endl;
            return true;
#else
            std::cerr << "[pcsx2-gs] only available on Windows" << std::endl;
            return false;
#endif
        }

        // SIGNAL, FINISH and LABEL as the GS would apply them (the same as the CPU frontend does).
        void applyRegister(uint8_t addr, uint64_t value)
        {
            const uint32_t id = static_cast<uint32_t>(value);
            const uint32_t mask = static_cast<uint32_t>(value >> 32);
            if (addr == kRegSignal)
            {
                const uint32_t lo = (static_cast<uint32_t>(m_priv.siglblid) & ~mask) | (id & mask);
                m_priv.siglblid = (m_priv.siglblid & 0xFFFFFFFF00000000ull) | lo;
                m_priv.csr.fetch_or(0x1);
            }
            else if (addr == kRegFinish)
                m_priv.csr.fetch_or(0x2);
            else if (addr == kRegLabel)
            {
                const uint32_t hi = (static_cast<uint32_t>(m_priv.siglblid >> 32) & ~mask) | (id & mask);
                m_priv.siglblid = (static_cast<uint64_t>(hi) << 32) | static_cast<uint32_t>(m_priv.siglblid);
            }
        }

        // Follows each path's GIFtags across packets (as the GIF does) to find PACKED A+D writes to
        // SIGNAL/FINISH/LABEL; REGLIST and IMAGE data cannot address them.
        struct PathState
        {
            uint64_t tagLo = 0, tagHi = 0;
            uint32_t loop = 0, nloop = 0, reg = 0;
        };

        void applyPrivilegedEffects(uint32_t path, const uint8_t *data, uint32_t size)
        {
            PathState &state = m_paths[path];
            for (uint32_t offset = 0; offset + 16u <= size;)
            {
                if (state.loop >= state.nloop)
                {
                    state.tagLo = word(data + offset);
                    state.tagHi = word(data + offset + 8);
                    state.nloop = static_cast<uint32_t>(state.tagLo & 0x7FFFu);
                    state.loop = state.reg = 0;
                    offset += 16u;
                    continue;
                }
                const uint32_t flg = static_cast<uint32_t>((state.tagLo >> 58) & 3u);
                const uint32_t nreg = ((state.tagLo >> 60) & 0xFu) ? static_cast<uint32_t>((state.tagLo >> 60) & 0xFu) : 16u;
                if (flg >= 2u) // IMAGE: NLOOP qwords of data
                {
                    const uint32_t count = std::min<uint32_t>(state.nloop - state.loop, (size - offset) / 16u);
                    state.loop += count;
                    offset += count * 16u;
                    continue;
                }
                if (flg == 0u) // PACKED: one register per qword
                {
                    if (((state.tagHi >> (state.reg * 4u)) & 0xFu) == 0xEu)
                        applyRegister(static_cast<uint8_t>(word(data + offset + 8) & 0x7Fu), word(data + offset));
                    offset += 16u;
                    if (++state.reg == nreg)
                    {
                        state.reg = 0;
                        ++state.loop;
                    }
                    continue;
                }
                // REGLIST: two registers per qword, the last qword padded when the count is odd.
                const uint64_t remainingRegs = static_cast<uint64_t>(state.nloop - state.loop) * nreg - state.reg;
                const uint32_t qwords = std::min<uint32_t>(static_cast<uint32_t>((remainingRegs + 1u) / 2u), (size - offset) / 16u);
                uint64_t regs = static_cast<uint64_t>(state.reg) + 2ull * qwords;
                state.loop += static_cast<uint32_t>(regs / nreg);
                state.reg = static_cast<uint32_t>(regs % nreg);
                if (state.loop >= state.nloop)
                    state.reg = 0;
                offset += qwords * 16u;
            }
        }

        GSRegisters &m_priv;
        std::array<PathState, 4> m_paths{};
        void *m_module = nullptr;
        std::filesystem::path m_dllDir;
        Ps2xGsOpenFn m_openFn = nullptr;
        Ps2xGsGifFn m_gif = nullptr;
        Ps2xGsRegsFn m_regs = nullptr;
        Ps2xGsVsyncFn m_vsync = nullptr;
        Ps2xGsSnapshotFn m_snapshot = nullptr;
        Ps2xGsCloseFn m_close = nullptr;
        bool m_open = false;
        uint32_t m_field = 0;
    };
}

std::unique_ptr<GSRasterBackend> CreatePcsx2GSBackend(GSRegisters &registers, void *parentWindow, const char *gameElfName)
{
    auto backend = std::make_unique<Pcsx2Backend>(registers);
    if (!backend->start(parentWindow, gameElfName))
        return nullptr;
    return backend;
}

void ps2xPcsx2SetOverlay(unsigned line, const char *text, uint32_t rgba)
{
    if (!g_active.load(std::memory_order_acquire))
        return;
    if (Ps2xGsSetOverlayFn fn = g_setOverlay.load(std::memory_order_acquire))
        fn(line, text, rgba);
}

bool ps2xPcsx2QueueSnapshot(const char *path)
{
    if (!g_active.load(std::memory_order_acquire) || !path)
        return false;
    if (Ps2xGsSnapshotFn fn = g_snapshot.load(std::memory_order_acquire))
    {
        fn(path);
        return true;
    }
    return false;
}

bool ps2xPcsx2Active()
{
    return g_active.load(std::memory_order_acquire);
}
