// Host renderer: replaces the game's Direct3D library, following Unleashed Recompiled.
#pragma once

struct SDL_Window;

namespace Video
{
    // Creates the Vulkan device and swap chain for the window (on the PS5, null: it presents
    // to the display). Returns false on failure.
    bool Init(SDL_Window* window);

    // Notes that the window was resized; the swap chain is rebuilt before the next present.
    void OnWindowResized();
}
