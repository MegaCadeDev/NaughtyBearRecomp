#include <stdafx.h>
#include "packages.h"
#include "package_file.h"
#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <os/logger.h>

// The package layout is in package_file.h. Decompression uses the game's statically linked
// XMemCreateDecompressionContext and XMemDecompress (called from 0x82B81C68 and 0x82B81D30).
namespace packages
{
    namespace
    {
        constexpr uint32_t XMEM_CREATE_DECOMPRESSION_CONTEXT = 0x830B5480;
        constexpr uint32_t XMEM_DECOMPRESS = 0x830B5458;
        constexpr uint32_t XMEMCODEC_LZX = 1;

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
        thread_local Decompressor decompressor;
        auto result = package_file::Read(path, contents, [&](const uint8_t* source, uint32_t sourceSize, uint8_t* destination, uint32_t size)
            {
                return decompressor.Init(0x100000) && decompressor.Decompress(source, sourceSize, destination, size);
            });
        if (result == package_file::Result::UnknownStorage)
            LOGFN_WARNING("Packages: {} has an unknown storage type", path.filename().string());
        else if (result == package_file::Result::DecompressFailed)
            LOGFN_WARNING("Packages: could not decompress {}", path.filename().string());
        return result == package_file::Result::Ok;
    }
}
