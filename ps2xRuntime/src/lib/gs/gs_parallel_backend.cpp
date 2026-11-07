#include "runtime/gs/gs_parallel_backend.h"
#include "runtime/host_display.h"

#include "gs_interface.hpp"
#include "context.hpp"
#include "device.hpp"
#include "thread_id.hpp"

#include <iostream>
#include <mutex>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <thread>

struct GSParallelBackend::Impl
{
    // The device belongs to the game window's swapchain; the mutex serializes it with present.
    Vulkan::Device &device = HostDisplay::get()->device();
    ParallelGS::GSInterface iface;
    bool initialized = false;
    std::mutex &mutex = HostDisplay::get()->deviceMutex();
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0;
    GSTransferSnapshot transferSnapshot{};
    // Field parity for interlaced scanout. parallel-gs deinterlaces from a history of
    // fields and expects consecutive vsyncs to alternate between phase 0 and 1.
    uint32_t fieldPhase = 0;
    // Host-visible copy of the scanout image, reused across presents; only for frame dumps.
    Vulkan::BufferHandle readback;

    // GIF parsing (GSInterface::gif_transfer) is the expensive part of the GS, and nothing the
    // guest can observe depends on it finishing early: privileged-register side effects
    // (SIGNAL/FINISH/LABEL) are applied by GS::processGIFPacket on the EE thread, and readback
    // is not implemented by this backend. So the EE thread only queues GIF packets, register
    // writes, image uploads and flushes; a worker thread replays them in order, and present()
    // and Reset() first wait until everything queued so far has been replayed.
    enum class CmdKind : uint8_t
    {
        Gif,
        Register,
        Hwreg,
        Flush,
        TexFlush
    };
    struct Cmd
    {
        CmdKind kind;
        uint8_t reg = 0;
        uint32_t path = 0;
        uint64_t value = 0;
        size_t offset = 0;
        size_t bytes = 0;
    };
    struct Batch
    {
        std::vector<Cmd> cmds;
        std::vector<uint8_t> data;
    };
    std::mutex queueMutex;
    std::condition_variable queueCv;
    Batch pending;
    Batch working;
    uint64_t submitted = 0; // commands queued (guarded by queueMutex)
    uint64_t completed = 0; // commands replayed (guarded by queueMutex)
    bool stopWorker = false;
    std::thread worker;
    static constexpr size_t kMaxPendingBytes = 32u * 1024u * 1024u;

    bool init();
    void enqueue(Cmd cmd, const uint8_t *data = nullptr, size_t bytes = 0);
    void drain();
    void workerLoop();
    void replay(const Batch &batch);
    void stop();
    PresentationFrame present(const GSPresentationRequest &request);
};

bool GSParallelBackend::Impl::init()
{
    if (initialized)
        return true;

    Util::register_thread_index(0);

    ParallelGS::GSOptions opts = {};
    opts.vram_size = 4 * 1024 * 1024;
    opts.super_sampling = ParallelGS::SuperSampling::X1;

    if (!iface.init(&device, opts))
    {
        std::cerr << "[GSParallelBackend] Failed to initialize GSInterface\n";
        return false;
    }

    ParallelGS::Hacks hacks;
    hacks.backbuffer_promotion = true;
    iface.set_hacks(hacks);

    HostDisplay::get()->setBeforeFrame([this] { iface.flush(); });

    initialized = true;
    worker = std::thread([this] { workerLoop(); });
    std::cout << "[GSParallelBackend] Vulkan parallel-gs backend initialized successfully!\n";
    return true;
}

void GSParallelBackend::Impl::enqueue(Cmd cmd, const uint8_t *data, size_t bytes)
{
    std::unique_lock<std::mutex> lock(queueMutex);
    // Bound memory if the EE runs far ahead of the worker.
    queueCv.wait(lock, [&] { return pending.data.size() + bytes <= kMaxPendingBytes || pending.cmds.empty(); });
    if (data && bytes)
    {
        cmd.offset = pending.data.size();
        cmd.bytes = bytes;
        pending.data.insert(pending.data.end(), data, data + bytes);
    }
    pending.cmds.push_back(cmd);
    ++submitted;
    lock.unlock();
    queueCv.notify_all();
}

void GSParallelBackend::Impl::drain()
{
    std::unique_lock<std::mutex> lock(queueMutex);
    const uint64_t target = submitted;
    queueCv.wait(lock, [&] { return completed >= target || !worker.joinable(); });
}

