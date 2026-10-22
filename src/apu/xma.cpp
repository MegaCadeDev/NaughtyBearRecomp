// XMA decoder emulation. The context layout and the register interface follow Xenia's
// documentation of the hardware (src/xenia/apu/xma_context.h, xma_register_table.inc;
// BSD licence, see NOTICE.md); decoding uses FFmpeg's XMA2 decoder on whole 2 KB packets
// (patched not to hold samples back: patches/FFmpeg).
//
// The XAudio library allocates contexts with XMACreateContext, fills in their input
// buffers (2 KB packets), output ring (256-byte blocks of 16-bit big-endian PCM) and loop
// points, and writes bit masks to the decoder's Kick / Lock / Clear registers. Those are
// plain stores into guest memory here, so a worker thread swaps them with zero to pick
// the writes up. A kick decodes until the output ring is full or the input runs out.
// The library functions that write the registers are hooked too, so that a kick is
// decoded before the library goes on (as with the hardware); the thread is a fallback.
#include <stdafx.h>
#include "xma.h"
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <os/logger.h>

#ifdef NB_HAVE_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}
#endif

namespace xma
{
    namespace
    {
        constexpr uint32_t CONTEXT_COUNT = 320;
        constexpr uint32_t CONTEXT_SIZE = 64;
        constexpr uint32_t BYTES_PER_PACKET = 2048;
        constexpr uint32_t BITS_PER_PACKET = BYTES_PER_PACKET * 8;
        constexpr uint32_t OUTPUT_BLOCK_SIZE = 256;

        // Decoder registers (dword index from 0x7FEA0000).
        constexpr uint32_t REGISTER_BASE = 0x7FEA0000;
        constexpr uint32_t REGISTER_CONTEXT_ARRAY_ADDRESS = 0x600;
        constexpr uint32_t REGISTER_KICK = 0x650;
        constexpr uint32_t REGISTER_LOCK = 0x690;
        constexpr uint32_t REGISTER_CLEAR = 0x6A0;
        constexpr uint32_t REGISTER_GROUP_SIZE = 10; // 10 x 32 contexts

        uint32_t* Register(uint32_t index)
        {
            return (uint32_t*)g_memory.Translate(REGISTER_BASE + index * 4);
        }

        uint8_t* TranslatePhysical(uint32_t address)
        {
            return (uint8_t*)g_memory.Translate(0xA0000000 | (address & 0x1FFFFFFF));
        }

        // XMA_CONTEXT_DATA, dword by dword (stored big-endian in guest memory).
        struct ContextData
        {
            uint32_t dword[16];

            uint32_t Get(uint32_t index, uint32_t shift, uint32_t bits) const { return (dword[index] >> shift) & ((1u << bits) - 1); }
            void Set(uint32_t index, uint32_t shift, uint32_t bits, uint32_t value)
            {
                uint32_t mask = ((1u << bits) - 1) << shift;
                dword[index] = (dword[index] & ~mask) | ((value << shift) & mask);
            }

            uint32_t InputBufferPacketCount(uint32_t buffer) const { return Get(buffer, 0, 12); }
            uint32_t LoopCount() const { return Get(0, 12, 8); }
            void SetLoopCount(uint32_t value) { Set(0, 12, 8, value); }
            bool InputBufferValid(uint32_t buffer) const { return Get(0, 20 + buffer, 1) != 0; }
            void SetInputBufferValid(uint32_t buffer, bool valid) { Set(0, 20 + buffer, 1, valid); }
            uint32_t OutputBlockCount() const { return Get(0, 22, 5); }
            uint32_t OutputWriteOffset() const { return Get(0, 27, 5); }
            void SetOutputWriteOffset(uint32_t value) { Set(0, 27, 5, value); }
            uint32_t SampleRate() const { return Get(1, 27, 2); }
            bool IsStereo() const { return Get(1, 29, 1) != 0; }
            bool OutputValid() const { return Get(1, 31, 1) != 0; }
            void SetOutputValid(bool valid) { Set(1, 31, 1, valid); }
            uint32_t ReadOffset() const { return Get(2, 0, 26); }
            void SetReadOffset(uint32_t value) { Set(2, 0, 26, value); }
            uint32_t LoopStart() const { return Get(3, 0, 26); }
            uint32_t LoopEnd() const { return Get(4, 0, 26); }
            uint32_t CurrentBuffer() const { return Get(4, 31, 1); }
            void SetCurrentBuffer(uint32_t value) { Set(4, 31, 1, value); }
            uint32_t InputBufferAddress(uint32_t buffer) const { return dword[5 + buffer]; }
            uint32_t OutputBufferAddress() const { return dword[7]; }
            uint32_t OutputReadOffset() const { return Get(9, 0, 5); }
        };

