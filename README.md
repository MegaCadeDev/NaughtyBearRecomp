# NaughtyBearRecomp

A static recompilation of *Naughty Bear* (Xbox 360) to native code.

Roadmap:
1. PC port (Linux first) built on XenonRecomp and Unleashed Recompiled's runtime.
2. PS5 homebrew port.

No game data is included. You need your own legally dumped copy of the game; put its
files in a `game/` folder (any layout below it, e.g. `game/Naughty_Bear/Default.xex`).
Starting the program finds `Default.xex` there: in a `game` folder next to the
executable, in the working directory, or in a parent of the executable's folder. A path
to the game can also be given on the command line. Saves (`save/`) and the shader cache
(`cache/`) are kept next to the `game` folder.

## Status

The whole executable recompiles and builds. The game boots, takes controller and keyboard
input, saves, and plays: menus and levels render in a Vulkan window with the game's
shaders (translated by XenosRecomp), textures, render targets, depth/stencil and render
states, including skinned characters. Saves are kept in `save/` (one folder per save
container) and persist between runs.

Audio goes out through SDL. The game's sounds are XMA; the console's XMA decoder is
emulated (`src/apu/xma.cpp`) with FFmpeg's XMA2 decoder, built from the
`thirdparty/FFmpeg` submodule with only that decoder (`patches/FFmpeg`). `NB_VOLUME=0..1`
sets the volume, `NB_SURROUND=1` asks for 5.1 output, `NB_AUDIO_DUMP=file.wav` records
the output and `NB_DEBUG_XMA=1` logs decoder statistics.

Shaders are translated when the game first runs: it decompresses its data packages,
finds every shader in them and translates them all (XenosRecomp and DXC, linked into the
game) while a progress bar shows, which takes a few seconds. They're kept in
`cache/shaders.bin`, so later runs start straight away; any shader that still turns up
during play is translated then. The cache is rebuilt when the translator or the game data
changes.

Debug options: `NB_SCREENSHOT=file.ppm[:frame]` (convert with `tools/ppm2png.py`),
`NB_DEBUG_DRAWS=frame` (log every draw of a frame), `NB_DEBUG_DECLS=1`, `NB_NO_BLEND=1`,
`NB_NO_DEPTH=1`, `NB_SHOW_RESOLVE=address` (present a resolved texture),
`NB_ONLY_DECL=hash`, `NB_DUMP_SHADERS=dir`, `NB_LOG_FILES=1`.

## Controls

Any SDL-supported controller works (Xbox 360 wired and wireless, Xbox One/Series,
PlayStation, Switch Pro and others), whether it's connected before or after starting; the
most recently used one is player 1. Mappings for controllers SDL doesn't know can be added
in a `gamecontrollerdb.txt` (SDL's format) next to the `game` folder. The keyboard also
controls player 1:

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
(`sudo apt install libsdl2-dev libvulkan-dev glslang-tools`).

```bash
git submodule update --init --recursive
tools/recompile.sh
CC=clang CXX=clang++ cmake -S . -B build/game -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/game
./build/game/NaughtyBear
```

## License

GPL-3.0 (see `COPYING`). The runtime is based on Unleashed Recompiled; see `NOTICE.md`
for what came from where.
