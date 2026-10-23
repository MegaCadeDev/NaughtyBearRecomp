# Notices

NaughtyBearRecomp is licensed under the GNU General Public License v3.0 (see `COPYING`).

## Unleashed Recompiled

The runtime in `src/` is based on [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp)
by hedge-dev, licensed under GPL-3.0, taken from commit `cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c`.

Files taken from it, with the changes made for Naughty Bear:

| File | Changes |
|-|-|
| `src/kernel/imports.cpp` | Removed audio/config/UI dependencies; stubs return 0 instead of nothing; moved file I/O, virtual memory, networking, printf and module functions to their own files; implemented `MmQueryStatistics`, `ExAllocatePoolTypeWithTag`/`ExFreePool`, `RtlRaiseException` logging; `ExCreateThread` runs the XAPI thread startup routine |
| `src/kernel/heap.cpp` | Removed Sonic-specific heap hooks; runtime heap moved to 0x20000-0x40000000 |
| `src/kernel/xam.cpp` | Input goes through `src/hid`; content and profile functions moved to `xam_content.cpp`; removed UI dependencies |
| `src/cpu/guest_thread.cpp`, `.h` | Removed Sonic-specific hooks; second thread argument; thread exit, timed waits and APCs |
| `src/apu/driver/sdl2_driver.cpp` | Adapted: no settings dependency; volume and surround from the environment; output recording |
| `src/os/logger.h` | Stub notices are logged once per call site in every build |
| `src/kernel/heap.h` | Exact-size physical allocator |
| `src/kernel/function.h`, `memory.*`, `xdm.*`, `freelist.h`, `xam.h`, `cpu/guest_stack_var.h`, `cpu/ppc_context.h`, `os/process.h`, `os/linux/*`, `apu/audio.*`, `mutex.h`, `framework.h`, `xxHashMap.h` | Unchanged |
| `src/stdafx.h`, `src/main.cpp` | Rewritten for this project, following the originals |
| `src/hid/hid.*` | Adapted from `hid/driver/sdl_hid.cpp`: single player 1, keyboard layout, scripted input, rumble from the main thread |

Files written for this project: `src/kernel/virtual_memory.cpp`, `src/kernel/io/nt_file.cpp`,
`src/kernel/xnet.cpp`, `src/kernel/crt_format.cpp`, `src/kernel/xex_modules.*`,
`src/kernel/xam_user.cpp`, `src/kernel/xam_content.cpp`, `src/kernel/packages.*`,
`src/apu/xma.*`, `src/user/*`, `src/import_stub.*`, `src/gpu/*` (renderer, headless GPU,
shader store and translator glue, copy shaders), `src/mods/*` (mod support), `src/ui/*`
(mods menu).

## Third-party code

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (MIT), used as a submodule with the patches in `patches/XenonRecomp/`.
- [o1heap](https://github.com/pavel-kirienko/o1heap) (MIT), in `thirdparty/o1heap`, copied from Unleashed Recompiled.
- [unordered_dense](https://github.com/martinus/unordered_dense) (MIT), submodule.
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (MIT), submodule, with the patches in `patches/XenosRecomp/`;
  its shader translator is built into the game.
- [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) (University of Illinois/NCSA),
  the `libdxcompiler.so` bundled with XenosRecomp, shipped next to the executable.
- [zstd](https://github.com/facebook/zstd) (BSD) and [smol-v](https://github.com/aras-p/smol-v) (MIT), from XenosRecomp.
- [plume](https://github.com/renderbag/plume) (MIT), submodule, the renderer abstraction Unleashed Recompiled uses.
- [SDL2](https://www.libsdl.org/) (zlib), from the system.
- [cgltf](https://github.com/jkuhlmann/cgltf) (MIT), submodule (v1.14), reads mods' glTF models.
- [stb_image and stb_image_write](https://github.com/nothings/stb) (MIT or public domain), submodule, decode
  mods' textures and write exported ones.
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT), submodule (v1.92.9b), draws the mods menu.
- [toml++](https://github.com/marzer/tomlplusplus) (MIT), from XenonRecomp, reads mods' `mod.toml`.
- [glslang](https://github.com/KhronosGroup/glslang), from the system, compiles the renderer's copy shaders at build time.
- Behaviour of several instructions and kernel functions, the Xenos register and fetch
  constant layouts, and the XMA decoder context layout and registers follow
  [Xenia](https://github.com/xenia-project/xenia) (BSD-3-Clause); no Xenia code is included.
- XMA audio is decoded with [FFmpeg](https://ffmpeg.org) (LGPL-2.1+), built from source
  (`thirdparty/FFmpeg`, with `patches/FFmpeg`) and linked statically.

## PS5 version

- The PS5 platform layer follows [UnleashedRecomp-Prospero](https://github.com/Nazky/UnleashedRecomp-Prospero)
  (GPL-3.0): the guest memory mapping (`src/kernel/memory.cpp`), the DualSense and
  `sceAudioOut` details (`src/hid/hid_ps5.cpp`, `src/apu/driver/ps5_driver.cpp`), the display
  surface in plume (`patches/plume/0002`) and the build and link steps (`ps5/tools/build.sh`).
- From it, unchanged or nearly: `ps5/tools/` (`setup-native-dependencies.sh`,
  `setup-pacbrew-dependencies.sh`, `build-radv.sh`, `mesa-python.sh`, `radv-link.sh`,
  `deploy.sh`, `send-controller.sh`, `rebuild-libc.sh`, `controllers/`), `ps5/tooling/`,
  `ps5/vendor/ps5/sdk/stubs/` and `ps5/runtime/`, which are from BlackBearReloaded's
  ps5-native-app-boilerplate and by Mihawk-99 (GPL-3.0-or-later). `ps5/runtime/libc.prx` is
  generated from that source by `ps5/tools/rebuild-libc.sh` (see `ps5/runtime/README.md`).
- Downloaded and built by `ps5/tools/build.sh`, not included: the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk)
  (GPL-3.0) as Mihawk-99's [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) fork,
  RADV from Mihawk-99's [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) (Mesa, MIT), the
  [PacBrew](https://github.com/ps5-payload-dev/pacbrew-repo) ports (zstd, BSD) and zlib (zlib
  license), and FFmpeg's XMA decoder cross-compiled from `thirdparty/FFmpeg` (LGPL-2.1+).
- `ps5/tooling/radv/clc-cache.tar.xz`: Mesa's internal OpenCL kernels for RADV, precompiled
  to SPIR-V (Mesa, MIT), from UnleashedRecomp-Prospero, so building RADV doesn't need
  LLVM's SPIR-V tools.
- `ps5/sce_sys/icon0.png` is original art, drawn by `ps5/tools/make-icon.py`.