        ContextData Load(uint32_t address)
        {
            ContextData data;
            auto* source = (const uint32_t*)g_memory.Translate(address);
            for (uint32_t i = 0; i < 16; i++)
                data.dword[i] = ByteSwap(source[i]);
            return data;
        }

        // Writes back the dwords the decoder updates (0-2 and 4), leaving the rest to the guest.
        void Store(uint32_t address, const ContextData& data)
        {
            auto* destination = (uint32_t*)g_memory.Translate(address);
            for (uint32_t i : { 0u, 1u, 2u, 4u })
                destination[i] = ByteSwap(data.dword[i]);
        }

        struct Context
        {
            std::mutex mutex;
            bool allocated = false;
            bool enabled = false;
            // Decoded 16-bit big-endian PCM not yet written to the output ring.
            std::vector<uint8_t> pending;
#ifdef NB_HAVE_FFMPEG
            AVCodecContext* codec = nullptr;
            AVPacket* packet = nullptr;
            AVFrame* frame = nullptr;
            uint32_t codecSampleRate = 0;
            bool codecStereo = false;
            bool codecFed = false; // packets sent since the last flush
#endif
        };

        uint32_t g_contextArray = 0; // guest address
        Context g_contexts[CONTEXT_COUNT];
        std::mutex g_allocationMutex;

        int SampleRate(uint32_t id)
        {
            constexpr int rates[] = { 24000, 32000, 44100, 48000 };
            return rates[id & 3];
        }

#ifdef NB_HAVE_FFMPEG
        // Statistics, logged with NB_DEBUG_XMA=1.
        std::atomic<uint64_t> g_packetsDecoded, g_packetErrors, g_samplesDecoded, g_kicks;
        std::atomic<uint64_t> g_works, g_stopFull, g_stopEmpty, g_blocksWritten, g_noOutputValid;
        std::atomic<uint64_t> g_opens, g_clears, g_loopJumps, g_swaps;

        void CloseCodec(Context& context)
        {
            if (context.codec != nullptr)
                avcodec_free_context(&context.codec);
            context.codecFed = false;
        }

        bool OpenCodec(Context& context, int sampleRate, bool stereo)
        {
            if (context.codec != nullptr && context.codecSampleRate == uint32_t(sampleRate) && context.codecStereo == stereo)
                return true;
            CloseCodec(context);
            g_opens++;

            const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
            if (codec == nullptr)
            {
                static bool reported = false;
                if (!reported)
                    LOGN_ERROR("XMA: FFmpeg has no XMA2 decoder");
                reported = true;
                return false;
            }

            context.codec = avcodec_alloc_context3(codec);
            context.codec->sample_rate = sampleRate;
            av_channel_layout_default(&context.codec->ch_layout, stereo ? 2 : 1);
            // XMA2WAVEFORMATEX: one stream, with the channel mask.
            context.codec->extradata = (uint8_t*)av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE);
            context.codec->extradata_size = 34;
            context.codec->extradata[0] = 1;
            context.codec->extradata[2] = stereo ? 3 : 4;
            if (avcodec_open2(context.codec, codec, nullptr) < 0)
            {
                LOGN_ERROR("XMA: could not open the decoder");
                CloseCodec(context);
                return false;
            }
            if (context.packet == nullptr)
                context.packet = av_packet_alloc();
            if (context.frame == nullptr)
                context.frame = av_frame_alloc();
            context.codecSampleRate = sampleRate;
            context.codecStereo = stereo;
            return true;
        }