void GSParallelBackend::Impl::workerLoop()
{
    Util::register_thread_index(0);
    for (;;)
    {
        size_t count = 0;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [&] { return stopWorker || !pending.cmds.empty(); });
            if (pending.cmds.empty())
                return;
            std::swap(pending, working);
            count = working.cmds.size();
        }
        queueCv.notify_all(); // pending has room again
        replay(working);
        working.cmds.clear();
        working.data.clear();
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            completed += count;
        }
        queueCv.notify_all();
    }
}

void GSParallelBackend::Impl::replay(const Batch &batch)
{
    std::lock_guard<std::mutex> lock(mutex);
    for (const Cmd &cmd : batch.cmds)
    {
        const uint8_t *data = batch.data.data() + cmd.offset;
        switch (cmd.kind)
        {
        case CmdKind::Gif:
            iface.gif_transfer(cmd.path, data, cmd.bytes);
            break;
        case CmdKind::Register:
            iface.write_register(static_cast<ParallelGS::RegisterAddr>(cmd.reg), cmd.value);
            break;
        case CmdKind::Hwreg:
        {
            const size_t qwords = cmd.bytes / 8;
            if (qwords > 0)
                iface.a_d_HWREG_multi(reinterpret_cast<const uint64_t *>(data), qwords);
            const size_t rem = cmd.bytes % 8;
            if (rem > 0)
            {
                uint64_t last = 0;
                std::memcpy(&last, data + qwords * 8, rem);
                iface.write_register(ParallelGS::RegisterAddr::HWREG, last);
            }
            break;
        }
        case CmdKind::Flush:
            iface.flush();
            break;
        case CmdKind::TexFlush:
            iface.write_register(ParallelGS::RegisterAddr::TEXFLUSH, static_cast<uint64_t>(0));
            break;
        }
    }
}

void GSParallelBackend::Impl::stop()
{
    if (!worker.joinable())
        return;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopWorker = true;
    }
    queueCv.notify_all();
    worker.join();
    HostDisplay::get()->setBeforeFrame(nullptr);
}

