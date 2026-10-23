// The translated shader cache file (cache/shaders.bin), shared by the runtime (src/gpu/
// shader_store.cpp) and the build-time precache tool (tools/shader_precache).
//
// File: a header, then the zstd-compressed entries: hash, specialization constants mask,
// SPIR-V size, SPIR-V.
#pragma once

#include <xxhash.h>
#include <zstd.h>
#include <fmt/format.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace shader_cache_file
{
    constexpr uint32_t MAGIC = 0x4853424E; // "NBSH"
    constexpr uint32_t FORMAT = 1;

    struct Header
    {
        uint32_t magic;
        uint32_t format;
        uint64_t translatorVersion;
        uint64_t dataFingerprint; // the game data the cache was precached from, 0 if not yet
        uint32_t count;
        uint32_t uncompressedSize;
    };

    struct Entry
    {
        uint64_t hash;
        uint32_t specConstantsMask;
        const std::vector<uint8_t>* spirv;
    };

    // Identifies the game data: names, sizes and times of the packages and the executable.
    inline uint64_t DataFingerprint(const std::filesystem::path& gameDirectory)
    {
        std::vector<std::string> entries;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(gameDirectory / "lu", ec))
        {
            auto time = entry.last_write_time(ec).time_since_epoch().count();
            entries.push_back(fmt::format("{}:{}:{}", entry.path().filename().string(), entry.file_size(ec), time));
        }
        entries.push_back(fmt::format("xex:{}", std::filesystem::file_size(gameDirectory / "Default.xex", ec)));
        std::sort(entries.begin(), entries.end());
        std::string joined;
        for (const auto& entry : entries)
            joined += entry + "\n";
        return XXH3_64bits(joined.data(), joined.size()) | 1;
    }

    // Writes the cache atomically (a temporary file, then a rename).
    inline bool Write(const std::filesystem::path& path, uint64_t translatorVersion, uint64_t fingerprint, const std::vector<Entry>& entries)
    {
        std::vector<uint8_t> body;
        for (const Entry& entry : entries)
        {
            uint32_t size = uint32_t(entry.spirv->size());
            size_t offset = body.size();
            body.resize(offset + 16 + size);
            memcpy(body.data() + offset, &entry.hash, 8);
            memcpy(body.data() + offset + 8, &entry.specConstantsMask, 4);
            memcpy(body.data() + offset + 12, &size, 4);
            memcpy(body.data() + offset + 16, entry.spirv->data(), size);
        }

        std::vector<uint8_t> compressed(ZSTD_compressBound(body.size()));
        size_t compressedSize = ZSTD_compress(compressed.data(), compressed.size(), body.data(), body.size(), 6);
        if (ZSTD_isError(compressedSize))
            return false;

        Header header{ MAGIC, FORMAT, translatorVersion, fingerprint, uint32_t(entries.size()), uint32_t(body.size()) };
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        auto temporary = path;
        temporary += ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary);
            file.write((const char*)&header, sizeof(header));
            file.write((const char*)compressed.data(), compressedSize);
            if (!file)
                return false;
        }
        std::filesystem::rename(temporary, path, ec);
        return !ec;
    }

    // Finds the shader containers in data, by their magic, and adds the ones not yet in found
    // (and that known() doesn't already have) by content hash. containerSize gives a
    // container's size, or 0 if it isn't one.
    template<typename ContainerSize, typename Known>
    void Scan(const uint8_t* data, size_t size, ContainerSize containerSize, Known known, std::map<uint64_t, std::vector<uint8_t>>& found)
    {
        for (size_t i = 0; i + 64 <= size; i += 4)
        {
            if (data[i] != 0x10 || data[i + 1] != 0x2A || data[i + 2] != 0x11)
                continue;
            size_t length = containerSize(data + i, size - i);
            if (length == 0)
                continue;
            uint64_t hash = XXH3_64bits(data + i, length);
            if (!found.count(hash) && !known(hash))
                found.emplace(hash, std::vector<uint8_t>(data + i, data + i + length));
            i += (length & ~size_t(3)) - 4;
        }
    }
}
