// Controller and keyboard input, reported to the game as player 1's Xbox 360 controller.
// Adapted from Unleashed Recompiled's SDL hid driver.
#pragma once

#include <xbox.h>

namespace hid
{
    // Registers the SDL event watch; call on the main thread after SDL_Init.
    void Init();

    uint32_t GetState(uint32_t userIndex, XAMINPUT_STATE* state);
    uint32_t SetState(uint32_t userIndex, XAMINPUT_VIBRATION* vibration);
    uint32_t GetCapabilities(uint32_t userIndex, XAMINPUT_CAPABILITIES* capabilities);

    // Called once per presented frame; drives NB_INPUT_SCRIPT.
    void OnFrame(uint64_t frame);
}
