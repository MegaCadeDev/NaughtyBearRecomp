// The mods menu, shown before the game starts when mods are installed.
#pragma once

#include <filesystem>

namespace launcher
{
    // Lets the player enable or disable each mod, then returns true to start the game or
    // false to quit. Returns true straight away when there are no mods, or when the menu is
    // turned off (unless forced).
    bool Run(const std::filesystem::path& modsDirectory, bool force);
}
