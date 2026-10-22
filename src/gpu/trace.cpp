// Records calls into the Direct3D library from game code, with their arguments, to help
// identify the API functions for the renderer port. Only built with NB_TRACE_D3D.
#include <stdafx.h>
#include <gpu/trace.h>
#include <kernel/memory.h>
#include <os/logger.h>
#include <map>

void HeadlessGpuOnDeviceCreated(uint32_t device);

namespace
{
    constexpr uint32_t CREATE_DEVICE = 0x82F26FA0;
    constexpr uint32_t SWAP = 0x82F33D50;

    // NB_TRACE_FRAME=N logs every call made during the Nth frame (frames end at Swap).
    const int64_t g_dumpFrame = [] { const char* v = getenv("NB_TRACE_FRAME"); return v ? std::atoll(v) : -1; }();
    std::atomic<int64_t> g_frame = 0;

    // NB_DUMP_SHADERS=dir writes every shader container the game constructs to dir,
    // named by type and hash, for offline translation with XenosRecomp.
    constexpr uint32_t CONSTRUCT_VERTEX_SHADER = 0x82F30050;
    constexpr uint32_t CONSTRUCT_PIXEL_SHADER = 0x82F2FF28;
    const char* g_shaderDumpDir = getenv("NB_DUMP_SHADERS");

    void DumpShader(const char* type, const PPCContext& ctx)
    {
        // r5 points to { container, ... }. The container header is { flags, virtualSize,
        // physicalSize, ... } and the microcode (physical part) follows the virtual part.
        uint32_t container = ByteSwap(*(uint32_t*)g_memory.Translate(ctx.r5.u32));
        if (container == 0)
            return;
        uint32_t size = ByteSwap(*(uint32_t*)g_memory.Translate(container + 4)) + ByteSwap(*(uint32_t*)g_memory.Translate(container + 8));
        if (size == 0 || size > 0x100000)
            return;
        const void* data = g_memory.Translate(container);
        uint64_t hash = XXH3_64bits(data, size);
        std::filesystem::path path = std::filesystem::path(g_shaderDumpDir) / fmt::format("{}_{:016x}.bin", type, hash);
        std::error_code ec;
        if (std::filesystem::exists(path, ec))
            return;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream(path, std::ios::binary).write((const char*)data, size);
    }
    constexpr int CALLS_LOGGED_PER_FUNCTION = 6;

    Mutex g_traceMutex;
    std::map<uint32_t, uint64_t> g_counts;
    thread_local uint32_t g_createDeviceOut;
}

void nb::trace::Enter(uint32_t function, const PPCContext& ctx)
{
    uint32_t lr = uint32_t(ctx.lr);
    if (function == CREATE_DEVICE)
        g_createDeviceOut = ctx.r8.u32;
    if (g_shaderDumpDir != nullptr && function == CONSTRUCT_VERTEX_SHADER)
        DumpShader("vs", ctx);
    if (g_shaderDumpDir != nullptr && function == CONSTRUCT_PIXEL_SHADER)
        DumpShader("ps", ctx);

    if (lr >= D3D_BEGIN && lr < D3D_END)
        return; // internal call

    uint64_t count;
    {
        std::lock_guard lock(g_traceMutex);
        count = ++g_counts[function];
    }
    bool dumping = g_frame == g_dumpFrame;
    if (function == SWAP)
    {
        int64_t frame = g_frame++;
        if (frame == g_dumpFrame || frame + 1 == g_dumpFrame)
            LOGF_IMPL(Utility, "d3d", "---- frame {} ----", frame + 1);
    }
    if (count <= CALLS_LOGGED_PER_FUNCTION || dumping)
    {
        LOGF_IMPL(Utility, "d3d", "{:08X}({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}) f1={} lr={:08X}", function,
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.f1.f64, lr);
    }
}

void nb::trace::Leave(uint32_t function, const PPCContext& ctx)
{
    if (function == CREATE_DEVICE && ctx.r3.u32 == 0 && g_createDeviceOut != 0)
    {
        uint32_t device = ByteSwap(*(uint32_t*)g_memory.Translate(g_createDeviceOut));
        LOGF_IMPL(Utility, "d3d", "device created at 0x{:08X}", device);
        HeadlessGpuOnDeviceCreated(device);
    }
}

void nb::trace::Report()
{
    std::vector<std::pair<uint64_t, uint32_t>> sorted;
    {
        std::lock_guard lock(g_traceMutex);
        for (auto [function, count] : g_counts)
            sorted.emplace_back(count, function);
    }
    std::sort(sorted.rbegin(), sorted.rend());
    std::string line;
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), 40); i++)
        line += fmt::format("{:08X}:{} ", sorted[i].second, sorted[i].first);
    LOGF_IMPL(Utility, "d3d", "call counts: {}", line);
}
