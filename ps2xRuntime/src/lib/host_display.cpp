#include "runtime/host_display.h"
#include "runtime/host_input.h"

#include "context.hpp"
#include "device.hpp"
#include "wsi.hpp"
#include "thread_id.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

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

    // Frames the device may have in flight, and so also the number of vertex buffer sets ImGui
    // needs: a set is reused only after this many presents, each of which advances a frame context.
    constexpr uint32_t kFrameContexts = 4;

    class GlfwPlatform final : public Vulkan::WSIPlatform
    {
    public:
        explicit GlfwPlatform(GLFWwindow *window) : m_window(window) {}

        VkSurfaceKHR create_surface(VkInstance instance, VkPhysicalDevice) override
        {
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (glfwCreateWindowSurface(instance, m_window, nullptr, &surface) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            return surface;
        }

        std::vector<const char *> get_instance_extensions() override
        {
            uint32_t count = 0;
            const char **names = glfwGetRequiredInstanceExtensions(&count);
            return std::vector<const char *>(names, names + count);
        }

        uint32_t get_surface_width() override
        {
            int width = 0, height = 0;
            glfwGetFramebufferSize(m_window, &width, &height);
            return static_cast<uint32_t>(width);
        }

        uint32_t get_surface_height() override
        {
            int width = 0, height = 0;
            glfwGetFramebufferSize(m_window, &width, &height);
            return static_cast<uint32_t>(height);
        }

        bool alive(Vulkan::WSI &) override { return !glfwWindowShouldClose(m_window); }
        void poll_input() override { glfwPollEvents(); }
        void poll_input_async(Granite::InputTrackerHandler *) override {}

        void requestResize() { resize = true; }

    private:
        GLFWwindow *m_window;
    };
}

struct HostDisplay::Impl
{
    GLFWwindow *window = nullptr;
    std::unique_ptr<GlfwPlatform> platform;
    Vulkan::WSI wsi;
    std::mutex deviceMutex;
    Vulkan::ImageHandle frame;
    uint32_t framePictureHeight = 0;
    VkRenderPass uiRenderPass = VK_NULL_HANDLE;
    bool uiReady = false;
    bool hasScanout = false;
    std::function<void()> beforeFrame;

    bool init(const char *title, int width, int height);
    bool initUi();
    void shutdown();
    void present();
};

bool HostDisplay::Impl::init(const char *title, int width, int height)
{
#if defined(_WIN32)
    timeBeginPeriod(1);
#endif
    if (!glfwInit())
    {
        std::cerr << "[HostDisplay] glfwInit failed" << std::endl;
        return false;
    }
    if (!glfwVulkanSupported())
    {
        std::cerr << "[HostDisplay] No Vulkan loader or driver found" << std::endl;
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!window)
    {
        std::cerr << "[HostDisplay] Failed to create the window" << std::endl;
        return false;
    }
    glfwSetWindowUserPointer(window, this);
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow *w, int, int)
                                   { static_cast<Impl *>(glfwGetWindowUserPointer(w))->platform->requestResize(); });
    hostinput::attach(window);

    Util::register_thread_index(0);
    if (!Vulkan::Context::init_loader(nullptr))
    {
        std::cerr << "[HostDisplay] Failed to initialize the Vulkan loader" << std::endl;
        return false;
    }

    platform = std::make_unique<GlfwPlatform>(window);
    wsi.set_platform(platform.get());
    if (!wsi.init_context_from_platform(1, {}) || !wsi.init_device())
    {
        std::cerr << "[HostDisplay] Failed to create the Vulkan device" << std::endl;
        return false;
    }
    wsi.get_device().init_frame_contexts(kFrameContexts);
    if (!wsi.init_surface_swapchain())
    {
        std::cerr << "[HostDisplay] Failed to create the swapchain" << std::endl;
        return false;
    }

    // These rebuild the swapchain, so they need one to exist already.
    // UNORM: scanout bytes reach the screen unchanged, with no gamma conversion in the blit.
    wsi.set_backbuffer_format(Vulkan::BackbufferFormat::UNORM);
    wsi.set_extra_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    // Guest VSyncs set the frame rate. Without tearing, and never blocking on the display: the GS
    // worker waits on deviceMutex() while a frame is presented.
    wsi.set_present_mode(Vulkan::PresentMode::UnlockedNoTearing);

    return initUi();
}

bool HostDisplay::Impl::initUi()
{
    Vulkan::Device &device = wsi.get_device();

    // ImGui builds its pipeline against a render pass, so it needs the swapchain format.
    if (!wsi.begin_frame())
    {
        std::cerr << "[HostDisplay] Failed to acquire the first swapchain image" << std::endl;
        return false;
    }

    VkAttachmentDescription attachment{};
    attachment.format = device.get_swapchain_view().get_format();
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    VkRenderPassCreateInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &attachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    if (vkCreateRenderPass(device.get_device(), &rpInfo, nullptr, &uiRenderPass) != VK_SUCCESS)
    {
        std::cerr << "[HostDisplay] Failed to create the UI render pass" << std::endl;
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForVulkan(window, true);

    VkInstance instance = device.get_instance();
    ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, [](const char *name, void *user) -> PFN_vkVoidFunction
                                   { return vkGetInstanceProcAddr(static_cast<VkInstance>(user), name); }, instance);

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_1;
    info.Instance = instance;
    info.PhysicalDevice = wsi.get_context().get_gpu();
    info.Device = device.get_device();
    info.QueueFamily = device.get_queue_info().family_indices[Vulkan::QUEUE_INDEX_GRAPHICS];
    info.Queue = device.get_queue_info().queues[Vulkan::QUEUE_INDEX_GRAPHICS];
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE;
    info.MinImageCount = 2;
    info.ImageCount = kFrameContexts;
    info.PipelineInfoMain.RenderPass = uiRenderPass;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&info))
    {
        std::cerr << "[HostDisplay] Failed to initialize ImGui's Vulkan backend" << std::endl;
        return false;
    }
    uiReady = true;
    return true;
}

