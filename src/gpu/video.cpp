// Host renderer for Naughty Bear, built on plume (Vulkan) following Unleashed Recompiled.
//
// The game's own Direct3D library keeps managing its state (it runs against the headless
// GPU), and the draw calls are mirrored here. The XDK device keeps an image of the GPU
// registers it programs, the fetch constants (textures and vertex streams) and the
// shader constants; at each draw these are read from the guest device (offsets in
// docs/d3d_map.md) and the draw is replayed with the shaders translated by XenosRecomp.
//
// Render targets: every guest surface (render target or depth/stencil) gets a host
// texture. Resolves (D3DDevice_Resolve, EndTiling) copy a surface into a host texture
// that stands in for the destination guest texture when it is sampled, and Swap presents
// the resolved front buffer.

#include <stdafx.h>
#include <gpu/shader_store.h>
#include <gpu/video.h>
#include <hid/hid.h>
#include <kernel/memory.h>
#include <os/logger.h>
#include <plume_render_interface.h>
#include <bit>

#include <blit.vert.h>
#include <blit.frag.h>

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
extern "C" PPC_FUNC(__imp__sub_82F37798); // D3DDevice_EndTiling
extern "C" PPC_FUNC(__imp__sub_82F398F0); // D3DDevice_Resolve
extern "C" PPC_FUNC(__imp__sub_82F33D50); // D3DDevice_Swap
extern "C" PPC_FUNC(__imp__sub_82F35788); // D3DDevice_Clear
extern "C" PPC_FUNC(__imp__sub_82F30050); // vertex shader construct
extern "C" PPC_FUNC(__imp__sub_82F2FF28); // pixel shader construct
extern "C" PPC_FUNC(__imp__sub_82F30188); // CreateVertexShader(container) -> object
extern "C" PPC_FUNC(__imp__sub_82F2FF60); // CreatePixelShader(container) -> object
extern "C" PPC_FUNC(__imp__sub_82F38988); // D3DDevice_DrawVertices
extern "C" PPC_FUNC(__imp__sub_82F38D78); // D3DDevice_DrawIndexedVertices
extern "C" PPC_FUNC(__imp__sub_82F38388); // D3DDevice_DrawVerticesUP
extern "C" PPC_FUNC(__imp__sub_82F388E8); // D3DDevice_DrawIndexedVerticesUP

namespace
{
    // ---- Guest device layout (docs/d3d_map.md) ----

    constexpr uint32_t DEVICE_FETCH_CONSTANTS = 0x480;    // 32 slots of 6 dwords (GPU registers 0x4800+)
    constexpr uint32_t DEVICE_VS_CONSTANTS = 0x780;
    constexpr uint32_t DEVICE_PS_CONSTANTS = 0x1780;
    constexpr uint32_t DEVICE_BOOL_CONSTANTS = 10112;     // GPU registers 0x4900+: VS bools, then PS bools at +16
    constexpr uint32_t DEVICE_VERTEX_DECLARATION = 0x2ED8;
    constexpr uint32_t DEVICE_PIXEL_SHADER = 0x3244;
    constexpr uint32_t DEVICE_VERTEX_SHADER = 0x3248;
    constexpr uint32_t DEVICE_INDEX_BUFFER = 12612;
    constexpr uint32_t DEVICE_RENDER_TARGETS = 12616;     // 4 surfaces
    constexpr uint32_t DEVICE_DEPTH_STENCIL = 12632;
    constexpr uint32_t DEVICE_STREAM_STRIDES = 12704;     // 16 bytes: stride / 4 per stream
    constexpr uint32_t DEVICE_SCISSOR_ENABLE = 12032;
    constexpr uint32_t DEVICE_VIEWPORT = 12824;           // D3DVIEWPORT9 X, Y, Width, Height as floats
    constexpr uint32_t DEVICE_SCISSOR_RECT = 12852;       // left, top, right, bottom
    // Vertex declaration object: element count at +24, D3DVERTEXELEMENT9[12 bytes] at +52.
    constexpr uint32_t DECLARATION_COUNT = 24;
    constexpr uint32_t DECLARATION_ELEMENTS = 52;
    // Surface object: RB_SURFACE_INFO image at +24, RB_COLOR_INFO / RB_DEPTH_INFO at +28,
    // EDRAM size in bytes at +44.
    constexpr uint32_t SURFACE_INFO = 24;
    constexpr uint32_t SURFACE_TARGET_INFO = 28;
    constexpr uint32_t SURFACE_SIZE = 44;
    // Buffer objects (vertex and index): address at +24; index buffers flag 32-bit indices
    // in the sign bit of their first dword.
    constexpr uint32_t BUFFER_ADDRESS = 24;
    // Texture objects carry their fetch constant at +0x1C.
    constexpr uint32_t TEXTURE_FETCH_CONSTANT = 0x1C;

    // The device keeps the GPU registers it programs as consecutive dwords, in groups (each
    // flushed to the command buffer by the draw functions' dirty-flag walk).
    struct RegisterGroup
    {
        uint32_t first;
        uint32_t count;
        uint32_t offset;
    };

    constexpr RegisterGroup REGISTER_GROUPS[] =
    {
        { 0x2000, 19, 10368 },
        { 0x2100, 21, 10444 },
        { 0x2180, 5, 10528 },
        { 0x2200, 12, 10548 },
        { 0x2280, 21, 10596 },
        { 0x2300, 38, 10680 },
    };

    // Xenos register numbers (as documented by Xenia).
    enum : uint32_t
    {
        RB_COLOR_MASK = 0x2104,
        RB_STENCILREFMASK_BF = 0x210C,
        RB_STENCILREFMASK = 0x210D,
        RB_ALPHA_REF = 0x210E,
        PA_CL_VPORT_XSCALE = 0x210F,
        PA_CL_VPORT_XOFFSET = 0x2110,
        PA_CL_VPORT_YSCALE = 0x2111,
        PA_CL_VPORT_YOFFSET = 0x2112,
        PA_CL_VPORT_ZSCALE = 0x2113,
        PA_CL_VPORT_ZOFFSET = 0x2114,
        RB_DEPTHCONTROL = 0x2200,
        RB_BLENDCONTROL0 = 0x2201,
        RB_COLORCONTROL = 0x2202,
        PA_SU_SC_MODE_CNTL = 0x2205,
        PA_CL_VTE_CNTL = 0x2206,
        // SetScissorRect (0x82F29630) keeps the scissor here: x in bits 0-14, y in 16-30.
        PA_SC_SCISSOR_TL = 0x2011,
        PA_SC_SCISSOR_BR = 0x2012,
    };

    constexpr uint32_t PRESENT_WIDTH = 1280;
    constexpr uint32_t PRESENT_HEIGHT = 720;
    constexpr RenderFormat BACKBUFFER_FORMAT = RenderFormat::B8G8R8A8_UNORM;
    constexpr uint32_t BUFFER_COUNT = 2;
    constexpr uint32_t TEXTURE_DESCRIPTOR_SIZE = 65536;
    constexpr uint32_t SAMPLER_DESCRIPTOR_SIZE = 1024;
    constexpr uint32_t VS_CONSTANTS_SIZE = 256 * 16;
    constexpr uint32_t PS_CONSTANTS_SIZE = 224 * 16;
    constexpr uint64_t UPLOAD_BUFFER_SIZE = 128 * 1024 * 1024;
    // Host vertex buffers unused for this many frames are released.
    constexpr uint64_t VERTEX_BUFFER_LIFETIME = 600;
    // Vertex streams smaller than this are decoded per draw instead of cached.
    constexpr uint32_t SMALL_STREAM_SIZE = 16 * 1024;

    enum NullDescriptor : uint32_t
    {
        NULL_TEXTURE_2D,
        NULL_TEXTURE_3D,
        NULL_TEXTURE_CUBE,
        NULL_COUNT
    };

    enum FixedSampler : uint32_t
    {
        SAMPLER_DEFAULT,      // linear, wrap
        SAMPLER_POINT_CLAMP,  // copies
        SAMPLER_LINEAR_CLAMP, // presentation
        SAMPLER_FIXED_COUNT
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
        uint32_t reserved[3]{};
        // Per vertex input location: its stream's decoded data, for vertex fetches indexed
        // by a computed register (vfetchIndexed in patches/XenosRecomp).
        struct
        {
            uint64_t address;
            uint32_t stride;
            uint32_t offset;
        } fetches[32]{};
    };
    static_assert(offsetof(SharedConstants, fetches) == 288);

    struct PushConstants
    {
        uint64_t vertexShaderConstants;
        uint64_t pixelShaderConstants;
        uint64_t sharedConstants;
    };

    // The blit shaders' push constants (src/gpu/shaders/blit.*); the same 24-byte range.
    struct BlitConstants
    {
        uint32_t textureIndex;
        uint32_t samplerIndex;
        float u0, v0, u1, v1;
    };
    static_assert(sizeof(BlitConstants) == sizeof(PushConstants));

    // XenosRecomp specialization constants.
    constexpr uint32_t SPEC_CONSTANT_ALPHA_TEST = 1 << 1;

    // D3DDECLUSAGE values of the Xbox 360 XDK.
    enum : uint32_t
    {
        D3DDECLUSAGE_POSITION = 0, D3DDECLUSAGE_BLENDWEIGHT = 1, D3DDECLUSAGE_BLENDINDICES = 2, D3DDECLUSAGE_NORMAL = 3,
        D3DDECLUSAGE_PSIZE = 4, D3DDECLUSAGE_TEXCOORD = 5, D3DDECLUSAGE_TANGENT = 6, D3DDECLUSAGE_BINORMAL = 7,
        D3DDECLUSAGE_TESSFACTOR = 8, D3DDECLUSAGE_POSITIONT = 9, D3DDECLUSAGE_COLOR = 10, D3DDECLUSAGE_FOG = 11,
        D3DDECLUSAGE_DEPTH = 12, D3DDECLUSAGE_SAMPLE = 13,
    };

    constexpr uint32_t D3DDECLTYPE_UNUSED = 0xFFFFFFFF;

