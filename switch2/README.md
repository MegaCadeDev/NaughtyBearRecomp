# Naughty Bear Recompiled for Switch 2

A homebrew version for the Switch 2: an NRO started from kaiHEN's hbmenu. It's the same
recompiled game and runtime as the PC version, built with
[ounce-devkit](https://github.com/ouncebrew/ounce-devkit) (libounce, the Switch 2 libnx, and
OunceVK, Mesa's NVK Vulkan driver). There are no mods or mods menu.

**Status:** builds; not yet tested on a console.

## What you need

- A Switch 2 running [kaiHEN](https://github.com/ouncebrew/kaihen), with its hbmenu.
- A Linux PC with Docker to build it, set up for the PC version (see the main
  [README](../README.md)): the Switch 2 version uses the same recompiled code in `generated/`.

## Building

1. Set up the PC build and recompile the game (see the main README). You don't need to run
   the PC version.
2. Build the Switch 2 version:

   ```bash
   make -C switch2
   ```

   The first build builds the ounce-devkit Docker image if it isn't on your machine, then its
   dependencies into `switch2/.deps` (FFmpeg's XMA decoder and Microsoft's shader compiler,
   DXC), which takes a while. The result is in `switch2/dist/switch/NaughtyBearRecomp`.

## Installing

1. Copy `switch2/dist/switch` to the root of the console's SD card, so the program is
   `/switch/NaughtyBearRecomp/NaughtyBearRecomp.nro`.
2. Copy the game's files (the folder with `Default.xex`, `lu/`, `streams/` and the rest) into
   `/switch/NaughtyBearRecomp/game` on the SD card.
3. Start hbmenu (hold R while starting a game) and choose Naughty Bear Recompiled. As on PC,
   the first start translates the game's shaders while a progress bar is shown, into
   `/switch/NaughtyBearRecomp/cache/shaders.bin`; later starts skip it.

The game needs a lot of memory (4 GB for the Xbox 360's address space alone); kaiHEN starts
hbmenu as a game, which gets the console's full share (about 9 GB).

Saves go to `/switch/NaughtyBearRecomp/save`, and the log to
`/switch/NaughtyBearRecomp/log.txt`; please include the log when reporting a problem. A crash
logs where it happened as `NaughtyBear.elf+0x...`; `switch2/tools/symbolize.sh log.txt` turns
that into function names, with the build that made the NRO.

Closing the game (from the HOME Menu, or after an error) ends the process and returns to the
HOME Menu; start hbmenu again to play again.

## Controls

| Xbox 360 | Switch 2 |
|-|-|
| A / B / X / Y | A / B / X / Y (by label) |
| LB / RB, LT / RT | L / R, ZL / ZR |
| Start / Back | + / − |
| Sticks, stick clicks, D-pad | the same |

Joy-Con 2 (attached or as a pair) and Pro Controllers work, through the system's controller
support. Rumble goes to all of them.

## No USB

The Switch 2 version never uses USB, and `switch2/tools/build.sh` fails the build if any of
libnx's USB services (`usbHs`, `usbDs`, `usbComms`) end up in the program. Under kaiHEN, a
program that used USB host (libusbhsfs, on `usb:hs`) left hbmenu black after it closed,
until kaiHEN was started again. It doesn't use the network either.

## How it differs from the PC version

| | PC | Switch 2 |
|-|-|-|
| Window | SDL window | the default display layer at 1080p (`VK_NN_vi_surface`, patches/plume/0003) |
| Guest memory | 4 GB `mmap`, committed as used | 4 GB from the heap, committed at start (src/kernel/memory.cpp) |
| Threads | system scheduler | spread over the cores the program may use (src/cpu/guest_thread.cpp) |
| Input | SDL controllers and keyboard | player 1's controller (libnx pad API, src/hid/hid_switch2.cpp) |
| Audio | SDL | `audout`, stereo (src/apu/driver/switch2_driver.cpp) |
| Crashes | signals | libnx's exception handler (src/os/crash_switch2.cpp) |
| Compiler | Clang | devkitA64's GCC (libnx's thread-local storage is GCC's); `compat/clang_builtins.h` covers the recompiled code's Clang builtins |
| Mods | yes | no |
