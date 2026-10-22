// Host renderer for Naughty Bear, built on plume (Vulkan) like Unleashed Recompiled's.
//
// This is the first step of replacing the game's Direct3D library: the game's own D3D
// code still runs (on the headless GPU), and these wrappers mirror what reaches the
// screen. BeginTiling supplies the clear color and Swap presents it. The rest of the
// D3D API is mapped in docs/d3d_map.md and will move over function by function.

#include <stdafx.h>
#include <gpu/video.h>
#include <kernel/memory.h>
#include <os/logger.h>
#include <plume_render_interface.h>
#include <bit>
#ifdef NB_TRACE_D3D
#include <gpu/trace.h>
#define TRACE_ENTER(f) nb::trace::Enter(f, ctx)
#define TRACE_LEAVE(f) nb::trace::Leave(f, ctx)
#else
#define TRACE_ENTER(f)
#define TRACE_LEAVE(f)
#endif

using namespace plume;

namespace plume
{
    extern std::unique_ptr<RenderInterface> CreateVulkanInterface(RenderWindow sdlWindow);
}

void HeadlessGpuOnDeviceCreated(uint32_t device);

// Original guest implementations (see docs/d3d_map.md).
extern "C" PPC_FUNC(__imp__sub_82F26FA0); // Direct3D_CreateDevice
extern "C" PPC_FUNC(__imp__sub_82F37138); // D3DDevice_BeginTiling
extern "C" PPC_FUNC(__imp__sub_82F33D50); // D3DDevice_Swap

namespace
{
    constexpr RenderFormat BACKBUFFER_FORMAT = RenderFormat::B8G8R8A8_UNORM;
    constexpr uint32_t BUFFER_COUNT = 2;

    std::unique_ptr<RenderInterface> g_interface;
    std::unique_ptr<RenderDevice> g_device;
    std::unique_ptr<RenderCommandQueue> g_queue;
    std::unique_ptr<RenderSwapChain> g_swapChain;
    std::unique_ptr<RenderCommandList> g_commandList;
    std::unique_ptr<RenderCommandFence> g_fence;
    std::unique_ptr<RenderCommandSemaphore> g_acquireSemaphore;
    std::unique_ptr<RenderCommandSemaphore> g_renderSemaphore;
    std::vector<std::unique_ptr<RenderFramebuffer>> g_framebuffers;

    std::mutex g_videoMutex;
    std::atomic<bool> g_resizePending = false;
    RenderColor g_clearColor = RenderColor(0.0f, 0.0f, 0.0f, 1.0f);

    void CreateFramebuffers()
    {
        g_framebuffers.clear();
        for (uint32_t i = 0; i < g_swapChain->getTextureCount(); i++)
        {
            const RenderTexture* texture = g_swapChain->getTexture(i);
            g_framebuffers.push_back(g_device->createFramebuffer(RenderFramebufferDesc(&texture, 1)));
        }
    }