    // Vertex input locations; must match patches/XenosRecomp (Unleashed's table plus the
    // extra blend, normal and colour channels Naughty Bear uses).
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
        { D3DDECLUSAGE_BLENDINDICES, 1, 19 }, { D3DDECLUSAGE_BLENDINDICES, 2, 20 }, { D3DDECLUSAGE_BLENDINDICES, 3, 21 },
        { D3DDECLUSAGE_NORMAL, 1, 22 }, { D3DDECLUSAGE_TANGENT, 1, 23 }, { D3DDECLUSAGE_BINORMAL, 1, 24 },
        { D3DDECLUSAGE_COLOR, 2, 25 }, { D3DDECLUSAGE_COLOR, 3, 26 },
    };

    // Inputs a shader may declare without the declaration providing them read zeroes from
    // this slot (stride 0).
    constexpr uint32_t DUMMY_SLOT = 16;
    constexpr uint32_t MAX_STREAMS = 16;

    const char* ConvertDeclUsage(uint32_t usage)
    {
        constexpr const char* names[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
            "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE" };
        return usage < std::size(names) ? names[usage] : "UNKNOWN";
    }

    // How a decoded element is handed to the shader: XenosRecomp declares normals, tangents
    // and binormals as uint4 holding float bits, and blend indices as uint4 integers.
    enum class ElementClass : uint8_t
    {
        Float,
        FloatBits,
        Integer,
    };

    ElementClass GetElementClass(uint32_t usage)
    {
        switch (usage)
        {
        case D3DDECLUSAGE_NORMAL:
        case D3DDECLUSAGE_TANGENT:
        case D3DDECLUSAGE_BINORMAL:
            return ElementClass::FloatBits;
        case D3DDECLUSAGE_BLENDINDICES:
            return ElementClass::Integer;
        default:
            return ElementClass::Float;
        }
    }

    // One vertex element: decoded on the CPU to four 32-bit values.
    struct ElementDecode
    {
        uint32_t sourceOffset;
        uint32_t type;
        ElementClass elementClass;
        uint8_t location;
    };

    // The elements of one vertex stream, in host order (16 bytes each).
    struct StreamLayout
    {
        uint32_t stream = 0;
        std::vector<ElementDecode> elements;
        uint64_t hash = 0;

        uint32_t HostStride() const { return uint32_t(elements.size()) * 16; }
    };

    struct VertexDeclaration
    {
        std::vector<RenderInputElement> inputElements;
        std::vector<StreamLayout> streams;
        uint64_t hash = 0;
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
    std::unique_ptr<RenderSampler> g_fixedSamplers[SAMPLER_FIXED_COUNT];
    std::unique_ptr<RenderShader> g_blitVertexShader;
    std::unique_ptr<RenderShader> g_blitPixelShader;
    // What is presented: the front buffer is copied here, then scaled to the window.
    std::unique_ptr<RenderTexture> g_presentTarget;
    std::unique_ptr<RenderFramebuffer> g_presentFramebuffer;
    uint32_t g_presentDescriptor = 0;
    std::vector<std::unique_ptr<RenderFramebuffer>> g_swapChainFramebuffers;
    std::atomic<bool> g_resizeRequested = false;
    std::unique_ptr<RenderBuffer> g_uploadBuffer;
    uint8_t* g_uploadMemory = nullptr;
    uint64_t g_uploadAddress = 0;
    uint64_t g_uploadOffset = 0;

    std::mutex g_videoMutex;
    uint32_t g_guestDevice = 0;
    bool g_inFrame = false;
    uint64_t g_frameIndex = 0; // frames submitted

    // Shaders: guest shader object -> container hash, recorded by the constructors.
    std::mutex g_shaderMutex;
    ankerl::unordered_dense::map<uint32_t, uint64_t> g_shaderHashes;
    ankerl::unordered_dense::map<uint64_t, std::unique_ptr<RenderShader>> g_shaders;
    ankerl::unordered_dense::map<uint64_t, VertexDeclaration> g_declarations;
    ankerl::unordered_dense::map<uint64_t, std::unique_ptr<RenderPipeline>> g_pipelines;
    std::set<uint64_t> g_missingShaders;

    // NB_SCREENSHOT=path[:frame[+step*count]] saves the presented image of that frame (default
    // 600) as PPM; with a step, a series of count frames (10 by default), numbered in the
    // file names.
    std::unique_ptr<RenderBuffer> g_readbackBuffer;
    std::string g_screenshotPath;
    uint64_t g_screenshotFrame = 600;
    uint64_t g_screenshotStep = 0;
    uint64_t g_screenshotCount = 1;
    uint64_t g_frameNumber = 0;

    // NB_DEBUG_DRAWS=frame logs every draw of that frame; NB_NO_BLEND=1 draws opaque.
    const uint64_t g_debugDrawsFrame = [] { const char* v = getenv("NB_DEBUG_DRAWS"); return v ? std::stoull(v) : 0ull; }();
    const bool g_noBlend = getenv("NB_NO_BLEND") != nullptr;
    uint64_t g_presentedFrames = 0;

    bool DebugThisFrame() { return g_debugDrawsFrame != 0 && g_presentedFrames + 1 == g_debugDrawsFrame; }

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
    uint8_t Load8(uint32_t address) { return *(uint8_t*)g_memory.Translate(address); }
    float LoadFloat(uint32_t address) { return std::bit_cast<float>(Load32(address)); }

    // GPU addresses are physical (the low 29 bits); physical memory is mapped once at
    // 0xA0000000 (kernel/heap.cpp).
    uint32_t GpuToGuest(uint32_t address) { return 0xA0000000 | (address & 0x1FFFFFFF); }

    uint32_t Reg(uint32_t index)
    {
        for (const auto& group : REGISTER_GROUPS)
        {
            if (index >= group.first && index < group.first + group.count)
                return Load32(g_guestDevice + group.offset + (index - group.first) * 4);
        }
        static std::set<uint32_t> reported;
        if (reported.insert(index).second)
            LOGF_WARNING("GPU register 0x{:04X} is not in the device image", index);
        return 0;
    }

    float RegFloat(uint32_t index) { return std::bit_cast<float>(Reg(index)); }

    void Skip(const std::string& reason)
    {
        g_drawsSkippedThisFrame++;
        static std::set<std::string> reported;
        if (reported.insert(reason).second)
            LOGF_WARNING("Draw skipped: {}", reason);
    }

    // Bump allocator in a host-visible buffer; reset every frame (frames are synchronous).
    std::pair<uint8_t*, uint64_t> Allocate(uint64_t size, uint64_t alignment = 256)
    {
        g_uploadOffset = (g_uploadOffset + alignment - 1) & ~(alignment - 1);
        if (g_uploadOffset + size > UPLOAD_BUFFER_SIZE)
        {
            static bool warned = false;
            if (!warned)
                LOGN_WARNING("Upload buffer full; frame truncated");
            warned = true;
            return { nullptr, 0 };
        }
        uint64_t offset = g_uploadOffset;
        g_uploadOffset += size;
        return { g_uploadMemory + offset, offset };
    }

    // Copies guest data, swapping every 32-bit word.
    std::pair<uint8_t*, uint64_t> UploadSwapped32(uint32_t guest, uint64_t size)
    {
        auto [memory, offset] = Allocate(size);
        if (memory != nullptr)
        {
            auto* source = (const uint32_t*)g_memory.Translate(guest);
            auto* destination = (uint32_t*)memory;
            for (uint64_t i = 0; i < size / 4; i++)
                destination[i] = ByteSwap(source[i]);
        }
        return { memory, offset };
    }

    RenderShader* GetShader(uint64_t hash)
    {
        if (auto it = g_shaders.find(hash); it != g_shaders.end())
            return it->second.get();

        const shader_store::Shader* translated = shader_store::Find(hash);
        if (translated == nullptr)
        {
            if (g_missingShaders.insert(hash).second)
                LOGF_WARNING("Shader {:016x} has not been translated", hash);
            return nullptr;
        }

        auto shader = g_device->createShader(translated->spirv.data(), translated->spirv.size(), "main", RenderShaderFormat::SPIRV);
        return (g_shaders[hash] = std::move(shader)).get();
    }

    uint32_t GetSpecConstantsMask(uint64_t hash)
    {
        const shader_store::Shader* translated = shader_store::Find(hash);
        return translated != nullptr ? translated->specConstantsMask : 0;
    }

    // ---- Vertex data ----

    float HalfToFloat(uint16_t half)
    {
        uint32_t sign = uint32_t(half >> 15) << 31;
        int32_t exponent = (half >> 10) & 0x1F;
        uint32_t mantissa = half & 0x3FF;
        if (exponent == 0)
        {
            if (mantissa == 0)
                return std::bit_cast<float>(sign);
            // Denormal: normalize it.
            exponent = 1;
            while ((mantissa & 0x400) == 0)
            {
                mantissa <<= 1;
                exponent--;
            }
            mantissa &= 0x3FF;
        }
        else if (exponent == 31)
        {
            return std::bit_cast<float>(sign | 0x7F800000 | (mantissa << 13));
        }
        return std::bit_cast<float>(sign | (uint32_t(exponent + 112) << 23) | (mantissa << 13));
    }

    // Decodes one vertex element. D3DDECLTYPE packs the Xenos vertex format (bits 0-5),
    // signedness (bit 8), integer i.e. not normalized (bit 9) and the component swizzle
    // (3 bits per component from bit 10; 4 = zero, 5 = one). 16- and 32-bit components are
    // in memory order; packed 8- and 10-bit formats are read from a big-endian dword with the
    // first component in its least significant bits.
    void DecodeVertexElement(const uint8_t* source, uint32_t type, float out[4])
    {
        uint32_t format = type & 0x3F;
        bool isSigned = ((type >> 8) & 1) != 0;
        bool isInteger = ((type >> 9) & 1) != 0;
        float component[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

        auto load32 = [&](uint32_t i) { return ByteSwap(*(const uint32_t*)(source + i * 4)); };
        auto load16 = [&](uint32_t i) { return ByteSwap(*(const uint16_t*)(source + i * 2)); };
        auto convert = [&](uint32_t value, uint32_t bits) -> float
            {
                if (isSigned)
                {
                    int32_t signedValue = bits == 32 ? int32_t(value) : int32_t(value << (32 - bits)) >> (32 - bits);
                    if (isInteger)
                        return float(signedValue);
                    return std::max(float(signedValue) / float((1ull << (bits - 1)) - 1), -1.0f);
                }
                if (isInteger)
                    return float(value);
                return float(double(value) / double((1ull << bits) - 1));
            };

        switch (format)
        {
        case 6: // k_8_8_8_8
        {
            uint32_t word = load32(0);
            for (uint32_t i = 0; i < 4; i++)
                component[i] = convert((word >> (i * 8)) & 0xFF, 8);
            break;
        }
        case 7: // k_2_10_10_10
        {
            uint32_t word = load32(0);
            component[0] = convert(word & 0x3FF, 10);
            component[1] = convert((word >> 10) & 0x3FF, 10);
            component[2] = convert((word >> 20) & 0x3FF, 10);
            component[3] = convert(word >> 30, 2);
            break;
        }
        case 16: // k_10_11_11
        {
            uint32_t word = load32(0);
            component[0] = convert(word & 0x7FF, 11);
            component[1] = convert((word >> 11) & 0x7FF, 11);
            component[2] = convert(word >> 22, 10);
            break;
        }
        case 17: // k_11_11_10
        {
            uint32_t word = load32(0);
            component[0] = convert(word & 0x3FF, 10);
            component[1] = convert((word >> 10) & 0x7FF, 11);
            component[2] = convert(word >> 21, 11);
            break;
        }
        case 25: // k_16_16
        case 26: // k_16_16_16_16
            for (uint32_t i = 0; i < (format == 25 ? 2u : 4u); i++)
                component[i] = convert(load16(i), 16);
            break;
        case 31: // k_16_16_FLOAT
        case 32: // k_16_16_16_16_FLOAT
            for (uint32_t i = 0; i < (format == 31 ? 2u : 4u); i++)
                component[i] = HalfToFloat(load16(i));
            break;
        case 33: // k_32
        case 34: // k_32_32
        case 35: // k_32_32_32_32
            for (uint32_t i = 0; i < (format == 35 ? 4u : format - 32); i++)
                component[i] = convert(load32(i), 32);
            break;
        case 36: // k_32_FLOAT
        case 37: // k_32_32_FLOAT
        case 38: // k_32_32_32_32_FLOAT
            for (uint32_t i = 0; i < (format == 38 ? 4u : format - 35); i++)
                component[i] = std::bit_cast<float>(load32(i));
            break;
        case 57: // k_32_32_32_FLOAT
            for (uint32_t i = 0; i < 3; i++)
                component[i] = std::bit_cast<float>(load32(i));
            break;
        default:
        {
            static std::set<uint32_t> reported;
            if (reported.insert(format).second)
                LOGF_WARNING("Vertex format {} not supported yet", format);
            break;
        }
        }

        uint32_t swizzle = type >> 10;
        for (uint32_t i = 0; i < 4; i++)
        {
            uint32_t select = (swizzle >> (i * 3)) & 7;
            out[i] = select < 4 ? component[select] : (select == 4 ? 0.0f : 1.0f);
        }
    }

    // Decodes vertexCount vertices of one stream into the layout's host form.
    void DecodeVertices(const StreamLayout& layout, const uint8_t* source, uint32_t stride, uint32_t vertexCount, uint8_t* destination)
    {
        for (uint32_t vertex = 0; vertex < vertexCount; vertex++)
        {
            const uint8_t* in = source + uint64_t(vertex) * stride;
            uint8_t* out = destination + uint64_t(vertex) * layout.HostStride();
            for (const auto& element : layout.elements)
            {
                float value[4];
                DecodeVertexElement(in + element.sourceOffset, element.type, value);
                if (element.elementClass == ElementClass::Integer)
                {
                    for (uint32_t i = 0; i < 4; i++)
                        ((uint32_t*)out)[i] = uint32_t(std::max(value[i], 0.0f));
                }
                else
                {
                    memcpy(out, value, 16);
                }
                out += 16;
            }
        }
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
        int32_t streamIndex[MAX_STREAMS];
        std::fill(std::begin(streamIndex), std::end(streamIndex), -1);
        std::vector<StreamLayout> layouts;

        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t element = object + DECLARATION_ELEMENTS + i * 12;
            uint32_t stream = Load16(element);
            uint32_t offset = Load16(element + 2);
            uint32_t type = Load32(element + 4);
            uint8_t usage = Load8(element + 9);
            uint8_t usageIndex = Load8(element + 10);
            if (stream == 0xFF || type == D3DDECLTYPE_UNUSED)
                break;
            if (stream >= MAX_STREAMS)
                continue;

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

            if (streamIndex[stream] < 0)
            {
                streamIndex[stream] = int32_t(layouts.size());
                layouts.emplace_back().stream = stream;
            }
            StreamLayout& layout = layouts[streamIndex[stream]];
            ElementClass elementClass = GetElementClass(usage);
            uint32_t hostOffset = layout.HostStride();
            layout.elements.push_back({ offset, type, elementClass, uint8_t(location) });

            RenderFormat format = elementClass == ElementClass::Float ? RenderFormat::R32G32B32A32_FLOAT : RenderFormat::R32G32B32A32_UINT;
            declaration.inputElements.emplace_back(ConvertDeclUsage(usage), usageIndex, location, format, stream, hostOffset);
            if (getenv("NB_DEBUG_DECLS"))
                LOGF_IMPL(Utility, "decl", "{:016x}: stream {} offset {} type {:08X} {}{}", hash, stream, offset, type, ConvertDeclUsage(usage), usageIndex);
        }

        for (auto& layout : layouts)
            layout.hash = XXH3_64bits(layout.elements.data(), layout.elements.size() * sizeof(ElementDecode));
        declaration.streams = std::move(layouts);

        // Every location a shader might declare gets an input, defaulting to zeroes.
        for (auto& l : LOCATIONS)
        {
            if (usedLocations.count(l.location))
                continue;
            usedLocations.insert(l.location);
            RenderFormat format = GetElementClass(l.usage) == ElementClass::Float ? RenderFormat::R32_FLOAT : RenderFormat::R32_UINT;
            declaration.inputElements.emplace_back(ConvertDeclUsage(l.usage), l.usageIndex, l.location, format, DUMMY_SLOT, 0);
        }

        return &(g_declarations[hash] = std::move(declaration));
    }

    // Vertex buffers decoded for the host, kept across frames and refreshed when the guest
    // data changes (checked once per frame).
    struct HostVertexBuffer
    {
        std::unique_ptr<RenderBuffer> buffer;
        uint64_t size = 0;
        uint64_t contentHash = 0;
        uint64_t checkedFrame = ~0ull;
    };

    ankerl::unordered_dense::map<uint64_t, HostVertexBuffer> g_vertexBuffers;

    const RenderBuffer* GetVertexBuffer(const StreamLayout& layout, uint32_t address, uint32_t sizeBytes, uint32_t stride, uint32_t& vertexCount)
    {
        vertexCount = sizeBytes / stride;
        if (vertexCount == 0)
            return nullptr;

        struct { uint32_t address, size, stride, pad; uint64_t layout; } keyData{ address, sizeBytes, stride, 0, layout.hash };
        uint64_t key = XXH3_64bits(&keyData, sizeof(keyData));
        HostVertexBuffer& entry = g_vertexBuffers[key];
        if (entry.checkedFrame == g_frameIndex && entry.buffer)
            return entry.buffer.get();

        const uint8_t* source = (const uint8_t*)g_memory.Translate(address);
        uint64_t contentHash = XXH3_64bits(source, sizeBytes);
        entry.checkedFrame = g_frameIndex;
        if (entry.buffer && entry.contentHash == contentHash)
            return entry.buffer.get();

        uint64_t hostSize = uint64_t(vertexCount) * layout.HostStride();
        if (!entry.buffer || entry.size < hostSize)
        {
            // CONSTANT gives the buffer a device address, for indexed fetches.
            entry.buffer = g_device->createBuffer(RenderBufferDesc::VertexBuffer(hostSize, RenderHeapType::UPLOAD, RenderBufferFlag::CONSTANT));
            entry.size = hostSize;
        }
        entry.contentHash = contentHash;
        auto* destination = (uint8_t*)entry.buffer->map();
        DecodeVertices(layout, source, stride, vertexCount, destination);
        entry.buffer->unmap();
        return entry.buffer.get();
    }

    void ReleaseUnusedVertexBuffers()
    {
        for (auto it = g_vertexBuffers.begin(); it != g_vertexBuffers.end();)
        {
            if (g_frameIndex - it->second.checkedFrame > VERTEX_BUFFER_LIFETIME)
                it = g_vertexBuffers.erase(it);
            else
                ++it;
        }
    }

    // ---- Primitives ----

    // D3DPRIMITIVETYPE: point list 1, line list 2, line strip 3, triangle list 4, fan 5,
    // strip 6, rectangle list 8, quad list 13. Everything is drawn as lists; strips, fans and
    // quads are converted on the CPU (strips may contain reset indices).
    template<typename Fetch>
    bool BuildListIndices(uint32_t primitiveType, uint32_t count, Fetch fetch, uint32_t resetIndex,
        std::vector<uint32_t>& out, RenderPrimitiveTopology& topology)
    {
        out.clear();
        switch (primitiveType)
        {
        case 1:
            topology = RenderPrimitiveTopology::POINT_LIST;
            for (uint32_t i = 0; i < count; i++)
                out.push_back(fetch(i));
            return true;
        case 2:
            topology = RenderPrimitiveTopology::LINE_LIST;
            for (uint32_t i = 0; i + 1 < count; i += 2)
                out.insert(out.end(), { fetch(i), fetch(i + 1) });
            return true;
        case 3:
            topology = RenderPrimitiveTopology::LINE_LIST;
            for (uint32_t i = 0; i + 1 < count; i++)
                out.insert(out.end(), { fetch(i), fetch(i + 1) });
            return true;
        case 4:
            topology = RenderPrimitiveTopology::TRIANGLE_LIST;
            for (uint32_t i = 0; i + 2 < count; i += 3)
                out.insert(out.end(), { fetch(i), fetch(i + 1), fetch(i + 2) });
            return true;
        case 5:
        {
            topology = RenderPrimitiveTopology::TRIANGLE_LIST;
            if (count < 3)
                return true;
            uint32_t center = fetch(0);
            for (uint32_t i = 1; i + 1 < count; i++)
                out.insert(out.end(), { center, fetch(i), fetch(i + 1) });
            return true;
        }
        case 6:
        {
            topology = RenderPrimitiveTopology::TRIANGLE_LIST;
            uint32_t window[2]{};
            uint32_t filled = 0;
            uint32_t parity = 0;
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t index = fetch(i);
                if (index == resetIndex)
                {
                    filled = 0;
                    parity = 0;
                    continue;
                }
                if (filled < 2)
                {
                    window[filled++] = index;
                    continue;
                }
                if (parity == 0)
                    out.insert(out.end(), { window[0], window[1], index });
                else
                    out.insert(out.end(), { window[1], window[0], index });
                window[0] = window[1];
                window[1] = index;
                parity ^= 1;
            }
            return true;
        }
        case 13:
            topology = RenderPrimitiveTopology::TRIANGLE_LIST;
            for (uint32_t i = 0; i + 3 < count; i += 4)
            {
                uint32_t a = fetch(i), b = fetch(i + 1), c = fetch(i + 2), d = fetch(i + 3);
                out.insert(out.end(), { a, b, c, a, c, d });
            }
            return true;
        default:
            return false;
        }
    }

    // ---- Textures and samplers ----
    //
    // Texture fetch constants (six dwords, layout as documented by Xenia): dword0 type,
    // clamp modes, pitch and tiling; dword1 format, endianness and base address; dword2
    // size; dword3 swizzle and filters; dword5 dimension. Textures are decoded from guest
    // memory (untiled and endian-swapped) and cached by their memory description.

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

    uint32_t g_nextTextureDescriptor = NULL_COUNT;
    std::vector<uint32_t> g_freeTextureDescriptors;

    uint32_t AllocateTextureDescriptor()
    {
        if (!g_freeTextureDescriptors.empty())
        {
            uint32_t index = g_freeTextureDescriptors.back();
            g_freeTextureDescriptors.pop_back();
            return index;
        }
        return g_nextTextureDescriptor++;
    }

    // Textures uploaded from guest memory, kept across frames. The game reuses memory for
    // new textures (each menu and loading screen loads its own), so the contents are hashed
    // and checked again when a texture is used in a new frame: every frame for small
    // textures, every TEXTURE_CHECK_INTERVAL frames for large ones.
    constexpr uint64_t TEXTURE_CHECK_INTERVAL = 16;
    constexpr uint64_t SMALL_TEXTURE_SIZE = 1024 * 1024;
    constexpr uint64_t TEXTURE_LIFETIME = 600;

    struct HostTexture
    {
        std::unique_ptr<RenderTexture> texture;
        std::unique_ptr<RenderTextureView> view;
        uint32_t descriptorIndex = 0;
        uint64_t contentHash = 0;
        uint64_t checkedFrame = 0;
        uint64_t usedFrame = 0;
    };

    ankerl::unordered_dense::map<uint64_t, HostTexture> g_textures;

    // Textures replaced or dropped during a frame; released once the frame has executed.
    std::vector<HostTexture> g_retiredTextures;

    void RetireTexture(HostTexture&& texture)
    {
        if (texture.texture)
            g_retiredTextures.push_back(std::move(texture));
    }

    void ReleaseRetiredTextures()
    {
        for (const auto& texture : g_retiredTextures)
            g_freeTextureDescriptors.push_back(texture.descriptorIndex);
        g_retiredTextures.clear();
    }

    void ReleaseUnusedTextures()
    {
        for (auto it = g_textures.begin(); it != g_textures.end();)
        {
            if (g_frameIndex - it->second.usedFrame > TEXTURE_LIFETIME)
            {
                RetireTexture(std::move(it->second));
                it = g_textures.erase(it);
            }
            else
                ++it;
        }
    }

    std::set<uint32_t> g_unsupportedTextureFormats;

    // ---- Render targets ----

    struct HostSurface
    {
        uint32_t width = 0;
        uint32_t height = 0;
        RenderFormat format = RenderFormat::UNKNOWN;
        bool isDepth = false;
        std::unique_ptr<RenderTexture> texture;
        std::unique_ptr<RenderTextureView> view;
        uint32_t descriptorIndex = 0;
        RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
    };

    ankerl::unordered_dense::map<uint64_t, std::unique_ptr<HostSurface>> g_surfaces;
    ankerl::unordered_dense::map<uint64_t, std::unique_ptr<RenderFramebuffer>> g_framebuffers;
    HostSurface* g_boundColor = nullptr;
    HostSurface* g_boundDepth = nullptr;
    bool g_framebufferBound = false;
    HostSurface* g_lastColor = nullptr; // presented when no front buffer was resolved

    // Xenos ColorRenderTargetFormat -> host format.
    RenderFormat ConvertColorTargetFormat(uint32_t format)
    {
        switch (format)
        {
        case 0: case 1: return RenderFormat::R8G8B8A8_UNORM;            // k_8_8_8_8(_GAMMA)
        case 2: case 10: return RenderFormat::R16G16B16A16_UNORM;       // k_2_10_10_10(_AS_10_10_10_10), widened
        case 3: case 12: return RenderFormat::R16G16B16A16_FLOAT;       // k_2_10_10_10_FLOAT(_AS_16_16_16_16)
        case 4: case 6: return RenderFormat::R16G16_FLOAT;              // k_16_16(_FLOAT)
        case 5: case 7: return RenderFormat::R16G16B16A16_FLOAT;        // k_16_16_16_16(_FLOAT)
        case 14: return RenderFormat::R32_FLOAT;                        // k_32_FLOAT
        case 15: return RenderFormat::R32G32_FLOAT;                     // k_32_32_FLOAT
        default:
        {
            static std::set<uint32_t> reported;
            if (reported.insert(format).second)
                LOGF_WARNING("Render target format {} not supported yet", format);
            return RenderFormat::R8G8B8A8_UNORM;
        }
        }
    }

    uint32_t EdramBytesPerPixel(uint32_t colorFormat)
    {
        switch (colorFormat)
        {
        case 5: case 7: case 12: case 15: return 8;
        default: return 4;
        }
    }

    HostSurface* GetSurface(uint32_t object, bool isDepth)
    {
        if (object == 0)
            return nullptr;

        uint32_t surfaceInfo = Load32(object + SURFACE_INFO);
        uint32_t targetInfo = Load32(object + SURFACE_TARGET_INFO);
        uint32_t edramSize = Load32(object + SURFACE_SIZE);
        struct { uint32_t object, surfaceInfo, targetInfo, size; } keyData{ object, surfaceInfo, targetInfo, edramSize };
        uint64_t key = XXH3_64bits(&keyData, sizeof(keyData));
        if (auto it = g_surfaces.find(key); it != g_surfaces.end())
            return it->second.get();

        uint32_t pitch = surfaceInfo & 0x3FFF;
        uint32_t samples = 1u << ((surfaceInfo >> 16) & 3);
        uint32_t colorFormat = (targetInfo >> 16) & 0xF;
        uint32_t bytesPerPixel = isDepth ? 4 : EdramBytesPerPixel(colorFormat);
        if (pitch == 0)
            return nullptr;
        uint32_t height = std::max<uint32_t>(1, edramSize / (pitch * bytesPerPixel * samples));

        auto surface = std::make_unique<HostSurface>();
        surface->width = pitch;
        surface->height = height;
        surface->isDepth = isDepth;
        surface->format = isDepth ? RenderFormat::D32_FLOAT_S8_UINT : ConvertColorTargetFormat(colorFormat);
        surface->texture = isDepth
            ? g_device->createTexture(RenderTextureDesc::DepthTarget(pitch, height, surface->format))
            : g_device->createTexture(RenderTextureDesc::ColorTarget(pitch, height, surface->format));
        RenderTextureViewDesc viewDesc = RenderTextureViewDesc::Texture2D(surface->format);
        viewDesc.mipLevels = 1;
        surface->view = surface->texture->createTextureView(viewDesc);
        surface->descriptorIndex = AllocateTextureDescriptor();
        g_textureDescriptorSet->setTexture(surface->descriptorIndex, surface->texture.get(), RenderTextureLayout::SHADER_READ, surface->view.get());

        LOGF_IMPL(Utility, "Video", "{} surface {:08X}: {}x{} format {} (EDRAM base {}, {} sample(s))",
            isDepth ? "Depth" : "Color", object, pitch, height, isDepth ? (targetInfo >> 16) & 1 : colorFormat, targetInfo & 0xFFF, samples);

        return (g_surfaces[key] = std::move(surface)).get();
    }

    void Transition(HostSurface* surface, RenderTextureLayout layout)
    {
        if (surface == nullptr || surface->layout == layout)
            return;
        RenderTextureBarrier barrier(surface->texture.get(), layout);
        g_commandList->barriers(RenderBarrierStage::GRAPHICS, nullptr, 0, &barrier, 1);
        surface->layout = layout;
        g_framebufferBound = false;
    }

    RenderFramebuffer* GetFramebuffer(HostSurface* color, HostSurface* depth)
    {
        struct { void* color; void* depth; } keyData{ color, depth };
        uint64_t key = XXH3_64bits(&keyData, sizeof(keyData));
        if (auto it = g_framebuffers.find(key); it != g_framebuffers.end())
            return it->second.get();

        const RenderTexture* colorTexture = color != nullptr ? color->texture.get() : nullptr;
        RenderFramebufferDesc desc(color != nullptr ? &colorTexture : nullptr, color != nullptr ? 1 : 0, depth != nullptr ? depth->texture.get() : nullptr);
        return (g_framebuffers[key] = g_device->createFramebuffer(desc)).get();
    }

    // Makes the device's current render target and depth/stencil surface the host targets.
    bool BindTargets()
    {
        HostSurface* color = GetSurface(Load32(g_guestDevice + DEVICE_RENDER_TARGETS), false);
        HostSurface* depth = GetSurface(Load32(g_guestDevice + DEVICE_DEPTH_STENCIL), true);
        if (color == nullptr && depth == nullptr)
            return false;

        // Depth and color must have matching sizes; drop a depth buffer that doesn't fit.
        if (color != nullptr && depth != nullptr && (depth->width < color->width || depth->height < color->height))
            depth = nullptr;

        Transition(color, RenderTextureLayout::COLOR_WRITE);
        Transition(depth, RenderTextureLayout::DEPTH_WRITE);
        if (!g_framebufferBound || color != g_boundColor || depth != g_boundDepth)
        {
            g_commandList->setFramebuffer(GetFramebuffer(color, depth));
            g_boundColor = color;
            g_boundDepth = depth;
            g_framebufferBound = true;
        }
        if (color != nullptr)
            g_lastColor = color;
        return true;
    }

    // Resolved textures: host copies of surfaces, standing in for guest textures at the
    // same (physical) base address.
    struct ResolvedTexture
    {
        uint32_t width = 0;
        uint32_t height = 0;
        RenderFormat format = RenderFormat::UNKNOWN;
        std::unique_ptr<RenderTexture> texture;
        std::unique_ptr<RenderTextureView> view;
        std::unique_ptr<RenderFramebuffer> framebuffer;
        uint32_t descriptorIndex = 0;
        RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
    };

    ankerl::unordered_dense::map<uint32_t, ResolvedTexture> g_resolvedTextures;

    void TransitionResolved(ResolvedTexture& resolved, RenderTextureLayout layout)
    {
        if (resolved.layout == layout)
            return;
        RenderTextureBarrier barrier(resolved.texture.get(), layout);
        g_commandList->barriers(RenderBarrierStage::GRAPHICS, nullptr, 0, &barrier, 1);
        resolved.layout = layout;
        g_framebufferBound = false;
    }

    // Reads a texture fetch constant from guest memory.
    void LoadFetchConstant(uint32_t address, uint32_t fetch[6])
    {
        for (uint32_t i = 0; i < 6; i++)
            fetch[i] = Load32(address + i * 4);
    }

    // Returns the descriptor index for a texture fetch constant, uploading it on first use.
    uint32_t GetTextureDescriptor(const uint32_t fetch[6])
    {
        if ((fetch[0] & 3) != 2)
            return NULL_TEXTURE_2D;

        uint32_t address = GpuToGuest(fetch[1] & 0xFFFFF000);
        if (auto it = g_resolvedTextures.find(address & 0x1FFFFFFF); it != g_resolvedTextures.end())
        {
            TransitionResolved(it->second, RenderTextureLayout::SHADER_READ);
            return it->second.descriptorIndex;
        }

        // The cache key leaves out the sampler fields (clamp modes, filters).
        uint32_t keyData[5] = { fetch[0] & 0xFFC00003, fetch[1], fetch[2], fetch[3] & 0x1FFE, fetch[5] & 0x600 };
        uint64_t key = XXH3_64bits(keyData, sizeof(keyData));
        auto existing = g_textures.find(key);
        if (existing != g_textures.end() && (existing->second.usedFrame == g_frameIndex || !existing->second.texture))
            return existing->second.descriptorIndex;

        uint32_t dimension = (fetch[5] >> 9) & 3;
        uint32_t formatId = fetch[1] & 0x3F;
        XenosFormatInfo info;
        if (dimension != 1 || !GetXenosFormat(formatId, info))
        {
            if (g_unsupportedTextureFormats.insert(formatId | (dimension << 8)).second)
                LOGF_WARNING("Texture format {} (dimension {}) not supported yet", formatId, dimension);
            g_textures[key].descriptorIndex = NULL_TEXTURE_2D;
            return NULL_TEXTURE_2D;
        }

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

        // The guest memory the texture occupies: tiled textures are laid out in 32x32 block tiles.
        uint64_t sourceSize = tiled ? uint64_t((pitchBlocks + 31) & ~31u) * ((blocksHigh + 31) & ~31u) * info.bytesPerBlock
                                    : uint64_t(pitchBlocks) * blocksHigh * info.bytesPerBlock;
        const uint8_t* source = (const uint8_t*)g_memory.Translate(address);

        if (existing != g_textures.end())
        {
            HostTexture& entry = existing->second;
            entry.usedFrame = g_frameIndex;
            uint64_t interval = sourceSize <= SMALL_TEXTURE_SIZE ? 1 : TEXTURE_CHECK_INTERVAL;
            if (g_frameIndex - entry.checkedFrame < interval)
                return entry.descriptorIndex;
            entry.checkedFrame = g_frameIndex;
            uint64_t contentHash = XXH3_64bits(source, sourceSize);
            if (contentHash == entry.contentHash)
                return entry.descriptorIndex;
            // Rewritten: upload it again (draws earlier in this frame keep the old one).
            RetireTexture(std::move(entry));
            g_textures.erase(existing);
        }

        auto [memory, offset] = Allocate(size, 512);
        if (memory == nullptr)
            return NULL_TEXTURE_2D;

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
        g_framebufferBound = false;

        host.descriptorIndex = AllocateTextureDescriptor();
        g_textureDescriptorSet->setTexture(host.descriptorIndex, host.texture.get(), RenderTextureLayout::SHADER_READ, host.view.get());
        host.contentHash = XXH3_64bits(source, sourceSize);
        host.checkedFrame = g_frameIndex;
        host.usedFrame = g_frameIndex;
        uint32_t descriptor = host.descriptorIndex;
        g_textures[key] = std::move(host);
        return descriptor;
    }

    // Samplers from the fetch constant's clamp and filter fields.
    ankerl::unordered_dense::map<uint32_t, uint32_t> g_samplerIndices;
    std::vector<std::unique_ptr<RenderSampler>> g_samplers;

    RenderTextureAddressMode ConvertClamp(uint32_t clamp)
    {
        switch (clamp)
        {
        case 0: return RenderTextureAddressMode::WRAP;
        case 1: return RenderTextureAddressMode::MIRROR;
        case 2: case 4: return RenderTextureAddressMode::CLAMP;
        case 3: case 5: return RenderTextureAddressMode::MIRROR_ONCE;
        default: return RenderTextureAddressMode::BORDER;
        }
    }

    uint32_t GetSamplerIndex(const uint32_t fetch[6])
    {
        uint32_t clamp = (fetch[0] >> 10) & 0x1FF;
        uint32_t filters = (fetch[3] >> 19) & 0x3F;
        uint32_t border = fetch[5] & 3;
        uint32_t key = clamp | (filters << 9) | (border << 15);
        if (auto it = g_samplerIndices.find(key); it != g_samplerIndices.end())
            return it->second;

        RenderSamplerDesc desc;
        desc.addressU = ConvertClamp(clamp & 7);
        desc.addressV = ConvertClamp((clamp >> 3) & 7);
        desc.addressW = ConvertClamp((clamp >> 6) & 7);
        desc.magFilter = (filters & 3) == 0 ? RenderFilter::NEAREST : RenderFilter::LINEAR;
        desc.minFilter = ((filters >> 2) & 3) == 0 ? RenderFilter::NEAREST : RenderFilter::LINEAR;
        desc.mipmapMode = ((filters >> 4) & 3) == 0 ? RenderMipmapMode::NEAREST : RenderMipmapMode::LINEAR;
        desc.borderColor = border == 0 ? RenderBorderColor::TRANSPARENT_BLACK : (border == 2 ? RenderBorderColor::OPAQUE_WHITE : RenderBorderColor::OPAQUE_BLACK);

        uint32_t index = SAMPLER_FIXED_COUNT + uint32_t(g_samplers.size());
        if (index >= SAMPLER_DESCRIPTOR_SIZE)
            return SAMPLER_DEFAULT;
        g_samplers.push_back(g_device->createSampler(desc));
        g_samplerDescriptorSet->setSampler(index, g_samplers.back().get());
        g_samplerIndices[key] = index;
        return index;
    }

    // ---- Pipelines ----

    RenderBlend ConvertBlend(uint32_t blend)
    {
        switch (blend)
        {
        case 0: return RenderBlend::ZERO;
        case 1: return RenderBlend::ONE;
        case 4: return RenderBlend::SRC_COLOR;
        case 5: return RenderBlend::INV_SRC_COLOR;
        case 6: return RenderBlend::SRC_ALPHA;
        case 7: return RenderBlend::INV_SRC_ALPHA;
        case 8: return RenderBlend::DEST_COLOR;
        case 9: return RenderBlend::INV_DEST_COLOR;
        case 10: return RenderBlend::DEST_ALPHA;
        case 11: return RenderBlend::INV_DEST_ALPHA;
        case 12: case 14: return RenderBlend::BLEND_FACTOR;
        case 13: case 15: return RenderBlend::INV_BLEND_FACTOR;
        case 16: return RenderBlend::SRC_ALPHA_SAT;
        default: return RenderBlend::ONE;
        }
    }

    RenderBlendOperation ConvertBlendOperation(uint32_t operation)
    {
        switch (operation)
        {
        case 1: return RenderBlendOperation::SUBTRACT;
        case 2: return RenderBlendOperation::MIN;
        case 3: return RenderBlendOperation::MAX;
        case 4: return RenderBlendOperation::REV_SUBTRACT;
        default: return RenderBlendOperation::ADD;
        }
    }

    RenderStencilOp ConvertStencilOp(uint32_t operation)
    {
        constexpr RenderStencilOp operations[] = { RenderStencilOp::KEEP, RenderStencilOp::ZERO, RenderStencilOp::REPLACE,
            RenderStencilOp::INCREMENT_AND_CLAMP, RenderStencilOp::DECREMENT_AND_CLAMP, RenderStencilOp::INVERT,
            RenderStencilOp::INCREMENT_AND_WRAP, RenderStencilOp::DECREMENT_AND_WRAP };
        return operations[operation & 7];
    }

    RenderComparisonFunction ConvertCompare(uint32_t function)
    {
        constexpr RenderComparisonFunction functions[] = { RenderComparisonFunction::NEVER, RenderComparisonFunction::LESS,
            RenderComparisonFunction::EQUAL, RenderComparisonFunction::LESS_EQUAL, RenderComparisonFunction::GREATER,
            RenderComparisonFunction::NOT_EQUAL, RenderComparisonFunction::GREATER_EQUAL, RenderComparisonFunction::ALWAYS };
        return functions[function & 7];
    }

    // Everything a pipeline depends on.
    struct PipelineKey
    {
        uint64_t vertexShader = 0;
        uint64_t pixelShader = 0;
        uint64_t declaration = 0;
        uint32_t blendControl = 0;
        uint32_t depthControl = 0;
        uint32_t colorMask = 0;
        uint32_t stencilRefMask = 0;
        uint32_t stencilRefMaskBack = 0;
        uint32_t specConstants = 0;
        RenderPrimitiveTopology topology = RenderPrimitiveTopology::UNKNOWN;
        RenderFormat colorFormat = RenderFormat::UNKNOWN;
        RenderFormat depthFormat = RenderFormat::UNKNOWN;
    };

    RenderPipeline* GetPipeline(const PipelineKey& key, RenderShader* vertexShader, RenderShader* pixelShader, const VertexDeclaration& declaration)
    {
        uint64_t hash = XXH3_64bits(&key, sizeof(key));
        if (auto it = g_pipelines.find(hash); it != g_pipelines.end())
            return it->second.get();

        std::vector<RenderInputSlot> slots;
        for (const auto& stream : declaration.streams)
            slots.emplace_back(stream.stream, stream.HostStride());
        slots.emplace_back(DUMMY_SLOT, 0);

        RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = g_pipelineLayout.get();
        desc.vertexShader = vertexShader;
        desc.pixelShader = pixelShader;
        desc.primitiveTopology = key.topology;
        desc.cullMode = RenderCullMode::NONE;
        desc.depthClipEnabled = true;

        if (key.depthFormat != RenderFormat::UNKNOWN)
        {
            desc.depthTargetFormat = key.depthFormat;
            static const bool noDepth = getenv("NB_NO_DEPTH") != nullptr;
            desc.depthEnabled = !noDepth && (key.depthControl & 2) != 0;
            desc.depthWriteEnabled = desc.depthEnabled && (key.depthControl & 4) != 0;
            desc.depthFunction = ConvertCompare((key.depthControl >> 4) & 7);

            // RB_DEPTHCONTROL: stencil enable bit 0, back face stencil bit 7, front function
            // and fail/pass/depth-fail operations from bit 8, back face ones from bit 20.
            // RB_STENCILREFMASK: reference bits 0-7, mask 8-15, write mask 16-23.
            desc.stencilEnabled = (key.depthControl & 1) != 0;
            if (desc.stencilEnabled)
            {
                uint32_t control = key.depthControl;
                desc.stencilFrontFace.compareFunction = ConvertCompare((control >> 8) & 7);
                desc.stencilFrontFace.failOp = ConvertStencilOp((control >> 11) & 7);
                desc.stencilFrontFace.passOp = ConvertStencilOp((control >> 14) & 7);
                desc.stencilFrontFace.depthFailOp = ConvertStencilOp((control >> 17) & 7);
                desc.stencilBackFace = desc.stencilFrontFace;
                if ((control & 0x80) != 0)
                {
                    desc.stencilBackFace.compareFunction = ConvertCompare((control >> 20) & 7);
                    desc.stencilBackFace.failOp = ConvertStencilOp((control >> 23) & 7);
                    desc.stencilBackFace.passOp = ConvertStencilOp((control >> 26) & 7);
                    desc.stencilBackFace.depthFailOp = ConvertStencilOp((control >> 29) & 7);
                }
                desc.stencilReference = key.stencilRefMask & 0xFF;
                desc.stencilReadMask = (key.stencilRefMask >> 8) & 0xFF;
                desc.stencilWriteMask = (key.stencilRefMask >> 16) & 0xFF;
            }
        }

        if (key.colorFormat != RenderFormat::UNKNOWN)
        {
            uint32_t blend = key.blendControl;
            RenderBlendDesc& blendDesc = desc.renderTargetBlend[0];
            blendDesc.srcBlend = ConvertBlend(blend & 0x1F);
            blendDesc.blendOp = ConvertBlendOperation((blend >> 5) & 7);
            blendDesc.dstBlend = ConvertBlend((blend >> 8) & 0x1F);
            blendDesc.srcBlendAlpha = ConvertBlend((blend >> 16) & 0x1F);
            blendDesc.blendOpAlpha = ConvertBlendOperation((blend >> 21) & 7);
            blendDesc.dstBlendAlpha = ConvertBlend((blend >> 24) & 0x1F);
            // ONE/ZERO/ADD on both is no blending.
            blendDesc.blendEnabled = !g_noBlend && (blend & 0x1FFF1FFF) != 0x00010001;
            blendDesc.renderTargetWriteMask = uint8_t(key.colorMask & 0xF);
            desc.renderTargetFormat[0] = key.colorFormat;
            desc.renderTargetCount = 1;
        }

        RenderSpecConstant specConstant(0, key.specConstants);
        desc.specConstants = &specConstant;
        desc.specConstantsCount = 1;

        desc.inputElements = declaration.inputElements.data();
        desc.inputElementsCount = uint32_t(declaration.inputElements.size());
        desc.inputSlots = slots.data();
        desc.inputSlotsCount = uint32_t(slots.size());

        auto pipeline = g_device->createGraphicsPipeline(desc);
        return (g_pipelines[hash] = std::move(pipeline)).get();
    }

    // Copy pipelines (src/gpu/shaders/blit.*), one per destination format.
    ankerl::unordered_dense::map<uint32_t, std::unique_ptr<RenderPipeline>> g_blitPipelines;

    RenderPipeline* GetBlitPipeline(RenderFormat format)
    {
        auto& pipeline = g_blitPipelines[uint32_t(format)];
        if (pipeline)
            return pipeline.get();

        RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = g_pipelineLayout.get();
        desc.vertexShader = g_blitVertexShader.get();
        desc.pixelShader = g_blitPixelShader.get();
        desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        desc.cullMode = RenderCullMode::NONE;
        desc.renderTargetFormat[0] = format;
        desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
        desc.renderTargetCount = 1;
        pipeline = g_device->createGraphicsPipeline(desc);
        return pipeline.get();
    }

    // Draws a texture (descriptor index) into the bound target's viewport.
    void Blit(RenderFormat targetFormat, uint32_t textureIndex, uint32_t samplerIndex, float u0, float v0, float u1, float v1,
        const RenderViewport& viewport)
    {
        g_commandList->setPipeline(GetBlitPipeline(targetFormat));
        BlitConstants constants{ textureIndex, samplerIndex, u0, v0, u1, v1 };
        g_commandList->setGraphicsPushConstants(0, &constants, 0, sizeof(constants));
        g_commandList->setViewports(&viewport, 1);
        RenderRect scissor(int32_t(viewport.x), int32_t(viewport.y), int32_t(viewport.x + viewport.width), int32_t(viewport.y + viewport.height));
        g_commandList->setScissors(&scissor, 1);
        g_commandList->drawInstanced(3, 1, 0, 0);
    }

    // ---- Frames ----

    void BeginFrame()
    {
        if (g_inFrame)
            return;
        g_inFrame = true;
        g_uploadOffset = 0;
        g_drawsThisFrame = 0;
        g_drawsSkippedThisFrame = 0;
        g_framebufferBound = false;

        g_commandList->begin();
        g_commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
        g_commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);
        g_commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1);
        g_commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2);
        g_commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 3);
    }

    // Copies the present target to the swap chain and presents it. gameFrame counts it as
    // one of the game's frames (pacing, statistics, scripted input).
    void SubmitPresentTarget(bool gameFrame)
    {
        // The window may have been resized (frames are synchronous, so nothing is in flight).
        if (g_resizeRequested.exchange(false) || g_swapChain->needsResize())
        {
            g_swapChain->resize();
            g_swapChainFramebuffers.clear();
        }

        uint32_t index = 0;
        bool acquired = !g_swapChain->isEmpty() && g_swapChain->getWidth() > 0 && g_swapChain->getHeight() > 0
            && g_swapChain->acquireTexture(g_acquireSemaphore.get(), &index);

        bool screenshot = false;
        if (gameFrame && !g_screenshotPath.empty())
        {
            uint64_t frame = ++g_frameNumber;
            screenshot = frame == g_screenshotFrame
                || (g_screenshotStep != 0 && frame > g_screenshotFrame && (frame - g_screenshotFrame) % g_screenshotStep == 0
                    && (frame - g_screenshotFrame) / g_screenshotStep < g_screenshotCount);
        }
        if (screenshot)
        {
            RenderTextureBarrier toCopy(g_presentTarget.get(), RenderTextureLayout::COPY_SOURCE);
            g_commandList->barriers(RenderBarrierStage::COPY, nullptr, 0, &toCopy, 1);
            g_commandList->copyTextureRegion(
                RenderTextureCopyLocation::PlacedFootprint(g_readbackBuffer.get(), BACKBUFFER_FORMAT, PRESENT_WIDTH, PRESENT_HEIGHT, 1, PRESENT_WIDTH),
                RenderTextureCopyLocation::Subresource(g_presentTarget.get()));
        }

        if (acquired)
        {
            // Scale the 1280x720 image to the window, keeping its aspect ratio (black bars).
            RenderTexture* backBuffer = g_swapChain->getTexture(index);
            RenderTextureBarrier barriers[] = {
                RenderTextureBarrier(g_presentTarget.get(), RenderTextureLayout::SHADER_READ),
                RenderTextureBarrier(backBuffer, RenderTextureLayout::COLOR_WRITE),
            };
            g_commandList->barriers(RenderBarrierStage::GRAPHICS, nullptr, 0, barriers, 2);

            if (g_swapChainFramebuffers.size() <= index)
                g_swapChainFramebuffers.resize(index + 1);
            if (!g_swapChainFramebuffers[index])
            {
                const RenderTexture* target = backBuffer;
                g_swapChainFramebuffers[index] = g_device->createFramebuffer(RenderFramebufferDesc(&target, 1));
            }
            g_commandList->setFramebuffer(g_swapChainFramebuffers[index].get());
            g_framebufferBound = false;
            g_commandList->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));

            float windowWidth = float(g_swapChain->getWidth());
            float windowHeight = float(g_swapChain->getHeight());
            float scale = std::min(windowWidth / PRESENT_WIDTH, windowHeight / PRESENT_HEIGHT);
            float width = std::max(1.0f, std::floor(PRESENT_WIDTH * scale));
            float height = std::max(1.0f, std::floor(PRESENT_HEIGHT * scale));
            RenderViewport viewport(std::floor((windowWidth - width) / 2), std::floor((windowHeight - height) / 2), width, height, 0.0f, 1.0f);
            Blit(BACKBUFFER_FORMAT, g_presentDescriptor, SAMPLER_LINEAR_CLAMP, 0.0f, 0.0f, 1.0f, 1.0f, viewport);

            RenderTextureBarrier toPresent(backBuffer, RenderTextureLayout::PRESENT);
            g_commandList->barriers(RenderBarrierStage::NONE, nullptr, 0, &toPresent, 1);
        }
        g_commandList->end();

        const RenderCommandList* lists[] = { g_commandList.get() };
        RenderCommandSemaphore* wait[] = { g_acquireSemaphore.get() };
        RenderCommandSemaphore* signal[] = { g_renderSemaphore.get() };
        g_queue->executeCommandLists(lists, 1, acquired ? wait : nullptr, acquired ? 1 : 0, acquired ? signal : nullptr, acquired ? 1 : 0, g_fence.get());
        if (gameFrame)
            PaceFrame();
        if (acquired)
            g_swapChain->present(index, signal, 1);
        g_queue->waitForCommandFence(g_fence.get());

        if (screenshot)
        {
            // B8G8R8A8 -> binary PPM (RGB).
            auto* pixels = (const uint8_t*)g_readbackBuffer->map();
            std::filesystem::path path = g_screenshotPath;
            if (g_screenshotStep != 0)
                path.replace_filename(fmt::format("{}_{}{}", path.stem().string(), g_frameNumber, path.extension().string()));
            std::ofstream file(path, std::ios::binary);
            file << "P6\n" << PRESENT_WIDTH << " " << PRESENT_HEIGHT << "\n255\n";
            for (uint32_t i = 0; i < PRESENT_WIDTH * PRESENT_HEIGHT; i++)
            {
                char rgb[3] = { char(pixels[i * 4 + 2]), char(pixels[i * 4 + 1]), char(pixels[i * 4 + 0]) };
                file.write(rgb, 3);
            }
            g_readbackBuffer->unmap();
            LOGFN("Video: saved frame {} to {}", g_frameNumber, path.string());
        }

        g_inFrame = false;
        // The frame has executed: textures it no longer needs can go.
        ReleaseRetiredTextures();
        g_frameIndex++;
        if ((g_frameIndex % 64) == 0)
        {
            ReleaseUnusedVertexBuffers();
            ReleaseUnusedTextures();
        }

        if (!gameFrame)
            return;

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


    // Copies the front buffer to the present target and on to the swap chain.
    void EndFrame(uint32_t frontBuffer)
    {
        BeginFrame(); // frames with no draws still present

        // What to show: the resolved front buffer, else the last render target drawn to.
        uint32_t sourceDescriptor = NULL_TEXTURE_2D;
        float u1 = 1.0f, v1 = 1.0f;
        if (frontBuffer != 0)
        {
            uint32_t fetch[6];
            LoadFetchConstant(frontBuffer + TEXTURE_FETCH_CONSTANT, fetch);
            uint32_t address = fetch[1] & 0x1FFFF000;
            if (auto it = g_resolvedTextures.find(address); it != g_resolvedTextures.end())
            {
                TransitionResolved(it->second, RenderTextureLayout::SHADER_READ);
                sourceDescriptor = it->second.descriptorIndex;
            }
        }
        // NB_SHOW_RESOLVE=address (hex) presents that resolved texture instead.
        static const uint32_t showResolve = [] { const char* v = getenv("NB_SHOW_RESOLVE"); return v ? uint32_t(std::stoul(v, nullptr, 16)) : 0u; }();
        if (showResolve != 0)
        {
            if (auto it = g_resolvedTextures.find(showResolve & 0x1FFFF000); it != g_resolvedTextures.end())
            {
                TransitionResolved(it->second, RenderTextureLayout::SHADER_READ);
                sourceDescriptor = it->second.descriptorIndex;
            }
        }
        if (sourceDescriptor == NULL_TEXTURE_2D && g_lastColor != nullptr)
        {
            Transition(g_lastColor, RenderTextureLayout::SHADER_READ);
            sourceDescriptor = g_lastColor->descriptorIndex;
            u1 = std::min(1.0f, float(PRESENT_WIDTH) / g_lastColor->width);
            v1 = std::min(1.0f, float(PRESENT_HEIGHT) / g_lastColor->height);
        }

        RenderTextureBarrier toTarget(g_presentTarget.get(), RenderTextureLayout::COLOR_WRITE);
        g_commandList->barriers(RenderBarrierStage::GRAPHICS, nullptr, 0, &toTarget, 1);
        g_commandList->setFramebuffer(g_presentFramebuffer.get());
        g_framebufferBound = false;
        if (sourceDescriptor == NULL_TEXTURE_2D)
            g_commandList->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
        else
            Blit(BACKBUFFER_FORMAT, sourceDescriptor, SAMPLER_LINEAR_CLAMP, 0.0f, 0.0f, u1, v1,
                RenderViewport(0.0f, 0.0f, float(PRESENT_WIDTH), float(PRESENT_HEIGHT), 0.0f, 1.0f));

        SubmitPresentTarget(true);
    }

    // Shows a progress bar (shader preparation on the first run).
    void DrawProgress(float fraction)
    {
        BeginFrame();
        RenderTextureBarrier toTarget(g_presentTarget.get(), RenderTextureLayout::COLOR_WRITE);
        g_commandList->barriers(RenderBarrierStage::GRAPHICS, nullptr, 0, &toTarget, 1);
        g_commandList->setFramebuffer(g_presentFramebuffer.get());
        g_framebufferBound = false;
        g_commandList->clearColor(0, RenderColor(0.06f, 0.05f, 0.04f, 1.0f));
        constexpr int32_t left = 340, right = 940, top = 600, bottom = 616;
        RenderRect track(left - 2, top - 2, right + 2, bottom + 2);
        g_commandList->clearColor(0, RenderColor(0.25f, 0.2f, 0.15f, 1.0f), &track, 1);
        RenderRect fill(left, top, left + int32_t((right - left) * std::clamp(fraction, 0.0f, 1.0f)), bottom);
        if (fill.right > fill.left)
            g_commandList->clearColor(0, RenderColor(0.95f, 0.55f, 0.1f, 1.0f), &fill, 1);
        SubmitPresentTarget(false);
    }

    // ---- Clears and resolves ----

    void ClearTargets(uint32_t colorAddress, bool clearDepth, float depth, uint32_t stencil)
    {
        if (!BindTargets())
            return;
        if (colorAddress != 0 && g_boundColor != nullptr)
        {
            RenderColor color(LoadFloat(colorAddress), LoadFloat(colorAddress + 4), LoadFloat(colorAddress + 8), LoadFloat(colorAddress + 12));
            g_commandList->clearColor(0, color);
        }
        if (clearDepth && g_boundDepth != nullptr)
            g_commandList->clearDepthStencil(true, true, depth, stencil & 0xFF);
    }

    // D3DDevice_Clear: D3DCLEAR flags are render targets 0-3 in bits 0-3, depth 0x10 and
    // stencil 0x20. Like the console, each rectangle is clipped to the viewport and, when
    // enabled, the scissor (as the game set them: device fields, not registers).
    void Clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float depth, uint32_t stencil)
    {
        if (!BindTargets())
            return;
        uint32_t width = g_boundColor != nullptr ? g_boundColor->width : g_boundDepth->width;
        uint32_t height = g_boundColor != nullptr ? g_boundColor->height : g_boundDepth->height;

        float viewportX = LoadFloat(g_guestDevice + DEVICE_VIEWPORT);
        float viewportY = LoadFloat(g_guestDevice + DEVICE_VIEWPORT + 4);
        RenderRect bounds(int32_t(viewportX), int32_t(viewportY),
            int32_t(viewportX + LoadFloat(g_guestDevice + DEVICE_VIEWPORT + 8)), int32_t(viewportY + LoadFloat(g_guestDevice + DEVICE_VIEWPORT + 12)));
        if (Load32(g_guestDevice + DEVICE_SCISSOR_ENABLE) != 0)
        {
            bounds.left = std::max(bounds.left, int32_t(Load32(g_guestDevice + DEVICE_SCISSOR_RECT)));
            bounds.top = std::max(bounds.top, int32_t(Load32(g_guestDevice + DEVICE_SCISSOR_RECT + 4)));
            bounds.right = std::min(bounds.right, int32_t(Load32(g_guestDevice + DEVICE_SCISSOR_RECT + 8)));
            bounds.bottom = std::min(bounds.bottom, int32_t(Load32(g_guestDevice + DEVICE_SCISSOR_RECT + 12)));
        }
        bounds.right = std::min<int32_t>(bounds.right, width);
        bounds.bottom = std::min<int32_t>(bounds.bottom, height);

        std::vector<RenderRect> clearRects;
        for (uint32_t i = 0; i < (rects != 0 ? count : 1); i++)
        {
            RenderRect rect(0, 0, int32_t(width), int32_t(height));
            if (rects != 0)
            {
                uint32_t address = rects + i * 16;
                rect = RenderRect(int32_t(Load32(address)), int32_t(Load32(address + 4)), int32_t(Load32(address + 8)), int32_t(Load32(address + 12)));
            }
            rect.left = std::max(rect.left, bounds.left);
            rect.top = std::max(rect.top, bounds.top);
            rect.right = std::min(rect.right, bounds.right);
            rect.bottom = std::min(rect.bottom, bounds.bottom);
            if (rect.left < rect.right && rect.top < rect.bottom)
                clearRects.push_back(rect);
        }
        if (clearRects.empty())
            return;

        if ((flags & 1) != 0 && g_boundColor != nullptr)
        {
            // D3DCOLOR is ARGB.
            RenderColor value(float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f, float(color & 0xFF) / 255.0f,
                float(color >> 24) / 255.0f);
            g_commandList->clearColor(0, value, clearRects.data(), uint32_t(clearRects.size()));
        }
        bool clearDepth = (flags & 0x10) != 0;
        bool clearStencil = (flags & 0x20) != 0;
        if ((clearDepth || clearStencil) && g_boundDepth != nullptr)
            g_commandList->clearDepthStencil(clearDepth, clearStencil, depth, stencil & 0xFF, clearRects.data(), uint32_t(clearRects.size()));
    }

    // D3DRESOLVE flags: render target index in bits 0-1, depth/stencil bit 2, clear the
    // render target bit 8, clear depth/stencil bit 9.
    void Resolve(uint32_t flags, uint32_t sourceRect, uint32_t destTexture, uint32_t destPoint)
    {
        if (destTexture == 0)
            return;

        bool fromDepth = (flags & 4) != 0;
        HostSurface* source = fromDepth
            ? GetSurface(Load32(g_guestDevice + DEVICE_DEPTH_STENCIL), true)
            : GetSurface(Load32(g_guestDevice + DEVICE_RENDER_TARGETS + (flags & 3) * 4), false);
        if (source == nullptr)
            return;

        uint32_t fetch[6];
        LoadFetchConstant(destTexture + TEXTURE_FETCH_CONSTANT, fetch);
        uint32_t address = fetch[1] & 0x1FFFF000;
        uint32_t width = (fetch[2] & 0x1FFF) + 1;
        uint32_t height = ((fetch[2] >> 13) & 0x1FFF) + 1;

        int32_t x0 = 0, y0 = 0, x1 = int32_t(std::min(width, source->width)), y1 = int32_t(std::min(height, source->height));
        if (sourceRect != 0)
        {
            x0 = int32_t(Load32(sourceRect));
            y0 = int32_t(Load32(sourceRect + 4));
            x1 = int32_t(Load32(sourceRect + 8));
            y1 = int32_t(Load32(sourceRect + 12));
        }
        int32_t dx = 0, dy = 0;
        if (destPoint != 0)
        {
            dx = int32_t(Load32(destPoint));
            dy = int32_t(Load32(destPoint + 4));
        }
        if (x1 <= x0 || y1 <= y0)
            return;

        RenderFormat format = fromDepth ? RenderFormat::R32_FLOAT : source->format;
        ResolvedTexture& resolved = g_resolvedTextures[address];
        if (!resolved.texture || resolved.width != width || resolved.height != height || resolved.format != format)
        {
            resolved.width = width;
            resolved.height = height;
            resolved.format = format;
            resolved.texture = g_device->createTexture(RenderTextureDesc::ColorTarget(width, height, format));
            RenderTextureViewDesc viewDesc = RenderTextureViewDesc::Texture2D(format);
            viewDesc.mipLevels = 1;
            resolved.view = resolved.texture->createTextureView(viewDesc);
            const RenderTexture* target = resolved.texture.get();
            resolved.framebuffer = g_device->createFramebuffer(RenderFramebufferDesc(&target, 1));
            if (resolved.descriptorIndex == 0)
                resolved.descriptorIndex = AllocateTextureDescriptor();
            g_textureDescriptorSet->setTexture(resolved.descriptorIndex, resolved.texture.get(), RenderTextureLayout::SHADER_READ, resolved.view.get());
            resolved.layout = RenderTextureLayout::UNKNOWN;
            LOGF_IMPL(Utility, "Video", "Resolve target {:08X}: {}x{} from a {} surface", address, width, height, fromDepth ? "depth" : "color");
        }

        Transition(source, RenderTextureLayout::SHADER_READ);
        TransitionResolved(resolved, RenderTextureLayout::COLOR_WRITE);
        g_commandList->setFramebuffer(resolved.framebuffer.get());
        g_framebufferBound = false;
        Blit(format, source->descriptorIndex, SAMPLER_POINT_CLAMP,
            float(x0) / source->width, float(y0) / source->height, float(x1) / source->width, float(y1) / source->height,
            RenderViewport(float(dx), float(dy), float(x1 - x0), float(y1 - y0), 0.0f, 1.0f));
        TransitionResolved(resolved, RenderTextureLayout::SHADER_READ);
    }

    // ---- Draws ----

    struct DrawCall
    {
        uint32_t primitiveType = 0;
        uint32_t count = 0;           // indices, or vertices for non-indexed draws
        bool indexed = false;

        // Draws from user memory (DrawVerticesUP / DrawIndexedVerticesUP).
        bool userPointer = false;
        uint32_t vertexData = 0;
        uint32_t vertexStride = 0;
        uint32_t vertexCount = 0;
        uint32_t indexData = 0;
        uint32_t indexSize = 2;

        // Draws from buffers (DrawVertices / DrawIndexedVertices).
        int32_t baseVertex = 0;
        uint32_t start = 0;            // first index, or first vertex
    };

    void Draw(const DrawCall& call)
    {
        std::lock_guard lock(g_videoMutex);
        if (g_guestDevice == 0)
            return;
        BeginFrame();

        uint64_t vsHash, psHash;
        {
            std::lock_guard shaderLock(g_shaderMutex);
            auto vs = g_shaderHashes.find(Load32(g_guestDevice + DEVICE_VERTEX_SHADER));
            auto ps = g_shaderHashes.find(Load32(g_guestDevice + DEVICE_PIXEL_SHADER));
            if (vs == g_shaderHashes.end() || ps == g_shaderHashes.end())
            {
                static int reported = 0;
                if (reported++ < 3)
                {
                    uint32_t object = Load32(g_guestDevice + DEVICE_VERTEX_SHADER);
                    std::string words;
                    for (uint32_t i = 0; i < 24 && object != 0; i++)
                        words += fmt::format(" {:08X}", Load32(object + i * 4));
                    LOGF_WARNING("Unknown shader objects: vs {:08X} ps {:08X}:{}", object, Load32(g_guestDevice + DEVICE_PIXEL_SHADER), words);
                    uint32_t known = g_shaderHashes.begin()->first;
                    words.clear();
                    for (uint32_t i = 0; i < 24; i++)
                        words += fmt::format(" {:08X}", Load32(known + i * 4));
                    LOGF_WARNING("A known shader object {:08X}:{}", known, words);
                }
                return Skip("no vertex or pixel shader bound");
            }
            vsHash = vs->second;
            psHash = ps->second;
        }

        RenderShader* vertexShader = GetShader(vsHash);
        RenderShader* pixelShader = GetShader(psHash);
        if (vertexShader == nullptr || pixelShader == nullptr)
            return Skip("shader not translated");

        const VertexDeclaration* declaration = GetDeclaration(Load32(g_guestDevice + DEVICE_VERTEX_DECLARATION));
        if (declaration == nullptr)
            return Skip("no vertex declaration");

        // NB_ONLY_DECL=hash draws only geometry with that vertex declaration (debugging).
        static const uint64_t onlyDeclaration = [] { const char* v = getenv("NB_ONLY_DECL"); return v ? std::stoull(v, nullptr, 16) : 0ull; }();
        if (onlyDeclaration != 0 && declaration->hash != onlyDeclaration && !call.userPointer)
            return;

        // Indices, as a list topology.
        static std::vector<uint32_t> indices;
        RenderPrimitiveTopology topology = RenderPrimitiveTopology::UNKNOWN;
        bool built = false;
        if (call.userPointer)
        {
            if (call.indexed)
            {
                uint32_t data = call.indexData;
                bool wide = call.indexSize == 4;
                built = BuildListIndices(call.primitiveType, call.count,
                    [&](uint32_t i) { return wide ? Load32(data + i * 4) : uint32_t(Load16(data + i * 2)); },
                    wide ? 0xFFFFFFFF : 0xFFFF, indices, topology);
            }
            else
            {
                built = BuildListIndices(call.primitiveType, call.count, [](uint32_t i) { return i; }, ~0u, indices, topology);
            }
        }
        else if (call.indexed)
        {
            uint32_t indexBuffer = Load32(g_guestDevice + DEVICE_INDEX_BUFFER);
            if (indexBuffer == 0)
                return Skip("no index buffer");
            bool wide = (Load32(indexBuffer) & 0x80000000) != 0;
            uint32_t data = Load32(indexBuffer + BUFFER_ADDRESS) + call.start * (wide ? 4 : 2);
            built = BuildListIndices(call.primitiveType, call.count,
                [&](uint32_t i) { return wide ? Load32(data + i * 4) : uint32_t(Load16(data + i * 2)); },
                wide ? 0xFFFFFFFF : 0xFFFF, indices, topology);
        }
        else
        {
            uint32_t first = call.start;
            built = BuildListIndices(call.primitiveType, call.count, [&](uint32_t i) { return first + i; }, ~0u, indices, topology);
        }
        if (!built)
            return Skip(fmt::format("primitive type {}", call.primitiveType));
        if (indices.empty())
            return;

        // Vertex streams.
        RenderVertexBufferView views[MAX_STREAMS];
        RenderInputSlot slots[MAX_STREAMS];
        uint32_t streamSlots[MAX_STREAMS];
        uint64_t streamAddresses[MAX_STREAMS]{};
        uint32_t streamCount = 0;
        for (const auto& layout : declaration->streams)
        {
            if (call.userPointer)
            {
                if (layout.stream != 0 || call.vertexStride == 0)
                    return Skip("user pointer draw with more than stream 0");
                uint64_t hostSize = std::max<uint64_t>(uint64_t(call.vertexCount) * layout.HostStride(), 16);
                auto [memory, offset] = Allocate(hostSize);
                if (memory == nullptr)
                    return Skip("upload buffer full");
                DecodeVertices(layout, (const uint8_t*)g_memory.Translate(call.vertexData), call.vertexStride, call.vertexCount, memory);
                views[streamCount] = RenderVertexBufferView(g_uploadBuffer->at(offset), uint32_t(hostSize));
                streamAddresses[streamCount] = g_uploadAddress + offset;
            }
            else
            {
                // Stream n uses vertex fetch constant 95 - n (two dwords: address | type, size | endian).
                uint32_t fetchAddress = g_guestDevice + DEVICE_FETCH_CONSTANTS + (95 - layout.stream) * 8;
                uint32_t fetch0 = Load32(fetchAddress);
                uint32_t fetch1 = Load32(fetchAddress + 4);
                uint32_t stride = Load8(g_guestDevice + DEVICE_STREAM_STRIDES + layout.stream) * 4;
                if ((fetch0 & 3) != 3 || stride == 0)
                    return Skip("vertex stream not set");
                uint32_t address = GpuToGuest(fetch0 & 0xFFFFFFFC);
                uint32_t size = fetch1 & 0x3FFFFFC;
                if (size < SMALL_STREAM_SIZE)
                {
                    // Small streams (bone palettes, per-object data) can be rewritten between
                    // draws, so they are decoded for each draw.
                    uint32_t vertexCount = size / stride;
                    uint64_t hostSize = std::max<uint64_t>(uint64_t(vertexCount) * layout.HostStride(), 16);
                    auto [memory, offset] = Allocate(hostSize);
                    if (memory == nullptr)
                        return Skip("upload buffer full");
                    DecodeVertices(layout, (const uint8_t*)g_memory.Translate(address), stride, vertexCount, memory);
                    views[streamCount] = RenderVertexBufferView(g_uploadBuffer->at(offset), uint32_t(hostSize));
                    streamAddresses[streamCount] = g_uploadAddress + offset;
                }
                else
                {
                    uint32_t vertexCount = 0;
                    const RenderBuffer* buffer = GetVertexBuffer(layout, address, size, stride, vertexCount);
                    if (buffer == nullptr)
                        return Skip("empty vertex buffer");
                    views[streamCount] = RenderVertexBufferView(buffer, uint32_t(uint64_t(vertexCount) * layout.HostStride()));
                    streamAddresses[streamCount] = buffer->getDeviceAddress();
                }
                if (DebugThisFrame() && (g_drawsThisFrame < 4 || declaration->streams.size() > 1))
                {
                    float v[4];
                    DecodeVertexElement((const uint8_t*)g_memory.Translate(address) + stride * (call.baseVertex + indices[0]) + layout.elements[0].sourceOffset, layout.elements[0].type, v);
                    LOGF_IMPL(Utility, "draw", "  stream {} @{:08X} size {} stride {} decl {:016x} v[{}] = ({}, {}, {}, {})", layout.stream, address, size, stride,
                        declaration->hash, indices[0], v[0], v[1], v[2], v[3]);
                }
            }
            slots[streamCount] = RenderInputSlot(layout.stream, layout.HostStride());
            streamSlots[streamCount] = layout.stream;
            streamCount++;
        }

        // Constants: the guest device holds them big-endian. Textures are looked up (and
        // uploaded) before the targets are bound, as uploads end the render pass.
        auto [vsMemory, vsOffset] = UploadSwapped32(g_guestDevice + DEVICE_VS_CONSTANTS, VS_CONSTANTS_SIZE);
        auto [psMemory, psOffset] = UploadSwapped32(g_guestDevice + DEVICE_PS_CONSTANTS, PS_CONSTANTS_SIZE);
        auto [sharedMemory, sharedOffset] = Allocate(sizeof(SharedConstants));
        if (vsMemory == nullptr || psMemory == nullptr || sharedMemory == nullptr)
            return Skip("upload buffer full");

        SharedConstants shared{};
        for (uint32_t i = 0; i < 16; i++)
        {
            uint32_t fetch[6];
            LoadFetchConstant(g_guestDevice + DEVICE_FETCH_CONSTANTS + i * 24, fetch);
            shared.texture2DIndices[i] = GetTextureDescriptor(fetch);
            if (DebugThisFrame() && (fetch[0] & 3) == 2)
            {
                uint32_t address = GpuToGuest(fetch[1] & 0xFFFFF000);
                LOGF_IMPL(Utility, "draw", "  texture {} @{:08X} format {} {}x{} tiled {} resolved {}", i, address, fetch[1] & 0x3F, (fetch[2] & 0x1FFF) + 1,
                    ((fetch[2] >> 13) & 0x1FFF) + 1, fetch[0] >> 31, g_resolvedTextures.contains(address & 0x1FFFFFFF));
            }
            shared.texture3DIndices[i] = NULL_TEXTURE_3D;
            shared.textureCubeIndices[i] = NULL_TEXTURE_CUBE;
            shared.samplerIndices[i] = (fetch[0] & 3) == 2 ? GetSamplerIndex(fetch) : SAMPLER_DEFAULT;
        }
        uint32_t vsBooleans = Load32(g_guestDevice + DEVICE_BOOL_CONSTANTS);
        uint32_t psBooleans = Load32(g_guestDevice + DEVICE_BOOL_CONSTANTS + 16);
        shared.booleans = (vsBooleans & 0xFFFF) | (psBooleans << 16);
        shared.swappedTexcoords = 0; // vertex data is decoded per element
        for (uint32_t i = 0; i < streamCount; i++)
        {
            const StreamLayout& layout = declaration->streams[i];
            for (uint32_t element = 0; element < layout.elements.size(); element++)
                shared.fetches[layout.elements[element].location] = { streamAddresses[i], layout.HostStride(), element * 16 };
        }

        if (!BindTargets())
            return Skip("no render target");

        // Render state, from the device's register image.
        uint32_t colorControl = Reg(RB_COLORCONTROL);
        PipelineKey key;
        key.vertexShader = vsHash;
        key.pixelShader = psHash;
        key.declaration = declaration->hash;
        key.topology = topology;
        key.colorFormat = g_boundColor != nullptr ? g_boundColor->format : RenderFormat::UNKNOWN;
        key.depthFormat = g_boundDepth != nullptr ? g_boundDepth->format : RenderFormat::UNKNOWN;
        key.blendControl = Reg(RB_BLENDCONTROL0);
        key.depthControl = Reg(RB_DEPTHCONTROL);
        key.colorMask = Reg(RB_COLOR_MASK) & 0xF;
        if ((key.depthControl & 1) != 0)
        {
            key.stencilRefMask = Reg(RB_STENCILREFMASK) & 0xFFFFFF;
            key.stencilRefMaskBack = Reg(RB_STENCILREFMASK_BF) & 0xFFFFFF;
        }
        uint32_t alphaFunction = colorControl & 7;
        if ((colorControl & 8) != 0 && alphaFunction != 7 && (GetSpecConstantsMask(psHash) & SPEC_CONSTANT_ALPHA_TEST) != 0)
        {
            // The translated shaders discard below a threshold: right for GREATER/GEQUAL,
            // the usual alpha test functions.
            key.specConstants |= SPEC_CONSTANT_ALPHA_TEST;
            shared.alphaThreshold = RegFloat(RB_ALPHA_REF);
            if (alphaFunction == 4) // GREATER
                shared.alphaThreshold = std::nextafter(shared.alphaThreshold, 2.0f);
        }
        memcpy(sharedMemory, &shared, sizeof(shared));

        RenderPipeline* pipeline = GetPipeline(key, vertexShader, pixelShader, *declaration);
        if (pipeline == nullptr)
            return Skip("pipeline creation failed");

        // Viewport from PA_CL_VPORT_*, or the whole target.
        uint32_t targetWidth = g_boundColor != nullptr ? g_boundColor->width : g_boundDepth->width;
        uint32_t targetHeight = g_boundColor != nullptr ? g_boundColor->height : g_boundDepth->height;
        RenderViewport viewport(0.0f, 0.0f, float(targetWidth), float(targetHeight), 0.0f, 1.0f);
        uint32_t vte = Reg(PA_CL_VTE_CNTL);
        if ((vte & 0xF) == 0xF)
        {
            float xScale = std::fabs(RegFloat(PA_CL_VPORT_XSCALE));
            float yScale = std::fabs(RegFloat(PA_CL_VPORT_YSCALE));
            float xOffset = RegFloat(PA_CL_VPORT_XOFFSET);
            float yOffset = RegFloat(PA_CL_VPORT_YOFFSET);
            if (xScale > 0.0f && yScale > 0.0f)
                viewport = RenderViewport(xOffset - xScale, yOffset - yScale, xScale * 2.0f, yScale * 2.0f, 0.0f, 1.0f);
        }
        if ((vte & 0x30) == 0x30)
        {
            float zScale = RegFloat(PA_CL_VPORT_ZSCALE);
            float zOffset = RegFloat(PA_CL_VPORT_ZOFFSET);
            // A negative scale flips depth (Vulkan allows minDepth > maxDepth).
            viewport.minDepth = std::clamp(zOffset, 0.0f, 1.0f);
            viewport.maxDepth = std::clamp(zOffset + zScale, 0.0f, 1.0f);
        }
        RenderRect scissor(0, 0, int32_t(targetWidth), int32_t(targetHeight));
        uint32_t scissorTopLeft = Reg(PA_SC_SCISSOR_TL);
        uint32_t scissorBottomRight = Reg(PA_SC_SCISSOR_BR);
        if (scissorBottomRight != 0)
        {
            scissor.left = std::min<int32_t>(scissorTopLeft & 0x7FFF, targetWidth);
            scissor.top = std::min<int32_t>((scissorTopLeft >> 16) & 0x7FFF, targetHeight);
            scissor.right = std::clamp<int32_t>(scissorBottomRight & 0x7FFF, scissor.left, targetWidth);
            scissor.bottom = std::clamp<int32_t>((scissorBottomRight >> 16) & 0x7FFF, scissor.top, targetHeight);
        }

        PushConstants pushConstants{ g_uploadAddress + vsOffset, g_uploadAddress + psOffset, g_uploadAddress + sharedOffset };

        auto [indexMemory, indexOffset] = Allocate(indices.size() * 4);
        auto [dummyMemory, dummyOffset] = Allocate(64);
        if (indexMemory == nullptr || dummyMemory == nullptr)
            return Skip("upload buffer full");
        memcpy(indexMemory, indices.data(), indices.size() * 4);
        memset(dummyMemory, 0, 64);

        if (DebugThisFrame())
        {
            LOGF_IMPL(Utility, "draw", "#{} decl {:016x} prim {} count {} indexed {} up {} vs {:016x} ps {:016x} rt {}x{} depth {} blend {:08X} zctl {:08X} vte {:08X} vp ({:.1f},{:.1f} {:.1f}x{:.1f} z {:.3f}-{:.3f}) streams {} cmask {:X} sref {:06X} sc {:08X}-{:08X} mode {:08X}",
                g_drawsThisFrame, declaration->hash, call.primitiveType, call.count, call.indexed, call.userPointer, vsHash, psHash, targetWidth, targetHeight,
                g_boundDepth != nullptr, key.blendControl, key.depthControl, vte, viewport.x, viewport.y, viewport.width, viewport.height,
                viewport.minDepth, viewport.maxDepth, streamCount, key.colorMask, Reg(RB_STENCILREFMASK), Reg(PA_SC_SCISSOR_TL), Reg(PA_SC_SCISSOR_BR), Reg(PA_SU_SC_MODE_CNTL));
        }

        g_commandList->setPipeline(pipeline);
        g_commandList->setGraphicsPushConstants(0, &pushConstants, 0, sizeof(pushConstants));
        g_commandList->setViewports(&viewport, 1);
        g_commandList->setScissors(&scissor, 1);
        for (uint32_t i = 0; i < streamCount; i++)
            g_commandList->setVertexBuffers(streamSlots[i], &views[i], 1, &slots[i]);
        RenderVertexBufferView dummyView(g_uploadBuffer->at(dummyOffset), 64);
        RenderInputSlot dummySlot(DUMMY_SLOT, 0);
        g_commandList->setVertexBuffers(DUMMY_SLOT, &dummyView, 1, &dummySlot);
        RenderIndexBufferView indexView(g_uploadBuffer->at(indexOffset), uint32_t(indices.size() * 4), RenderFormat::R32_UINT);
        g_commandList->setIndexBuffer(&indexView);
        g_commandList->drawIndexedInstanced(uint32_t(indices.size()), 1, 0, call.baseVertex, 0);
        g_drawsThisFrame++;
    }

    // ---- Setup ----

    void CreatePipelineLayout()
    {
        RenderDescriptorSetBuilder textureSet;
        textureSet.begin();
        textureSet.addTexture(0, TEXTURE_DESCRIPTOR_SIZE);
        textureSet.end(true, TEXTURE_DESCRIPTOR_SIZE);
        g_textureDescriptorSet = textureSet.create(g_device.get());

        // Null textures read as white so untextured geometry stays visible; the component
        // mapping does it without uploading anything.
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

        RenderSamplerDesc pointClamp;
        pointClamp.minFilter = pointClamp.magFilter = RenderFilter::NEAREST;
        pointClamp.mipmapMode = RenderMipmapMode::NEAREST;
        pointClamp.addressU = pointClamp.addressV = pointClamp.addressW = RenderTextureAddressMode::CLAMP;
        RenderSamplerDesc linearClamp;
        linearClamp.addressU = linearClamp.addressV = linearClamp.addressW = RenderTextureAddressMode::CLAMP;
        g_fixedSamplers[SAMPLER_DEFAULT] = g_device->createSampler(RenderSamplerDesc());
        g_fixedSamplers[SAMPLER_POINT_CLAMP] = g_device->createSampler(pointClamp);
        g_fixedSamplers[SAMPLER_LINEAR_CLAMP] = g_device->createSampler(linearClamp);
        for (uint32_t i = 0; i < SAMPLER_FIXED_COUNT; i++)
            g_samplerDescriptorSet->setSampler(i, g_fixedSamplers[i].get());

        RenderPipelineLayoutBuilder layout;
        layout.begin(false, true);
        layout.addDescriptorSet(textureSet);
        layout.addDescriptorSet(textureSet);
        layout.addDescriptorSet(textureSet);
        layout.addDescriptorSet(samplerSet);
        layout.addPushConstant(0, 4, sizeof(PushConstants), RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
        layout.end();
        g_pipelineLayout = layout.create(g_device.get());

        g_blitVertexShader = g_device->createShader(g_blitVS, sizeof(g_blitVS), "main", RenderShaderFormat::SPIRV);
        g_blitPixelShader = g_device->createShader(g_blitPS, sizeof(g_blitPS), "main", RenderShaderFormat::SPIRV);
    }

    // The shader container's hash, as XenosRecomp computes it (XXH3 over virtual + physical parts).
    uint64_t HashShaderContainer(uint32_t container)
    {
        if (container == 0)
            return 0;
        uint32_t size = Load32(container + 4) + Load32(container + 8);
        return XXH3_64bits(g_memory.Translate(container), size);
    }

    // NB_DUMP_SHADERS=dir writes the shader containers the game creates there (debugging;
    // tools/translate_shaders.sh translates such a folder offline).
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

    g_presentTarget = g_device->createTexture(RenderTextureDesc::ColorTarget(PRESENT_WIDTH, PRESENT_HEIGHT, BACKBUFFER_FORMAT));
    const RenderTexture* target = g_presentTarget.get();
    g_presentFramebuffer = g_device->createFramebuffer(RenderFramebufferDesc(&target, 1));
    g_presentDescriptor = AllocateTextureDescriptor();
    g_textureDescriptorSet->setTexture(g_presentDescriptor, g_presentTarget.get(), RenderTextureLayout::SHADER_READ);

    g_uploadBuffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(UPLOAD_BUFFER_SIZE,
        RenderBufferFlag::CONSTANT | RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
    g_uploadMemory = (uint8_t*)g_uploadBuffer->map();
    g_uploadAddress = g_uploadBuffer->getDeviceAddress();

    if (const char* screenshot = getenv("NB_SCREENSHOT"))
    {
        g_screenshotPath = screenshot;
        if (auto colon = g_screenshotPath.rfind(':'); colon != std::string::npos)
        {
            std::string frames = g_screenshotPath.substr(colon + 1);
            g_screenshotPath.resize(colon);
            g_screenshotFrame = std::stoull(frames);
            if (auto plus = frames.find('+'); plus != std::string::npos)
            {
                g_screenshotStep = std::max<uint64_t>(1, std::stoull(frames.substr(plus + 1)));
                auto times = frames.find('*', plus);
                g_screenshotCount = times != std::string::npos ? std::stoull(frames.substr(times + 1)) : 10;
            }
        }
        g_readbackBuffer = g_device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(PRESENT_WIDTH) * PRESENT_HEIGHT * 4));
    }

    shader_store::Init();
    LOGN("Video: Vulkan device and swap chain ready");
    return true;
}

