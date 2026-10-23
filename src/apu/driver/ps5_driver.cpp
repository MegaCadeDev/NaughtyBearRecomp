// PS5 audio output through sceAudioOut: a 48 kHz stereo port taking 256 samples at a time,
// the game's XAudio frame size, as UnleashedRecomp-Prospero opens it (GPL-3.0). Frames are
// six channels (5.1) of big-endian floats, channel-major; they're downmixed to stereo as on
// PC. sceAudioOutOutput blocks until the port has room, which paces the audio thread.
#if defined(__PROSPERO__)

#include <stdafx.h>
#include <apu/audio.h>
#include <cpu/guest_thread.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <os/logger.h>

extern "C"
{
    int32_t sceAudioOutInit(void);
    int32_t sceAudioOutOpen(int32_t userId, int32_t type, int32_t index, uint32_t length, uint32_t frequency, uint32_t format);
    int32_t sceAudioOutOutput(int32_t handle, const void* samples);
}

namespace
{
    constexpr int32_t SCE_USER_SERVICE_USER_ID_SYSTEM = 0xFF;
    constexpr int32_t SCE_AUDIO_OUT_PORT_TYPE_MAIN = 0;
    constexpr uint32_t SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO = 1;

    int32_t g_port = -1;
    PPCFunc* g_clientCallback{};
    uint32_t g_clientCallbackParam{}; // pointer in guest memory
    bool g_audioThreadStarted = false;

    void AudioThread()
    {
        GuestThreadContext ctx(0);
        while (true)
        {
            // The game mixes a frame and submits it (XAudioSubmitFrame), which blocks until
            // the port takes it.
            ctx.ppcContext.r3.u32 = g_clientCallbackParam;
            g_clientCallback(ctx.ppcContext, g_memory.base);
        }
    }
}

void XAudioInitializeSystem()
{
    sceAudioOutInit();
    g_port = sceAudioOutOpen(SCE_USER_SERVICE_USER_ID_SYSTEM, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, XAUDIO_NUM_SAMPLES, XAUDIO_SAMPLES_HZ,
        SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (g_port < 0)
        LOGFN_ERROR("Audio: sceAudioOutOpen failed (0x{:08X})", uint32_t(g_port));
    else
        LOGFN("Audio: {} Hz, 2 channels", XAUDIO_SAMPLES_HZ);
}

void XAudioRegisterClient(PPCFunc* callback, uint32_t param)
{
    if (g_port < 0 || g_audioThreadStarted)
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
    auto floatSamples = reinterpret_cast<be<float>*>(samples);

    // Front left/right, centre on both at 0.75, LFE dropped, surrounds to their side.
    std::array<int16_t, 2 * XAUDIO_NUM_SAMPLES> pcm;
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
    sceAudioOutOutput(g_port, pcm.data());
}

#endif
