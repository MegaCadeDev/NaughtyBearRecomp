// Minimal test host: loads the game's executable into guest memory and runs the
// recompiled entry point. Kernel imports are logging stubs for now.

#include "import_stub.h"

#include <file.h>
#include <image.h>

#include <algorithm>
#include <csignal>
#include <malloc.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <sys/mman.h>

namespace
{
    constexpr size_t kGuestMemorySize = 0x100000000ull; // the full 32-bit guest address space

    // Where the test host places the main thread's stack and processor control block.
    constexpr uint32_t kStackBase = 0x70000000;
    constexpr uint32_t kStackSize = 0x100000;
    constexpr uint32_t kPcrAddress = 0x7F000000;
    constexpr uint32_t kThreadAddress = 0x7F001000;

    uint8_t* gBase = nullptr;

    void OnCrash(int sig, siginfo_t* info, void*)
    {
        auto addr = reinterpret_cast<uintptr_t>(info->si_addr);
        auto base = reinterpret_cast<uintptr_t>(gBase);
        if (addr >= base && addr < base + kGuestMemorySize)
            fprintf(stderr, "\n[nb] signal %d: guest memory access at 0x%08llX\n", sig, (unsigned long long)(addr - base));
        else
            fprintf(stderr, "\n[nb] signal %d: host address %p\n", sig, info->si_addr);
        _exit(128 + sig);
    }
}

void nb::ReportStub(const char* name, const PPCContext& ctx)
{
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::lock_guard lock(mutex);
    if (seen.insert(name).second)
    {
        fprintf(stderr, "[nb] stub %-40s r3=%08X r4=%08X r5=%08X r6=%08X lr=%08X\n", name,
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
    }
}

int main(int argc, char** argv)
{
    const char* xexPath = argc > 1 ? argv[1] : "game/Naughty_Bear/Default.xex";
    auto file = LoadFile(xexPath);
    if (file.empty())
    {
        fprintf(stderr, "[nb] could not read %s\n", xexPath);
        return EXIT_FAILURE;
    }
    auto image = Image::ParseImage(file.data(), file.size());
    fprintf(stderr, "[nb] loaded %s: image 0x%08zX size 0x%X entry 0x%08zX\n", xexPath, image.base, image.size, image.entry_point);

    // Reserve the whole guest address space up front; pages are committed on first touch.
    gBase = static_cast<uint8_t*>(mmap(nullptr, kGuestMemorySize, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    if (gBase == MAP_FAILED)
    {
        perror("[nb] mmap");
        return EXIT_FAILURE;
    }

    struct sigaction sa{};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGTRAP, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);

    // The loader's buffer holds the stored blocks, which can be shorter than the image size
    // the header declares; guest memory is already zeroed, so copy only what the buffer holds.
    size_t loaded = std::min<size_t>(image.size, malloc_usable_size(image.data.get()));
    memcpy(gBase + image.base, image.data.get(), loaded);
    fprintf(stderr, "[nb] copied 0x%zX of 0x%X image bytes\n", loaded, image.size);

    // Indirect calls look up host functions in a table placed right after the image.
    size_t mapped = 0;
    for (auto* m = PPCFuncMappings; m->host != nullptr; m++, mapped++)
        PPC_LOOKUP_FUNC(gBase, m->guest) = m->host;
    fprintf(stderr, "[nb] %zu functions mapped\n", mapped);

    // r13 points at the processor control block; offset 0x100 holds the current thread.
    uint8_t* base = gBase; // name used by the PPC_* memory macros
    PPC_STORE_U32(kPcrAddress + 0x100, kThreadAddress);

    PPCContext ctx{};
    ctx.r1.u64 = kStackBase + kStackSize - 0x100;
    ctx.r13.u64 = kPcrAddress;
    ctx.fpscr.loadFromHost();

    auto* entry = PPC_LOOKUP_FUNC(gBase, image.entry_point);
    fprintf(stderr, "[nb] calling entry point 0x%08zX\n", image.entry_point);
    entry(ctx, gBase);
    fprintf(stderr, "[nb] entry point returned r3=0x%08X\n", ctx.r3.u32);
    return EXIT_SUCCESS;
}
