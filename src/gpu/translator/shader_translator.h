#pragma once

// Translates the game's Xbox 360 shaders for the host: XenosRecomp turns a shader container
// into HLSL and DXC compiles that to SPIR-V. Built as its own library (XenosRecomp has
// its own byte-order helpers that would clash with the runtime's).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace shader_translator
{
    // Size of the shader container at data, or 0 if there isn't a valid one there.
    size_t ContainerSize(const uint8_t* data, size_t available);

    // Thread-safe; each thread keeps its own compiler.
    bool Translate(const uint8_t* container, std::vector<uint8_t>& spirv, uint32_t& specConstantsMask, std::string& error);

    // Changes whenever translated shaders would change (the translator or its HLSL header).
    uint64_t Version();
}
