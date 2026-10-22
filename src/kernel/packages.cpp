#include <stdafx.h>
#include "packages.h"
#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <os/logger.h>

// Package layout (big-endian):
//   0x00  0x03783336 ("\3x36")
//   0x34  offset (from 0x20) of the storage descriptor
//   0x40  offset (from 0x20) of the payload
// Storage descriptor: type (0 stored, 2 compressed), uncompressed size, chunk size,
// chunk count, ?, chunk count again, then the compressed size of each chunk. Chunks are
// consecutive in the payload and each decompresses on its own to the chunk size (the last
// one to the remainder). A chunk whose compressed size equals its size is stored as is.
//
// The game decompresses with XMemCreateDecompressionContext(XMEMCODEC_LZX,
// { 0, WindowSize 1 MB, CompressionPartitionSize 64 KB }) and XMemDecompress, both in its
// statically linked XDK code (called from 0x82B81C68 and 0x82B81D30).
namespace packages
{
    namespace
    {
        constexpr uint32_t PACKAGE_MAGIC = 0x03783336;
        constexpr uint32_t XMEM_CREATE_DECOMPRESSION_CONTEXT = 0x830B5480;
        constexpr uint32_t XMEM_DECOMPRESS = 0x830B5458;
        constexpr uint32_t XMEMCODEC_LZX = 1;

        uint32_t Load32(const uint8_t* data) { return ByteSwap(*(const uint32_t*)data); }

        // A decompression context and guest buffers, one per thread.
        struct Decompressor
        {
            uint32_t context = 0;
            uint8_t* buffers = nullptr; // source, then destination, then the size word
            uint32_t capacity = 0;

            bool Init(uint32_t chunkSize)
            {
                if (context == 0)
                {
                    auto* params = (be<uint32_t>*)g_userHeap.Alloc(16);
                    params[0] = 0;       // Flags
                    params[1] = 0x100000; // WindowSize
                    params[2] = 0x10000;  // CompressionPartitionSize
                    params[3] = 0;        // the context, out
                    uint32_t result = GuestToHostFunction<uint32_t>(XMEM_CREATE_DECOMPRESSION_CONTEXT, XMEMCODEC_LZX,
                        g_memory.MapVirtual(params), 0u, g_memory.MapVirtual(params + 3));
                    context = params[3];
                    g_userHeap.Free(params);
                    if (int32_t(result) < 0 || context == 0)
                    {
                        LOGFN_ERROR("Packages: XMemCreateDecompressionContext failed (0x{:08X})", result);
                        context = 0;
                        return false;
                    }
                }

                // Compressed chunks can be slightly larger than the data they hold.
                uint32_t needed = chunkSize * 2 + 0x10000;
                if (capacity < needed)
                {
                    if (buffers != nullptr)
                        g_userHeap.Free(buffers);
                    buffers = (uint8_t*)g_userHeap.AllocPhysical(needed + 16, 128);
                    capacity = needed;
                }
                return buffers != nullptr;
            }

            bool Decompress(const uint8_t* source, uint32_t sourceSize, uint8_t* destination, uint32_t destinationSize)
            {
                uint32_t half = (capacity - 0x10000) / 2;
                if (sourceSize > half + 0x10000 || destinationSize > half)
                    return false;
                uint8_t* guestSource = buffers;
                uint8_t* guestDestination = buffers + half + 0x10000;
                auto* guestSize = (be<uint32_t>*)(buffers + capacity);
                memcpy(guestSource, source, sourceSize);
                *guestSize = destinationSize;
                uint32_t result = GuestToHostFunction<uint32_t>(XMEM_DECOMPRESS, context, g_memory.MapVirtual(guestDestination),
                    g_memory.MapVirtual(guestSize), g_memory.MapVirtual(guestSource), sourceSize);
                if (int32_t(result) < 0 || *guestSize != destinationSize)
                    return false;
                memcpy(destination, guestDestination, destinationSize);
                return true;
            }
        };
    }

    bool Read(const std::filesystem::path& path, std::vector<uint8_t>& contents)
    {
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (data.size() < 0x60 || Load32(data.data()) != PACKAGE_MAGIC)
            return false;

        uint64_t descriptor = 0x20 + uint64_t(Load32(data.data() + 0x34));
        uint64_t payload = 0x20 + uint64_t(Load32(data.data() + 0x40));
        if (descriptor + 24 > data.size() || payload > data.size())
            return false;

        const uint8_t* storage = data.data() + descriptor;
        uint32_t type = Load32(storage);
        uint32_t size = Load32(storage + 4);

        if (type == 0)
        {
            if (payload + size > data.size())
                return false;
            contents.assign(data.begin() + payload, data.begin() + payload + size);
            return true;
        }
        if (type != 2)
        {
            LOGFN_WARNING("Packages: {} has unknown storage type {}", path.filename().string(), type);
            return false;
        }

        uint32_t chunkSize = Load32(storage + 8);
        uint32_t chunkCount = Load32(storage + 12);
        if (chunkSize == 0 || chunkCount == 0 || descriptor + 24 + uint64_t(chunkCount) * 4 > data.size())
            return false;

        thread_local Decompressor decompressor;
        if (!decompressor.Init(chunkSize))
            return false;

        contents.resize(size);
        uint64_t sourceOffset = payload;
        for (uint32_t chunk = 0; chunk < chunkCount; chunk++)
        {
            uint32_t compressedSize = Load32(storage + 24 + chunk * 4);
            uint64_t outputOffset = uint64_t(chunk) * chunkSize;
            if (outputOffset >= size || sourceOffset + compressedSize > data.size())
                return false;
            uint32_t outputSize = uint32_t(std::min<uint64_t>(chunkSize, size - outputOffset));
            if (compressedSize == outputSize)
            {
                // Stored as is: compression didn't make it smaller.
                memcpy(contents.data() + outputOffset, data.data() + sourceOffset, outputSize);
            }
            else if (!decompressor.Decompress(data.data() + sourceOffset, compressedSize, contents.data() + outputOffset, outputSize))
            {
                LOGFN_WARNING("Packages: could not decompress chunk {} of {}", chunk, path.filename().string());
                return false;
            }
            sourceOffset += compressedSize;
        }
        return true;
    }
}
