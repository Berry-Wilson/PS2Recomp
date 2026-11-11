#include "runtime/gs/gs_pcsx2_backend.h"
#include "runtime/host_display.h"

#include "../../../pcsx2-gs/ps2x_gs_api.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>

#if defined(_WIN32)
// windows.h clashes with this runtime's headers, so only what is used is declared.
extern "C" __declspec(dllimport) void *__stdcall LoadLibraryExW(const wchar_t *, void *, unsigned long);
extern "C" __declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void *, wchar_t *, unsigned long);
extern "C" __declspec(dllimport) unsigned long __stdcall GetLastError();
constexpr unsigned long kLoadWithAlteredSearchPath = 0x00000008;
#endif

extern std::atomic<bool> g_ps2xUpscaleCycleRequested; // ps2_runtime.cpp: F5 wants the next upscale step
extern std::atomic<int> g_ps2xUpscaleRate;            // ps2_runtime.cpp: shown on screen after F5

namespace
{
    std::mutex g_elfNameMutex;
    std::string g_elfName;
    std::atomic<Ps2xGsSetOverlayFn> g_setOverlay{nullptr};
    std::atomic<Ps2xGsSnapshotFn> g_snapshot{nullptr}; // set once PCSX2's GS is open
    std::atomic<Ps2xGsSetSoftwareRenderingFn> g_setSoftware{nullptr}; // likewise; optional
    std::atomic<int> g_vsyncsSinceMoviePicture{1 << 20}; // vsyncs since the MPEG decoder last gave a picture
    bool g_movieSoftware = false;                         // the software renderer is on for a movie (GS thread)
    constexpr int kMovieEndVsyncs = 15;

    // "SLUS_207.32" -> "SLUS-20732", the form PCSX2's GameIndex.yaml uses.
    std::string serialFromElfName(const std::string &name)
    {
        std::string serial;
        for (char c : name)
        {
            if (c == '_')
                serial.push_back('-');
            else if (c != '.' && c != ';')
                serial.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            if (c == ';')
                break;
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
        {
            // PS2X_GS_BACKEND=cpu (or software), the old name for a renderer on the CPU.
            const char *backend = std::getenv("PS2X_GS_BACKEND");
            if (backend && (std::strcmp(backend, "cpu") == 0 || std::strcmp(backend, "software") == 0))
                return PS2XGS_RENDERER_SOFTWARE;
            return PS2XGS_RENDERER_AUTO;
        }
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
        std::cerr << "[GSPcsx2Backend] ignoring PS2X_PCSX2_RENDERER=" << value
                  << ", it must be dx11, dx12, vulkan, opengl or software" << std::endl;
        return PS2XGS_RENDERER_AUTO;
    }

    // The upscale steps F5 goes through.
    constexpr float kUpscaleSteps[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};

    // Privileged register offsets from 0x12000000.
    constexpr size_t kPmode = 0x00, kSmode1 = 0x10, kSmode2 = 0x20, kDispfb1 = 0x70, kDisplay1 = 0x80,
                     kDispfb2 = 0x90, kDisplay2 = 0xA0, kBgcolor = 0xE0;
    // NTSC, interlaced, as the BIOS's SetGsCrt programs it: PCSX2 tells the video mode from SMODE1,
    // which this runtime does not set.
    constexpr uint64_t kSmode1Ntsc = 0x740834504ull;
}

void ps2xPcsx2SetOverlay(unsigned line, const char *text, uint32_t rgba)
{
    if (Ps2xGsSetOverlayFn fn = g_setOverlay.load(std::memory_order_acquire))
        fn(line, text, rgba);
}

void ps2xPcsx2QueueSnapshot(const char *path)
{
    if (Ps2xGsSnapshotFn fn = g_snapshot.load(std::memory_order_acquire); fn && path)
        fn(path);
}

void ps2xPcsx2NoteMoviePicture()
{
    g_vsyncsSinceMoviePicture.store(0, std::memory_order_relaxed);
}

void ps2xSetGameElfName(const char *name)
{
    std::lock_guard<std::mutex> lock(g_elfNameMutex);
    g_elfName = name ? name : "";
}

struct GSPcsx2Backend::Impl
{
    void *module = nullptr;
    Ps2xGsOpenFn open = nullptr;
    Ps2xGsGifFn gif = nullptr;
    Ps2xGsRegsFn regs = nullptr;
    Ps2xGsVsyncFn vsync = nullptr;
    Ps2xGsSetUpscaleFn setUpscale = nullptr;
    Ps2xGsSnapshotFn snapshot = nullptr;
    Ps2xGsSetSoftwareRenderingFn setSoftware = nullptr; // optional: older DLLs keep the GPU renderer for movies
    Ps2xGsCloseFn close = nullptr;

    std::filesystem::path dllDir;
    std::once_flag openOnce;
    std::atomic<bool> ready{false};
    uint32_t field = 0;
    float upscale = 2.0f;

