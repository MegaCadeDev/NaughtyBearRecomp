# Naughty Bear Recompiled for PS5

A native PS5 homebrew build of the recompilation: the same game code and runtime as the PC
version, compiled for the PS5's CPU, rendering with Vulkan through a RADV driver built for
the console. Title ID **PPSA99862** (chosen to not clash with other homebrew; see below).

It's a homebrew title for a jailbroken console, not a retail package. It includes no game
files: you need your own copy of the Xbox 360 game, as for the PC version. Mods aren't
supported on the PS5.

**Status:** boots on a console; in testing.

## What you need

- A jailbroken PS5 that runs native homebrew titles from `/data/homebrew` (a homebrew
  enabler such as etaHEN with a homebrew launcher), with FTP access.
- A Linux PC to build it, set up for the PC version (see the main [README](../README.md)),
  plus `meson`, `bison`, `flex` and `python3-ply` for the Vulkan driver.

## Building

1. Set up the PC build and recompile the game (see the main README): the PS5 version uses
   the same recompiled code in `generated/`. You don't need to run the PC version.
2. Build the PS5 version:

   ```bash
   make -C ps5
   ```

   The first build downloads and builds its dependencies into `ps5/.deps` (the PS5 payload
   SDK, the PacBrew ports, RADV from PS5 Mesa, FFmpeg's XMA decoder and Microsoft's shader
   compiler, DXC), which takes a while. The title is in `ps5/dist/PPSA99862`.

## Installing

1. Copy the title to the console's `/data/homebrew/PPSA99862`. With FTP enabled on the
   console:

   ```bash
   make -C ps5 deploy PS5_HOST=192.168.1.50
   ```

2. Copy the game's files (the folder with `Default.xex`, `lu/`, `streams/` and the rest) into
   the title's `game` folder: `/data/homebrew/PPSA99862/game` on the console.
3. Start Naughty Bear Recompiled from the homebrew launcher (or `make -C ps5 launch
   PS5_HOST=...`, which sends a launcher payload to port 9021). As on PC, the first start
   translates the game's shaders while a progress bar is shown, into
   `/data/homebrew/PPSA99862/cache/shaders.bin`; later starts skip it.

Everything stays in the title's folder (a title can't write to `/data` outside it): saves
go to `/data/homebrew/PPSA99862/save`, and the log to `/data/homebrew/PPSA99862/log.txt`;
please include the log when reporting a problem. Deploying an update doesn't remove the
game files or saves, but `make -C ps5 undeploy` deletes the whole folder, them included.

## Controls

| Xbox 360 | DualSense |
|-|-|
| A / B / X / Y | Cross / Circle / Square / Triangle |
| LB / RB, LT / RT | L1 / R1, L2 / R2 |
| Start / Back | Options / touchpad button |
| Sticks, stick clicks, D-pad | the same |

## How it differs from the PC version

| | PC | PS5 |
|-|-|-|
| Window | SDL window | full screen on the display (`VK_KHR_display`, patches/plume/0002) |
| Guest memory | 4 GB `mmap` | 4 GB of shared memory at a fixed address (`ps5platform/shm.h`) |
| Input | SDL controllers and keyboard | the initial user's DualSense (`scePad`, src/hid/hid_ps5.cpp) |
| Audio | SDL | `sceAudioOut`, stereo (src/apu/driver/ps5_driver.cpp) |
| Shaders | translated on first run | the same, with DXC built for the console (ps5/tools/build-dxc.sh) |
| Mods | yes | no |

## The title ID

PS5 homebrew titles use IDs of the form `PPSA99xxx`, claimed first come, first served; two
titles with the same ID can't be installed together. PPSA99862 was checked against the
community catalogs ([ps5-homebrew-catalog](https://github.com/darkxex/ps5-homebrew-catalog),
Overdrive, Orbit Store, ProsperoStore) and GitHub on 2026-10-23, and nothing used it.

## Credits

The PS5 platform layer follows [UnleashedRecomp-Prospero](https://github.com/Nazky/UnleashedRecomp-Prospero)
(GPL-3.0): the build, link and signing scripts and the native app tooling in `ps5/tools`
and `ps5/tooling` come from it and from BlackBearReloaded's ps5-native-app-boilerplate
(GPL-3.0), with Mihawk-99's PS5 payload SDK and PS5 Mesa (RADV) fork. See
[NOTICE.md](../NOTICE.md).
