// Used when no shader cache has been generated yet (tools/translate_shaders.sh).
#include <gpu/shader_cache.h>

ShaderCacheEntry g_shaderCacheEntries[] = { { 0, 0, 0, 0, 0, 0, nullptr } };
const size_t g_shaderCacheEntryCount = 0;
const uint8_t g_compressedSpirvCache[] = { 40, 181, 47, 253, 32, 0, 1, 0, 0 };
const size_t g_spirvCacheCompressedSize = 9;
const size_t g_spirvCacheDecompressedSize = 0;
