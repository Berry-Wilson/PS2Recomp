#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace Vulkan
{
    class Device;
    class Image;
}

// The game window: a GLFW window with no client API whose Vulkan swapchain (Granite's WSI) is the
// only thing that draws to it. It owns the Vulkan device that parallel-gs renders with, and draws
// the scanout image straight to the swapchain, with no copy through the CPU. ImGui (the debug panel
// and overlays) draws on top in the same frame.
class HostDisplay
{
public:
    static bool create(const char *title, int width, int height);
    static HostDisplay *get();
    static void destroy(); // only after everything using device() is gone

    // Granite's device is single threaded here: hold deviceMutex() around any use of device().
    Vulkan::Device &device();
    std::mutex &deviceMutex();

    void pollEvents();
    bool shouldClose() const;
    void hide();

    // The image to show from now on, in TRANSFER_SRC_OPTIMAL. Call with deviceMutex() held.
    void setScanout(Vulkan::Image *image);
    bool hasScanout() const;
    // Runs with deviceMutex() held before each frame. Acquiring a swapchain image waits for every
    // open command buffer on the device, so a renderer that keeps one open between calls must
    // submit it here.
    void setBeforeFrame(std::function<void()> fn);
    // For a backend that renders on the CPU: RGBA8, width * height * 4 bytes.
    void setCpuFrame(const uint8_t *rgba, uint32_t width, uint32_t height);

    void beginUi(); // starts an ImGui frame; ImGui calls are valid until present()
    void present();

    ~HostDisplay();

private:
    HostDisplay();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
