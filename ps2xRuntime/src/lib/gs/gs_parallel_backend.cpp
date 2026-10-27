#include <cstdio>
#include <cstdlib>
#include "runtime/gs/gs_parallel_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/ps2_memory.h"
#include "ps2_log.h"
#include "gs_gl_interop.h"
#include "gs_interface.hpp"
#include "thread_id.hpp"
#include <algorithm>
#include <cstring>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <stdexcept>
#include <iostream>

namespace
{
    constexpr size_t VramSize = 4 * 1024 * 1024;
    uint64_t word(const void *data)
    {
        uint64_t value;
        std::memcpy(&value, data, 8);
        return value;
    }

    unsigned bitsPerPixel(unsigned psm)
    {
        switch (psm & 63)
        {
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return 24;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return 16;
        case GS_PSM_T8:
        case GS_PSM_T8H:
            return 8;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return 4;
        default:
            return 32;
        }
    }

    using Reader = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);
    using Writer = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    struct PixelAccess
    {
        Reader read;
        Writer write;
    };

    PixelAccess pixelAccess(unsigned psm)
    {
        using namespace GSMem;
        switch (psm & 63)
        {
#define PSM(id, name) \
    case GS_PSM_##id: \
        return {Read##name, Write##name}
            PSM(CT32, CT32);
            PSM(CT24, CT24);
            PSM(CT16, CT16);
            PSM(CT16S, CT16S);
            PSM(T8, P8);
            PSM(T8H, P8H);
            PSM(T4, P4);
            PSM(T4HL, P4HL);
            PSM(T4HH, P4HH);
            PSM(Z32, Z32);
            PSM(Z24, Z24);
            PSM(Z16, Z16);
            PSM(Z16S, Z16S);
#undef PSM
        default:
            return {ReadNull, WriteNull};
        }
    }

    class ParallelBackend final : public GSRasterBackend, private ParallelGS::SignalInterface
    {
        // All GSInterface/Device entry points are serialized, including presentation.
        mutable std::recursive_mutex mutex;
        GSGLInterop interop;
        mutable ParallelGS::GSInterface gs;
        GSRegisters &priv;
        struct Lock
        {
            std::lock_guard<std::recursive_mutex> guard;
            explicit Lock(std::recursive_mutex &m) : guard(m) { Util::register_thread_index(0); }
        };

        // GIF packets are decoded on a dedicated GS thread so the VU1/VIF1 worker (PATH1/2) and the
        // EE (PATH3) only copy them into a queue. Any other entry point drains the queue first.
        // PS2X_PGS_THREAD=0 decodes synchronously instead.
        struct GifPacket
        {
            uint32_t path = 0;
            std::vector<uint8_t> data;
        };
        mutable std::mutex queueMutex;
        mutable std::condition_variable queueCv;
        mutable std::condition_variable idleCv;
        std::deque<GifPacket> queue;
        std::vector<std::vector<uint8_t>> freeBuffers;
        bool gsBusy = false;
        bool gsSleeping = false; // the GS thread is blocked on queueCv
        size_t queuedBytes = 0;
        bool gsStopping = false;
        bool gsThreaded = true;
        bool highResScanout = false;
        std::thread gsThread;

        void drain() const
        {
            if (!gsThreaded)
                return;
            std::unique_lock<std::mutex> lock(queueMutex);
            if (queue.empty() && !gsBusy)
                return;
            if (gsSleeping)
                queueCv.notify_one();
            idleCv.wait(lock, [&] { return queue.empty() && !gsBusy; });
        }

        struct SyncLock
        {
            explicit SyncLock(const ParallelBackend &backend) : drained((backend.drain(), 0)), inner(backend.mutex) {}
            int drained;
            Lock inner;
        };

        void decodeGIF(uint32_t path, const uint8_t *data, uint32_t size)
        {
            Lock lock(mutex);
            observeGIF(path, data, size);
            gs.gif_transfer(path, data, size);
        }

        void gsThreadMain()
        {
            std::deque<GifPacket> batch;
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    while (!gsStopping && queue.empty())
                    {
                        gsSleeping = true;
                        queueCv.wait_for(lock, std::chrono::milliseconds(1));
                        gsSleeping = false;
                    }
                    if (queue.empty())
                        return; // stopping
                    batch.swap(queue);
                    queuedBytes = 0;
                    gsBusy = true;
                }
                for (GifPacket &packet : batch)
                    decodeGIF(packet.path, packet.data.data(), static_cast<uint32_t>(packet.data.size()));
                std::lock_guard<std::mutex> lock(queueMutex);
                for (GifPacket &packet : batch)
                    if (freeBuffers.size() < 64u)
                        freeBuffers.push_back(std::move(packet.data));
                batch.clear();
                gsBusy = false;
                if (queue.empty())
                    idleCv.notify_all();
            }
        }
        uint64_t bitblt = 0, trxreg = 0;
        GSTransferSnapshot transfer;
        size_t transferBytes = 0, uploadedBytes = 0;
        std::array<uint8_t, 8> uploadTail{};
        size_t uploadTailSize = 0;
        std::array<uint8_t, 16> fifoTail{};
        size_t fifoOffset = 16;
        mutable uint64_t readbacks = 0;

        void observeRegister(uint8_t addr, uint64_t value)
        {
            if (addr == GS_REG_BITBLTBUF)
                bitblt = value;
            if (addr == GS_REG_TRXREG)
                trxreg = value;
            if (addr == GS_REG_TRXDIR)
            {
                transfer = {};
                transfer.direction = value & 3;
                transfer.totalPixels = uint32_t(trxreg & 0xfff) * uint32_t((trxreg >> 32) & 0xfff);
                unsigned psm = unsigned(bitblt >> (transfer.direction == 1 ? 24 : 56)) & 63;
                transferBytes = (size_t(transfer.totalPixels) * bitsPerPixel(psm) + 7) / 8;
                transfer.localToHostPendingBytes = transfer.direction == 1 ? transferBytes : 0;
                if (transfer.direction == 1 && transferBytes)
                    ++readbacks;
                transfer.copiedPixels = transfer.direction == 2 ? transfer.totalPixels : 0;
                uploadedBytes = uploadTailSize = 0;
                fifoOffset = fifoTail.size();
            }

            if (addr == GS_REG_HWREG)
                observeImage(8);
        }
        void observeImage(size_t bytes)
        {
            if (transfer.direction != 0)
                return;
            uploadedBytes = std::min(transferBytes, uploadedBytes + bytes);
            transfer.copiedPixels = std::min<size_t>(transfer.totalPixels, uploadedBytes * 8 / bitsPerPixel(unsigned(bitblt >> 56)));
            auto width = uint32_t(trxreg & 0xfff);
            if (width)
            {
                transfer.x = transfer.copiedPixels % width;
                transfer.y = transfer.copiedPixels / width;
            }
        }

        // Observe transfer boundaries only; the upstream decoder alone executes commands.
        // Start from upstream's saved PATH state, including packets split across DMA calls.
        void observeGIF(uint32_t path, const uint8_t *data, size_t size)
        {
            auto state = gs.get_gif_path(path);
            for (size_t offset = 0; offset < size;)
            {
                if (state.loop == state.tag.NLOOP)
                {
                    std::memcpy(&state.tag, data + offset, 16);
                    state.reg = state.loop = 0;
                    offset += 16;
                    continue;
                }
                unsigned nreg = state.tag.NREG ? state.tag.NREG : 16;
                if (state.tag.FLG >= 2)
                {
                    size_t count = std::min<size_t>(state.tag.NLOOP - state.loop, (size - offset) / 16);
                    observeImage(count * 16);
                    state.loop += uint32_t(count);
                    offset += count * 16;
                }
                else
                {
                    // Only packed A+D can address the transfer registers (>= 0x50).
                    const uint64_t descriptors = word(reinterpret_cast<const uint8_t *>(&state.tag) + 8);
                    if (state.tag.FLG == 0 && ((descriptors >> (state.reg * 4)) & 15) == 14)
                        observeRegister(uint8_t(word(data + offset + 8) & 127), word(data + offset));
                    unsigned count = state.tag.FLG == 0 ? 1 : 2;
                    while (count-- && state.loop < state.tag.NLOOP)
                        if (++state.reg == nreg)
                        {
                            state.reg = 0;
                            ++state.loop;
                        }
                    offset += 16;
                }
            }
        }

        bool on_signal(uint64_t value) override
        {
            auto mask = uint32_t(value >> 32);
            auto id = (uint32_t(priv.siglblid) & ~mask) | (uint32_t(value) & mask);
            priv.siglblid = (priv.siglblid & 0xffffffff00000000ull) | id;
            priv.csr.fetch_or(1);
            return false;
        }

        bool on_finish(uint64_t) override
        {
            gs.flush();
            interop.device().wait_idle();
            priv.csr.fetch_or(2);
            return false;
        }

        bool on_label(uint64_t value) override
        {
            auto mask = uint32_t(value >> 32);
            auto id = (uint32_t(priv.siglblid >> 32) & ~mask) | (uint32_t(value) & mask);
            priv.siglblid = (uint64_t(id) << 32) | uint32_t(priv.siglblid);
            return false;
        }

    public:
        explicit ParallelBackend(GSRegisters &registers) : priv(registers)
        {
            // Internal resolution: PS2X_PGS_SSAA=1/2/4/8/16 super-samples every draw. From 4x on the
            // scanout is also built at double resolution (PS2X_PGS_HIRES=0 keeps native size).
            // PS2X_PGS_SS_TEXTURES=1 keeps the super samples when a render target is sampled as a
            // texture (off by default: it corrupts dhp1's sky); PS2X_PGS_NO_MIPMAPS=1 always
            // samples LOD0 for sharper upscaled textures.
            ParallelGS::GSOptions options{};
            {
                const char *ssaaEnv = std::getenv("PS2X_PGS_SSAA");
                const unsigned ssaa = ssaaEnv ? unsigned(std::strtoul(ssaaEnv, nullptr, 10)) : 1u;
                options.super_sampling = ssaa >= 16 ? ParallelGS::SuperSampling::X16
                                         : ssaa >= 8 ? ParallelGS::SuperSampling::X8
                                         : ssaa >= 4 ? ParallelGS::SuperSampling::X4
                                         : ssaa >= 2 ? ParallelGS::SuperSampling::X2
                                                     : ParallelGS::SuperSampling::X1;
                const char *texEnv = std::getenv("PS2X_PGS_SS_TEXTURES");
                options.super_sampled_textures = ssaa >= 2 && texEnv && texEnv[0] == '1';
                options.ordered_super_sampling = true;
                const char *hiresEnv = std::getenv("PS2X_PGS_HIRES");
                highResScanout = ssaa >= 4 && !(hiresEnv && hiresEnv[0] == '0');
                std::fprintf(stderr, "[parallel-gs] SSAA %ux, super-sampled textures %s, high-res scanout %s\n",
                             unsigned(options.super_sampling), options.super_sampled_textures ? "on" : "off",
                             highResScanout ? "on" : "off");
            }
            if (!gs.init(&interop.device(), options))
                throw std::runtime_error("parallel-gs: GS initialization failed; check Vulkan feature diagnostics above");
            gs.set_signal_interface(this);
            // The sampler-feedback analysis pass (used for framebuffer-sourced textures) crashes the
            // AMD driver while its compute pipeline is built; PS2X_PGS_SAMPLER_FEEDBACK=1 keeps it.
            const char *feedbackEnv = std::getenv("PS2X_PGS_SAMPLER_FEEDBACK");
            if (!(feedbackEnv && feedbackEnv[0] == '1'))
            {
                ParallelGS::DebugMode mode;
                mode.disable_sampler_feedback = true;
                gs.set_debug_mode(mode);
            }
            const char *mipEnv = std::getenv("PS2X_PGS_NO_MIPMAPS");
            if (mipEnv && mipEnv[0] == '1')
            {
                ParallelGS::Hacks hacks;
                hacks.disable_mipmaps = true;
                gs.set_hacks(hacks);
            }
            const char *threadEnv = std::getenv("PS2X_PGS_THREAD");
            gsThreaded = !(threadEnv && threadEnv[0] == '0');
            if (gsThreaded)
                gsThread = std::thread([this] { gsThreadMain(); });
        }

        ~ParallelBackend() override
        {
            if (gsThread.joinable())
            {
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    gsStopping = true;
                }
                queueCv.notify_all();
                gsThread.join();
            }
            Lock lock(mutex);
            gs.flush();
            interop.device().wait_idle();
        }

        bool UsesRawCommands() const override
        {
            return true;
        }

        uint64_t GetReadbackCount() const override
        {
            SyncLock lock(*this);
            return readbacks;
        }

        void Initialize(uint8_t *vram, uint32_t size) override
        {
            SyncLock lock(*this);
            if (!vram || size < VramSize)
                throw std::invalid_argument("parallel-gs requires 4 MiB initial VRAM");
            std::memcpy(gs.map_vram_write(0, VramSize), vram, VramSize);
            gs.end_vram_write(0, VramSize);
        }

        void Reset() override
        {
            SyncLock lock(*this);
            gs.flush();
            interop.device().wait_idle();
            gs.reset_context_state();
            bitblt = trxreg = transferBytes = uploadedBytes = uploadTailSize = 0;
            fifoOffset = 16;
            transfer = {};
        }

        void ProcessGIF(uint32_t path, const uint8_t *data, uint32_t size) override
        {
            if (path < 1 || path > 3 || size % 16)
                throw std::invalid_argument("parallel-gs: invalid GIF path or size");
            if (!data || !size)
                return;
            if (!gsThreaded)
            {
                decodeGIF(path, data, size);
                return;
            }
            std::lock_guard<std::mutex> lock(queueMutex);
            GifPacket packet;
            packet.path = path;
            if (!freeBuffers.empty())
            {
                packet.data = std::move(freeBuffers.back());
                freeBuffers.pop_back();
            }
            packet.data.assign(data, data + size);
            queuedBytes += size;
            queue.push_back(std::move(packet));
            // Waking a thread costs a kernel call per packet. A sleeping GS thread polls every
            // millisecond anyway and drain() wakes it at once, so only wake it for a large batch.
            if (gsSleeping && (queue.size() >= 32u || queuedBytes >= 128u * 1024u))
                queueCv.notify_one();
        }

        void WriteRegisterRaw(uint8_t addr, uint64_t value) override
        {
            SyncLock lock(*this);
            if (addr >= 128)
                return; // Reserved addresses never index upstream's 128-entry table.
            observeRegister(addr, value);
            // Compatibility aliases used by native stubs, not actual GIF registers.
            switch (addr)
            {
            case 0x59:
                priv.dispfb1 = value;
                return;
            case 0x5a:
                priv.display1 = value;
                return;
            case 0x5b:
                priv.dispfb2 = value;
                return;
            case 0x5c:
                priv.display2 = value;
                return;
            case 0x5f:
                priv.bgcolor = value;
                return;
            }
            gs.write_register(static_cast<ParallelGS::RegisterAddr>(addr), value);
        }

        void Submit(const GSPrimitiveBatch &) override
        {
            throw std::logic_error("parallel-gs received a CPU primitive batch");
        }

        void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override
        {
        }

        void BeginTransfer(const GSTransferCommand &) override
        {
            throw std::logic_error("parallel-gs requires raw transfer registers");
        }

        void UploadImage(const uint8_t *data, uint32_t size) override
        {
            SyncLock lock(*this);
            if (!data || transfer.direction != 0)
                return;
            size = uint32_t(std::min<size_t>(size, transferBytes - uploadedBytes));
            observeImage(size);
            while (size)
            {
                auto n = std::min<size_t>(size, 8 - uploadTailSize);
                std::memcpy(uploadTail.data() + uploadTailSize, data, n);
                uploadTailSize += n;
                data += n;
                size -= uint32_t(n);
                if (uploadTailSize == 8 || uploadedBytes == transferBytes && size == 0)
                {
                    std::fill(uploadTail.begin() + uploadTailSize, uploadTail.end(), 0);
                    gs.write_register(ParallelGS::RegisterAddr::HWREG, word(uploadTail.data()));
                    uploadTailSize = 0;
                }
            }
        }

        void Flush() override
        {
            SyncLock lock(*this);
            gs.flush();
        }

        void TextureFlush() override
        {
            WriteRegisterRaw(GS_REG_TEXFLUSH, 0);
        }

        void Sync(GSSyncReason reason) override
        {
            SyncLock lock(*this);
            if (reason == GSSyncReason::Finish || reason == GSSyncReason::Reset)
            {
                gs.flush();
                interop.device().wait_idle();
            }
            // map_vram_read and the upstream FIFO synchronize their own readbacks.
        }

        PresentationFrame Present(const GSPresentationRequest &request) override
        {
            SyncLock lock(*this);
            gs.flush();
            auto &p = gs.get_priv_register_state();
            p.qwords_lo[0] = request.pmode;
            p.qwords_lo[2] = request.smode1;
            p.qwords_lo[4] = request.smode2;
            p.qwords_lo[12] = request.syncv;
            p.qwords_lo[14] = request.dispfb1;
            p.qwords_lo[16] = request.display1;
            p.qwords_lo[18] = request.dispfb2;
            p.qwords_lo[20] = request.display2;
            p.qwords_lo[28] = request.bgcolor;
            ParallelGS::VSyncInfo info{};
            info.phase = request.field;
            info.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            info.dst_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            info.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;
            info.crtc_offsets = true;
            // Progressive scanout with field-blend compensation (like PCSX2's deinterlacing):
            // interlaced games otherwise show alternating dark lines. PS2X_PGS_INTERLACED=1
            // keeps the raw field scanout.
            static const bool rawInterlaced = []
            {
                const char *env = std::getenv("PS2X_PGS_INTERLACED");
                return env && env[0] == '1';
            }();
            info.force_progressive = !rawInterlaced;
            info.anti_blur = !rawInterlaced;
            info.high_resolution_scanout = highResScanout;
            return interop.present(gs.vsync(info));
        }

        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
        {
            SyncLock lock(*this);
            auto *mapped = const_cast<uint8_t *>(static_cast<const uint8_t *>(gs.map_vram_read(0, VramSize)));
            ++readbacks;
            return pixelAccess(psm).read(mapped, base, bw, x, y);
        }

        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
        {
            SyncLock lock(*this);
            gs.map_vram_read(0, VramSize); // Preserve untouched pixels and packed-format lanes.
            auto *mapped = static_cast<uint8_t *>(gs.map_vram_write(0, VramSize));
            ++readbacks;
            pixelAccess(psm).write(mapped, base, bw, x, y, value);
            gs.end_vram_write(0, VramSize);
        }

        void SnapshotVram(std::vector<uint8_t> &out) const override
        {
            SyncLock lock(*this);
            auto *mapped = static_cast<const uint8_t *>(gs.map_vram_read(0, VramSize));
            ++readbacks;
            out.assign(mapped, mapped + VramSize);
        }

        bool ClearFramebuffer(const GSContext &ctx, uint32_t rgba) override
        {
            SyncLock lock(*this);
            if (!ctx.frame.fbw || (ctx.frame.psm != GS_PSM_CT32 && ctx.frame.psm != GS_PSM_CT24 &&
                                   ctx.frame.psm != GS_PSM_CT16 && ctx.frame.psm != GS_PSM_CT16S))
                return false;

            gs.map_vram_read(0, VramSize); // FBMSK and untouched VRAM require current contents.
            auto *mapped = static_cast<uint8_t *>(gs.map_vram_write(0, VramSize));
            ++readbacks;
            auto access = pixelAccess(ctx.frame.psm);

            if (ctx.fba & 1)
                rgba |= 0x80000000u;
            if (bitsPerPixel(ctx.frame.psm) == 16)
                rgba = ((rgba >> 3) & 31) | ((rgba >> 6) & 0x3e0) | ((rgba >> 9) & 0x7c00) | ((rgba >> 16) & 0x8000);

            for (uint32_t y = ctx.scissor.y0; y <= ctx.scissor.y1; ++y)
                for (uint32_t x = ctx.scissor.x0; x <= ctx.scissor.x1; ++x)
                {
                    auto old = access.read(mapped, ctx.frame.fbp * 32, ctx.frame.fbw, x, y);
                    access.write(mapped, ctx.frame.fbp * 32, ctx.frame.fbw, x, y,
                                 (rgba & ~ctx.frame.fbmsk) | (old & ctx.frame.fbmsk));
                }
            gs.end_vram_write(0, VramSize);
            return true;
        }

        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
        {
            SyncLock lock(*this);
            if (!dst)
                return 0;
            auto count = uint32_t(std::min<size_t>(maxBytes, transfer.localToHostPendingBytes));
            for (uint32_t i = 0; i < count; ++i)
            {
                if (fifoOffset == 16)
                {
                    gs.read_transfer_fifo(fifoTail.data(), 1);
                    fifoOffset = 0;
                }
                dst[i] = fifoTail[fifoOffset++];
            }
            transfer.localToHostPendingBytes -= count;
            return count;
        }

        GSTransferSnapshot GetTransferSnapshot() const override
        {
            SyncLock lock(*this);
            return transfer;
        }

        void ReadRegisterState(GSDebugSnapshot &s) const override
        {
            SyncLock lock(*this);
            const auto &r = gs.get_register_state();
            for (unsigned i = 0; i < 2; ++i)
            {
                auto &c = s.ctx[i];
                const auto &v = r.ctx[i];
                auto f = v.frame.bits, t = v.tex0.bits, sc = v.scissor.bits, z = v.zbuf.bits;
                c.frame = {uint32_t(f & 511), uint32_t((f >> 16) & 63), uint8_t((f >> 24) & 63), uint32_t(f >> 32)};
                c.zbuf = {uint32_t(z & 511), uint8_t(((z >> 24) & 15) | 0x30), bool((z >> 32) & 1)};
                c.scissor = {uint16_t(sc & 2047), uint16_t((sc >> 16) & 2047), uint16_t((sc >> 32) & 2047), uint16_t((sc >> 48) & 2047)};
                c.xyoffset = {uint16_t(v.xyoffset.bits), uint16_t(v.xyoffset.bits >> 32)};
                c.tex0 = {uint32_t(t & 16383), uint8_t((t >> 14) & 63), uint8_t((t >> 20) & 63), uint8_t((t >> 26) & 15),
                          uint8_t((t >> 30) & 15), uint8_t((t >> 34) & 1), uint8_t((t >> 35) & 3), uint32_t((t >> 37) & 16383),
                          uint8_t((t >> 51) & 15), uint8_t((t >> 55) & 1), uint8_t((t >> 56) & 31), uint8_t(t >> 61)};
                c.tex1 = v.tex1.bits;
                c.miptbp1 = v.miptbl_1_3.bits;
                c.miptbp2 = v.miptbl_4_6.bits;
                c.clamp = v.clamp.bits;
                c.alpha = v.alpha.bits;
                c.test = v.test.bits;
                c.fba = v.fba.bits;
            }

            auto p = r.prim.bits;
            s.prim = {GSPrimType(p & 7), bool(p & 8), bool(p & 16), bool(p & 32), bool(p & 64), bool(p & 128), bool(p & 256), bool(p & 512), bool(p & 1024)};
            auto b = r.bitbltbuf.bits, t = r.trxpos.bits, a = r.texa.bits, c = r.texclut.bits;
            s.texa = {uint8_t(a), bool(a & 0x8000), uint8_t(a >> 32)};
            s.texclut = {uint8_t(c & 63), uint8_t((c >> 6) & 63), uint16_t((c >> 12) & 1023)};
            s.scanmsk = r.scanmsk.bits;
            s.dimx = r.dimx.bits;
            s.dthe = r.dthe.bits;
            s.colclamp = r.colclamp.bits;
            s.bitbltbuf = {uint32_t(b & 16383), uint8_t((b >> 16) & 63), uint8_t((b >> 24) & 63),
                           uint32_t((b >> 32) & 16383), uint8_t((b >> 48) & 63), uint8_t((b >> 56) & 63)};

            s.trxpos = {uint16_t(t & 2047), uint16_t((t >> 16) & 2047), uint16_t((t >> 32) & 2047), uint16_t((t >> 48) & 2047), uint8_t((t >> 59) & 3)};
            s.trxreg = {uint16_t(r.trxreg.bits & 4095), uint16_t((r.trxreg.bits >> 32) & 4095)};
            s.trxdir = uint32_t(r.trxdir.bits & 3);
        }
    };
}

std::unique_ptr<GSRasterBackend> CreateParallelGSBackend(GSRegisters &registers)
{
    std::cerr << "[gs] using parallel-gs (Vulkan) backend" << std::endl;
    return std::make_unique<ParallelBackend>(registers);
}
