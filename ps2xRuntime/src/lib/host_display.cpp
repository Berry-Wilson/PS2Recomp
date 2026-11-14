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
};

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
    GLFWwindow *window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!window)
    {
        std::cerr << "[HostDisplay] Failed to create the window" << std::endl;
        return false;
    }
    display->m_impl->window = window;
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
