# NaughtyBearRecomp

A static recompilation of *Naughty Bear* (Xbox 360) to native code.

Roadmap:
1. PC port (Linux first) built on XenonRecomp and Unleashed Recompiled's runtime.
2. PS5 homebrew port.

No game data is included. You need your own legally dumped copy of the game;
place it in `game/` (the executable at `game/Naughty_Bear/Default.xex`).

## Status

The whole executable recompiles and builds. The game boots through its C runtime,
memory, file system, threads and networking setup, and stops when Direct3D waits for
the GPU: graphics, audio output and input are not implemented yet.

## Building

Requires Clang 18+, CMake 3.20+ and Ninja.

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
