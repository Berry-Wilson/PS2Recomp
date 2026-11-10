#pragma once

#include <memory>

// The game window: a GLFW window with no client API. PCSX2's GS (GSPcsx2Backend) draws the picture
// into a child of it with its own swapchain; this class only owns the window, its input and the
// ImGui context. ImGui has nothing to draw to here, so the debug panel and overlays run as code but
// are not shown; the VSync/s and F5 lines go to PCSX2's overlay instead.
class HostDisplay
{
public:
    static bool create(const char *title, int width, int height);
    static HostDisplay *get();
    static void destroy();
    // The width / height to show the image at, from PS2X_ASPECT (only "16:9" so far). 0 keeps the
    // image's own shape.
    static float outputAspect();
    // The window's native handle (an HWND on Windows), for the renderer that draws into it.
    void *nativeWindow() const;

    void pollEvents();
    bool shouldClose() const;
    void hide();

    void beginUi(); // starts an ImGui frame; ImGui calls are valid until present()
    void present(); // ends the ImGui frame

    ~HostDisplay();

private:
    HostDisplay();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
