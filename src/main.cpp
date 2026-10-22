// Naughty Bear runtime entry point. Boot sequence adapted from Unleashed Recompiled's
// main.cpp (KiSystemStartup / LdrLoadModule), without its installer, UI and mod loader.

#include <stdafx.h>
#include <apu/xma.h>
#include <apu/audio.h>
#include <cpu/guest_thread.h>
#include <gpu/video.h>
#include <hid/hid.h>
#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <kernel/xam.h>
#include <kernel/xex_modules.h>
#include <os/logger.h>
#include <user/paths.h>

#include <file.h>
#include <image.h>
#include <xex.h>

#include <csignal>
#include <malloc.h>

Memory g_memory;
Heap g_userHeap;

static std::filesystem::path g_gamePath;

// The executable's image in guest memory (the shader precache scans it).
uint32_t g_imageBase = 0;
uint32_t g_imageSize = 0;

std::filesystem::path& GetGamePath()
{
    return g_gamePath;
}

std::filesystem::path GetSavePath(bool checkForMods)
{
    return std::filesystem::current_path() / "save";
}

static void OnCrash(int sig, siginfo_t* info, void*)
{
    auto addr = reinterpret_cast<uintptr_t>(info->si_addr);
    auto base = reinterpret_cast<uintptr_t>(g_memory.base);
    if (addr >= base && addr < base + PPC_MEMORY_SIZE)
        fprintf(stderr, "\n[nb] signal %d: guest memory access at 0x%08llX\n", sig, (unsigned long long)(addr - base));
    else
        fprintf(stderr, "\n[nb] signal %d: host address %p\n", sig, info->si_addr);
    _exit(128 + sig);
}

static void InstallCrashHandler()
{
    struct sigaction sa{};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO;
    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE })
        sigaction(sig, &sa, nullptr);
}

static void KiSystemStartup()
{
    g_userHeap.Init();

    // The game reads its data from the "game:" and "D:" roots.
    const auto gameContent = XamMakeContent(XCONTENTTYPE_RESERVED, "Game");
    const std::string gamePath = (const char*)GetGamePath().u8string().c_str();
    XamRegisterContent(gameContent, gamePath);
    XamContentCreateEx(0, "game", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);
    XamContentCreateEx(0, "D", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);

    XAudioInitializeSystem();
    xma::Init();
}

// Loads the executable into guest memory and returns its entry point.
static uint32_t LdrLoadModule(const std::filesystem::path& path)
{
    auto file = LoadFile(path);
    if (file.empty())
        return 0;

    // XenonUtils handles the XEX encryption and compression.
    auto image = Image::ParseImage(file.data(), file.size());

    // The loader's buffer holds the stored blocks, which can be shorter than the image size
    // the header declares; guest memory starts zeroed, so copy only what the buffer holds.
    size_t loaded = std::min<size_t>(image.size, malloc_usable_size(image.data.get()));
    memcpy(g_memory.Translate(image.base), image.data.get(), loaded);
    g_imageBase = uint32_t(image.base);
    g_imageSize = uint32_t(loaded);

    // Function imports become thunks that call the runtime; remember them by export name so
    // XexGetProcedureAddress can hand them out.
    for (const auto& symbol : image.symbols)
    {
        if (symbol.type == Symbol_Function && symbol.name.starts_with("__imp__"))
            RegisterImportThunk(std::string_view(symbol.name).substr(7), uint32_t(symbol.address));
    }

    // Resources (XEX_HEADER_RESOURCE_INFO): a size, then 16-byte entries of name, address, size.
    if (const auto* info = (const uint8_t*)getOptHeaderPtr(file.data(), XEX_HEADER_RESOURCE_INFO))
    {
        uint32_t headerSize = *(const be<uint32_t>*)info;
        for (uint32_t offset = 4; offset + 16 <= headerSize; offset += 16)
        {
            const uint8_t* entry = info + offset;
            std::string name((const char*)entry, strnlen((const char*)entry, 8));
            RegisterResource(name, *(const be<uint32_t>*)(entry + 8), *(const be<uint32_t>*)(entry + 12));
            LOGFN("Resource {}: 0x{:08X}, 0x{:X} bytes", name, uint32_t(*(const be<uint32_t>*)(entry + 8)), uint32_t(*(const be<uint32_t>*)(entry + 12)));
        }
    }

    LOGFN("Loaded {}: image 0x{:08X}, size 0x{:X}, entry 0x{:08X}", path.string(), image.base, image.size, image.entry_point);
    return uint32_t(image.entry_point);
}

int main(int argc, char** argv)
{
    os::logger::Init();
    setvbuf(stdout, nullptr, _IONBF, 0); // keep log lines when the process dies

    std::filesystem::path xexPath = argc > 1 ? argv[1] : "game/Naughty_Bear/Default.xex";
    if (std::filesystem::is_directory(xexPath))
        xexPath /= "Default.xex";
    g_gamePath = std::filesystem::absolute(xexPath).parent_path();

    if (g_memory.base == nullptr)
    {
        LOGN_ERROR("Could not reserve the 4 GB guest address space.");
        return EXIT_FAILURE;
    }

    InstallCrashHandler();
    KiSystemStartup();

    uint32_t entry = LdrLoadModule(xexPath);
    if (entry == 0)
    {
        LOGFN_ERROR("Could not load {}", xexPath.string());
        return EXIT_FAILURE;
    }

    // SDL wants its window and events on the main thread, so the game gets its own.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0)
    {
        LOGFN_ERROR("SDL_Init failed: {}", SDL_GetError());
        return EXIT_FAILURE;
    }
    // Fixed 1280x720 until the renderer scales its output to the window.
    SDL_Window* window = SDL_CreateWindow("Naughty Bear", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
        SDL_WINDOW_VULKAN);
    if (window == nullptr || !Video::Init(window))
    {
        LOGFN_ERROR("Could not create the game window: {}", SDL_GetError());
        return EXIT_FAILURE;
    }

    hid::Init();

    std::thread game([entry]
        {
            GuestThread::Start({ entry, 0, 0 });
            LOGN("Entry point returned.");
            SDL_Event quit{ SDL_QUIT };
            SDL_PushEvent(&quit);
        });
    game.detach();

    for (SDL_Event event; SDL_WaitEvent(&event);)
    {
        if (event.type == SDL_QUIT)
            break;
        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
            Video::OnWindowResized();
    }

    // The game has no orderly shutdown path yet.
    _exit(EXIT_SUCCESS);
}
