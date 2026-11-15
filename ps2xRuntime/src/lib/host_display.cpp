#include "runtime/host_display.h"
#include "runtime/host_input.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#if defined(_WIN32)
// glfw3native.h includes windows.h, whose min and max macros break std::min and std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

#if defined(_WIN32)
// raylib's InitWindow used to set this. The guest timers and every sleep in the runtime rely on it;
// without it Sleep(1) takes up to 15 ms. Declared here because windows.h clashes with this runtime's
// headers.
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);
extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int);
#endif

namespace
{
    std::unique_ptr<HostDisplay> s_display;
}

struct HostDisplay::Impl
{
    GLFWwindow *window = nullptr;
    // Where the window goes when it leaves full screen. Without a position, it is centred.
    int windowedWidth = 0;
    int windowedHeight = 0;
    int windowedX = 0;
    int windowedY = 0;
    bool hasWindowedPos = false;
};

namespace
{
    // The monitor with most of the window on it, else the primary one.
    GLFWmonitor *monitorUnder(GLFWwindow *window)
    {
        int wx = 0, wy = 0, ww = 0, wh = 0;
        glfwGetWindowPos(window, &wx, &wy);
        glfwGetWindowSize(window, &ww, &wh);
        int count = 0;
        GLFWmonitor **monitors = glfwGetMonitors(&count);
        GLFWmonitor *best = glfwGetPrimaryMonitor();
        long long bestArea = 0;
        for (int i = 0; i < count; ++i)
        {
            const GLFWvidmode *mode = glfwGetVideoMode(monitors[i]);
            if (!mode)
                continue;
            int mx = 0, my = 0;
            glfwGetMonitorPos(monitors[i], &mx, &my);
            const long long w = std::max(0, std::min(wx + ww, mx + mode->width) - std::max(wx, mx));
            const long long h = std::max(0, std::min(wy + wh, my + mode->height) - std::max(wy, my));
            if (w * h > bestArea)
            {
                bestArea = w * h;
                best = monitors[i];
            }
        }
        return best;
    }
}

HostDisplay::HostDisplay() : m_impl(std::make_unique<Impl>()) {}

HostDisplay::~HostDisplay()
{
    hostinput::detach();
    if (m_impl->window)
        glfwDestroyWindow(m_impl->window);
    glfwTerminate();
#if defined(_WIN32)
    timeEndPeriod(1);
#endif
}

void *HostDisplay::nativeWindow() const
{
#if defined(_WIN32)
    return glfwGetWin32Window(m_impl->window);
#else
    return nullptr;
#endif
}

float HostDisplay::outputAspect()
{
    static const float aspect = []()
    {
        const char *value = std::getenv("PS2X_ASPECT");
        return value && std::strcmp(value, "16:9") == 0 ? 16.0f / 9.0f : 0.0f;
    }();
    return aspect;
}

bool HostDisplay::create(const char *title, int width, int height)
{
    if (s_display)
        return true;
    if (outputAspect() > 0.0f)
        width = static_cast<int>(std::lround(height * outputAspect()));
    if (const char *size = std::getenv("PS2X_WINDOW_SIZE"); size && size[0])
    {
        int w = 0, h = 0;
        char extra = 0;
        if (std::sscanf(size, "%dx%d%c", &w, &h, &extra) == 2 && w > 0 && h > 0)
        {
            width = w;
            height = h;
        }
        else
        {
            std::cerr << "[HostDisplay] ignoring PS2X_WINDOW_SIZE=" << size << ", it must look like 1280x896" << std::endl;
        }
    }

#if defined(_WIN32)
    timeBeginPeriod(1);
#endif
    std::unique_ptr<HostDisplay> display(new HostDisplay());
    if (!glfwInit())
    {
        std::cerr << "[HostDisplay] glfwInit failed" << std::endl;
        return false;
    }
    // The renderer makes its own swapchain on a child window, so this one gets no client API.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE); // full screen stays up when another window takes focus
    const int windowedWidth = width;
    const int windowedHeight = height;

    // PS2X_FULLSCREEN=1 covers the primary monitor at its current mode, so the display mode doesn't
    // change, and PS2X_WINDOW_SIZE isn't used. The child window follows this one's size.
    GLFWmonitor *monitor = nullptr;
    if (const char *value = std::getenv("PS2X_FULLSCREEN"); value && std::strcmp(value, "1") == 0)
    {
        monitor = glfwGetPrimaryMonitor();
        if (const GLFWvidmode *mode = monitor ? glfwGetVideoMode(monitor) : nullptr)
        {
            glfwWindowHint(GLFW_RED_BITS, mode->redBits);
            glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
            glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
            glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
            width = mode->width;
            height = mode->height;
        }
        else
        {
            std::cerr << "[HostDisplay] no monitor found for PS2X_FULLSCREEN, opening a window" << std::endl;
            monitor = nullptr;
        }
    }
    GLFWwindow *window = glfwCreateWindow(width, height, title, monitor, nullptr);
    if (!window)
    {
        std::cerr << "[HostDisplay] Failed to create the window" << std::endl;
        return false;
    }
    display->m_impl->window = window;
    display->m_impl->windowedWidth = windowedWidth;
    display->m_impl->windowedHeight = windowedHeight;
    display->m_impl->hasWindowedPos = monitor == nullptr;
    if (!monitor)
        glfwGetWindowPos(window, &display->m_impl->windowedX, &display->m_impl->windowedY);
    hostinput::attach(window);

    s_display = std::move(display);
    return true;
}

HostDisplay *HostDisplay::get()
{
    return s_display.get();
}

void HostDisplay::destroy()
{
    s_display.reset();
}

void HostDisplay::pollEvents()
{
    glfwPollEvents();
    hostinput::pollGamepads();
}

bool HostDisplay::shouldClose() const
{
    return glfwWindowShouldClose(m_impl->window) != 0;
}

void HostDisplay::hide()
{
    glfwHideWindow(m_impl->window);
}

void HostDisplay::toggleFullscreen()
{
    Impl &impl = *m_impl;
    if (GLFWmonitor *current = glfwGetWindowMonitor(impl.window))
    {
        int x = impl.windowedX, y = impl.windowedY;
        if (!impl.hasWindowedPos)
        {
            int ax = 0, ay = 0, aw = 0, ah = 0;
            glfwGetMonitorWorkarea(current, &ax, &ay, &aw, &ah);
            x = ax + std::max(0, (aw - impl.windowedWidth) / 2);
            y = ay + std::max(0, (ah - impl.windowedHeight) / 2);
        }
        glfwSetWindowMonitor(impl.window, nullptr, x, y, impl.windowedWidth, impl.windowedHeight, GLFW_DONT_CARE);
        std::cerr << "[HostDisplay] windowed, " << impl.windowedWidth << "x" << impl.windowedHeight << std::endl;
        return;
    }
    glfwGetWindowPos(impl.window, &impl.windowedX, &impl.windowedY);
    glfwGetWindowSize(impl.window, &impl.windowedWidth, &impl.windowedHeight);
    impl.hasWindowedPos = true;
    GLFWmonitor *monitor = monitorUnder(impl.window);
    const GLFWvidmode *mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    if (!mode)
    {
        std::cerr << "[HostDisplay] no monitor found for full screen" << std::endl;
        return;
    }
    // The monitor's current mode, so the display mode doesn't change.
    glfwSetWindowMonitor(impl.window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    std::cerr << "[HostDisplay] full screen, " << mode->width << "x" << mode->height << std::endl;
}
