// Host renderer for Naughty Bear, built on plume (Vulkan) following Unleashed Recompiled.
//
// The game's own Direct3D library keeps managing its state (it runs against the headless
// GPU), and the draw calls are mirrored here: at each draw the bound shaders, constants
// and vertex declaration are read from the guest device (offsets in docs/d3d_map.md),
// and the draw is replayed with the shaders translated by XenosRecomp. A frame is
// BeginTiling (clear) ... draws ... Swap (present).
//
// Not handled yet: textures (a white texture is bound), render states (alpha blending is
// assumed), depth, render target switches, and the buffer-based DrawVertices /
// DrawIndexedVertices.

#include <stdafx.h>
#include <gpu/shader_cache.h>
#include <gpu/video.h>
#include <hid/hid.h>
#include <kernel/memory.h>
#include <os/logger.h>
#include <plume_render_interface.h>
#include <smolv.h>
#include <zstd.h>
#include <bit>

#ifdef NB_TRACE_D3D
#include <gpu/trace.h>
#define TRACE_ENTER(f) nb::trace::Enter(f, ctx)
#define TRACE_LEAVE(f) nb::trace::Leave(f, ctx)
#else
#define TRACE_ENTER(f)
#define TRACE_LEAVE(f)
#endif

using namespace plume;

namespace plume
{
    extern std::unique_ptr<RenderInterface> CreateVulkanInterface(RenderWindow sdlWindow);
}

void HeadlessGpuOnDeviceCreated(uint32_t device);

// Original guest implementations (see docs/d3d_map.md).
extern "C" PPC_FUNC(__imp__sub_82F26FA0); // Direct3D_CreateDevice
extern "C" PPC_FUNC(__imp__sub_82F37138); // D3DDevice_BeginTiling
extern "C" PPC_FUNC(__imp__sub_82F33D50); // D3DDevice_Swap
extern "C" PPC_FUNC(__imp__sub_82F30050); // vertex shader construct
extern "C" PPC_FUNC(__imp__sub_82F2FF28); // pixel shader construct
extern "C" PPC_FUNC(__imp__sub_82F30188); // CreateVertexShader(container) -> object
extern "C" PPC_FUNC(__imp__sub_82F2FF60); // CreatePixelShader(container) -> object
extern "C" PPC_FUNC(__imp__sub_82F38388); // D3DDevice_DrawVerticesUP
extern "C" PPC_FUNC(__imp__sub_82F388E8); // D3DDevice_DrawIndexedVerticesUP
extern "C" PPC_FUNC(__imp__sub_82F2C640); // D3DDevice_SetTexture

namespace
{
    // Guest device fields (docs/d3d_map.md).
    constexpr uint32_t DEVICE_VS_CONSTANTS = 0x780;
    constexpr uint32_t DEVICE_PS_CONSTANTS = 0x1780;
    constexpr uint32_t DEVICE_VERTEX_DECLARATION = 0x2ED8;
    constexpr uint32_t DEVICE_PIXEL_SHADER = 0x3244;
    constexpr uint32_t DEVICE_VERTEX_SHADER = 0x3248;
    // Vertex declaration object: element count at +24, D3DVERTEXELEMENT9[12 bytes] at +52.
    constexpr uint32_t DECLARATION_COUNT = 24;
    constexpr uint32_t DECLARATION_ELEMENTS = 52;

    constexpr uint32_t RENDER_WIDTH = 1280;
    constexpr uint32_t RENDER_HEIGHT = 720;
    constexpr RenderFormat BACKBUFFER_FORMAT = RenderFormat::B8G8R8A8_UNORM;
    constexpr uint32_t BUFFER_COUNT = 2;
    constexpr uint32_t TEXTURE_DESCRIPTOR_SIZE = 65536;
    constexpr uint32_t SAMPLER_DESCRIPTOR_SIZE = 1024;
    constexpr uint32_t VS_CONSTANTS_SIZE = 256 * 16;
    constexpr uint32_t PS_CONSTANTS_SIZE = 224 * 16;
    constexpr uint64_t UPLOAD_BUFFER_SIZE = 64 * 1024 * 1024;

    enum NullDescriptor : uint32_t
    {
        NULL_TEXTURE_2D,
        NULL_TEXTURE_3D,
        NULL_TEXTURE_CUBE,
        NULL_COUNT
    };

    // Matches XenosRecomp's shader_common.h.
    struct SharedConstants
    {
        uint32_t texture2DIndices[16]{};
        uint32_t texture3DIndices[16]{};
        uint32_t textureCubeIndices[16]{};
        uint32_t samplerIndices[16]{};
        uint32_t booleans{};
        uint32_t swappedTexcoords{};
        float halfPixelOffsetX{};
        float halfPixelOffsetY{};
        float alphaThreshold{};
    };

    struct PushConstants
    {
        uint64_t vertexShaderConstants;
        uint64_t pixelShaderConstants;
        uint64_t sharedConstants;
    };

    // D3DDECLUSAGE and D3DDECLTYPE values of the Xbox 360 XDK.
    enum : uint32_t
    {
        D3DDECLUSAGE_POSITION = 0, D3DDECLUSAGE_BLENDWEIGHT = 1, D3DDECLUSAGE_BLENDINDICES = 2, D3DDECLUSAGE_NORMAL = 3,
        D3DDECLUSAGE_PSIZE = 4, D3DDECLUSAGE_TEXCOORD = 5, D3DDECLUSAGE_TANGENT = 6, D3DDECLUSAGE_BINORMAL = 7,
        D3DDECLUSAGE_TESSFACTOR = 8, D3DDECLUSAGE_POSITIONT = 9, D3DDECLUSAGE_COLOR = 10, D3DDECLUSAGE_FOG = 11,
        D3DDECLUSAGE_DEPTH = 12, D3DDECLUSAGE_SAMPLE = 13,
    };

    enum : uint32_t
    {
        D3DDECLTYPE_FLOAT1 = 0x2C83A4, D3DDECLTYPE_FLOAT2 = 0x2C23A5, D3DDECLTYPE_FLOAT3 = 0x2A23B9, D3DDECLTYPE_FLOAT4 = 0x1A23A6,
        D3DDECLTYPE_D3DCOLOR = 0x182886, D3DDECLTYPE_UBYTE4 = 0x1A2286, D3DDECLTYPE_UBYTE4_2 = 0x1A2386, D3DDECLTYPE_SHORT2 = 0x2C2359,
        D3DDECLTYPE_SHORT4 = 0x1A235A, D3DDECLTYPE_UBYTE4N = 0x1A2086, D3DDECLTYPE_UBYTE4N_2 = 0x1A2186, D3DDECLTYPE_SHORT2N = 0x2C2159,
        D3DDECLTYPE_SHORT4N = 0x1A215A, D3DDECLTYPE_USHORT2N = 0x2C2059, D3DDECLTYPE_USHORT4N = 0x1A205A, D3DDECLTYPE_UINT1 = 0x2C82A1,
        D3DDECLTYPE_UDEC3 = 0x2A2287, D3DDECLTYPE_DEC3N = 0x2A2187, D3DDECLTYPE_DEC3N_2 = 0x2A2190, D3DDECLTYPE_DEC3N_3 = 0x2A2390,
        D3DDECLTYPE_FLOAT16_2 = 0x2C235F, D3DDECLTYPE_FLOAT16_4 = 0x1A2360, D3DDECLTYPE_UNUSED = 0xFFFFFFFF,
    };

    // Vertex input locations; must match patches/XenosRecomp (Unleashed's table plus
    // BLENDWEIGHT1-3, which Naughty Bear uses).
    struct Location
    {
        uint32_t usage;
        uint32_t usageIndex;
        uint32_t location;
    };

