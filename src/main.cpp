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
#include <os/process.h>
#include <user/paths.h>
#if !defined(__PROSPERO__)
#include <ui/launcher.h>
#include <SDL.h>
#endif

#include <file.h>
#include <image.h>
#include <xex.h>

#include <csignal>
#if defined(__PROSPERO__)
#include <malloc_np.h>
#else
#include <malloc.h>
#endif

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

// Saves and the shader cache live next to the "game" folder (the working directory when
// the game's path is given on the command line). On the PS5, everything is in the title's
// own folder (/data/homebrew/PPSA99862, which it sees as /app0): a title can't write to
// /data outside it.
#if defined(__PROSPERO__)
static const std::filesystem::path PS5_USER_PATH = "/app0";
static std::filesystem::path g_userPath = PS5_USER_PATH;
#else
static std::filesystem::path g_userPath = std::filesystem::current_path();
#endif

std::filesystem::path GetSavePath(bool checkForMods)
{
    return g_userPath / "save";
}

static void OnCrash(int sig, siginfo_t* info, void*)
{
    auto addr = reinterpret_cast<uintptr_t>(info->si_addr);
    auto base = reinterpret_cast<uintptr_t>(g_memory.base);
    char message[160];
    if (addr >= base && addr < base + PPC_MEMORY_SIZE)
        snprintf(message, sizeof(message), "signal %d: guest memory access at 0x%08llX", sig, (unsigned long long)(addr - base));
    else
        snprintf(message, sizeof(message), "signal %d: host address %p", sig, info->si_addr);
    fprintf(stderr, "\n[nb] %s\n", message);
#if defined(__PROSPERO__)
    os::process::ShowMessage(std::string("Naughty Bear Recompiled crashed (") + message + "). See log.txt in its folder.");
#endif
    os::process::Exit(128 + sig);
}

// Logs a fatal error, shows it to the player and ends.
[[noreturn]] static void Fail(const std::string& message)
{
    LOGN_ERROR(message);
    os::process::ShowMessage("Naughty Bear Recompiled: " + message);
    os::process::Exit(EXIT_FAILURE);
}

static void InstallCrashHandler()
{
    struct sigaction sa{};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO;
    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGSYS })
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

// Finds Default.xex in a "game" folder (searched with its subfolders) next to the executable,
// in the working directory, or in a parent of the executable's folder. On the PS5, in the
// game folder in the title's own folder.
static std::filesystem::path FindGameExecutable()
{
    std::error_code ec;
    std::vector<std::filesystem::path> roots;
#if defined(__PROSPERO__)
    roots = { PS5_USER_PATH / "game" };
#else
    auto executableDir = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
    if (!ec)
    {
        for (auto dir = executableDir; !dir.empty(); dir = dir.parent_path())
        {
            roots.push_back(dir / "game");
            if (dir == dir.root_path() || roots.size() >= 4)
                break;
        }
    }
    roots.insert(roots.begin() + std::min<size_t>(1, roots.size()), std::filesystem::current_path(ec) / "game");
#endif

    for (const auto& root : roots)
    {
        if (!std::filesystem::is_directory(root, ec))
            continue;
        for (auto it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
                break;
            if (it.depth() > 4)
            {
                it.disable_recursion_pending();
                continue;
            }
            std::string name = it->path().filename().string();
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (name == "default.xex" && it->is_regular_file(ec))
            {
                LOGFN("Found the game at {}", it->path().string());
                return it->path();
            }
        }
    }
    return {};
}