    bool load();
    void ensureOpen();
};

bool GSPcsx2Backend::Impl::load()
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
        if (!std::filesystem::exists(dll))
            dll = dir / "ps2x-gs.dll";
    }
    // Its own dependencies (SDL3, FreeType, ...) sit next to it.
    dll = std::filesystem::absolute(dll);
    dllDir = dll.parent_path();
    module = LoadLibraryExW(dll.wstring().c_str(), nullptr, kLoadWithAlteredSearchPath);
    if (!module)
    {
        std::cerr << "[GSPcsx2Backend] cannot load " << dll.string() << " (error " << GetLastError() << ")" << std::endl;
        return false;
    }
    auto get = [this](const char *name) { return GetProcAddress(module, name); };
    open = reinterpret_cast<Ps2xGsOpenFn>(get("ps2xgs_open"));
    gif = reinterpret_cast<Ps2xGsGifFn>(get("ps2xgs_gif"));
    regs = reinterpret_cast<Ps2xGsRegsFn>(get("ps2xgs_regs"));
    vsync = reinterpret_cast<Ps2xGsVsyncFn>(get("ps2xgs_vsync"));
    setUpscale = reinterpret_cast<Ps2xGsSetUpscaleFn>(get("ps2xgs_set_upscale"));
    snapshot = reinterpret_cast<Ps2xGsSnapshotFn>(get("ps2xgs_snapshot"));
    close = reinterpret_cast<Ps2xGsCloseFn>(get("ps2xgs_close"));
    if (!open || !gif || !regs || !vsync || !setUpscale || !snapshot || !close)
    {
        std::cerr << "[GSPcsx2Backend] " << dll.string() << " is missing functions" << std::endl;
        return false;
    }
    // Optional: an older DLL without it just shows no overlay.
    g_setOverlay.store(reinterpret_cast<Ps2xGsSetOverlayFn>(get("ps2xgs_set_overlay")), std::memory_order_release);
    setSoftware = reinterpret_cast<Ps2xGsSetSoftwareRenderingFn>(get("ps2xgs_set_software_rendering"));
    std::cerr << "[GSPcsx2Backend] loaded " << dll.string() << std::endl;
    return true;
#else
    std::cerr << "[GSPcsx2Backend] only built for Windows so far" << std::endl;
    return false;
#endif
}

// Opens on first use rather than at start-up, so the game's ELF (its serial) is known by then.
void GSPcsx2Backend::Impl::ensureOpen()
{
    std::call_once(openOnce, [this]
    {
        if (!load())
            return;
        std::string serial;
        if (const char *value = std::getenv("PS2X_GAME_SERIAL"); value && value[0])
            serial = value;
        else
        {
            std::lock_guard<std::mutex> lock(g_elfNameMutex);
            serial = serialFromElfName(g_elfName);
        }
        upscale = envFloat("PS2X_PCSX2_UPSCALE", 2.0f);
        // PCSX2's resources (shaders, fonts, GameIndex.yaml) and its cache and logs live next to the DLL.
        std::string resources = (dllDir / "resources").string();
        if (const char *value = std::getenv("PS2X_PCSX2_RESOURCES"); value && value[0])
            resources = value;
        const std::string dataDir = (dllDir / "gs-data").string();

        Ps2xGsConfig config{};
        config.size = sizeof(config);
        config.parent_window = HostDisplay::get() ? HostDisplay::get()->nativeWindow() : nullptr;
        config.renderer = envRenderer();
        config.upscale = upscale;
        config.widescreen = HostDisplay::outputAspect() > 0.0f ? 1 : 0;
        config.sharpness = static_cast<int>(envFloat("PS2X_PCSX2_SHARPNESS", 0.0f));
        config.vsync = 0; // the runtime paces frames itself
        config.osd = envFloat("PS2X_PCSX2_OSD", 0.0f) != 0.0f ? 1 : 0;
        config.data_dir = dataDir.c_str();
        config.resources_dir = resources.c_str();
        config.serial = serial.empty() ? nullptr : serial.c_str();
        if (!open(&config))
        {
            std::cerr << "[GSPcsx2Backend] PCSX2's GS did not start" << std::endl;
            return;
        }
        g_ps2xUpscaleRate.store(static_cast<int>(upscale), std::memory_order_relaxed);
        std::cerr << "[GSPcsx2Backend] PCSX2 GS open, internal resolution " << upscale << "x"
                  << (serial.empty() ? std::string() : ", game " + serial) << std::endl;
        g_snapshot.store(snapshot, std::memory_order_release);
        g_setSoftware.store(setSoftware, std::memory_order_release);
        ready.store(true, std::memory_order_release);
    });
}

GSPcsx2Backend::GSPcsx2Backend() : m_impl(std::make_unique<Impl>()) {}

GSPcsx2Backend::~GSPcsx2Backend()
{
    if (m_impl && m_impl->ready.load(std::memory_order_acquire))
        m_impl->close();
}

