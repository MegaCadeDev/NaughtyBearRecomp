// The layout of the game's data packages (lu/*.lu), shared by the runtime (src/kernel/
// packages.cpp, which decompresses with the game's own XMemDecompress) and the build-time
// shader precache tool (tools/shader_precache, with its own LZX decompressor).
//
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
// { 0, WindowSize 1 MB, CompressionPartitionSize 64 KB }) and XMemDecompress.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace package_file
{
    constexpr uint32_t MAGIC = 0x03783336;
    constexpr uint32_t LZX_WINDOW_SIZE = 0x100000;

    // Decompresses one chunk: (source, source size, destination, destination size) -> success.
    using Decompress = std::function<bool(const uint8_t*, uint32_t, uint8_t*, uint32_t)>;

    enum class Result
    {
        Ok,
        NotAPackage,
        UnknownStorage,
        Corrupt,
        DecompressFailed,
    };

    inline uint32_t Load32(const uint8_t* data)
    {
        return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
    }

    // Reads a package's contents (its payload, decompressed).
    inline Result Read(const std::filesystem::path& path, std::vector<uint8_t>& contents, const Decompress& decompress)
    {
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (data.size() < 0x60 || Load32(data.data()) != MAGIC)
            return Result::NotAPackage;

        uint64_t descriptor = 0x20 + uint64_t(Load32(data.data() + 0x34));
        uint64_t payload = 0x20 + uint64_t(Load32(data.data() + 0x40));
        if (descriptor + 24 > data.size() || payload > data.size())
            return Result::Corrupt;

        const uint8_t* storage = data.data() + descriptor;
        uint32_t type = Load32(storage);
        uint32_t size = Load32(storage + 4);

        if (type == 0)
        {
            if (payload + size > data.size())
                return Result::Corrupt;
            contents.assign(data.begin() + payload, data.begin() + payload + size);
            return Result::Ok;
        }
        if (type != 2)
            return Result::UnknownStorage;

        uint32_t chunkSize = Load32(storage + 8);
        uint32_t chunkCount = Load32(storage + 12);
        if (chunkSize == 0 || chunkCount == 0 || descriptor + 24 + uint64_t(chunkCount) * 4 > data.size())
            return Result::Corrupt;

        contents.resize(size);
        uint64_t sourceOffset = payload;
        for (uint32_t chunk = 0; chunk < chunkCount; chunk++)
        {
            uint32_t compressedSize = Load32(storage + 24 + chunk * 4);
            uint64_t outputOffset = uint64_t(chunk) * chunkSize;
            if (outputOffset >= size || sourceOffset + compressedSize > data.size())
                return Result::Corrupt;
            uint32_t outputSize = uint32_t(std::min<uint64_t>(chunkSize, size - outputOffset));
            if (compressedSize == outputSize)
            {
                // Stored as is: compression didn't make it smaller.
                std::copy_n(data.data() + sourceOffset, outputSize, contents.data() + outputOffset);
            }
            else if (!decompress(data.data() + sourceOffset, compressedSize, contents.data() + outputOffset, outputSize))
            {
                return Result::DecompressFailed;
            }
            sourceOffset += compressedSize;
        }
        return Result::Ok;
    }
}
