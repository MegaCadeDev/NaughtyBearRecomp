// Which data package each piece of guest memory was loaded from, so meshes can be known by
// the package they belong to (naughtybear, ninjabear, ...) rather than by hash.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace origins
{
#if defined(__PROSPERO__)
    // Only mods need this; the PS5 version has none.
    inline void NoteRead(const std::filesystem::path&, uint32_t, uint32_t) {}
    inline void NoteDecompress(uint32_t, uint32_t, uint32_t) {}
    inline void NoteCopy(uint32_t, uint32_t, uint32_t) {}
    inline std::string Find(uint32_t) { return {}; }
#else
    // A data package (lu/*.lu) was read into guest memory.
    void NoteRead(const std::filesystem::path& file, uint32_t address, uint32_t size);

    // Guest memory was decompressed from source to destination: the output comes from the
    // same package as the input.
    void NoteDecompress(uint32_t source, uint32_t destination, uint32_t size);

    // Guest memory was copied: a large copy of package data is package data too.
    void NoteCopy(uint32_t source, uint32_t destination, uint32_t size);

    // The package (file name without extension) the memory at address came from, or empty.
    std::string Find(uint32_t address);
#endif
}