    void Present()
    {
        std::lock_guard lock(g_videoMutex);
        if (!g_swapChain)
            return;

        // The swap chain's textures exist only after the first resize, so framebuffers are
        // created lazily here (as in Unleashed Recompiled).
        if (g_framebuffers.empty() || g_resizePending.exchange(false) || g_swapChain->needsResize())
        {
            g_framebuffers.clear();
            if (!g_swapChain->resize())
                return;
            CreateFramebuffers();
        }
        if (g_swapChain->getWidth() == 0 || g_swapChain->getHeight() == 0)
            return;

        uint32_t index = 0;
        if (!g_swapChain->acquireTexture(g_acquireSemaphore.get(), &index))
            return;

        RenderTexture* texture = g_swapChain->getTexture(index);
        g_commandList->begin();
        RenderTextureBarrier toColor(texture, RenderTextureLayout::COLOR_WRITE);
        g_commandList->barriers(RenderBarrierStage::GRAPHICS, nullptr, 0, &toColor, 1);
        g_commandList->setFramebuffer(g_framebuffers[index].get());
        g_commandList->clearColor(0, g_clearColor);
        RenderTextureBarrier toPresent(texture, RenderTextureLayout::PRESENT);
        g_commandList->barriers(RenderBarrierStage::NONE, nullptr, 0, &toPresent, 1);
        g_commandList->end();

        const RenderCommandList* lists[] = { g_commandList.get() };
        RenderCommandSemaphore* wait[] = { g_acquireSemaphore.get() };
        RenderCommandSemaphore* signal[] = { g_renderSemaphore.get() };
        g_queue->executeCommandLists(lists, 1, wait, 1, signal, 1, g_fence.get());
        g_swapChain->present(index, signal, 1);
        g_queue->waitForCommandFence(g_fence.get());

        static uint64_t presented = 0;
        static auto lastReport = std::chrono::steady_clock::now();
        presented++;
        auto now = std::chrono::steady_clock::now();
        if (now - lastReport >= std::chrono::seconds(1))
        {
            LOGFN("Video: {} frames presented ({}x{}), clear color {:.2f} {:.2f} {:.2f} {:.2f}", presented,
                g_swapChain->getWidth(), g_swapChain->getHeight(), g_clearColor.r, g_clearColor.g, g_clearColor.b, g_clearColor.a);
            lastReport = now;
        }
    }

    float LoadFloat(uint32_t address)
    {
        uint32_t bits = ByteSwap(*(uint32_t*)g_memory.Translate(address));
        return std::bit_cast<float>(bits);
    }
}

bool Video::Init(SDL_Window* window)
{
    g_interface = CreateVulkanInterface(window);
    if (!g_interface)
    {
        LOGN_ERROR("Could not create the Vulkan interface.");
        return false;
    }

    g_device = g_interface->createDevice();
    if (!g_device)
    {
        LOGN_ERROR("Could not create a Vulkan device.");
        return false;
    }

    g_queue = g_device->createCommandQueue(RenderCommandListType::DIRECT);
    g_commandList = g_queue->createCommandList();
    g_fence = g_device->createCommandFence();
    g_acquireSemaphore = g_device->createCommandSemaphore();
    g_renderSemaphore = g_device->createCommandSemaphore();
    g_swapChain = g_queue->createSwapChain(window, BUFFER_COUNT, BACKBUFFER_FORMAT, 1);
    g_swapChain->setVsyncEnabled(true);

    LOGN("Video: Vulkan device and swap chain ready");
    return true;
}

void Video::OnWindowResized()
{
    g_resizePending = true;
}

// Direct3D_CreateDevice: let the guest create its device, then hand it to the headless GPU.
PPC_FUNC(sub_82F26FA0)
{
    uint32_t deviceOut = ctx.r8.u32;
    TRACE_ENTER(0x82F26FA0);
    __imp__sub_82F26FA0(ctx, base);
    TRACE_LEAVE(0x82F26FA0);
    if (ctx.r3.u32 == 0 && deviceOut != 0)
        HeadlessGpuOnDeviceCreated(ByteSwap(*(uint32_t*)g_memory.Translate(deviceOut)));
}

// D3DDevice_BeginTiling(dev, Flags, Count, pTileRects, pClearColor, ClearZ, ClearStencil):
// the frame's clear color.
PPC_FUNC(sub_82F37138)
{
    if (uint32_t color = ctx.r7.u32)
        g_clearColor = RenderColor(LoadFloat(color), LoadFloat(color + 4), LoadFloat(color + 8), LoadFloat(color + 12));
    TRACE_ENTER(0x82F37138);
    __imp__sub_82F37138(ctx, base);
}

// D3DDevice_Swap: show the frame.
PPC_FUNC(sub_82F33D50)
{
    TRACE_ENTER(0x82F33D50);
    Present();
    __imp__sub_82F33D50(ctx, base);
}