void GSPcsx2Backend::Initialize(uint8_t *, uint32_t)
{
}

void GSPcsx2Backend::Reset()
{
}

void GSPcsx2Backend::ProcessGif(uint32_t path, const uint8_t *data, uint32_t sizeBytes)
{
    m_impl->ensureOpen();
    if (!m_impl->ready.load(std::memory_order_acquire) || !data || sizeBytes < 16)
        return;
    // The arbiter numbers paths 1-3; anything else is treated as PATH3.
    m_impl->gif(path >= 1 && path <= 3 ? path : 3u, data, sizeBytes & ~15u);
}

// A register write outside any GIF packet becomes a one-register A+D packet. It goes on PATH2 so
// that it cannot land in the middle of a PATH3 image transfer.
void GSPcsx2Backend::WriteRegister(uint8_t regAddr, uint64_t value)
{
    m_impl->ensureOpen();
    if (!m_impl->ready.load(std::memory_order_acquire))
        return;
    uint64_t packet[4];
    packet[0] = 1ull | (1ull << 15) | (1ull << 60); // NLOOP 1, EOP, PACKED, NREG 1
    packet[1] = 0xEull;                             // A+D
    packet[2] = value;
    packet[3] = regAddr;
    m_impl->gif(2u, packet, sizeof(packet));
}

// Image data written through HWREG, after BITBLTBUF/TRXPOS/TRXREG/TRXDIR were set the same way.
void GSPcsx2Backend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    m_impl->ensureOpen();
    if (!m_impl->ready.load(std::memory_order_acquire) || !data || !sizeBytes)
        return;
    const uint32_t qwords = (sizeBytes + 15u) / 16u;
    std::vector<uint8_t> packet(16u + qwords * 16u, 0u);
    const uint64_t tag = static_cast<uint64_t>(qwords & 0x7FFFu) | (1ull << 15) | (2ull << 58); // IMAGE, EOP
    std::memcpy(packet.data(), &tag, sizeof(tag));
    std::memcpy(packet.data() + 16, data, sizeBytes);
    m_impl->gif(2u, packet.data(), static_cast<unsigned>(packet.size()));
}

PresentationFrame GSPcsx2Backend::Present(const GSPresentationRequest &request)
{
    m_impl->ensureOpen();
    PresentationFrame result{};
    if (!m_impl->ready.load(std::memory_order_acquire))
        return result;

    // F5: the next internal resolution.
    if (g_ps2xUpscaleCycleRequested.exchange(false, std::memory_order_acq_rel))
    {
        size_t next = 0;
        for (size_t i = 0; i < std::size(kUpscaleSteps); ++i)
        {
            if (kUpscaleSteps[i] > m_impl->upscale + 0.01f)
            {
                next = i;
                break;
            }
        }
        m_impl->upscale = kUpscaleSteps[next];
        m_impl->setUpscale(m_impl->upscale);
        g_ps2xUpscaleRate.store(static_cast<int>(m_impl->upscale), std::memory_order_relaxed);
    }

    uint8_t *regs = static_cast<uint8_t *>(m_impl->regs());
    auto put = [regs](size_t offset, uint64_t value) { std::memcpy(regs + offset, &value, sizeof(value)); };
    put(kPmode, request.pmode);
    put(kSmode1, kSmode1Ntsc);
    put(kSmode2, request.smode2);
    put(kDispfb1, request.dispfb1);
    put(kDisplay1, request.display1);
    put(kDispfb2, request.dispfb2);
    put(kDisplay2, request.display2);
    put(kBgcolor, request.bgcolor);
    // Movies on the software renderer (ps2xPcsx2NoteMoviePicture). Queued before this vsync, so the
    // switch lands between frames.
    {
        static const bool keepHardware = []()
        {
            const char *value = std::getenv("PS2X_PCSX2_MOVIE_RENDERER");
            return value && std::strcmp(value, "hardware") == 0;
        }();
        const int since = g_vsyncsSinceMoviePicture.load(std::memory_order_relaxed);
        if (since <= kMovieEndVsyncs)
            g_vsyncsSinceMoviePicture.fetch_add(1, std::memory_order_relaxed);
        const bool movie = since < kMovieEndVsyncs;
        if (!keepHardware && movie != g_movieSoftware)
        {
            if (Ps2xGsSetSoftwareRenderingFn fn = g_setSoftware.load(std::memory_order_acquire))
                fn(movie ? 1 : 0);
            g_movieSoftware = movie;
        }
    }
    m_impl->vsync(m_impl->field);
    m_impl->field ^= 1u;

    result.presentedToDisplay = true;
    result.width = 640;
    result.height = 448;
    return result;
}

void GSPcsx2Backend::QueueSnapshot(const char *path)
{
    if (m_impl->ready.load(std::memory_order_acquire) && path)
        m_impl->snapshot(path);
}
