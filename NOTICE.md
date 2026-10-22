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
| `src/kernel/xam.cpp` | Input reports no controller until the SDL input layer is ported; removed UI dependencies |
| `src/cpu/guest_thread.cpp`, `.h` | Removed Sonic-specific hooks; second thread argument |
| `src/os/logger.h` | Stub notices are logged once per call site in every build |
| `src/kernel/function.h`, `memory.*`, `heap.h`, `xdm.*`, `freelist.h`, `xam.h`, `cpu/guest_stack_var.h`, `cpu/ppc_context.h`, `os/process.h`, `os/linux/*`, `apu/audio.*`, `mutex.h`, `framework.h`, `xxHashMap.h` | Unchanged |
| `src/stdafx.h`, `src/main.cpp` | Rewritten for this project, following the originals |

Files written for this project: `src/kernel/virtual_memory.cpp`, `src/kernel/io/nt_file.cpp`,
`src/kernel/xnet.cpp`, `src/kernel/crt_format.cpp`, `src/kernel/xex_modules.*`,
`src/apu/driver/null_driver.cpp`, `src/user/*`, `src/import_stub.*`, `src/gpu/*`.

## Third-party code

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (MIT), used as a submodule with the patches in `patches/XenonRecomp/`.
- [o1heap](https://github.com/pavel-kirienko/o1heap) (MIT), in `thirdparty/o1heap`, copied from Unleashed Recompiled.
- [unordered_dense](https://github.com/martinus/unordered_dense) (MIT), submodule.
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (MIT), submodule, with the patches in `patches/XenosRecomp/`.
- [plume](https://github.com/renderbag/plume) (MIT), submodule, the renderer abstraction Unleashed Recompiled uses.
- [SDL2](https://www.libsdl.org/) (zlib), from the system.
- Behaviour of several instructions and kernel functions follows [Xenia](https://github.com/xenia-project/xenia) (BSD-3-Clause); no Xenia code is included.