void Video::OnWindowResized()
{
    g_resizeRequested = true;
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

    // First run: translate the whole game's shaders (shows a progress bar).
    shader_store::Precache([](float fraction)
        {
            std::lock_guard lock(g_videoMutex);
            DrawProgress(fraction);
        });
}

// D3DDevice_BeginTiling(dev, Flags, Count, pTileRects, pClearColor, ClearZ (f1), ClearStencil (r9)).
// Flag 1 (D3DTILING_SKIP_FIRST_TILE_CLEAR) keeps the current contents.
PPC_FUNC(sub_82F37138)
{
    TRACE_ENTER(0x82F37138);
    uint32_t flags = ctx.r4.u32;
    uint32_t clearColor = ctx.r7.u32;
    float clearZ = float(ctx.f1.f64);
    uint32_t clearStencil = ctx.r9.u32;
    {
        std::lock_guard lock(g_videoMutex);
        if (g_guestDevice != 0)
        {
            BeginFrame();
            if ((flags & 1) == 0)
                ClearTargets(clearColor, true, clearZ, clearStencil);
        }
    }
    __imp__sub_82F37138(ctx, base);
}

// D3DDevice_EndTiling(dev, ResolveFlags, pResolveRects, pDestTexture, pClearColor, ClearZ, ClearStencil, pParameters).
PPC_FUNC(sub_82F37798)
{
    TRACE_ENTER(0x82F37798);
    {
        std::lock_guard lock(g_videoMutex);
        if (g_guestDevice != 0)
        {
            BeginFrame();
            Resolve(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0);
        }
    }
    __imp__sub_82F37798(ctx, base);
}

