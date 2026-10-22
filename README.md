# NaughtyBearRecomp

A static recompilation of *Naughty Bear* (Xbox 360) to native code.

Roadmap:
1. PC port (Linux first) built on XenonRecomp and Unleashed Recompiled's runtime.
2. PS5 homebrew port.

No game data is included. You need your own legally dumped copy of the game;
place it in `game/` (the executable at `game/Naughty_Bear/Default.xex`).

## Status

The whole executable recompiles and builds. The game boots, takes controller and keyboard
input, saves, and plays: menus and levels render in a Vulkan window with the game's
shaders (translated by XenosRecomp), textures, render targets, depth/stencil and render
states, including skinned characters. Saves are kept in `save/` (one folder per save
container) and persist between runs.

Audio goes out through SDL. The game's sounds are XMA, decoded with FFmpeg
(`sudo apt install libavcodec-dev libavutil-dev`, then re-run CMake); without FFmpeg the
game runs silent. `NB_VOLUME=0..1` sets the volume, `NB_SURROUND=1` asks for 5.1 output.

Shaders the game creates are written to `shaders/dump/` while it runs (or to
`NB_DUMP_SHADERS=dir`). Playing through new areas collects their shaders; then run
`tools/translate_shaders.sh` and rebuild to add them to the cache. Draws whose shader is
not in the cache yet are skipped.

Debug options: `NB_SCREENSHOT=file.ppm[:frame]` (convert with `tools/ppm2png.py`),
`NB_DEBUG_DRAWS=frame` (log every draw of a frame), `NB_DEBUG_DECLS=1`, `NB_NO_BLEND=1`,
`NB_NO_DEPTH=1`, `NB_SHOW_RESOLVE=address` (present a resolved texture),
`NB_ONLY_DECL=hash`, `NB_LOG_FILES=1`.

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
(`sudo apt install libsdl2-dev libvulkan-dev glslang-tools`), and FFmpeg for audio
(`sudo apt install libavcodec-dev libavutil-dev`).

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