        // Moves the decoder's available frames to the pending PCM.
        void ReceiveFrames(Context& context)
        {
            while (avcodec_receive_frame(context.codec, context.frame) == 0)
            {
                AVFrame* frame = context.frame;
                uint32_t channels = context.codecStereo ? 2 : 1;
                g_samplesDecoded += frame->nb_samples;
                size_t start = context.pending.size();
                context.pending.resize(start + size_t(frame->nb_samples) * channels * 2);
                uint8_t* out = context.pending.data() + start;
                for (int i = 0; i < frame->nb_samples; i++)
                {
                    for (uint32_t channel = 0; channel < channels; channel++)
                    {
                        const float* plane = (const float*)frame->extended_data[std::min<uint32_t>(channel, frame->ch_layout.nb_channels - 1)];
                        int16_t sample = int16_t(std::clamp(plane[i] * 32767.0f, -32768.0f, 32767.0f));
                        *out++ = uint8_t(uint16_t(sample) >> 8);
                        *out++ = uint8_t(sample);
                    }
                }
                av_frame_unref(frame);
            }
        }

        void DecodePacket(Context& context, const uint8_t* data)
        {
            g_packetsDecoded++;

            av_packet_unref(context.packet);
            if (av_new_packet(context.packet, BYTES_PER_PACKET) < 0)
                return;
            memcpy(context.packet->data, data, BYTES_PER_PACKET);
            if (avcodec_send_packet(context.codec, context.packet) == 0)
                context.codecFed = true;
            else
                g_packetErrors++;
            ReceiveFrames(context);
        }

#endif

        // Writes whole blocks of pending PCM to the output ring; returns false when it's full.
        bool WriteOutput(Context& context, ContextData& data)
        {
            uint32_t blockCount = data.OutputBlockCount();
            if (blockCount == 0)
                return false;
            uint8_t* output = TranslatePhysical(data.OutputBufferAddress());
            uint32_t write = data.OutputWriteOffset() % blockCount;
            uint32_t read = data.OutputReadOffset() % blockCount;
            // One block stays free so that a full ring isn't mistaken for an empty one.
            uint32_t freeBlocks = (read + blockCount - write - 1) % blockCount;

            uint32_t pendingBlocks = uint32_t(context.pending.size() / OUTPUT_BLOCK_SIZE);
            uint32_t blocks = std::min(freeBlocks, pendingBlocks);
            for (uint32_t i = 0; i < blocks; i++)
            {
                memcpy(output + write * OUTPUT_BLOCK_SIZE, context.pending.data() + i * OUTPUT_BLOCK_SIZE, OUTPUT_BLOCK_SIZE);
                write = (write + 1) % blockCount;
            }
            context.pending.erase(context.pending.begin(), context.pending.begin() + size_t(blocks) * OUTPUT_BLOCK_SIZE);
#ifdef NB_HAVE_FFMPEG
            g_blocksWritten += blocks;
#endif
            data.SetOutputWriteOffset(write);
            return pendingBlocks <= freeBlocks;
        }

        void SwapInputBuffer(ContextData& data)
        {
#ifdef NB_HAVE_FFMPEG
            g_swaps++;
#endif
            data.SetInputBufferValid(data.CurrentBuffer(), false);
            data.SetCurrentBuffer(data.CurrentBuffer() ^ 1);
            data.SetReadOffset(0);
        }

