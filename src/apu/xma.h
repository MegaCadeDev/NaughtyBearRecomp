#pragma once

// The Xbox 360's XMA hardware decoder, emulated for the game's XAudio library: contexts
// live in guest memory, the library programs them and kicks/locks/clears them through the
// decoder registers at 0x7FEA0000, and decoded PCM goes to each context's output ring.
namespace xma
{
    void Init();
    uint32_t AllocateContext();
    void ReleaseContext(uint32_t context);
}