    constexpr Location LOCATIONS[] =
    {
        { D3DDECLUSAGE_POSITION, 0, 0 }, { D3DDECLUSAGE_NORMAL, 0, 1 }, { D3DDECLUSAGE_TANGENT, 0, 2 }, { D3DDECLUSAGE_BINORMAL, 0, 3 },
        { D3DDECLUSAGE_TEXCOORD, 0, 4 }, { D3DDECLUSAGE_TEXCOORD, 1, 5 }, { D3DDECLUSAGE_TEXCOORD, 2, 6 }, { D3DDECLUSAGE_TEXCOORD, 3, 7 },
        { D3DDECLUSAGE_COLOR, 0, 8 }, { D3DDECLUSAGE_BLENDINDICES, 0, 9 }, { D3DDECLUSAGE_BLENDWEIGHT, 0, 10 }, { D3DDECLUSAGE_COLOR, 1, 11 },
        { D3DDECLUSAGE_TEXCOORD, 4, 12 }, { D3DDECLUSAGE_TEXCOORD, 5, 13 }, { D3DDECLUSAGE_TEXCOORD, 6, 14 }, { D3DDECLUSAGE_TEXCOORD, 7, 15 },
        { D3DDECLUSAGE_POSITION, 1, 15 },
        { D3DDECLUSAGE_BLENDWEIGHT, 1, 16 }, { D3DDECLUSAGE_BLENDWEIGHT, 2, 17 }, { D3DDECLUSAGE_BLENDWEIGHT, 3, 18 },
    };

    // Inputs a shader may declare without the declaration providing them read zeroes from
    // this stream (stride 0).
    constexpr uint32_t DUMMY_STREAM = 15;

    const char* ConvertDeclUsage(uint32_t usage)
    {
        constexpr const char* names[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
            "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE" };
        return usage < std::size(names) ? names[usage] : "UNKNOWN";
    }

    RenderFormat ConvertDeclType(uint32_t type)
    {
        switch (type)
        {
        case D3DDECLTYPE_FLOAT1: return RenderFormat::R32_FLOAT;
        case D3DDECLTYPE_FLOAT2: return RenderFormat::R32G32_FLOAT;
        case D3DDECLTYPE_FLOAT3: return RenderFormat::R32G32B32_FLOAT;
        case D3DDECLTYPE_FLOAT4: return RenderFormat::R32G32B32A32_FLOAT;
        case D3DDECLTYPE_D3DCOLOR: return RenderFormat::B8G8R8A8_UNORM;
        case D3DDECLTYPE_UBYTE4:
        case D3DDECLTYPE_UBYTE4_2: return RenderFormat::R8G8B8A8_UINT;
        case D3DDECLTYPE_SHORT2: return RenderFormat::R16G16_SINT;
        case D3DDECLTYPE_SHORT4: return RenderFormat::R16G16B16A16_SINT;
        case D3DDECLTYPE_UBYTE4N:
        case D3DDECLTYPE_UBYTE4N_2: return RenderFormat::R8G8B8A8_UNORM;
        case D3DDECLTYPE_SHORT2N: return RenderFormat::R16G16_SNORM;
        case D3DDECLTYPE_SHORT4N: return RenderFormat::R16G16B16A16_SNORM;
        case D3DDECLTYPE_USHORT2N: return RenderFormat::R16G16_UNORM;
        case D3DDECLTYPE_USHORT4N: return RenderFormat::R16G16B16A16_UNORM;
        case D3DDECLTYPE_UINT1:
        case D3DDECLTYPE_UDEC3:
        case D3DDECLTYPE_DEC3N:
        case D3DDECLTYPE_DEC3N_2:
        case D3DDECLTYPE_DEC3N_3: return RenderFormat::R32_UINT;
        case D3DDECLTYPE_FLOAT16_2: return RenderFormat::R16G16_FLOAT;
        case D3DDECLTYPE_FLOAT16_4: return RenderFormat::R16G16B16A16_FLOAT;
        default:
            LOGF_WARNING("Unknown vertex element type 0x{:X}", type);
            return RenderFormat::R32_FLOAT;
        }
    }


    // How one vertex element is rewritten into the host layout.
    enum class ElementConversion
    {
        Swap32,        // 32-bit words (floats, D3DCOLOR, packed 8-bit and 10-bit formats)
        Swap16,        // 16-bit components (normalized shorts, halves)
        ShortToFloat,  // non-normalized shorts: the GPU fetch yields floats with integer values
        UbyteToFloat,  // non-normalized bytes, same
    };

    struct ElementCopy
    {
        uint32_t sourceOffset;
        uint32_t destinationOffset;
        uint32_t components;   // for the *ToFloat conversions
        uint32_t sourceBytes;
        ElementConversion conversion;
    };

    struct VertexDeclaration
    {
        std::vector<RenderInputElement> inputElements;
        // Stream 0 is rewritten into a packed host layout of this stride.
        std::vector<ElementCopy> copies;
        uint32_t hostStride = 0;
        uint64_t hash = 0;
    };

    struct PipelineKey
    {
        uint64_t vertexShader;
        uint64_t pixelShader;
        uint64_t declaration;
        uint32_t stride;
        RenderPrimitiveTopology topology;
    };

    std::unique_ptr<RenderInterface> g_interface;
    std::unique_ptr<RenderDevice> g_device;
    std::unique_ptr<RenderCommandQueue> g_queue;
    std::unique_ptr<RenderSwapChain> g_swapChain;
    std::unique_ptr<RenderCommandList> g_commandList;
    std::unique_ptr<RenderCommandFence> g_fence;
    std::unique_ptr<RenderCommandSemaphore> g_acquireSemaphore;
    std::unique_ptr<RenderCommandSemaphore> g_renderSemaphore;
    std::unique_ptr<RenderPipelineLayout> g_pipelineLayout;
    std::unique_ptr<RenderDescriptorSet> g_textureDescriptorSet;
    std::unique_ptr<RenderDescriptorSet> g_samplerDescriptorSet;
    std::unique_ptr<RenderTexture> g_nullTextures[NULL_COUNT];
    std::unique_ptr<RenderTextureView> g_nullTextureViews[NULL_COUNT];
    std::unique_ptr<RenderSampler> g_defaultSampler;
    std::unique_ptr<RenderTexture> g_renderTarget;
    std::unique_ptr<RenderFramebuffer> g_renderTargetFramebuffer;
    std::unique_ptr<RenderBuffer> g_uploadBuffer;
    uint8_t* g_uploadMemory = nullptr;
    uint64_t g_uploadAddress = 0;
    uint64_t g_uploadOffset = 0;

    std::mutex g_videoMutex;
    uint32_t g_guestDevice = 0;
    bool g_inFrame = false;
    RenderColor g_clearColor = RenderColor(0.0f, 0.0f, 0.0f, 1.0f);

    // Shaders: guest shader object -> container hash, recorded by the constructors.
    std::mutex g_shaderMutex;
    ankerl::unordered_dense::map<uint32_t, uint64_t> g_shaderHashes;
    std::unique_ptr<uint8_t[]> g_shaderCache;
    ankerl::unordered_dense::map<uint64_t, const ShaderCacheEntry*> g_shaderCacheEntriesByHash;
    ankerl::unordered_dense::map<uint64_t, std::unique_ptr<RenderShader>> g_shaders;
    ankerl::unordered_dense::map<uint64_t, VertexDeclaration> g_declarations;
    ankerl::unordered_dense::map<uint64_t, std::unique_ptr<RenderPipeline>> g_pipelines;
    std::set<uint64_t> g_missingShaders;

    // NB_SCREENSHOT=path[:frame] saves the render target of that frame (default 600) as PPM.
    std::unique_ptr<RenderBuffer> g_readbackBuffer;
    std::string g_screenshotPath;
    uint64_t g_screenshotFrame = 600;
    uint64_t g_frameNumber = 0;

    // Texture objects bound per sampler (D3DDevice_SetTexture).
    uint32_t g_samplerTextures[32]{};

    // NB_DEBUG_DRAWS=frame logs every draw of that frame; NB_NO_BLEND=1 draws opaque.
    const uint64_t g_debugDrawsFrame = [] { const char* v = getenv("NB_DEBUG_DRAWS"); return v ? std::stoull(v) : 0ull; }();
    const bool g_noBlend = getenv("NB_NO_BLEND") != nullptr;
    uint64_t g_presentedFrames = 0;