        // Decodes for one kick: until the output ring is full or the input runs out.
        void Work(uint32_t id)
        {
            Context& context = g_contexts[id];
            std::lock_guard lock(context.mutex);
            if (!context.allocated || !context.enabled)
                return;
            context.enabled = false;

            uint32_t address = g_contextArray + id * CONTEXT_SIZE;
            ContextData data = Load(address);
#ifdef NB_HAVE_FFMPEG
            g_works++;
            if (!data.OutputValid())
                g_noOutputValid++;
#endif
            if (!data.OutputValid())
                return;

#ifdef NB_HAVE_FFMPEG
            for (uint32_t guard = 0; guard < 4096; guard++)
            {
                if (!WriteOutput(context, data))
                {
                    g_stopFull++;
                    break; // output ring full
                }

                if (!data.InputBufferValid(0) && !data.InputBufferValid(1))
                {
                    // Out of input: hand over a trailing partial block (padded with
                    // silence). The decoder keeps its state, in case more input follows.
                    size_t remainder = context.pending.size() % OUTPUT_BLOCK_SIZE;
                    if (remainder != 0)
                    {
                        context.pending.resize(context.pending.size() + OUTPUT_BLOCK_SIZE - remainder, 0);
                        continue;
                    }
                    if (context.pending.empty())
                        data.SetOutputValid(false);
                    g_stopEmpty++;
                    break;
                }

                uint32_t buffer = data.CurrentBuffer();
                if (!data.InputBufferValid(buffer))
                {
                    data.SetCurrentBuffer(buffer ^ 1);
                    data.SetReadOffset(0);
                    continue;
                }

                uint32_t packetCount = data.InputBufferPacketCount(buffer);
                uint32_t packetIndex = data.ReadOffset() / BITS_PER_PACKET;
                if (packetIndex >= packetCount)
                {
                    // End of this buffer: loop back, or move on to the other buffer.
                    uint32_t loopCount = data.LoopCount();
                    if (loopCount > 0 && data.LoopStart() < data.LoopEnd())
                    {
                        data.SetReadOffset(data.LoopStart());
                        if (loopCount < 255)
                            data.SetLoopCount(loopCount - 1);
                    }
                    else
                    {
                        SwapInputBuffer(data);
                    }
                    continue;
                }

                if (!OpenCodec(context, SampleRate(data.SampleRate()), data.IsStereo()))
                {
                    SwapInputBuffer(data);
                    continue;
                }

                const uint8_t* input = TranslatePhysical(data.InputBufferAddress(buffer));
                DecodePacket(context, input + size_t(packetIndex) * BYTES_PER_PACKET);

                uint32_t next = (packetIndex + 1) * BITS_PER_PACKET;
                uint32_t loopCount = data.LoopCount();
                if (loopCount > 0 && data.LoopStart() < data.LoopEnd() && next > data.LoopEnd())
                {
                    next = data.LoopStart();
                    g_loopJumps++;
                    if (loopCount < 255)
                        data.SetLoopCount(loopCount - 1);
                }
                data.SetReadOffset(next);
            }
#else
            // Without FFmpeg, consume the input as silence so the game doesn't wait forever.
            data.SetInputBufferValid(0, false);
            data.SetInputBufferValid(1, false);
            data.SetOutputValid(false);
#endif

            Store(address, data);
        }

        void Clear(uint32_t id)
        {
            Context& context = g_contexts[id];
            std::lock_guard lock(context.mutex);
#ifdef NB_HAVE_FFMPEG
            g_clears++;
#endif
            uint32_t address = g_contextArray + id * CONTEXT_SIZE;
            ContextData data = Load(address);
            data.SetInputBufferValid(0, false);
            data.SetInputBufferValid(1, false);
            data.SetOutputValid(false);
            data.SetOutputWriteOffset(0);
            data.Set(9, 0, 5, 0);
            Store(address, data);
            ((uint32_t*)g_memory.Translate(address))[9] = ByteSwap(data.dword[9]);
            context.pending.clear();
#ifdef NB_HAVE_FFMPEG
            if (context.codec != nullptr)
                avcodec_flush_buffers(context.codec);
            context.codecFed = false;
#endif
        }

        // Picks up writes to a group of bit-mask registers.
        template<typename Function>
        bool TakeRegisterBits(uint32_t firstRegister, Function function)
        {
            bool any = false;
            for (uint32_t group = 0; group < REGISTER_GROUP_SIZE; group++)
            {
                // The library writes these with byte-reversed stores, so they read natively.
                uint32_t bits = std::atomic_ref<uint32_t>(*Register(firstRegister + group)).exchange(0);
                for (uint32_t bit = 0; bits != 0; bit++, bits >>= 1)
                {
                    if ((bits & 1) != 0 && group * 32 + bit < CONTEXT_COUNT)
                    {
                        function(group * 32 + bit);
                        any = true;
                    }
                }
            }
            return any;
        }

