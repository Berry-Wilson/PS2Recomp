#pragma once

// Plain C interface of ps2x-gs.dll, PCSX2's GS (graphics synthesizer) built as a library. The runtime
// loads the DLL at run time and talks to it only through these functions, so the two never share a
// C++ type and the DLL can be built with PCSX2's own toolchain (clang-cl). SPDX-License-Identifier: GPL-3.0+

#ifdef __cplusplus
extern "C" {
#endif

enum Ps2xGsRenderer
{
    PS2XGS_RENDERER_AUTO = 0,
    PS2XGS_RENDERER_DX11 = 1,
    PS2XGS_RENDERER_DX12 = 2,
    PS2XGS_RENDERER_VULKAN = 3,
    PS2XGS_RENDERER_OPENGL = 4,
    PS2XGS_RENDERER_SOFTWARE = 5
};

typedef struct Ps2xGsConfig
{
    unsigned size;          // sizeof(Ps2xGsConfig), so the struct can grow
    void *parent_window;    // HWND to draw inside (a child window fills its client area)
    int renderer;           // Ps2xGsRenderer
    float upscale;          // internal resolution multiplier, 1 = the PS2's
    int widescreen;         // 1: show the picture at 16:9 (the game draws it narrowed), 0: 4:3
    int sharpness;          // 0 off, 1 low, 2 high: anisotropic filtering and CAS sharpening
    int vsync;              // 1: wait for the display's refresh when presenting
    int osd;                // 1: PCSX2's statistics overlay
    const char *data_dir;   // where PCSX2 writes its shader cache and logs (created if missing)
    const char *resources_dir; // PCSX2's resources folder (shaders, fonts, GameIndex.yaml)
    const char *serial;     // the game's serial, for PCSX2's per-game fixes, e.g. "SLUS-20732"
} Ps2xGsConfig;

typedef int (*Ps2xGsOpenFn)(const Ps2xGsConfig *config);
typedef void (*Ps2xGsGifFn)(unsigned path, const void *data, unsigned bytes);
typedef void *(*Ps2xGsRegsFn)(void);
typedef void (*Ps2xGsVsyncFn)(unsigned field);
typedef void (*Ps2xGsSetUpscaleFn)(float upscale);
typedef void (*Ps2xGsSnapshotFn)(const char *path);
typedef void (*Ps2xGsCloseFn)(void);
typedef void (*Ps2xGsSetOverlayFn)(unsigned line, const char *text, unsigned rgba);

// Returns 1 on success.
// int ps2xgs_open(const Ps2xGsConfig *config);
// One GIF packet as the GS received it on PATH1, 2 or 3 (path 1-3). The GS keeps each path's state,
// so a transfer may continue in the next packet on the same path.
// void ps2xgs_gif(unsigned path, const void *data, unsigned bytes);
// The privileged registers, laid out as at 0x12000000 (PMODE at 0, ..., CSR at 0x1000): write them
// before each vsync.
// void *ps2xgs_regs(void);
// The end of a frame: the GS thread draws what was queued and presents it.
// void ps2xgs_vsync(unsigned field);
// Changes the internal resolution while running.
// void ps2xgs_set_upscale(float upscale);
// Saves the next presented frame as a PNG.
// void ps2xgs_snapshot(const char *path);
// void ps2xgs_close(void);
// Text drawn over the picture in the top-left corner, one line per slot (0-3), in the runtime's
// overlay style (20 px, with a shadow). rgba is 0xAABBGGRR. Empty or null text clears the line.
// void ps2xgs_set_overlay(unsigned line, const char *text, unsigned rgba);

#ifdef __cplusplus
}
#endif
