// Headless GPU: lets the game's own Direct3D library run without a GPU.
//
// Direct3D blocks on fences the GPU writes back, on the ring buffer read pointer, and on
// vertical blank interrupts. With no GPU, those never move. This stand-in makes all
// submitted work look complete at once and raises a vertical blank at 60 Hz. Nothing is
// drawn here: the renderer mirrors the draws (src/gpu/video.cpp, docs/d3d_map.md).

#include <stdafx.h>
#include <cpu/guest_thread.h>
#include <gpu/trace.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <os/logger.h>

namespace
{
    // Device fields found in this XDK version (docs/d3d_map.md).
    constexpr uint32_t DEVICE_COMPLETED_FENCE_PTR = 10896; // pointer the GPU writes completed fences to
    constexpr uint32_t DEVICE_CURRENT_FENCE = 10908;
    // CP_RB_WPTR in the GPU's MMIO block, which Direct3D writes the ring write pointer to.
    constexpr uint32_t CP_RB_WPTR_ADDRESS = 0x7FC80000 + 0x01C5 * 4;

    std::atomic<uint32_t> g_device = 0;
    std::atomic<uint32_t> g_interruptCallback = 0;
    std::atomic<uint32_t> g_interruptContext = 0;
    std::atomic<uint32_t> g_rptrWriteback = 0;
    std::once_flag g_threadStarted;

    uint32_t Load(uint32_t address) { return ByteSwap(*(uint32_t*)g_memory.Translate(address)); }
    void Store(uint32_t address, uint32_t value) { *(uint32_t*)g_memory.Translate(address) = ByteSwap(value); }

    void HeadlessThread()
    {
        GuestThreadContext guest(0);
        auto next = std::chrono::steady_clock::now();
        uint64_t frame = 0;
        while (true)
        {
            next += std::chrono::microseconds(16667);
            std::this_thread::sleep_until(next);

            // Everything submitted so far is "done".
            if (uint32_t device = g_device)
            {
                uint32_t completedPtr = Load(device + DEVICE_COMPLETED_FENCE_PTR);
                if (completedPtr != 0)
                    Store(completedPtr, Load(device + DEVICE_CURRENT_FENCE));
            }
            if (uint32_t writeback = g_rptrWriteback)
                Store(writeback, Load(CP_RB_WPTR_ADDRESS));

            // Vertical blank interrupt (source 0).
            if (uint32_t callback = g_interruptCallback)
            {
                guest.ppcContext.r3.u64 = 0;
                guest.ppcContext.r4.u64 = g_interruptContext;
                g_memory.FindFunction(callback)(guest.ppcContext, g_memory.base);
            }

#ifdef NB_TRACE_D3D
            if (++frame % 300 == 0)
                nb::trace::Report();
#endif
        }
    }

    void StartHeadlessThread()
    {
        std::call_once(g_threadStarted, [] { std::thread(HeadlessThread).detach(); });
    }
}

void HeadlessGpuOnDeviceCreated(uint32_t device)
{
    g_device = device;
    StartHeadlessThread();
}

uint32_t VdSetGraphicsInterruptCallback(uint32_t callback, uint32_t context)
{
    g_interruptCallback = callback;
    g_interruptContext = context;
    StartHeadlessThread();
    return 0;
}

uint32_t VdEnableRingBufferRPtrWriteBack(uint32_t writebackAddress, uint32_t blockSizeLog2)
{
    g_rptrWriteback = writebackAddress;
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__VdSetGraphicsInterruptCallback, VdSetGraphicsInterruptCallback);
GUEST_FUNCTION_HOOK(__imp__VdEnableRingBufferRPtrWriteBack, VdEnableRingBufferRPtrWriteBack);
