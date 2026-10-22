// Audio output through SDL2, ported from Unleashed Recompiled's driver. The game's XAudio
// render client is called every 256 samples (48 kHz) on an audio thread; each call mixes
// a frame and submits it through XAudioSubmitRenderDriverFrame. Frames are six channels
// (5.1) of big-endian floats, channel-major; they are queued to SDL, downmixed to stereo
// unless NB_SURROUND=1 and the device has six channels. NB_VOLUME=0..1 scales the output.
#include <stdafx.h>
#include <apu/audio.h>
#include <cpu/guest_thread.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <os/logger.h>
#include <SDL.h>

static PPCFunc* g_clientCallback{};
static uint32_t g_clientCallbackParam{}; // pointer in guest memory
static SDL_AudioDeviceID g_audioDevice{};
static bool g_downMixToStereo = true;
static float g_volume = 1.0f;

static void CreateAudioDevice()
{
    if (g_audioDevice != 0)
        SDL_CloseAudioDevice(g_audioDevice);

    bool surround = getenv("NB_SURROUND") != nullptr;
    SDL_AudioSpec desired{}, obtained{};
    desired.freq = XAUDIO_SAMPLES_HZ;
    desired.format = AUDIO_F32SYS;
    desired.channels = surround ? XAUDIO_NUM_CHANNELS : 2;
    desired.samples = XAUDIO_NUM_SAMPLES;
    g_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, surround ? SDL_AUDIO_ALLOW_CHANNELS_CHANGE : 0);

    if (g_audioDevice != 0 && obtained.channels != 2 && obtained.channels != XAUDIO_NUM_CHANNELS)
    {
        SDL_CloseAudioDevice(g_audioDevice);
        desired.channels = 2;
        g_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    }

    if (g_audioDevice == 0)
    {
        LOGFN_ERROR("Failed to open audio device: {}", SDL_GetError());
        return;
    }

    g_downMixToStereo = obtained.channels == 2;
    LOGFN("Audio: {} Hz, {} channels", obtained.freq, obtained.channels);
}

void XAudioInitializeSystem()
{
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "Naughty Bear");

    if (const char* volume = getenv("NB_VOLUME"))
        g_volume = std::clamp(float(std::atof(volume)), 0.0f, 1.0f);

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
    {
        LOGFN_ERROR("Failed to init audio subsystem: {}", SDL_GetError());
        return;
    }

    CreateAudioDevice();
}

static std::unique_ptr<std::thread> g_audioThread;
static std::atomic<bool> g_audioThreadShouldExit;

static void AudioThread()
{
    using namespace std::chrono_literals;

    GuestThreadContext ctx(0);

    size_t channels = g_downMixToStereo ? 2 : XAUDIO_NUM_CHANNELS;

    while (!g_audioThreadShouldExit)
    {
        uint32_t queuedAudioSize = SDL_GetQueuedAudioSize(g_audioDevice);
        constexpr size_t MAX_LATENCY = 10;
        const size_t callbackAudioSize = channels * XAUDIO_NUM_SAMPLES * sizeof(float);

        if ((queuedAudioSize / callbackAudioSize) <= MAX_LATENCY)
        {
            ctx.ppcContext.r3.u32 = g_clientCallbackParam;
            g_clientCallback(ctx.ppcContext, g_memory.base);
        }

        auto now = std::chrono::steady_clock::now();
        constexpr auto INTERVAL = 1000000000ns * XAUDIO_NUM_SAMPLES / XAUDIO_SAMPLES_HZ;
        auto next = now + (INTERVAL - now.time_since_epoch() % INTERVAL);

        std::this_thread::sleep_for(std::chrono::floor<std::chrono::milliseconds>(next - now));

        while (std::chrono::steady_clock::now() < next)
            std::this_thread::yield();
    }
}

void XAudioRegisterClient(PPCFunc* callback, uint32_t param)
{
    if (g_audioDevice == 0 || g_audioThread)
        return;

    auto* clientParam = static_cast<uint32_t*>(g_userHeap.Alloc(sizeof(param)));
    *clientParam = ByteSwap(param);
    g_clientCallbackParam = g_memory.MapVirtual(clientParam);
    g_clientCallback = callback;

    SDL_PauseAudioDevice(g_audioDevice, 0);
    g_audioThreadShouldExit = false;
    g_audioThread = std::make_unique<std::thread>(AudioThread);
}

void XAudioSubmitFrame(void* samples)
{
    auto floatSamples = reinterpret_cast<be<float>*>(samples);

    if (g_downMixToStereo)
    {
        // Front left/right, centre on both at 0.75, LFE dropped, surrounds to their side.
        std::array<float, 2 * XAUDIO_NUM_SAMPLES> audioFrames;

        for (size_t i = 0; i < XAUDIO_NUM_SAMPLES; i++)
        {
            float ch0 = floatSamples[0 * XAUDIO_NUM_SAMPLES + i];
            float ch1 = floatSamples[1 * XAUDIO_NUM_SAMPLES + i];
            float ch2 = floatSamples[2 * XAUDIO_NUM_SAMPLES + i];
            float ch4 = floatSamples[4 * XAUDIO_NUM_SAMPLES + i];
            float ch5 = floatSamples[5 * XAUDIO_NUM_SAMPLES + i];

            audioFrames[i * 2 + 0] = (ch0 + ch2 * 0.75f + ch4) * g_volume;
            audioFrames[i * 2 + 1] = (ch1 + ch2 * 0.75f + ch5) * g_volume;
        }

        SDL_QueueAudio(g_audioDevice, audioFrames.data(), sizeof(audioFrames));
    }
    else
    {
        std::array<float, XAUDIO_NUM_CHANNELS * XAUDIO_NUM_SAMPLES> audioFrames;

        for (size_t i = 0; i < XAUDIO_NUM_SAMPLES; i++)
        {
            for (size_t j = 0; j < XAUDIO_NUM_CHANNELS; j++)
                audioFrames[i * XAUDIO_NUM_CHANNELS + j] = floatSamples[j * XAUDIO_NUM_SAMPLES + i] * g_volume;
        }

        SDL_QueueAudio(g_audioDevice, audioFrames.data(), sizeof(audioFrames));
    }
}
