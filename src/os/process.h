#pragma once

namespace os::process
{
    inline bool g_consoleVisible;

    std::filesystem::path GetExecutablePath();
    std::filesystem::path GetExecutableRoot();
    std::filesystem::path GetWorkingDirectory();
    bool SetWorkingDirectory(const std::filesystem::path& path);
    bool StartProcess(const std::filesystem::path& path, const std::vector<std::string>& args, std::filesystem::path work = {});
    void CheckConsole();
    void ShowConsole();

    // Shows a short message to the player: a system notification on the PS5, nothing
    // elsewhere (it's in the log).
    void ShowMessage(std::string_view message);

    // Ends the process. A PS5 title has to ask the system to close it: exit() raises SIGSYS.
    [[noreturn]] void Exit(int code);
}
