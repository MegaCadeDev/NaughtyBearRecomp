# NaughtyBearRecomp

A static recompilation of *Naughty Bear* (Xbox 360) to native code.

Roadmap:
1. PC port (Linux first) built on XenonRecomp and Unleashed Recompiled's runtime.
2. PS5 homebrew port.

No game data is included. You need your own legally dumped copy of the game;
place it in `game/` (the executable at `game/Naughty_Bear/Default.xex`).

## Status

The whole executable recompiles and builds. The game boots, renders its title screen
and menus in a Vulkan window, and takes controller and keyboard input: draws are replayed with the game's shaders (translated by XenosRecomp)
and textures. Render states, depth, render-to-texture and the buffer-based draws used in
gameplay are still to do (`docs/d3d_map.md`), as is audio output. Saves are kept in
`save/` (one folder per save container) and persist between runs.

To capture the shaders the renderer needs, run once with `NB_DUMP_SHADERS=shaders/dump`,
then `tools/translate_shaders.sh`, and rebuild.

Debug options: `NB_SCREENSHOT=file.ppm[:frame]` (convert with `tools/ppm2png.py`),
`NB_DEBUG_DRAWS=frame`, `NB_NO_BLEND=1`, `NB_DEBUG_CLEAR=1`, `NB_LOG_FILES=1`.

## Controls

Any SDL-supported controller works (Xbox, PlayStation, Switch Pro and others); the most
recently used one is player 1. The keyboard also controls player 1:

| Xbox 360 | Keyboard |
|-|-|
| Left stick | W A S D |
| Right stick | I J K L |
| D-pad | Arrow keys |
| A / B / X / Y | Space / E / F / R |
| LB / RB | Q / Tab |
| LT / RT | Z / C |
| Start / Back | Enter / Escape |
| Left / right stick click | Left Shift / V |

`NB_INPUT_SCRIPT="start@400+4,a@1150+4"` presses buttons at given frames, for automated
testing.

## Building

Requires Clang 18+, CMake 3.20+, Ninja, SDL2 and Vulkan
(`sudo apt install libsdl2-dev libvulkan-dev`).

```bash
git submodule update --init --recursive
tools/recompile.sh
CC=clang CXX=clang++ cmake -S . -B build/game -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/game
./build/game/NaughtyBear game/Naughty_Bear/Default.xex
```

## License

GPL-3.0 (see `COPYING`). The runtime is based on Unleashed Recompiled; see `NOTICE.md`
for what came from where.