    // Frame pacing. On the console, Swap waits for the vertical blank at 60 Hz, as many
    // times as the device's PresentationInterval says (Naughty Bear asks for 2: 30 fps).
    // Here the swap chain follows the host display's refresh rate instead, so frames are
    // paced to the console's rate explicitly. NB_FPS overrides it.
    constexpr uint32_t D3DPRESENT_PARAMETERS_INTERVAL = 13 * 4;
    constexpr uint32_t D3DPRESENT_INTERVAL_IMMEDIATE = 0x80000000;
    std::chrono::nanoseconds g_framePeriod = std::chrono::nanoseconds(1000000000 / 60);
    std::chrono::steady_clock::time_point g_nextFrame;

    void SetPresentationInterval(uint32_t interval)
    {
        double fps = 60.0;
        if (interval == D3DPRESENT_INTERVAL_IMMEDIATE)
            fps = 0.0;
        else if (interval >= 1 && interval <= 4)
            fps = 60.0 / interval;
        if (const char* overrideFps = getenv("NB_FPS"))
            fps = std::atof(overrideFps);

        g_framePeriod = fps > 0.0 ? std::chrono::nanoseconds(int64_t(1e9 / fps)) : std::chrono::nanoseconds(0);
        LOGFN("Video: presentation interval {}, pacing frames to {:.1f} fps", interval, fps);
    }

    void PaceFrame()
    {
        if (g_framePeriod.count() == 0)
            return;
        auto now = std::chrono::steady_clock::now();
        if (g_nextFrame.time_since_epoch().count() == 0 || now - g_nextFrame > g_framePeriod * 2)
            g_nextFrame = now; // first frame, or fell far behind: don't try to catch up
        else
            std::this_thread::sleep_until(g_nextFrame);
        g_nextFrame += g_framePeriod;
    }

    uint64_t g_drawsThisFrame = 0;
    uint64_t g_drawsSkippedThisFrame = 0;

    uint32_t Load32(uint32_t address) { return ByteSwap(*(uint32_t*)g_memory.Translate(address)); }
    uint16_t Load16(uint32_t address) { return ByteSwap(*(uint16_t*)g_memory.Translate(address)); }
    float LoadFloat(uint32_t address) { return std::bit_cast<float>(Load32(address)); }

    // Bump allocator in a host-visible buffer; reset every frame (frames are synchronous).
    std::pair<uint8_t*, uint64_t> Allocate(uint64_t size, uint64_t alignment = 256)
    {
        g_uploadOffset = (g_uploadOffset + alignment - 1) & ~(alignment - 1);
        if (g_uploadOffset + size > UPLOAD_BUFFER_SIZE)
        {
            LOGN_WARNING("Upload buffer full; frame truncated");
            return { nullptr, 0 };
        }
        uint64_t offset = g_uploadOffset;
        g_uploadOffset += size;
        return { g_uploadMemory + offset, offset };
    }

    // Copies guest data, swapping every 32-bit word (or 16-bit for 16-bit indices).
    template<typename T>
    std::pair<uint8_t*, uint64_t> UploadSwapped(uint32_t guest, uint64_t size)
    {
        auto [memory, offset] = Allocate(size);
        if (memory != nullptr)
        {
            auto* source = (const T*)g_memory.Translate(guest);
            auto* destination = (T*)memory;
            for (uint64_t i = 0; i < size / sizeof(T); i++)
                destination[i] = ByteSwap(source[i]);
        }
        return { memory, offset };
    }

    void LoadShaderCache()
    {
        g_shaderCache = std::make_unique<uint8_t[]>(g_spirvCacheDecompressedSize);
        ZSTD_decompress(g_shaderCache.get(), g_spirvCacheDecompressedSize, g_compressedSpirvCache, g_spirvCacheCompressedSize);
        for (size_t i = 0; i < g_shaderCacheEntryCount; i++)
            g_shaderCacheEntriesByHash[g_shaderCacheEntries[i].hash] = &g_shaderCacheEntries[i];
        LOGFN("Video: {} translated shaders in the cache", g_shaderCacheEntryCount);
    }

    RenderShader* GetShader(uint64_t hash)
    {
        if (auto it = g_shaders.find(hash); it != g_shaders.end())
            return it->second.get();

        auto entry = g_shaderCacheEntriesByHash.find(hash);
        if (entry == g_shaderCacheEntriesByHash.end())
        {
            if (g_missingShaders.insert(hash).second)
                LOGF_WARNING("Shader {:016x} is not in the cache; dump and translate it (docs/d3d_map.md)", hash);
            return nullptr;
        }

        const uint8_t* encoded = g_shaderCache.get() + entry->second->spirvOffset;
        std::vector<uint8_t> decoded(smolv::GetDecodedBufferSize(encoded, entry->second->spirvSize));
        if (!smolv::Decode(encoded, entry->second->spirvSize, decoded.data(), decoded.size()))
            return nullptr;

        auto shader = g_device->createShader(decoded.data(), decoded.size(), "main", RenderShaderFormat::SPIRV);
        return (g_shaders[hash] = std::move(shader)).get();
    }

    const VertexDeclaration* GetDeclaration(uint32_t object)
    {
        if (object == 0)
            return nullptr;

        uint32_t count = Load32(object + DECLARATION_COUNT);
        if (count == 0 || count > 32)
            return nullptr;
        uint64_t hash = XXH3_64bits(g_memory.Translate(object + DECLARATION_ELEMENTS), count * 12);
        if (auto it = g_declarations.find(hash); it != g_declarations.end())
            return &it->second;

        VertexDeclaration declaration;
        declaration.hash = hash;
        std::set<uint32_t> usedLocations;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t element = object + DECLARATION_ELEMENTS + i * 12;
            uint32_t stream = Load16(element);
            uint32_t offset = Load16(element + 2);
            uint32_t type = Load32(element + 4);
            uint8_t usage = *(uint8_t*)g_memory.Translate(element + 9);
            uint8_t usageIndex = *(uint8_t*)g_memory.Translate(element + 10);
            if (stream == 0xFF || type == D3DDECLTYPE_UNUSED)
                break;

            uint32_t location = ~0u;
            for (auto& l : LOCATIONS)
            {
                if (l.usage == usage && l.usageIndex == usageIndex)
                {
                    location = l.location;
                    break;
                }
            }
            if (location == ~0u || usedLocations.count(location))
                continue;
            usedLocations.insert(location);

            RenderFormat format = ConvertDeclType(type);
            ElementCopy copy{ offset, declaration.hostStride, 0, 4, ElementConversion::Swap32 };
            switch (type)
            {
            case D3DDECLTYPE_FLOAT2: copy.sourceBytes = 8; break;
            case D3DDECLTYPE_FLOAT3: copy.sourceBytes = 12; break;
            case D3DDECLTYPE_FLOAT4: copy.sourceBytes = 16; break;
            case D3DDECLTYPE_SHORT2N: case D3DDECLTYPE_USHORT2N: case D3DDECLTYPE_FLOAT16_2:
                copy = { offset, declaration.hostStride, 0, 4, ElementConversion::Swap16 };
                break;
            case D3DDECLTYPE_SHORT4N: case D3DDECLTYPE_USHORT4N: case D3DDECLTYPE_FLOAT16_4:
                copy = { offset, declaration.hostStride, 0, 8, ElementConversion::Swap16 };
                break;
            case D3DDECLTYPE_SHORT2:
                copy = { offset, declaration.hostStride, 2, 4, ElementConversion::ShortToFloat };
                format = RenderFormat::R32G32_FLOAT;
                break;
            case D3DDECLTYPE_SHORT4:
                copy = { offset, declaration.hostStride, 4, 8, ElementConversion::ShortToFloat };
                format = RenderFormat::R32G32B32A32_FLOAT;
                break;
            case D3DDECLTYPE_UBYTE4: case D3DDECLTYPE_UBYTE4_2:
                copy = { offset, declaration.hostStride, 4, 4, ElementConversion::UbyteToFloat };
                format = RenderFormat::R32G32B32A32_FLOAT;
                break;
            }
            // Unleashed Recompiled's shaders read float3 normals as raw bits (for R11G11B10 support).
            if ((usage == D3DDECLUSAGE_NORMAL || usage == D3DDECLUSAGE_TANGENT || usage == D3DDECLUSAGE_BINORMAL) && type == D3DDECLTYPE_FLOAT3)
                format = RenderFormat::R32G32B32_UINT;

            uint32_t hostBytes = copy.conversion == ElementConversion::ShortToFloat || copy.conversion == ElementConversion::UbyteToFloat
                ? copy.components * 4 : copy.sourceBytes;
            if (stream == 0)
            {
                offset = declaration.hostStride;
                declaration.copies.push_back(copy);
                declaration.hostStride += hostBytes;
            }

            declaration.inputElements.emplace_back(ConvertDeclUsage(usage), usageIndex, location, format, stream, offset);
        }