void HostDisplay::Impl::present()
{
    int fbWidth = 0, fbHeight = 0;
    glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
    if (fbWidth <= 0 || fbHeight <= 0) // minimized: there is no swapchain to draw to
    {
        ImGui::EndFrame();
        return;
    }

    Util::register_thread_index(0);
    std::lock_guard<std::mutex> lock(deviceMutex);

    if (beforeFrame)
        beforeFrame();

    Vulkan::Device &device = wsi.get_device();
    if (!wsi.begin_frame())
    {
        ImGui::EndFrame();
        return;
    }

    auto cmd = device.request_command_buffer();
    const Vulkan::ImageView &view = device.get_swapchain_view();
    const Vulkan::Image &target = view.get_image();

    cmd->swapchain_touch_in_stages(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
    cmd->image_barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0,
                       VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkClearValue black{};
    black.color.float32[3] = 1.0f;
    cmd->clear_image(target, black);

    if (frame)
    {
        // Aspect-preserving fit, centred; the clear above is the letterbox. Nearest filtering,
        // the same as the OpenGL texture this replaces, except that it would make a stretched
        // image's pixels uneven.
        const float outputAspect = HostDisplay::outputAspect();
        const float srcHeight = static_cast<float>(frame->get_height());
        const float srcWidth = outputAspect > 0.0f ? srcHeight * outputAspect : static_cast<float>(frame->get_width());
        const float dstWidth = static_cast<float>(target.get_width());
        const float dstHeight = static_cast<float>(target.get_height());
        const float scale = std::min(dstWidth / srcWidth, dstHeight / srcHeight);
        const int32_t w = std::max(1, static_cast<int32_t>(std::lround(srcWidth * scale)));
        const int32_t h = std::max(1, static_cast<int32_t>(std::lround(srcHeight * scale)));
        const int32_t x = (static_cast<int32_t>(target.get_width()) - w) / 2;
        const int32_t y = (static_cast<int32_t>(target.get_height()) - h) / 2;

        cmd->barrier(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        cmd->blit_image(target, *frame,
                        {x, y, 0}, {w, h, 1},
                        {0, 0, 0}, {static_cast<int32_t>(frame->get_width()), static_cast<int32_t>(framePictureHeight), 1},
                        0, 0, 0, 0, 1, outputAspect > 0.0f ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }

    // The render pass below loads from the swapchain's own layout and returns the image to it.
    cmd->image_barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, target.get_swapchain_layout(),
                       VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

    ImGui::Render();
    {
        Vulkan::RenderPassInfo rp{};
        rp.color_attachments[0] = &view;
        rp.num_color_attachments = 1;
        rp.load_attachments = 1u;
        rp.store_attachments = 1u;
        cmd->begin_render_pass(rp);
        // ImGui uploads new font pages with its own submit, so it needs the queue.
        device.external_queue_lock();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd->get_command_buffer());
        device.external_queue_unlock();
        cmd->end_render_pass();
    }

    device.submit(cmd);
    wsi.end_frame();
}

void HostDisplay::Impl::shutdown()
{
    frame.reset();
    if (uiReady)
    {
        wsi.get_device().wait_idle();
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        uiReady = false;
    }
    if (uiRenderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(wsi.get_device().get_device(), uiRenderPass, nullptr);
        uiRenderPass = VK_NULL_HANDLE;
    }
    hostinput::detach();
}

HostDisplay::HostDisplay() : m_impl(std::make_unique<Impl>()) {}

HostDisplay::~HostDisplay()
{
    m_impl->shutdown();
    // The WSI destroys the swapchain and surface, so it has to go before the window.
    GLFWwindow *window = m_impl->window;
    m_impl.reset();
    if (window)
        glfwDestroyWindow(window);
    glfwTerminate();
#if defined(_WIN32)
    timeEndPeriod(1);
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
    if (s_display)
        return true;
    std::unique_ptr<HostDisplay> display(new HostDisplay());
    if (!display->m_impl->init(title, width, height))
        return false;
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

Vulkan::Device &HostDisplay::device()
{
    return m_impl->wsi.get_device();
}

std::mutex &HostDisplay::deviceMutex()
{
    return m_impl->deviceMutex;
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

void HostDisplay::setScanout(Vulkan::Image *image, uint32_t pictureHeight)
{
    if (image)
        image->add_reference(); // the handle below takes over one reference
    m_impl->frame = Vulkan::ImageHandle(image);
    m_impl->framePictureHeight = image ? std::clamp<uint32_t>(pictureHeight, 1u, image->get_height()) : 0u;
    m_impl->hasScanout = image != nullptr;
}

void HostDisplay::setBeforeFrame(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(m_impl->deviceMutex);
    m_impl->beforeFrame = std::move(fn);
}

bool HostDisplay::hasScanout() const
{
    return m_impl->hasScanout;
}

void HostDisplay::setCpuFrame(const uint8_t *rgba, uint32_t width, uint32_t height)
{
    Util::register_thread_index(0);
    std::lock_guard<std::mutex> lock(m_impl->deviceMutex);
    auto info = Vulkan::ImageCreateInfo::immutable_2d_image(width, height, VK_FORMAT_R8G8B8A8_UNORM);
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    Vulkan::ImageInitialData initial{};
    initial.data = rgba;
    m_impl->frame = device().create_image(info, &initial);
    m_impl->framePictureHeight = height;
}

void HostDisplay::beginUi()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void HostDisplay::present()
{
    m_impl->present();
}