int main(int argc, char** argv)
{
#if defined(__PROSPERO__)
    // The log goes to a file the player can read over FTP. The game folder is created too, so
    // there's a place to copy the game's files to.
    std::error_code directoryError;
    std::filesystem::create_directories(PS5_USER_PATH / "game", directoryError);
    freopen((PS5_USER_PATH / "log.txt").c_str(), "w", stdout);
    dup2(fileno(stdout), fileno(stderr));
#endif
    os::logger::Init();
    setvbuf(stdout, nullptr, _IONBF, 0); // keep log lines when the process dies

    // Options: --mods-menu shows the mods menu even when it's turned off, --no-mods-menu
    // skips it; anything else is the path to the game.
    std::filesystem::path xexPath;
    bool forceModsMenu = false, skipModsMenu = false;
    for (int i = 1; i < argc; i++)
    {
        std::string_view argument = argv[i];
        if (argument == "--mods-menu")
            forceModsMenu = true;
        else if (argument == "--no-mods-menu")
            skipModsMenu = true;
        else
            xexPath = argument;
    }
    if (!xexPath.empty())
    {
        if (std::filesystem::is_directory(xexPath))
            xexPath /= "Default.xex";
    }
    else
    {
        xexPath = FindGameExecutable();
        if (xexPath.empty())
        {
#if defined(__PROSPERO__)
            Fail("Could not find the game: copy its files (the folder with Default.xex) into /data/homebrew/PPSA99862/game.");
#else
            Fail("Could not find the game: put its files (with Default.xex) in a \"game\" folder next to the executable.");
#endif
        }
#if !defined(__PROSPERO__)
        for (auto dir = xexPath.parent_path(); dir.has_relative_path(); dir = dir.parent_path())
        {
            if (dir.filename() == "game")
            {
                g_userPath = dir.parent_path();
                break;
            }
        }
#endif
    }
    g_gamePath = std::filesystem::absolute(xexPath).parent_path();

    InstallCrashHandler();
    if (g_memory.base == nullptr)
        Fail("Could not reserve the 4 GB guest address space (" + g_memory.error + ").");

    KiSystemStartup();

    uint32_t entry = LdrLoadModule(xexPath);
    if (entry == 0)
        Fail("Could not load " + xexPath.string());

#if defined(__PROSPERO__)
    // No window or mods menu: the renderer presents straight to the display. The main
    // thread applies rumble; the game runs on its own thread.
    if (!Video::Init(nullptr))
        Fail("Could not start the renderer.");
    hid::Init();
    StartHostThread([entry]
        {
            GuestThread::Start({ entry, 0, 0 });
            LOGN("Entry point returned.");
            os::process::Exit(EXIT_SUCCESS);
        });
    while (true)
    {
        hid::Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
#else
    // SDL wants its window and events on the main thread, so the game gets its own.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0)
        Fail(fmt::format("SDL_Init failed: {}", SDL_GetError()));
    if (!skipModsMenu && !launcher::Run(g_userPath / "mods", forceModsMenu))
        return EXIT_SUCCESS;

    // The game renders at 1280x720; the renderer scales that to the window.
    // NB_WINDOW_SIZE=WxH sets the starting size.
    int windowWidth = 1280, windowHeight = 720;
    if (const char* size = getenv("NB_WINDOW_SIZE"))
        sscanf(size, "%dx%d", &windowWidth, &windowHeight);
    SDL_Window* window = SDL_CreateWindow("Naughty Bear", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        std::max(windowWidth, 320), std::max(windowHeight, 180), SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (window != nullptr)
        SDL_SetWindowMinimumSize(window, 320, 180);
    if (window == nullptr || !Video::Init(window))
        Fail(fmt::format("Could not create the game window: {}", SDL_GetError()));

    hid::Init();

    std::thread game([entry]
        {
            GuestThread::Start({ entry, 0, 0 });
            LOGN("Entry point returned.");
            SDL_Event quit{ SDL_QUIT };
            SDL_PushEvent(&quit);
        });
    game.detach();

    // Events, and rumble requests (applied here: SDL's joystick calls belong on this thread).
    for (bool running = true; running;)
    {
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, 8))
        {
            do
            {
                if (event.type == SDL_QUIT)
                    running = false;
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                    Video::OnWindowResized();
            } while (SDL_PollEvent(&event));
        }
        hid::Update();
    }

    // The game has no orderly shutdown path yet.
    _exit(EXIT_SUCCESS);
#endif
}