        // Every location a shader might declare gets an input, defaulting to zeroes.
        for (auto& l : LOCATIONS)
        {
            if (usedLocations.count(l.location))
                continue;
            usedLocations.insert(l.location);
            RenderFormat format = RenderFormat::R32_FLOAT;
            if (l.usage == D3DDECLUSAGE_NORMAL || l.usage == D3DDECLUSAGE_TANGENT || l.usage == D3DDECLUSAGE_BINORMAL || l.usage == D3DDECLUSAGE_BLENDINDICES)
                format = RenderFormat::R32_UINT;
            declaration.inputElements.emplace_back(ConvertDeclUsage(l.usage), l.usageIndex, l.location, format, DUMMY_STREAM, 0);
        }

        return &(g_declarations[hash] = std::move(declaration));
    }

    RenderPipeline* GetPipeline(const PipelineKey& key, RenderShader* vertexShader, RenderShader* pixelShader, const VertexDeclaration& declaration)
    {
        uint64_t hash = XXH3_64bits(&key, sizeof(key));
        if (auto it = g_pipelines.find(hash); it != g_pipelines.end())
            return it->second.get();

        RenderInputSlot slots[16];
        uint32_t slotCount = 0;
        bool seen[16]{};
        for (auto& element : declaration.inputElements)
        {
            if (seen[element.slotIndex])
                continue;
            seen[element.slotIndex] = true;
            slots[slotCount++] = RenderInputSlot(element.slotIndex, element.slotIndex == DUMMY_STREAM ? 0 : std::max<uint32_t>(declaration.hostStride, 4));
        }

        RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = g_pipelineLayout.get();
        desc.vertexShader = vertexShader;
        desc.pixelShader = pixelShader;
        desc.primitiveTopology = key.topology;
        desc.cullMode = RenderCullMode::NONE;
        desc.depthEnabled = false;
        desc.depthWriteEnabled = false;
        desc.depthClipEnabled = true;
        desc.renderTargetFormat[0] = BACKBUFFER_FORMAT;
        desc.renderTargetBlend[0] = g_noBlend ? RenderBlendDesc::Copy() : RenderBlendDesc::AlphaBlend();
        desc.renderTargetCount = 1;
        desc.inputElements = declaration.inputElements.data();
        desc.inputElementsCount = uint32_t(declaration.inputElements.size());
        desc.inputSlots = slots;
        desc.inputSlotsCount = slotCount;

        auto pipeline = g_device->createGraphicsPipeline(desc);
        return (g_pipelines[hash] = std::move(pipeline)).get();
    }

    void BeginFrame()
    {
        if (g_inFrame)
            return;
        g_inFrame = true;
        g_uploadOffset = 0;
        g_drawsThisFrame = 0;
        g_drawsSkippedThisFrame = 0;

        g_commandList->begin();
        RenderTextureBarrier barrier(g_renderTarget.get(), RenderTextureLayout::COLOR_WRITE);
        g_commandList->barriers(RenderBarrierStage::GRAPHICS, nullptr, 0, &barrier, 1);
        g_commandList->setFramebuffer(g_renderTargetFramebuffer.get());
        g_commandList->clearColor(0, g_clearColor);

        RenderViewport viewport(0.0f, 0.0f, float(RENDER_WIDTH), float(RENDER_HEIGHT), 0.0f, 1.0f);
        RenderRect scissor(0, 0, RENDER_WIDTH, RENDER_HEIGHT);
        g_commandList->setViewports(&viewport, 1);
        g_commandList->setScissors(&scissor, 1);
        g_commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
        g_commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);
        g_commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1);
        g_commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2);
        g_commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 3);
    }

    // Copies the frame to the swap chain and presents it.
    void EndFrame()
    {
        BeginFrame(); // frames with no draws still present the clear

        uint32_t index = 0;
        bool acquired = g_swapChain->getWidth() == RENDER_WIDTH && g_swapChain->getHeight() == RENDER_HEIGHT
            && g_swapChain->acquireTexture(g_acquireSemaphore.get(), &index);

        bool screenshot = !g_screenshotPath.empty() && ++g_frameNumber == g_screenshotFrame;
        if (screenshot)
        {
            RenderTextureBarrier toCopy(g_renderTarget.get(), RenderTextureLayout::COPY_SOURCE);
            g_commandList->barriers(RenderBarrierStage::COPY, nullptr, 0, &toCopy, 1);
            g_commandList->copyTextureRegion(
                RenderTextureCopyLocation::PlacedFootprint(g_readbackBuffer.get(), BACKBUFFER_FORMAT, RENDER_WIDTH, RENDER_HEIGHT, 1, RENDER_WIDTH),
                RenderTextureCopyLocation::Subresource(g_renderTarget.get()));
        }

        if (acquired)
        {
            RenderTexture* backBuffer = g_swapChain->getTexture(index);
            RenderTextureBarrier toCopy[] = {
                RenderTextureBarrier(g_renderTarget.get(), RenderTextureLayout::COPY_SOURCE),
                RenderTextureBarrier(backBuffer, RenderTextureLayout::COPY_DEST),
            };
            g_commandList->barriers(RenderBarrierStage::COPY, nullptr, 0, toCopy, 2);
            g_commandList->copyTexture(backBuffer, g_renderTarget.get());
            RenderTextureBarrier toPresent(backBuffer, RenderTextureLayout::PRESENT);
            g_commandList->barriers(RenderBarrierStage::NONE, nullptr, 0, &toPresent, 1);
        }
        g_commandList->end();

        const RenderCommandList* lists[] = { g_commandList.get() };
        RenderCommandSemaphore* wait[] = { g_acquireSemaphore.get() };
        RenderCommandSemaphore* signal[] = { g_renderSemaphore.get() };
        g_queue->executeCommandLists(lists, 1, acquired ? wait : nullptr, acquired ? 1 : 0, acquired ? signal : nullptr, acquired ? 1 : 0, g_fence.get());
        PaceFrame();
        if (acquired)
            g_swapChain->present(index, signal, 1);
        g_queue->waitForCommandFence(g_fence.get());

        if (screenshot)
        {
            // B8G8R8A8 -> binary PPM (RGB).
            auto* pixels = (const uint8_t*)g_readbackBuffer->map();
            std::ofstream file(g_screenshotPath, std::ios::binary);
            file << "P6\n" << RENDER_WIDTH << " " << RENDER_HEIGHT << "\n255\n";
            for (uint32_t i = 0; i < RENDER_WIDTH * RENDER_HEIGHT; i++)
            {
                char rgb[3] = { char(pixels[i * 4 + 2]), char(pixels[i * 4 + 1]), char(pixels[i * 4 + 0]) };
                file.write(rgb, 3);
            }
            g_readbackBuffer->unmap();
            LOGFN("Video: saved frame {} to {}", g_frameNumber, g_screenshotPath);
        }

        if (!acquired && g_swapChain->needsResize())
            g_swapChain->resize();
        g_inFrame = false;

        static uint64_t presented = 0;
        static auto lastReport = std::chrono::steady_clock::now();
        presented++;
        g_presentedFrames = presented;
        hid::OnFrame(presented);
        auto now = std::chrono::steady_clock::now();
        if (now - lastReport >= std::chrono::seconds(2))
        {
            LOGFN("Video: {} frames, last frame {} draws ({} skipped)", presented, g_drawsThisFrame, g_drawsSkippedThisFrame);
            lastReport = now;
        }
    }

    // ---- Textures ----
    //
    // Texture objects carry the GPU texture fetch constant at +0x1C (six dwords, layout as
    // documented by Xenia): dword0 type/pitch/tiled, dword1 format/endian/base address,
    // dword2 size, dword3 swizzle, dword5 dimension. Textures are decoded from guest memory
    // (untiled and endian-swapped) and cached by address and fetch constant.

    constexpr uint32_t TEXTURE_FETCH_CONSTANT = 0x1C;

    struct XenosFormatInfo
    {
        RenderFormat format;
        uint32_t blockSize;  // texels per block side (1 or 4)
        uint32_t bytesPerBlock;
    };

    // Xenos texture formats (Xenia's xenos::TextureFormat numbering).
    bool GetXenosFormat(uint32_t format, XenosFormatInfo& info)
    {
        switch (format)
        {
        case 2:  info = { RenderFormat::R8_UNORM, 1, 1 }; return true;            // k_8
        case 6:  info = { RenderFormat::R8G8B8A8_UNORM, 1, 4 }; return true;      // k_8_8_8_8
        case 10: info = { RenderFormat::R8G8_UNORM, 1, 2 }; return true;          // k_8_8
        case 18: info = { RenderFormat::BC1_UNORM, 4, 8 }; return true;           // k_DXT1
        case 19: info = { RenderFormat::BC2_UNORM, 4, 16 }; return true;          // k_DXT2_3
        case 20: info = { RenderFormat::BC3_UNORM, 4, 16 }; return true;          // k_DXT4_5
        case 25: info = { RenderFormat::R16G16_UNORM, 1, 4 }; return true;        // k_16_16
        case 31: info = { RenderFormat::R16G16_FLOAT, 1, 4 }; return true;        // k_16_16_FLOAT
        case 32: info = { RenderFormat::R16G16B16A16_FLOAT, 1, 8 }; return true;  // k_16_16_16_16_FLOAT
        case 36: info = { RenderFormat::R32_FLOAT, 1, 4 }; return true;           // k_32_FLOAT
        case 49: info = { RenderFormat::BC5_UNORM, 4, 16 }; return true;          // k_DXN
        case 50: info = { RenderFormat::R8G8B8A8_UNORM, 1, 4 }; return true;      // k_8_8_8_8_AS_16_16_16_16
        case 51: info = { RenderFormat::BC1_UNORM, 4, 8 }; return true;           // k_DXT1_AS_16_16_16_16
        case 52: info = { RenderFormat::BC2_UNORM, 4, 16 }; return true;          // k_DXT2_3_AS_16_16_16_16
        case 53: info = { RenderFormat::BC3_UNORM, 4, 16 }; return true;          // k_DXT4_5_AS_16_16_16_16
        default: return false;
        }
    }

    // Block index of (x, y) in a tiled Xenos texture (the XDK's XGAddress2DTiledOffset).
    uint32_t TiledBlockIndex(uint32_t x, uint32_t y, uint32_t widthInBlocks, uint32_t bytesPerBlock)
    {
        uint32_t alignedWidth = (widthInBlocks + 31) & ~31u;
        uint32_t logBpp = (bytesPerBlock >> 2) + ((bytesPerBlock >> 1) >> (bytesPerBlock >> 2));
        uint32_t macro = ((x >> 5) + (y >> 5) * (alignedWidth >> 5)) << (logBpp + 7);
        uint32_t micro = ((x & 7) + ((y & 6) << 2)) << logBpp;
        uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + logBpp)) + ((y & 1) << 4);
        return (((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> logBpp;
    }

    void SwapEndian(uint8_t* data, uint32_t size, uint32_t endian)
    {
        switch (endian)
        {
        case 1: // 8 in 16
            for (uint32_t i = 0; i + 1 < size; i += 2)
                std::swap(data[i], data[i + 1]);
            break;
        case 2: // 8 in 32
            for (uint32_t i = 0; i + 3 < size; i += 4)
            {
                std::swap(data[i], data[i + 3]);
                std::swap(data[i + 1], data[i + 2]);
            }
            break;
        case 3: // 16 in 32
            for (uint32_t i = 0; i + 3 < size; i += 4)
            {
                std::swap(data[i], data[i + 2]);
                std::swap(data[i + 1], data[i + 3]);
            }
            break;
        }
    }

    RenderSwizzle ConvertSwizzle(uint32_t component)
    {
        switch (component)
        {
        case 0: return RenderSwizzle::R;
        case 1: return RenderSwizzle::G;
        case 2: return RenderSwizzle::B;
        case 3: return RenderSwizzle::A;
        case 4: return RenderSwizzle::ZERO;
        default: return RenderSwizzle::ONE;
        }
    }

    struct HostTexture
    {
        std::unique_ptr<RenderTexture> texture;
        std::unique_ptr<RenderTextureView> view;
        uint32_t descriptorIndex = 0;
    };

    ankerl::unordered_dense::map<uint64_t, HostTexture> g_textures;
    uint32_t g_nextTextureDescriptor = NULL_COUNT;
    std::set<uint32_t> g_unsupportedTextureFormats;

    // Returns the descriptor index for the texture object, uploading it on first use.
    uint32_t GetTextureDescriptor(uint32_t object)
    {
        if (object == 0)
            return NULL_TEXTURE_2D;

        uint32_t fetch[6];
        for (uint32_t i = 0; i < 6; i++)
            fetch[i] = Load32(object + TEXTURE_FETCH_CONSTANT + i * 4);

        uint64_t key = XXH3_64bits(fetch, sizeof(fetch));
        if (auto it = g_textures.find(key); it != g_textures.end())
            return it->second.descriptorIndex;

        uint32_t type = fetch[0] & 3;
        uint32_t dimension = (fetch[5] >> 9) & 3;
        uint32_t formatId = fetch[1] & 0x3F;
        XenosFormatInfo info;
        if (type != 2 || dimension != 1 || !GetXenosFormat(formatId, info))
        {
            if (g_unsupportedTextureFormats.insert(formatId | (dimension << 8)).second)
                LOGF_WARNING("Texture format {} (dimension {}) not supported yet", formatId, dimension);
            g_textures[key].descriptorIndex = NULL_TEXTURE_2D;
            return NULL_TEXTURE_2D;
        }

        uint32_t address = fetch[1] & 0xFFFFF000;
        uint32_t endian = (fetch[1] >> 6) & 3;
        uint32_t width = (fetch[2] & 0x1FFF) + 1;
        uint32_t height = ((fetch[2] >> 13) & 0x1FFF) + 1;
        bool tiled = (fetch[0] >> 31) != 0;
        uint32_t pitch = ((fetch[0] >> 22) & 0x1FF) * 32;
        if (pitch == 0)
            pitch = width;

        uint32_t blocksWide = (width + info.blockSize - 1) / info.blockSize;
        uint32_t blocksHigh = (height + info.blockSize - 1) / info.blockSize;
        uint32_t pitchBlocks = std::max(pitch / info.blockSize, blocksWide);
        uint64_t size = uint64_t(blocksWide) * blocksHigh * info.bytesPerBlock;

        auto [memory, offset] = Allocate(size, 512);
        if (memory == nullptr)
            return NULL_TEXTURE_2D;

        const uint8_t* source = (const uint8_t*)g_memory.Translate(address);
        for (uint32_t by = 0; by < blocksHigh; by++)
        {
            for (uint32_t bx = 0; bx < blocksWide; bx++)
            {
                uint32_t sourceBlock = tiled ? TiledBlockIndex(bx, by, pitchBlocks, info.bytesPerBlock) : by * pitchBlocks + bx;
                uint8_t* destination = memory + (uint64_t(by) * blocksWide + bx) * info.bytesPerBlock;
                memcpy(destination, source + uint64_t(sourceBlock) * info.bytesPerBlock, info.bytesPerBlock);
                SwapEndian(destination, info.bytesPerBlock, endian);
            }
        }

        HostTexture host;
        host.texture = g_device->createTexture(RenderTextureDesc::Texture2D(width, height, 1, info.format));
        uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
        RenderTextureViewDesc viewDesc = RenderTextureViewDesc::Texture2D(info.format);
        viewDesc.mipLevels = 1;
        viewDesc.componentMapping = RenderComponentMapping(ConvertSwizzle(swizzle & 7), ConvertSwizzle((swizzle >> 3) & 7),
            ConvertSwizzle((swizzle >> 6) & 7), ConvertSwizzle((swizzle >> 9) & 7));
        host.view = host.texture->createTextureView(viewDesc);

        RenderTextureBarrier toCopy(host.texture.get(), RenderTextureLayout::COPY_DEST);
        g_commandList->barriers(RenderBarrierStage::COPY, nullptr, 0, &toCopy, 1);
        g_commandList->copyTextureRegion(RenderTextureCopyLocation::Subresource(host.texture.get()),
            RenderTextureCopyLocation::PlacedFootprint(g_uploadBuffer.get(), info.format, width, height, 1, blocksWide * info.blockSize, offset));
        RenderTextureBarrier toRead(host.texture.get(), RenderTextureLayout::SHADER_READ);
        g_commandList->barriers(RenderBarrierStage::GRAPHICS, nullptr, 0, &toRead, 1);

        host.descriptorIndex = g_nextTextureDescriptor++;
        g_textureDescriptorSet->setTexture(host.descriptorIndex, host.texture.get(), RenderTextureLayout::SHADER_READ, host.view.get());
        uint32_t descriptor = host.descriptorIndex;
        g_textures[key] = std::move(host);
        return descriptor;
    }


    RenderPrimitiveTopology ConvertPrimitiveType(uint32_t type)
    {
        switch (type)
        {
        case 1: return RenderPrimitiveTopology::POINT_LIST;
        case 2: return RenderPrimitiveTopology::LINE_LIST;
        case 3: return RenderPrimitiveTopology::LINE_STRIP;
        case 4: return RenderPrimitiveTopology::TRIANGLE_LIST;
        case 5: return RenderPrimitiveTopology::TRIANGLE_FAN;
        case 6: return RenderPrimitiveTopology::TRIANGLE_STRIP;
        case 13: return RenderPrimitiveTopology::TRIANGLE_LIST; // quad list, expanded below
        default: return RenderPrimitiveTopology::UNKNOWN;
        }
    }

    // Records one draw from user memory. indexFormat is 0 for non-indexed draws.
    void DrawUP(uint32_t primitiveType, uint32_t vertexData, uint32_t vertexCount, uint32_t stride,
        uint32_t indexData, uint32_t indexCount, uint32_t indexSize)
    {
        std::lock_guard lock(g_videoMutex);
        if (g_guestDevice == 0)
            return;
        BeginFrame();

        auto skip = [](const char* reason)
            {
                g_drawsSkippedThisFrame++;
                static std::set<std::string> reported;
                if (reported.insert(reason).second)
                    LOGF_WARNING("Draw skipped: {}", reason);
            };

        RenderPrimitiveTopology topology = ConvertPrimitiveType(primitiveType);
        if (topology == RenderPrimitiveTopology::UNKNOWN)
            return skip(fmt::format("primitive type {}", primitiveType).c_str());
        if (stride == 0 || (stride & 3) != 0 || vertexCount == 0)
            return skip("vertex stride");

        uint64_t vsHash, psHash;
        {
            std::lock_guard shaderLock(g_shaderMutex);
            auto vs = g_shaderHashes.find(Load32(g_guestDevice + DEVICE_VERTEX_SHADER));
            auto ps = g_shaderHashes.find(Load32(g_guestDevice + DEVICE_PIXEL_SHADER));
            if (vs == g_shaderHashes.end() || ps == g_shaderHashes.end())
            {
                static int debugCount = 0;
                if (debugCount++ < 8)
                    LOGF_WARNING("vs=0x{:08X} ps=0x{:08X} decl=0x{:08X} known shaders={}", Load32(g_guestDevice + DEVICE_VERTEX_SHADER),
                        Load32(g_guestDevice + DEVICE_PIXEL_SHADER), Load32(g_guestDevice + DEVICE_VERTEX_DECLARATION), g_shaderHashes.size());
                return skip("no vertex or pixel shader bound");
            }
            vsHash = vs->second;
            psHash = ps->second;
        }

        RenderShader* vertexShader = GetShader(vsHash);
        RenderShader* pixelShader = GetShader(psHash);
        if (vertexShader == nullptr || pixelShader == nullptr)
            return skip("shader not translated");

        const VertexDeclaration* declaration = GetDeclaration(Load32(g_guestDevice + DEVICE_VERTEX_DECLARATION));
        if (declaration == nullptr)
            return skip("no vertex declaration");

        PipelineKey key{ vsHash, psHash, declaration->hash, stride, topology };
        RenderPipeline* pipeline = GetPipeline(key, vertexShader, pixelShader, *declaration);
        if (pipeline == nullptr)
            return skip("pipeline creation failed");

        // Constants: the guest device holds them big-endian.
        auto [vsMemory, vsOffset] = UploadSwapped<uint32_t>(g_guestDevice + DEVICE_VS_CONSTANTS, VS_CONSTANTS_SIZE);
        auto [psMemory, psOffset] = UploadSwapped<uint32_t>(g_guestDevice + DEVICE_PS_CONSTANTS, PS_CONSTANTS_SIZE);
        auto [sharedMemory, sharedOffset] = Allocate(sizeof(SharedConstants));
        if (vsMemory == nullptr || psMemory == nullptr || sharedMemory == nullptr)
            return skip("upload buffer full");

        SharedConstants shared{};
        for (uint32_t i = 0; i < 16; i++)
        {
            shared.texture2DIndices[i] = GetTextureDescriptor(g_samplerTextures[i]);
            shared.texture3DIndices[i] = NULL_TEXTURE_3D;
            shared.textureCubeIndices[i] = NULL_TEXTURE_CUBE;
            shared.samplerIndices[i] = 0;
        }
        shared.swappedTexcoords = 0; // vertex data is swapped per element (see VertexDeclaration)
        memcpy(sharedMemory, &shared, sizeof(shared));

        PushConstants pushConstants{ g_uploadAddress + vsOffset, g_uploadAddress + psOffset, g_uploadAddress + sharedOffset };

        // Vertices (and indices) from guest memory, endian-swapped.
        // Vertices are rewritten into the declaration's host layout (see ElementCopy).
        uint32_t hostStride = std::max<uint32_t>(declaration->hostStride, 4);
        auto [vertexMemory, vertexOffset] = Allocate(uint64_t(vertexCount) * hostStride);
        if (vertexMemory != nullptr)
        {
            const uint8_t* source = (const uint8_t*)g_memory.Translate(vertexData);
            for (uint32_t vertex = 0; vertex < vertexCount; vertex++)
            {
                const uint8_t* in = source + uint64_t(vertex) * stride;
                uint8_t* out = vertexMemory + uint64_t(vertex) * hostStride;
                for (const auto& copy : declaration->copies)
                {
                    const uint8_t* from = in + copy.sourceOffset;
                    uint8_t* to = out + copy.destinationOffset;
                    switch (copy.conversion)
                    {
                    case ElementConversion::Swap32:
                        for (uint32_t i = 0; i < copy.sourceBytes; i += 4)
                            *(uint32_t*)(to + i) = ByteSwap(*(const uint32_t*)(from + i));
                        break;
                    case ElementConversion::Swap16:
                        for (uint32_t i = 0; i < copy.sourceBytes; i += 2)
                            *(uint16_t*)(to + i) = ByteSwap(*(const uint16_t*)(from + i));
                        break;
                    case ElementConversion::ShortToFloat:
                        for (uint32_t i = 0; i < copy.components; i++)
                            ((float*)to)[i] = float(int16_t(ByteSwap(*(const uint16_t*)(from + i * 2))));
                        break;
                    case ElementConversion::UbyteToFloat:
                        // The GPU reads the word big-endian: X is the least significant byte.
                        for (uint32_t i = 0; i < 4; i++)
                            ((float*)to)[i] = float(from[3 - i]);
                        break;
                    }
                }
            }
        }
        uint64_t vertexBytes = uint64_t(vertexCount) * hostStride;
        auto [dummyMemory, dummyOffset] = Allocate(64);
        if (vertexMemory == nullptr || dummyMemory == nullptr)
            return skip("upload buffer full");
        memset(dummyMemory, 0, 64);

        // Quad lists become indexed triangle lists.
        std::vector<uint32_t> quadIndices;
        if (primitiveType == 13)
        {
            uint32_t sourceCount = indexCount != 0 ? indexCount : vertexCount;
            for (uint32_t quad = 0; quad + 4 <= sourceCount; quad += 4)
            {
                auto index = [&](uint32_t i) -> uint32_t
                    {
                        if (indexCount == 0)
                            return quad + i;
                        return indexSize == 4 ? Load32(indexData + (quad + i) * 4) : Load16(indexData + (quad + i) * 2);
                    };
                for (uint32_t i : { 0u, 1u, 2u, 0u, 2u, 3u })
                    quadIndices.push_back(index(i));
            }
        }

        if (g_debugDrawsFrame != 0 && g_presentedFrames + 1 == g_debugDrawsFrame)
        {
            std::string textures;
            for (uint32_t i = 0; i < 4; i++)
            {
                if (uint32_t object = g_samplerTextures[i])
                {
                    uint32_t f1 = Load32(object + TEXTURE_FETCH_CONSTANT + 4), f2 = Load32(object + TEXTURE_FETCH_CONSTANT + 8);
                    textures += fmt::format(" t{}=fmt{} {}x{} @{:08X}", i, f1 & 0x3F, (f2 & 0x1FFF) + 1, ((f2 >> 13) & 0x1FFF) + 1, f1 & 0xFFFFF000);
                }
            }
            float* v = (float*)vertexMemory;
            LOGF_IMPL(Utility, "draw", "#{} prim {} verts {} idx {} stride {} vs {:016x} ps {:016x} pos0 ({:.3f}, {:.3f}, {:.3f}, {:.3f}){}",
                g_drawsThisFrame, primitiveType, vertexCount, indexCount, stride, vsHash, psHash, v[0], v[1], v[2], stride >= 16 ? v[3] : 0.0f, textures);
        }

        g_commandList->setPipeline(pipeline);
        g_commandList->setGraphicsPushConstants(0, &pushConstants, 0, sizeof(pushConstants));
        RenderVertexBufferView views[] = {
            RenderVertexBufferView(g_uploadBuffer->at(vertexOffset), uint32_t(vertexBytes)),
            RenderVertexBufferView(g_uploadBuffer->at(dummyOffset), 64),
        };
        RenderInputSlot slots[] = { RenderInputSlot(0, hostStride), RenderInputSlot(DUMMY_STREAM, 0) };
        g_commandList->setVertexBuffers(0, &views[0], 1, &slots[0]);
        g_commandList->setVertexBuffers(DUMMY_STREAM, &views[1], 1, &slots[1]);

        if (!quadIndices.empty())
        {
            auto [indexMemory, indexOffset] = Allocate(quadIndices.size() * 4);
            if (indexMemory == nullptr)
                return skip("upload buffer full");
            memcpy(indexMemory, quadIndices.data(), quadIndices.size() * 4);
            RenderIndexBufferView view(g_uploadBuffer->at(indexOffset), uint32_t(quadIndices.size() * 4), RenderFormat::R32_UINT);
            g_commandList->setIndexBuffer(&view);
            g_commandList->drawIndexedInstanced(uint32_t(quadIndices.size()), 1, 0, 0, 0);
        }
        else if (indexCount != 0)
        {
            auto [indexMemory, indexOffset] = indexSize == 4
                ? UploadSwapped<uint32_t>(indexData, uint64_t(indexCount) * 4)
                : UploadSwapped<uint16_t>(indexData, (uint64_t(indexCount) * 2 + 3) & ~3ull);
            if (indexMemory == nullptr)
                return skip("upload buffer full");
            RenderIndexBufferView view(g_uploadBuffer->at(indexOffset), indexCount * indexSize, indexSize == 4 ? RenderFormat::R32_UINT : RenderFormat::R16_UINT);
            g_commandList->setIndexBuffer(&view);
            g_commandList->drawIndexedInstanced(indexCount, 1, 0, 0, 0);
        }
        else
        {
            g_commandList->drawInstanced(vertexCount, 1, 0, 0);
        }
        g_drawsThisFrame++;
    }

    void CreatePipelineLayout()
    {
        RenderDescriptorSetBuilder textureSet;
        textureSet.begin();
        textureSet.addTexture(0, TEXTURE_DESCRIPTOR_SIZE);
        textureSet.end(true, TEXTURE_DESCRIPTOR_SIZE);
        g_textureDescriptorSet = textureSet.create(g_device.get());

        // Null textures read as white so untextured geometry is visible until textures are
        // implemented; the component mapping does it without uploading anything.
        for (uint32_t i = 0; i < NULL_COUNT; i++)
        {
            RenderTextureDesc desc;
            desc.width = 1;
            desc.height = 1;
            desc.depth = 1;
            desc.mipLevels = 1;
            desc.format = RenderFormat::R8_UNORM;
            RenderTextureViewDesc viewDesc;
            viewDesc.format = desc.format;
            viewDesc.componentMapping = RenderComponentMapping(RenderSwizzle::ONE, RenderSwizzle::ONE, RenderSwizzle::ONE, RenderSwizzle::ONE);
            viewDesc.mipLevels = 1;
            switch (i)
            {
            case NULL_TEXTURE_2D:
                desc.dimension = RenderTextureDimension::TEXTURE_2D;
                desc.arraySize = 1;
                viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
                break;
            case NULL_TEXTURE_3D:
                desc.dimension = RenderTextureDimension::TEXTURE_3D;
                desc.arraySize = 1;
                viewDesc.dimension = RenderTextureViewDimension::TEXTURE_3D;
                break;
            case NULL_TEXTURE_CUBE:
                desc.dimension = RenderTextureDimension::TEXTURE_2D;
                desc.arraySize = 6;
                desc.flags = RenderTextureFlag::CUBE;
                viewDesc.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
                break;
            }
            g_nullTextures[i] = g_device->createTexture(desc);
            g_nullTextureViews[i] = g_nullTextures[i]->createTextureView(viewDesc);
            g_textureDescriptorSet->setTexture(i, g_nullTextures[i].get(), RenderTextureLayout::SHADER_READ, g_nullTextureViews[i].get());
        }

        RenderDescriptorSetBuilder samplerSet;
        samplerSet.begin();
        samplerSet.addSampler(0, SAMPLER_DESCRIPTOR_SIZE);
        samplerSet.end(true, SAMPLER_DESCRIPTOR_SIZE);
        g_samplerDescriptorSet = samplerSet.create(g_device.get());
        g_defaultSampler = g_device->createSampler(RenderSamplerDesc());
        g_samplerDescriptorSet->setSampler(0, g_defaultSampler.get());

        RenderPipelineLayoutBuilder layout;
        layout.begin(false, true);
        layout.addDescriptorSet(textureSet);
        layout.addDescriptorSet(textureSet);
        layout.addDescriptorSet(textureSet);
        layout.addDescriptorSet(samplerSet);
        layout.addPushConstant(0, 4, sizeof(PushConstants), RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
        layout.end();
        g_pipelineLayout = layout.create(g_device.get());
    }

    // The shader container's hash, as XenosRecomp computes it (XXH3 over virtual + physical parts).
    uint64_t HashShaderContainer(uint32_t container)
    {
        if (container == 0)
            return 0;
        uint32_t size = Load32(container + 4) + Load32(container + 8);
        return XXH3_64bits(g_memory.Translate(container), size);
    }

    // NB_DUMP_SHADERS=dir writes every shader container the game creates to dir, for
    // tools/translate_shaders.sh.
    const char* g_shaderDumpDir = getenv("NB_DUMP_SHADERS");

    void DumpShader(uint32_t container, uint64_t hash, bool pixel)
    {
        if (g_shaderDumpDir == nullptr || container == 0)
            return;
        std::filesystem::path path = std::filesystem::path(g_shaderDumpDir) / fmt::format("{}_{:016x}.bin", pixel ? "ps" : "vs", hash);
        std::error_code ec;
        if (std::filesystem::exists(path, ec))
            return;
        std::filesystem::create_directories(path.parent_path(), ec);
        uint32_t size = Load32(container + 4) + Load32(container + 8);
        std::ofstream(path, std::ios::binary).write((const char*)g_memory.Translate(container), size);
    }

    void RecordShader(uint32_t object, uint64_t hash)
    {
        if (object == 0 || hash == 0)
            return;
        std::lock_guard lock(g_shaderMutex);
        g_shaderHashes[object] = hash;
    }
}

bool Video::Init(SDL_Window* window)
{
    g_interface = CreateVulkanInterface(window);
    if (!g_interface)
    {
        LOGN_ERROR("Could not create the Vulkan interface.");
        return false;
    }

    g_device = g_interface->createDevice();
    if (!g_device)
    {
        LOGN_ERROR("Could not create a Vulkan device.");
        return false;
    }

    g_queue = g_device->createCommandQueue(RenderCommandListType::DIRECT);
    g_commandList = g_queue->createCommandList();
    g_fence = g_device->createCommandFence();
    g_acquireSemaphore = g_device->createCommandSemaphore();
    g_renderSemaphore = g_device->createCommandSemaphore();
    g_swapChain = g_queue->createSwapChain(window, BUFFER_COUNT, BACKBUFFER_FORMAT, 1);
    g_swapChain->setVsyncEnabled(true);
    g_swapChain->resize();

    CreatePipelineLayout();

    g_renderTarget = g_device->createTexture(RenderTextureDesc::ColorTarget(RENDER_WIDTH, RENDER_HEIGHT, BACKBUFFER_FORMAT));
    const RenderTexture* target = g_renderTarget.get();
    g_renderTargetFramebuffer = g_device->createFramebuffer(RenderFramebufferDesc(&target, 1));

    g_uploadBuffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(UPLOAD_BUFFER_SIZE,
        RenderBufferFlag::CONSTANT | RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
    g_uploadMemory = (uint8_t*)g_uploadBuffer->map();
    g_uploadAddress = g_uploadBuffer->getDeviceAddress();

    if (const char* screenshot = getenv("NB_SCREENSHOT"))
    {
        g_screenshotPath = screenshot;
        if (auto colon = g_screenshotPath.rfind(':'); colon != std::string::npos)
        {
            g_screenshotFrame = std::stoull(g_screenshotPath.substr(colon + 1));
            g_screenshotPath.resize(colon);
        }
        g_readbackBuffer = g_device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(RENDER_WIDTH) * RENDER_HEIGHT * 4));
    }

    LoadShaderCache();
    LOGN("Video: Vulkan device and swap chain ready");
    return true;
}

void Video::OnWindowResized()
{
}

// Direct3D_CreateDevice: let the guest create its device, then hand it to the headless GPU.
PPC_FUNC(sub_82F26FA0)
{
    uint32_t deviceOut = ctx.r8.u32;
    if (uint32_t params = ctx.r7.u32)
        SetPresentationInterval(Load32(params + D3DPRESENT_PARAMETERS_INTERVAL));
    TRACE_ENTER(0x82F26FA0);
    __imp__sub_82F26FA0(ctx, base);
    TRACE_LEAVE(0x82F26FA0);
    if (ctx.r3.u32 == 0 && deviceOut != 0)
    {
        g_guestDevice = Load32(deviceOut);
        HeadlessGpuOnDeviceCreated(g_guestDevice);
    }
}

// D3DDevice_BeginTiling(dev, Flags, Count, pTileRects, pClearColor, ClearZ, ClearStencil).
PPC_FUNC(sub_82F37138)
{
    static const bool debugClear = getenv("NB_DEBUG_CLEAR") != nullptr;
    if (uint32_t color = ctx.r7.u32)
        g_clearColor = RenderColor(LoadFloat(color), LoadFloat(color + 4), LoadFloat(color + 8), LoadFloat(color + 12));
    if (debugClear)
        g_clearColor = RenderColor(1.0f, 0.0f, 1.0f, 1.0f);
    {
        std::lock_guard lock(g_videoMutex);
        if (g_inFrame)
            g_commandList->clearColor(0, g_clearColor);
        else
            BeginFrame();
    }
    TRACE_ENTER(0x82F37138);
    __imp__sub_82F37138(ctx, base);
}

// D3DDevice_Swap: show the frame.
PPC_FUNC(sub_82F33D50)
{
    TRACE_ENTER(0x82F33D50);
    {
        std::lock_guard lock(g_videoMutex);
        EndFrame();
    }
    __imp__sub_82F33D50(ctx, base);
}

// Shader creation: remember which container each shader object came from. Objects are
// built either in place (constructors taking { container, ... } in r5) or allocated by
// CreateVertexShader/CreatePixelShader (container in r3, object returned in r3).
PPC_FUNC(sub_82F30050)
{
    TRACE_ENTER(0x82F30050);
    uint64_t hash = HashShaderContainer(Load32(ctx.r5.u32));
    DumpShader(Load32(ctx.r5.u32), hash, false);
    RecordShader(ctx.r3.u32, hash);
    __imp__sub_82F30050(ctx, base);
}

PPC_FUNC(sub_82F2FF28)
{
    TRACE_ENTER(0x82F2FF28);
    uint64_t hash = HashShaderContainer(Load32(ctx.r5.u32));
    DumpShader(Load32(ctx.r5.u32), hash, true);
    RecordShader(ctx.r3.u32, hash);
    __imp__sub_82F2FF28(ctx, base);
}

PPC_FUNC(sub_82F30188)
{
    uint64_t hash = HashShaderContainer(ctx.r3.u32);
    DumpShader(ctx.r3.u32, hash, false);
    __imp__sub_82F30188(ctx, base);
    RecordShader(ctx.r3.u32, hash);
}

PPC_FUNC(sub_82F2FF60)
{
    uint64_t hash = HashShaderContainer(ctx.r3.u32);
    DumpShader(ctx.r3.u32, hash, true);
    __imp__sub_82F2FF60(ctx, base);
    RecordShader(ctx.r3.u32, hash);
}

// D3DDevice_DrawVerticesUP(dev, PrimitiveType, VertexCount, pVertexStreamZeroData, VertexStreamZeroStride).
PPC_FUNC(sub_82F38388)
{
    TRACE_ENTER(0x82F38388);
    DrawUP(ctx.r4.u32, ctx.r6.u32, ctx.r5.u32, ctx.r7.u32, 0, 0, 0);
    __imp__sub_82F38388(ctx, base);
}

// D3DDevice_DrawIndexedVerticesUP(dev, PrimitiveType, MinVertexIndex, NumVertices, IndexCount,
//     pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride (stack)).
PPC_FUNC(sub_82F388E8)
{
    TRACE_ENTER(0x82F388E8);
    uint32_t stride = Load32(ctx.r1.u32 + 0x54);
    uint32_t indexSize = (ctx.r9.u32 & 4) ? 4 : 2;
    uint32_t vertexCount = ctx.r5.u32 + ctx.r6.u32; // indices are absolute, so upload from vertex 0
    DrawUP(ctx.r4.u32, ctx.r10.u32, vertexCount, stride, ctx.r8.u32, ctx.r7.u32, indexSize);
    __imp__sub_82F388E8(ctx, base);
}

// D3DDevice_SetTexture(dev, Sampler, pTexture, dirty bit).
PPC_FUNC(sub_82F2C640)
{
    TRACE_ENTER(0x82F2C640);
    uint32_t sampler = ctx.r4.u32;
    uint32_t texture = ctx.r5.u32;
    if (sampler < std::size(g_samplerTextures))
        g_samplerTextures[sampler] = texture;

    __imp__sub_82F2C640(ctx, base);
}