// D3DDevice_Resolve(dev, Flags, pSourceRect, pDestTexture, pDestPoint, DestLevel, DestSliceOrFace,
//     pClearColor, ClearZ, ClearStencil, pParameters).
PPC_FUNC(sub_82F398F0)
{
    TRACE_ENTER(0x82F398F0);
    uint32_t flags = ctx.r4.u32;
    uint32_t clearColor = ctx.r10.u32;
    float clearZ = float(ctx.f1.f64);
    {
        std::lock_guard lock(g_videoMutex);
        if (g_guestDevice != 0)
        {
            BeginFrame();
            Resolve(flags, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
            if ((flags & 0x300) != 0)
                ClearTargets((flags & 0x100) != 0 ? clearColor : 0, (flags & 0x200) != 0, clearZ, 0);
        }
    }
    __imp__sub_82F398F0(ctx, base);
}

// D3DDevice_Clear(dev, Count, pRects, Flags, Color, Z (f1), Stencil (r9)). The game's menus
// and HUD (Scaleform) clear the stencil buffer before drawing each mask.
PPC_FUNC(sub_82F35788)
{
    TRACE_ENTER(0x82F35788);
    {
        std::lock_guard lock(g_videoMutex);
        if (g_guestDevice != 0)
        {
            BeginFrame();
            Clear(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, float(ctx.f1.f64), ctx.r9.u32);
        }
    }
    __imp__sub_82F35788(ctx, base);
}

// D3DDevice_Swap(dev, pFrontBuffer, pParameters): show the frame.
PPC_FUNC(sub_82F33D50)
{
    TRACE_ENTER(0x82F33D50);
    {
        std::lock_guard lock(g_videoMutex);
        EndFrame(ctx.r4.u32);
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
    shader_store::Ensure((const uint8_t*)g_memory.Translate(Load32(ctx.r5.u32)), hash);
    DumpShader(Load32(ctx.r5.u32), hash, false);
    RecordShader(ctx.r3.u32, hash);
    __imp__sub_82F30050(ctx, base);
}

PPC_FUNC(sub_82F2FF28)
{
    TRACE_ENTER(0x82F2FF28);
    uint64_t hash = HashShaderContainer(Load32(ctx.r5.u32));
    shader_store::Ensure((const uint8_t*)g_memory.Translate(Load32(ctx.r5.u32)), hash);
    DumpShader(Load32(ctx.r5.u32), hash, true);
    RecordShader(ctx.r3.u32, hash);
    __imp__sub_82F2FF28(ctx, base);
}

PPC_FUNC(sub_82F30188)
{
    uint64_t hash = HashShaderContainer(ctx.r3.u32);
    shader_store::Ensure((const uint8_t*)g_memory.Translate(ctx.r3.u32), hash);
    DumpShader(ctx.r3.u32, hash, false);
    __imp__sub_82F30188(ctx, base);
    RecordShader(ctx.r3.u32, hash);
}

PPC_FUNC(sub_82F2FF60)
{
    uint64_t hash = HashShaderContainer(ctx.r3.u32);
    shader_store::Ensure((const uint8_t*)g_memory.Translate(ctx.r3.u32), hash);
    DumpShader(ctx.r3.u32, hash, true);
    __imp__sub_82F2FF60(ctx, base);
    RecordShader(ctx.r3.u32, hash);
}

// D3DDevice_DrawVertices(dev, PrimitiveType, StartVertex, VertexCount).
PPC_FUNC(sub_82F38988)
{
    TRACE_ENTER(0x82F38988);
    DrawCall call;
    call.primitiveType = ctx.r4.u32;
    call.start = ctx.r5.u32;
    call.count = ctx.r6.u32;
    Draw(call);
    __imp__sub_82F38988(ctx, base);
}

// D3DDevice_DrawIndexedVertices(dev, PrimitiveType, BaseVertexIndex, StartIndex, IndexCount).
PPC_FUNC(sub_82F38D78)
{
    TRACE_ENTER(0x82F38D78);
    DrawCall call;
    call.primitiveType = ctx.r4.u32;
    call.indexed = true;
    call.baseVertex = int32_t(ctx.r5.u32);
    call.start = ctx.r6.u32;
    call.count = ctx.r7.u32;
    Draw(call);
    __imp__sub_82F38D78(ctx, base);
}

// Draws from vertices the game writes into the command buffer: BeginVertices /
// BeginIndexedVertices return pointers to fill, and the draw is made at EndVertices. The
// game uses these directly (particles, such as fire) as well as through DrawVerticesUP and
// DrawIndexedVerticesUP, which call Begin*, copy the data and end the draw inline.
extern "C" PPC_FUNC(__imp__sub_82F37EF0); // D3DDevice_BeginVertices
extern "C" PPC_FUNC(__imp__sub_82F383D0); // D3DDevice_BeginIndexedVertices
extern "C" PPC_FUNC(__imp__sub_82F37EE0); // D3DDevice_EndVertices

namespace
{
    std::optional<DrawCall> g_pendingVertices;

    void DrawPendingVertices()
    {
        if (g_pendingVertices)
        {
            DrawCall call = *g_pendingVertices;
            g_pendingVertices.reset();
            Draw(call);
        }
    }
}

// D3DDevice_BeginVertices(dev, PrimitiveType, VertexCount, VertexStreamZeroStride) -> vertex pointer.
PPC_FUNC(sub_82F37EF0)
{
    DrawCall call;
    call.primitiveType = ctx.r4.u32;
    call.count = ctx.r5.u32;
    call.userPointer = true;
    call.vertexCount = ctx.r5.u32;
    call.vertexStride = ctx.r6.u32;
    __imp__sub_82F37EF0(ctx, base);
    g_pendingVertices.reset();
    if (ctx.r3.u32 != 0)
    {
        call.vertexData = ctx.r3.u32;
        g_pendingVertices = call;
    }
}

// D3DDevice_BeginIndexedVertices(dev, PrimitiveType, BaseVertexIndex, NumVertices, IndexCount,
//     IndexDataFormat, VertexStreamZeroStride, ppIndexData, ppVertexData (stack)) -> HRESULT.
PPC_FUNC(sub_82F383D0)
{
    DrawCall call;
    call.primitiveType = ctx.r4.u32;
    call.baseVertex = int32_t(ctx.r5.u32); // -MinVertexIndex: the vertices start at MinVertexIndex
    call.vertexCount = ctx.r6.u32;
    call.count = ctx.r7.u32;
    call.indexed = true;
    call.indexSize = (ctx.r8.u32 & 4) ? 4 : 2;
    call.vertexStride = ctx.r9.u32;
    call.userPointer = true;
    uint32_t indexPointer = ctx.r10.u32;
    uint32_t vertexPointer = Load32(ctx.r1.u32 + 0x54);
    __imp__sub_82F383D0(ctx, base);
    g_pendingVertices.reset();
    if (ctx.r3.u32 == 0 && indexPointer != 0 && vertexPointer != 0)
    {
        call.indexData = Load32(indexPointer);
        call.vertexData = Load32(vertexPointer);
        g_pendingVertices = call;
    }
}

// D3DDevice_EndVertices(dev): the vertices are written; draw them.
PPC_FUNC(sub_82F37EE0)
{
    DrawPendingVertices();
    __imp__sub_82F37EE0(ctx, base);
}

// D3DDevice_DrawVerticesUP / DrawIndexedVerticesUP: Begin*, copy, and the end inline.
PPC_FUNC(sub_82F38388)
{
    TRACE_ENTER(0x82F38388);
    __imp__sub_82F38388(ctx, base);
    DrawPendingVertices();
}

PPC_FUNC(sub_82F388E8)
{
    TRACE_ENTER(0x82F388E8);
    __imp__sub_82F388E8(ctx, base);
    DrawPendingVertices();
}