PresentationFrame GSParallelBackend::Impl::present(const GSPresentationRequest &request)
{
    PresentationFrame result{};
    if (!initialized)
        return result;

    drain();
    Util::register_thread_index(0);
    std::lock_guard<std::mutex> lock(mutex);

    auto &priv = iface.get_priv_register_state();
    std::memcpy(&priv.pmode, &request.pmode, sizeof(uint64_t));
    std::memcpy(&priv.smode2, &request.smode2, sizeof(uint64_t));
    std::memcpy(&priv.dispfb1, &request.dispfb1, sizeof(uint64_t));
    std::memcpy(&priv.display1, &request.display1, sizeof(uint64_t));
    std::memcpy(&priv.dispfb2, &request.dispfb2, sizeof(uint64_t));
    std::memcpy(&priv.display2, &request.display2, sizeof(uint64_t));
    std::memcpy(&priv.bgcolor, &request.bgcolor, sizeof(uint64_t));

    // Ensure smode1 is configured for NTSC analog display so parallel-gs can scan out the frame.
    priv.smode1.CMOD = ParallelGS::SMODE1Bits::CMOD_NTSC;
    priv.smode1.LC = ParallelGS::SMODE1Bits::LC_ANALOG;

    const auto &regs = iface.get_register_state();
    uint32_t activeFbp = regs.ctx[0].frame.desc.FBP;
    if (activeFbp == 0 && request.contextFrames[0].fbp != 0)
    {
        activeFbp = request.contextFrames[0].fbp;
    }

    // Before the guest has programmed the display (PMODE still 0), DISPFB is meaningless, so
    // scan out the active draw buffer instead. Once PMODE is set, FBP 0 is a legitimate
    // display buffer: Drakengard renders to FBP 80 and copies the finished frame to FBP 0,
    // and redirecting then shows the half-drawn render target.
    const bool displayUnconfigured = request.pmode == 0;
    if (displayUnconfigured && priv.dispfb1.FBP == 0 && activeFbp != 0)
    {
        priv.dispfb1.FBP = activeFbp;
        if (priv.dispfb1.FBW == 0)
            priv.dispfb1.FBW = regs.ctx[0].frame.desc.FBW ? regs.ctx[0].frame.desc.FBW : 10;
        priv.dispfb1.PSM = regs.ctx[0].frame.desc.PSM;
    }
    if (displayUnconfigured && priv.dispfb2.FBP == 0 && activeFbp != 0)
    {
        priv.dispfb2.FBP = activeFbp;
        if (priv.dispfb2.FBW == 0)
            priv.dispfb2.FBW = regs.ctx[0].frame.desc.FBW ? regs.ctx[0].frame.desc.FBW : 10;
        priv.dispfb2.PSM = regs.ctx[0].frame.desc.PSM;
    }

    // Default display geometry if unconfigured
    if (priv.display1.DW == 0)
    {
        priv.display1.DX = 652;
        priv.display1.DY = 50;
        priv.display1.MAGH = 3;
        priv.display1.MAGV = 0;
        priv.display1.DW = 2559;
        priv.display1.DH = 447;
    }
    if (priv.display2.DW == 0)
    {
        priv.display2 = priv.display1;
    }

    // Respect guest's display circuit configuration, ensuring display is enabled.
    if (request.pmode != 0)
    {
        // Use guest PMODE, but ensure at least one display circuit is active.
        if (priv.pmode.EN1 == 0 && priv.pmode.EN2 == 0)
        {
            priv.pmode.EN1 = 1;
        }
    }
    else
    {
        // Initial fallback before guest configures display.
        priv.pmode.EN1 = 1;
        priv.dispfb1.FBP = activeFbp ? activeFbp : 0;
        priv.dispfb1.FBW = 10;
        priv.dispfb1.PSM = 0;
    }

    ParallelGS::VSyncInfo vsync{};
    vsync.phase = fieldPhase;
    fieldPhase ^= 1u;
    vsync.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vsync.dst_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    vsync.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;
    vsync.force_progressive = true;
    vsync.anti_blur = true;
    vsync.adapt_to_internal_horizontal_resolution = true;
    vsync.overscan = false;
    vsync.crtc_offsets = false;
    vsync.raw_circuit_scanout = false;

    iface.flush();
    auto scanout = iface.vsync(vsync);

    if (scanout.image)
    {
        const uint32_t width = scanout.image->get_width();
        const uint32_t height = scanout.image->get_height();

        if (width > 0 && height > 0)
        {
            // The video mode can be taller than what the game scans out (448 rows in a 480-row image
            // here). DISPLAY holds the game's own height; a field-rendered height covers two rows.
            uint32_t pictureHeight = 0u;
            if (priv.pmode.EN1)
                pictureHeight = std::max(pictureHeight, (priv.display1.DH + 1u) / (priv.display1.MAGV + 1u));
            if (priv.pmode.EN2)
                pictureHeight = std::max(pictureHeight, (priv.display2.DH + 1u) / (priv.display2.MAGV + 1u));
            if (pictureHeight * 2u <= height)
                pictureHeight *= 2u;
            if (pictureHeight == 0u || pictureHeight > height)
                pictureHeight = height;
            static uint32_t s_loggedPicture = 0u;
            static uint32_t s_loggedHeight = 0u;
            if (pictureHeight != s_loggedPicture || height != s_loggedHeight)
            {
                s_loggedPicture = pictureHeight;
                s_loggedHeight = height;
                std::cerr << "[GSParallelBackend] picture is " << pictureHeight << " rows of a " << width << "x" << height
                          << " scanout, DH1=" << priv.display1.DH << " DH2=" << priv.display2.DH << " EN=" << priv.pmode.EN1
                          << priv.pmode.EN2 << std::endl;
            }
            HostDisplay::get()->setScanout(scanout.image.get(), pictureHeight);
            result.width = width;
            result.height = height;
            result.presentedToDisplay = true;
        }

        static const bool s_wantPixels = std::getenv("PS2X_DUMP_FRAMES_DIR") != nullptr;
        if (s_wantPixels && width > 0 && height > 0)
        {
            auto cmd = device.request_command_buffer();
            const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * sizeof(uint32_t);
            if (!readback || readback->get_create_info().size < bytes)
            {
                Vulkan::BufferCreateInfo bufinfo = {};
                bufinfo.size = bytes;
                bufinfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                bufinfo.domain = Vulkan::BufferDomain::CachedHost;
                readback = device.create_buffer(bufinfo);
            }

            cmd->copy_image_to_buffer(*readback, *scanout.image, 0, {},
                                      { width, height, 1 },
                                      0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
            cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            // Wait for this copy only. Device::wait_idle() would also free Granite's buffer pools,
            // forcing every later command buffer to allocate fresh uniform/staging VkBuffers.
            Vulkan::Fence fence;
            device.submit(cmd, &fence);
            fence->wait();

            const auto *mapped = static_cast<const uint8_t *>(device.map_host_buffer(*readback, Vulkan::MEMORY_ACCESS_READ_BIT));
            if (mapped)
            {
                result.pixels.resize(bytes);
                std::memcpy(result.pixels.data(), mapped, bytes);

                // Scanout alpha is not meaningful for presentation.
                for (size_t i = 3; i < result.pixels.size(); i += 4)
                    result.pixels[i] = 0xFF;
            }
        }
    }

    result.displayFbp = static_cast<uint32_t>(priv.dispfb1.FBP * 32);
    result.sourceFbp = result.displayFbp;
    return result;
}

GSParallelBackend::GSParallelBackend()
    : m_impl(std::make_unique<Impl>())
{
}

GSParallelBackend::~GSParallelBackend()
{
    if (m_impl)
        m_impl->stop();
}

void GSParallelBackend::ProcessGif(uint32_t path, const uint8_t *data, uint32_t sizeBytes)
{
    if (!m_impl || !m_impl->initialized || !data || sizeBytes < 16)
        return;
    Impl::Cmd cmd{Impl::CmdKind::Gif};
    cmd.path = std::min<uint32_t>(path, 3u);
    m_impl->enqueue(cmd, data, sizeBytes & ~15u);
}

void GSParallelBackend::WriteRegister(uint8_t regAddr, uint64_t value)
{
    if (!m_impl || !m_impl->initialized)
        return;
    Impl::Cmd cmd{Impl::CmdKind::Register};
    cmd.reg = regAddr;
    cmd.value = value;
    m_impl->enqueue(cmd);
}

void GSParallelBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    if (m_impl)
    {
        m_impl->vram = vram;
        m_impl->vramSize = vramSize;
        m_impl->init();
    }
}

