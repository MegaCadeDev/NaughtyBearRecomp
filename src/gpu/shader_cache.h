// Translated shader cache produced by XenosRecomp (tools/translate_shaders.sh).
// Same layout as Unleashed Recompiled's shader_cache.h.
#pragma once

#include <cstddef>
#include <cstdint>

struct ShaderCacheEntry
{
    const uint64_t hash;
    const uint32_t dxilOffset;
    const uint32_t dxilSize;
    const uint32_t spirvOffset;
    const uint32_t spirvSize;
    const uint32_t specConstantsMask;
    struct GuestShader* guestShader;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
