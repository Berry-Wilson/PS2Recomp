#pragma once

#include "runtime/gs/gs_backend.h"

#include <memory>
#include <vector>

// PCSX2's GS, built as ps2x-gs.dll (ps2xRuntime/pcsx2-gs), draws the picture. It receives the raw
// GIF stream and the privileged registers at each vsync, and draws into the game window with its own
// swapchain (DirectX 11/12, Vulkan, OpenGL or its software renderer), at an upscaled internal
// resolution.
class GSPcsx2Backend final : public GSRasterBackend
{
public:
    GSPcsx2Backend();
    ~GSPcsx2Backend() override;

    bool HandlesRawGif() const override { return true; }
    void ProcessGif(uint32_t path, const uint8_t *data, uint32_t sizeBytes) override;
    void WriteRegister(uint8_t regAddr, uint64_t value) override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override {}
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override {}

    void BeginTransfer(const GSTransferCommand &command) override {}
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override {}
    void TextureFlush() override {}
    void Sync(GSSyncReason reason) override {}
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override { return true; }
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override { return 0u; }

    uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override { return 0u; }
    void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void SnapshotVram(std::vector<uint8_t> &out) const override { out.clear(); }
    GSTransferSnapshot GetTransferSnapshot() const override { return {}; }

    // Saves the next presented frame as a PNG (F12, frame dumps).
    void QueueSnapshot(const char *path);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// The game's ELF file name (e.g. "SLUS_207.32"), set by PS2Runtime::loadELF. The backend turns it
// into the serial PCSX2's game database uses (SLUS-20732) to apply its per-game fixes.
void ps2xSetGameElfName(const char *name);

// Overlay text for when PCSX2's GS presents (the runtime's own ImGui output is not shown then):
// line 0-3 in the top-left corner, rgba as IM_COL32. Empty text clears the line. Does nothing
// until the DLL is loaded.
void ps2xPcsx2SetOverlay(unsigned line, const char *text, uint32_t rgba);

// Asks PCSX2 to save the next presented frame as a PNG at `path` (absolute), at its internal
// resolution. Does nothing until the DLL is open.
void ps2xPcsx2QueueSnapshot(const char *path);

// The game asked the MPEG decoder for a movie picture. PCSX2's GPU renderers can garble movies that
// double-buffer across render targets of different widths (Drakengard; see docs/movie-corruption.md in
// the game repo), so while pictures keep coming the movie is drawn with PCSX2's software renderer, as
// its SoftwareRendererFMV fix does; it switches back once no picture has been asked for in 15
// vsyncs, a quarter of a second (PCSX2 likewise watches for the decoder going idle). Games need not call
// sceMpegDelete when a movie ends or is skipped. PS2X_PCSX2_MOVIE_RENDERER=hardware keeps the GPU
// renderer.
void ps2xPcsx2NoteMoviePicture();
