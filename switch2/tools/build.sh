#!/usr/bin/env bash
# Builds the Switch 2 version: a homebrew NRO for kaiHEN's hbmenu, in
# dist/switch/NaughtyBearRecomp, ready to copy to the SD card's /switch folder.
#
#   1. the devkit: ounce-devkit's Docker image (devkitPro's devkitA64 with libounce, OunceVK
#      and kaiHEN's library), built from its source when it isn't on this machine;
#   2. dependencies, in that image: FFmpeg's XMA decoder, DXC, and OunceVK prepared for volk
#      (into switch2/.deps);
#   3. the game and runtime, compiled and linked into the NRO with switch2/CMakeLists.txt;
#   4. a check that nothing in the program uses USB (see below), and the folder for the SD
#      card.
#
# No USB: under kaiHEN, a program that used USB host (libusbhsfs, on usb:hs) left hbmenu
# black after it closed, until kaiHEN was started again (found with the Dolphin-NS2 port).
# Nothing here uses it, and step 4 fails the build if any USB function is linked in.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
REPO=$(cd -- "$ROOT/.." && pwd)
BUILD="$ROOT/build"
DIST="$ROOT/dist/switch/NaughtyBearRecomp"
DEVKIT="$ROOT/tools/devkit.sh"
# The ounce-devkit release the port was made with (devkit 20261114: libounce 9382b1e,
# OunceVK f5afcd2, kaiHEN eb81e19).
DEVKIT_REVISION=f347815
export DEVKIT_IMAGE="${DEVKIT_IMAGE:-ounce-devkit:20261114}"

echo "==> [1/4] Devkit ($DEVKIT_IMAGE)"
if ! docker image inspect "$DEVKIT_IMAGE" > /dev/null 2>&1; then
    source="$ROOT/.deps/ounce-devkit"
    [[ -d $source/.git ]] || git clone -q https://github.com/ouncebrew/ounce-devkit.git "$source"
    git -C "$source" checkout -q "$DEVKIT_REVISION"
    # --network=host: Docker's own DNS doesn't work on every machine.
    docker build --network=host -t "$DEVKIT_IMAGE" "$source"
fi

echo "==> [2/4] Dependencies"
[[ -f $REPO/generated/ppc/ppc_func_mapping.cpp ]] || { echo "No recompiled code in generated/ppc: run tools/recompile.sh first." >&2; exit 2; }
"$REPO/tools/apply_patches.sh" > /dev/null
"$DEVKIT" bash switch2/tools/build-ffmpeg.sh
"$DEVKIT" bash switch2/tools/prepare-ouncevk.sh
"$DEVKIT" bash switch2/tools/build-dxc.sh

echo "==> [3/4] Compiling and linking"
mkdir -p "$BUILD"
"$DEVKIT" sh -c 'cmake -S switch2 -B switch2/build/cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/libounce/cmake/Switch2.cmake \
    -DCMAKE_BUILD_TYPE=Release -DNB_VERSION="$0" > switch2/build/cmake-configure.log && cmake --build switch2/build/cmake' "${NB_VERSION:-1.0.0}"

echo "==> [4/4] Checking and staging $DIST"
# Any of libnx's USB services (usb:hs host, usb:ds device, usbComms) linked in fails the build.
usb=$("$DEVKIT" sh -c '$DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm switch2/build/cmake/NaughtyBear.elf' | grep -E ' [TtDdBb] (usbHs|usbDs|usbComms|usbHsFs)' || true)
if [[ -n $usb ]]; then
    printf 'error: the program uses USB, which breaks hbmenu under kaiHEN:\n%s\n' "$usb" >&2
    exit 1
fi
rm -rf -- "$DIST"
mkdir -p "$DIST/game"
cp "$BUILD/cmake/NaughtyBear.nro" "$DIST/NaughtyBearRecomp.nro"
printf 'Copy the game'"'"'s files here: the folder with Default.xex, lu/, streams/ and the rest.\n' > "$DIST/game/PUT_GAME_FILES_HERE.txt"
printf 'Built %s (NaughtyBearRecomp.nro %s bytes)\n' "$DIST" "$(stat -c %s "$DIST/NaughtyBearRecomp.nro")"