        std::mutex g_registerMutex;
    }

    // Acts on Lock/Clear/Kick register writes, then decodes the kicked contexts.
    void ProcessRegisters()
        {
            std::lock_guard registerLock(g_registerMutex);
            TakeRegisterBits(REGISTER_LOCK, [](uint32_t id)
                {
                    std::lock_guard lock(g_contexts[id].mutex);
                    g_contexts[id].enabled = false;
                });
            TakeRegisterBits(REGISTER_CLEAR, [](uint32_t id) { Clear(id); });
            TakeRegisterBits(REGISTER_KICK, [](uint32_t id)
                {
#ifdef NB_HAVE_FFMPEG
                    g_kicks++;
#endif
                    std::lock_guard lock(g_contexts[id].mutex);
                    g_contexts[id].enabled = true;
                });

            for (uint32_t id = 0; id < CONTEXT_COUNT; id++)
            {
                if (g_contexts[id].enabled)
                    Work(id);
            }
        }

    namespace
    {
        void WorkerThread()
        {
            while (true)
            {
                ProcessRegisters();

#ifdef NB_HAVE_FFMPEG
                static const bool debug = getenv("NB_DEBUG_XMA") != nullptr;
                static auto lastReport = std::chrono::steady_clock::now();
                if (debug && std::chrono::steady_clock::now() - lastReport > std::chrono::seconds(2))
                {
                    lastReport = std::chrono::steady_clock::now();
                    LOGF_IMPL(Utility, "XMA", "{} kicks, {} packets ({} rejected), {} samples; {} works ({} no output), {} full, {} empty, {} blocks",
                        g_kicks.load(), g_packetsDecoded.load(), g_packetErrors.load(), g_samplesDecoded.load(), g_works.load(),
                        g_noOutputValid.load(), g_stopFull.load(), g_stopEmpty.load(), g_blocksWritten.load());
                    LOGF_IMPL(Utility, "XMA", "{} opens, {} clears, {} loop jumps, {} buffer swaps", g_opens.load(), g_clears.load(), g_loopJumps.load(), g_swaps.load());
                }
#endif

                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }
    }

    void Init()
    {
        void* array = g_userHeap.AllocPhysical(CONTEXT_COUNT * CONTEXT_SIZE, 256);
        memset(array, 0, CONTEXT_COUNT * CONTEXT_SIZE);
        g_contextArray = g_memory.MapVirtual(array);
        // Read with a byte-reversed load: store it natively. The hardware holds the
        // physical address.
        *Register(REGISTER_CONTEXT_ARRAY_ADDRESS) = g_contextArray & 0x1FFFFFFF;

#ifdef NB_HAVE_FFMPEG
        av_log_set_level(AV_LOG_QUIET);
#else
        LOGN_WARNING("XMA: built without FFmpeg; game audio will be silent");
#endif
        std::thread(WorkerThread).detach();
    }

    uint32_t AllocateContext()
    {
        std::lock_guard lock(g_allocationMutex);
        for (uint32_t id = 0; id < CONTEXT_COUNT; id++)
        {
            Context& context = g_contexts[id];
            std::lock_guard contextLock(context.mutex);
            if (!context.allocated)
            {
                context.allocated = true;
                context.enabled = false;
                context.pending.clear();
                uint32_t address = g_contextArray + id * CONTEXT_SIZE;
                memset(g_memory.Translate(address), 0, CONTEXT_SIZE);
                return address;
            }
        }
        LOGN_WARNING("XMA: out of contexts");
        return 0;
    }

    void ReleaseContext(uint32_t context)
    {
        if (context < g_contextArray || context >= g_contextArray + CONTEXT_COUNT * CONTEXT_SIZE)
            return;
        uint32_t id = (context - g_contextArray) / CONTEXT_SIZE;
        Context& state = g_contexts[id];
        std::lock_guard lock(state.mutex);
        state.allocated = false;
        state.enabled = false;
        state.pending.clear();
#ifdef NB_HAVE_FFMPEG
        if (state.codec != nullptr)
            avcodec_flush_buffers(state.codec);
        state.codecFed = false;
#endif
        memset(g_memory.Translate(context), 0, CONTEXT_SIZE);
    }
}

// The XAudio library's XMA register writers: run the decoder as soon as they return.
extern "C" PPC_FUNC(__imp__sub_82E96888); // kicks contexts
extern "C" PPC_FUNC(__imp__sub_82E96718); // locks contexts
extern "C" PPC_FUNC(__imp__sub_82E95EF0); // clears contexts

PPC_FUNC(sub_82E96888)
{
    __imp__sub_82E96888(ctx, base);
    xma::ProcessRegisters();
}

PPC_FUNC(sub_82E96718)
{
    __imp__sub_82E96718(ctx, base);
    xma::ProcessRegisters();
}

PPC_FUNC(sub_82E95EF0)
{
    __imp__sub_82E95EF0(ctx, base);
    xma::ProcessRegisters();
}
