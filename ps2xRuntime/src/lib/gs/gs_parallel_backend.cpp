#include "runtime/gs/gs_parallel_backend.h"

#include "gs_interface.hpp"
#include "context.hpp"
#include "device.hpp"
#include "thread_id.hpp"

#include <iostream>
#include <mutex>
#include <cstring>
#include <algorithm>

struct GSParallelBackend::Impl
{
    Vulkan::Context context;
    Vulkan::Device device;
    ParallelGS::GSInterface iface;
    bool initialized = false;
    std::mutex mutex;
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0;
    GSTransferSnapshot transferSnapshot{};
    // Field parity for interlaced scanout. parallel-gs deinterlaces from a history of
    // fields and expects consecutive vsyncs to alternate between phase 0 and 1.
    uint32_t fieldPhase = 0;

    bool init();
    void processGif(uint32_t path, const uint8_t *data, uint32_t sizeBytes);
    void writeRegister(uint8_t regAddr, uint64_t value);
    PresentationFrame present(const GSPresentationRequest &request);
};

bool GSParallelBackend::Impl::init()
{
    if (initialized)
        return true;

    Util::register_thread_index(0);

    if (!Vulkan::Context::init_loader(nullptr))
    {
        std::cerr << "[GSParallelBackend] Failed to initialize Vulkan loader\n";
        return false;
    }

    context.set_num_thread_indices(1);
    if (!context.init_instance_and_device(nullptr, 0, nullptr, 0,
                                         Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
                                         Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
                                         Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT))
    {
        std::cerr << "[GSParallelBackend] Failed to initialize Vulkan instance and device\n";
        return false;
    }

    device.set_context(context);
    device.init_frame_contexts(4);

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

    initialized = true;
    std::cout << "[GSParallelBackend] Vulkan parallel-gs backend initialized successfully!\n";
    return true;
}

void GSParallelBackend::Impl::processGif(uint32_t path, const uint8_t *data, uint32_t sizeBytes)
{
    if (!initialized || !data || sizeBytes < 16)
        return;

    Util::register_thread_index(0);
    std::lock_guard<std::mutex> lock(mutex);
    const size_t alignedBytes = sizeBytes & ~15ull;
    if (alignedBytes > 0)
    {
        const uint32_t clampedPath = std::min<uint32_t>(path, 3u);
        iface.gif_transfer(clampedPath, data, alignedBytes);
    }
}

void GSParallelBackend::Impl::writeRegister(uint8_t regAddr, uint64_t value)
{
    if (!initialized)
        return;

    Util::register_thread_index(0);
    std::lock_guard<std::mutex> lock(mutex);
    iface.write_register(static_cast<ParallelGS::RegisterAddr>(regAddr), value);
}

PresentationFrame GSParallelBackend::Impl::present(const GSPresentationRequest &request)
{
    PresentationFrame result{};
    if (!initialized)
        return result;

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
    vsync.dst_layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    vsync.dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
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
            auto cmd = device.request_command_buffer();
            cmd->image_barrier(*scanout.image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

            Vulkan::BufferCreateInfo bufinfo = {};
            bufinfo.size = static_cast<VkDeviceSize>(width) * height * sizeof(uint32_t);
            bufinfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bufinfo.domain = Vulkan::BufferDomain::CachedHost;
            auto readback = device.create_buffer(bufinfo);

            cmd->copy_image_to_buffer(*readback, *scanout.image, 0, {},
                                      { width, height, 1 },
                                      0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
            cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            device.submit(cmd);
            device.wait_idle();

            const auto *mapped = static_cast<const uint8_t *>(device.map_host_buffer(*readback, Vulkan::MEMORY_ACCESS_READ_BIT));
            if (mapped)
            {
                result.width = width;
                result.height = height;
                result.pixels.resize(bufinfo.size);
                std::memcpy(result.pixels.data(), mapped, bufinfo.size);

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

GSParallelBackend::~GSParallelBackend() = default;

void GSParallelBackend::ProcessGif(uint32_t path, const uint8_t *data, uint32_t sizeBytes)
{
    if (m_impl)
        m_impl->processGif(path, data, sizeBytes);
}

void GSParallelBackend::WriteRegister(uint8_t regAddr, uint64_t value)
{
    if (m_impl)
        m_impl->writeRegister(regAddr, value);
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
    {
        Util::register_thread_index(0);
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const size_t qwords = sizeBytes / 8;
        if (qwords > 0)
        {
            m_impl->iface.a_d_HWREG_multi(reinterpret_cast<const uint64_t *>(data), qwords);
        }
        const size_t rem = sizeBytes % 8;
        if (rem > 0)
        {
            uint64_t last = 0;
            std::memcpy(&last, data + qwords * 8, rem);
            m_impl->iface.write_register(ParallelGS::RegisterAddr::HWREG, last);
        }
    }
}

void GSParallelBackend::Flush()
{
    if (m_impl && m_impl->initialized)
    {
        Util::register_thread_index(0);
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->iface.flush();
    }
}

void GSParallelBackend::TextureFlush()
{
    if (m_impl && m_impl->initialized)
    {
        Util::register_thread_index(0);
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->iface.write_register(ParallelGS::RegisterAddr::TEXFLUSH, static_cast<uint64_t>(0));
    }
}

void GSParallelBackend::Sync(GSSyncReason)
{
    if (m_impl && m_impl->initialized)
    {
        Util::register_thread_index(0);
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->iface.flush();
    }
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
