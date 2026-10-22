// Where the runtime finds game files and keeps saves.
// Stand-in for Unleashed Recompiled's user/paths.h, which is built around its installer.
#pragma once

#include <filesystem>

// Game files: the folder holding Default.xex, set at startup.
std::filesystem::path& GetGamePath();

// Save data, next to the folder the game files are in (see main.cpp).
std::filesystem::path GetSavePath(bool checkForMods);