void GSParallelBackend::Reset()
{
    if (m_impl && m_impl->initialized)
    {
        m_impl->drain();
        Util::register_thread_index(0);
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->iface.reset_context_state();
    }
}

void GSParallelBackend::Submit(const GSPrimitiveBatch &)
{
}

void GSParallelBackend::LoadClut(const GSTex0Reg &, const GSTexClutReg &)
{
}

void GSParallelBackend::BeginTransfer(const GSTransferCommand &)
{
}

void GSParallelBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    if (m_impl && m_impl->initialized && data && sizeBytes > 0)
        m_impl->enqueue(Impl::Cmd{Impl::CmdKind::Hwreg}, data, sizeBytes);
}

void GSParallelBackend::Flush()
{
    if (m_impl && m_impl->initialized)
        m_impl->enqueue(Impl::Cmd{Impl::CmdKind::Flush});
}

void GSParallelBackend::TextureFlush()
{
    if (m_impl && m_impl->initialized)
        m_impl->enqueue(Impl::Cmd{Impl::CmdKind::TexFlush});
}

void GSParallelBackend::Sync(GSSyncReason)
{
    if (m_impl && m_impl->initialized)
        m_impl->enqueue(Impl::Cmd{Impl::CmdKind::Flush});
}

PresentationFrame GSParallelBackend::Present(const GSPresentationRequest &request)
{
    if (m_impl)
        return m_impl->present(request);
    return {};
}

bool GSParallelBackend::ClearFramebuffer(const GSContext &, uint32_t)
{
    return true;
}

uint32_t GSParallelBackend::ConsumeLocalToHostBytes(uint8_t *, uint32_t)
{
    return 0u;
}

uint32_t GSParallelBackend::ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const
{
    return 0u;
}

void GSParallelBackend::WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
{
}

void GSParallelBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    if (m_impl && m_impl->vram && m_impl->vramSize > 0)
    {
        out.assign(m_impl->vram, m_impl->vram + m_impl->vramSize);
    }
}

GSTransferSnapshot GSParallelBackend::GetTransferSnapshot() const
{
    return m_impl ? m_impl->transferSnapshot : GSTransferSnapshot{};
}
