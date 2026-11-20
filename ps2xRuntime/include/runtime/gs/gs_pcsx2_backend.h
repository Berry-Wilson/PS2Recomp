#pragma once

#include "runtime/gs/gs_backend.h"

#include <memory>

struct GSRegisters;

// PCSX2's GS, built as ps2x-gs.dll (ps2xRuntime/pcsx2-gs), draws the picture into a child of the
// game window with its own swapchain (DirectX 11/12, Vulkan, OpenGL or its software renderer), at
// an upscaled internal resolution. It receives the raw GIF stream and, at each vsync, the
// privileged display registers. The DLL reports nothing back, so SIGNAL/FINISH/LABEL are applied
// to the privileged registers here, as the GIF stream is handed over.
//
// Returns null when the DLL cannot be loaded or PCSX2's GS does not start (the caller keeps its
// other renderer). parentWindow is the game window's HWND.
std::unique_ptr<GSRasterBackend> CreatePcsx2GSBackend(GSRegisters &registers, void *parentWindow, const char *gameElfName);

// Text over the picture in the top-left corner (line 0-3, rgba 0xAABBGGRR); empty text clears the
// line. Does nothing unless PCSX2's GS is running.
void ps2xPcsx2SetOverlay(unsigned line, const char *text, uint32_t rgba);

// Shows or hides PCSX2's statistics overlay (PS2X_PCSX2_OSD=1 shows it from the start). Does
// nothing unless PCSX2's GS is running.
void ps2xPcsx2ToggleOsd();

// Saves the next presented frame as a PNG at `path`, at the internal resolution. Returns false
// unless PCSX2's GS is running.
bool ps2xPcsx2QueueSnapshot(const char *path);

// Whether PCSX2's GS is drawing the picture (the host window then shows its child window).
bool ps2xPcsx2Active();
