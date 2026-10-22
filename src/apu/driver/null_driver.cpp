// Silent audio backend: accepts the game's audio client and discards its frames.
// Stands in for Unleashed Recompiled's SDL2 driver until audio output is wired up.
#include <stdafx.h>
#include <apu/audio.h>

void XAudioInitializeSystem()
{
}

void XAudioRegisterClient(PPCFunc* callback, uint32_t param)
{
}

void XAudioSubmitFrame(void* samples)
{
}
