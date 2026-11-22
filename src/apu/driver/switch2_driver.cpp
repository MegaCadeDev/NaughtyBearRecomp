// Switch 2 audio output through audout: 48 kHz stereo 16-bit, in buffers of 256 samples (the
// game's XAudio frame size). Frames are six channels (5.1) of big-endian floats,
// channel-major; they're downmixed to stereo as on PC. Submitting waits while every buffer is
// queued, which paces the audio thread.
#if defined(__SWITCH2__)

#include <stdafx.h>
#include <apu/audio.h>
#include <cpu/guest_thread.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <os/logger.h>
#include <os/switch2.h>

namespace
{
    // About 32 ms queued (six 256-sample frames).
    constexpr size_t BUFFER_COUNT = 6;
    constexpr size_t BUFFER_BYTES = 0x1000; // audout wants page-aligned, page-sized buffers
    static_assert(2 * XAUDIO_NUM_SAMPLES * sizeof(int16_t) <= BUFFER_BYTES);

    bool g_ready = false;
    AudioOutBuffer g_buffers[BUFFER_COUNT];
    std::vector<AudioOutBuffer*> g_free;
    PPCFunc* g_clientCallback{};
    uint32_t g_clientCallbackParam{}; // pointer in guest memory
    bool g_audioThreadStarted = false;

    void AudioThread()
    {
        GuestThreadContext ctx(0);
        while (true)
        {
            // The game mixes a frame and submits it (XAudioSubmitFrame), which waits for a free
            // buffer.
            ctx.ppcContext.r3.u32 = g_clientCallbackParam;
            g_clientCallback(ctx.ppcContext, g_memory.base);
        }
    }
}

void XAudioInitializeSystem()
{
    Result result = audoutInitialize();
    if (R_SUCCEEDED(result))
        result = audoutStartAudioOut();
    if (R_FAILED(result))
    {
        LOGFN_ERROR("Audio: audout failed (0x{:X})", result);
        return;
    }
    for (auto& buffer : g_buffers)
    {
        buffer = {};
        buffer.buffer = aligned_alloc(0x1000, BUFFER_BYTES);
        memset(buffer.buffer, 0, BUFFER_BYTES);
        buffer.buffer_size = BUFFER_BYTES;
        buffer.data_size = 2 * XAUDIO_NUM_SAMPLES * sizeof(int16_t);
        g_free.push_back(&buffer);
    }
    g_ready = true;
    LOGFN("Audio: {} Hz, {} channels (audout)", audoutGetSampleRate(), audoutGetChannelCount());
}

void XAudioRegisterClient(PPCFunc* callback, uint32_t param)
{
    if (!g_ready || g_audioThreadStarted)
        return;

    auto* clientParam = static_cast<uint32_t*>(g_userHeap.Alloc(sizeof(param)));
    *clientParam = ByteSwap(param);
    g_clientCallbackParam = g_memory.MapVirtual(clientParam);
    g_clientCallback = callback;
    g_audioThreadStarted = true;
    StartHostThread(AudioThread);
}

void XAudioSubmitFrame(void* samples)
{
    // A free buffer: one audout has finished with, waiting if all are queued.
    while (g_free.empty())
    {
        AudioOutBuffer* released = nullptr;
        uint32_t count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, UINT64_MAX)))
            return;
        if (released != nullptr)
            g_free.push_back(released);
    }
    AudioOutBuffer* buffer = g_free.back();
    g_free.pop_back();

    // Front left/right, centre on both at 0.75, LFE dropped, surrounds to their side.
    auto floatSamples = reinterpret_cast<be<float>*>(samples);
    auto* pcm = static_cast<int16_t*>(buffer->buffer);
    for (size_t i = 0; i < XAUDIO_NUM_SAMPLES; i++)
    {
        float ch0 = floatSamples[0 * XAUDIO_NUM_SAMPLES + i];
        float ch1 = floatSamples[1 * XAUDIO_NUM_SAMPLES + i];
        float ch2 = floatSamples[2 * XAUDIO_NUM_SAMPLES + i];
        float ch4 = floatSamples[4 * XAUDIO_NUM_SAMPLES + i];
        float ch5 = floatSamples[5 * XAUDIO_NUM_SAMPLES + i];
        pcm[i * 2 + 0] = int16_t(std::clamp(ch0 + ch2 * 0.75f + ch4, -1.0f, 1.0f) * 32767.0f);
        pcm[i * 2 + 1] = int16_t(std::clamp(ch1 + ch2 * 0.75f + ch5, -1.0f, 1.0f) * 32767.0f);
    }
    if (R_FAILED(audoutAppendAudioOutBuffer(buffer)))
        g_free.push_back(buffer);
}

#endif
