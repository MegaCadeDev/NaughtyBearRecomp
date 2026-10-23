# NaughtyBearRecomp

A static recompilation of *Naughty Bear* (Xbox 360) to native code. The game's PowerPC
executable is translated to C++ by [XenonRecomp](https://github.com/hedge-dev/XenonRecomp)
and runs on a runtime adapted from
[Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp), which stands in for
the Xbox 360 kernel, graphics, audio and input.

Roadmap:
1. PC port (Linux first). **Playable.**
2. PS5 homebrew port.

No game data is included. You need your own legally dumped copy of the game.

## Status

The game runs from boot to gameplay:

- **Graphics** (Vulkan): menus, cutscenes and levels, with the game's own shaders
  (translated to SPIR-V on the first run), textures, render targets and resolves, the
  post-processing chain, depth and stencil, blending and alpha test, skinned characters,
  and particles such as fire.
- **Audio**: music, ambience, speech and effects. The console's XMA hardware decoder is
  emulated with FFmpeg; output goes through SDL (stereo, or 5.1 on request).
- **Input**: any SDL-supported controller (Xbox 360 wired included), and the keyboard.
- **Saving**: save games persist between runs.
- Loading, timing (30 fps, as on the console) and the game's threads behave as on the
  console.

Known issues:

- Textures are uploaded without their mipmaps, so distant or small textures shimmer.
- A few depth-only draws per frame (probably shadow casters) are skipped: their vertex
  shaders are created by a path that isn't hooked yet.
- Some texture formats (3D and cube textures among them) aren't decoded yet.
- Online features are offline: the game reports no Xbox LIVE connection.

## Running

Put the game's files in a `game/` folder, in any layout below it (for example
`game/Naughty_Bear/Default.xex` with `lu/`, `streams/` and the rest next to it), and start
the program:

```bash
./build/game/NaughtyBear
```

It finds `Default.xex` in a `game` folder next to the executable, in the working directory,
or in a parent of the executable's folder. A path to the game can also be given on the
command line.

The window can be resized (or maximized); the game renders at 1280x720 and is scaled to
fit, keeping its aspect ratio. Pressing the controller's Guide (Xbox) button closes the
game.

On the first run the game prepares its shaders: it decompresses its data packages, finds
every shader and translates them all while a progress bar is shown (a few seconds). They're
kept in `cache/shaders.bin`, so later runs start straight away, and the cache rebuilds
itself if the game data or the translator changes. Saves go to `save/`. Both folders are
kept next to the `game` folder.

### Controls

Any SDL-supported controller works (Xbox 360 wired and wireless, Xbox One/Series,
PlayStation, Switch Pro and others), whether it's connected before or after starting; the
most recently used one is player 1, and rumble works. Mappings for controllers SDL doesn't
know can be added in a `gamecontrollerdb.txt` (SDL's format) next to the `game` folder. The
keyboard also controls player 1:

| Xbox 360 | Keyboard |
|-|-|
| Left stick | W A S D |
| Right stick | I J K L |
| D-pad | Arrow keys |
| A / B / X / Y | Space / E / F / R |
| LB / RB | Q / Tab |
| LT / RT | Z / C |
| Start / Back | Enter / Escape |
| Guide (closes the game) | (close the window) |
| Left / right stick click | Left Shift / V |

### Options

Set as environment variables:

| Variable | Effect |
|-|-|
| `NB_VOLUME=0..1` | Audio volume |
| `NB_SURROUND=1` | Ask for 5.1 audio output (stereo otherwise) |
| `NB_FPS=n` | Pace frames to `n` fps instead of the game's 30 |
| `NB_WINDOW_SIZE=WxH` | Starting window size (default 1280x720) |

### Mods

Models from a `mods/` folder (next to the `game` folder) can replace the game's: each mod is
a folder with a `mod.toml` and its `.glb` models. The project includes and distributes no
mods; making them, and having the rights to what's in them, is up to their authors. See
[docs/modding.md](docs/modding.md).

Debugging:

| Variable | Effect |
|-|-|
| `NB_SCREENSHOT=file.ppm[:frame[+step*count]]` | Save a frame (default 600), or a series of `count` frames `step` apart (numbered files); convert with `tools/ppm2png.py` |
| `NB_INPUT_SCRIPT="start@400+4,a@1150+4,rx+@1300+30"` | Press buttons (or push a stick: `lx`, `ly`, `rx`, `ry` with `+`/`-`) at given frames for a number of frames, for automated runs |
| `NB_DEBUG_DRAWS=frame` | Log every draw of a frame (shaders, state, viewport) |
| `NB_DEBUG_DECLS=1` | Log vertex declarations as they're first seen |
| `NB_SHOW_RESOLVE=address` | Present a resolved texture instead of the front buffer |
| `NB_NO_BLEND=1`, `NB_NO_DEPTH=1` | Disable blending / depth testing |
| `NB_ONLY_DECL=hash` | Draw only geometry with that vertex declaration |
| `NB_DUMP_SHADERS=dir` | Write the shader containers the game creates |
| `NB_AUDIO_DUMP=file.wav` | Record the audio output |
| `NB_DEBUG_XMA=1` | Log XMA decoder statistics |
| `NB_MOD_EXPORT=frame` | Write every mesh drawn in that frame to `mods/_export/` as `.glb`, with their mesh IDs (for modding) |
| `NB_LOG_FILES=1` | Log every file the game opens |

## Building

Requires Clang 18+, CMake 3.20+, Ninja, SDL2, Vulkan and glslang
(`sudo apt install clang lld llvm cmake ninja-build libsdl2-dev libvulkan-dev glslang-tools`).
The game itself is needed to build: recompilation reads `game/Naughty_Bear/Default.xex`.

```bash
git submodule update --init --recursive
tools/recompile.sh
CC=clang CXX=clang++ cmake -S . -B build/game -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/game
```

`tools/recompile.sh` applies the patches in `patches/` to the submodules
(`tools/apply_patches.sh`), builds XenonRecomp and the analysis tools, and writes the
translated game to `generated/ppc` (not in the repository). The CMake build compiles that
with the runtime, and builds FFmpeg's XMA decoder from `thirdparty/FFmpeg`.
`libdxcompiler.so` (the shader compiler) is copied next to the executable.

## Project layout

| Path | Contents |
|-|-|
| `src/main.cpp` | Startup: finds and loads the game, the window, the main loop |
| `src/kernel/` | The Xbox 360 kernel and XAM: memory, threads and synchronization, files, saves, packages |
| `src/cpu/` | Guest threads and their contexts |
| `src/gpu/` | The renderer (`video.cpp`), headless GPU, shader translation and cache |
| `src/apu/` | Audio output and the XMA decoder |
| `src/hid/` | Controller and keyboard input |
| `src/mods/` | Mod support: `mod.toml` files and glTF models |
| `config/` | XenonRecomp configuration for the game (`NB.toml`, jump tables) |
| `patches/` | Patches to the submodules (XenonRecomp, XenosRecomp, plume, FFmpeg) |
| `tools/` | Recompilation and shader scripts; `tools/nbanalyse` has the reverse-engineering tools |
| `docs/` | How it works: [runtime](docs/runtime.md), [graphics](docs/d3d_map.md) |

## License

GPL-3.0 (see `COPYING`). The runtime is based on Unleashed Recompiled; see `NOTICE.md`
for what came from where and the third-party components.
