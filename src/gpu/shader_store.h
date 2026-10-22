#pragma once

// The translated shader cache, kept on disk (cache/shaders.bin next to the save folder).
//
// On the first run, Precache translates every shader in the game: it decompresses the
// data packages (lu/*.lu) and scans them and the executable for shader containers. Any
// shader that still turns up later is translated when the game creates it. The cache is
// rebuilt when the translator or the game data changes.
namespace shader_store
{
    struct Shader
    {
        std::vector<uint8_t> spirv;
        uint32_t specConstantsMask = 0;
    };

    // Loads the cache from disk.
    void Init();

    // Translates the whole game's shaders if the cache doesn't cover them yet. Runs on a
    // guest thread once the game is up (package decompression uses the game's code);
    // progress (0..1) is reported as it goes.
    void Precache(const std::function<void(float)>& progress);

    // The translated shader for a container hash, or nullptr.
    const Shader* Find(uint64_t hash);

    // Makes sure the shader in this (guest) container is translated; translates it now if not.
    void Ensure(const uint8_t* container, uint64_t hash);
}
