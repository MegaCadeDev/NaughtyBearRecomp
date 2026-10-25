#!/usr/bin/env bash
# Builds the PC and PS5 versions from a clean recompile and packages them for a release, in
# out/release:
#
#   NaughtyBearRecomp-<version>-linux-x64.tar.gz   the PC version (executable, DXC, docs)
#   NaughtyBearRecomp-<version>-ps5-PPSA99862.zip  the PS5 title folder, for /data/homebrew
#   SHA256SUMS, RELEASE_NOTES.md
#
# usage: tools/package-release.sh [--pc-only | --ps5-only] [version]
#
# The version defaults to `git describe`. Recompiling needs the game's Default.xex in
# game/Naughty_Bear; NB_GAME_DIR (the folder with Default.xex) links it there instead, for a
# fresh checkout such as the release workflow's. NB_PS5_DEPS (a folder) keeps the PS5 build's
# dependencies (several GB, slow to build) there instead of in ps5/.deps.
#
# The packages hold the built programs and never the game's files: the script checks that
# no game data (executables, packages, the shader cache, saves, mods) went in.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

build_pc=1
build_ps5=1
version=""
for arg in "$@"; do
    case $arg in
        --pc-only) build_ps5=0 ;;
        --ps5-only) build_pc=0 ;;
        -*) echo "usage: $0 [--pc-only | --ps5-only] [version]" >&2; exit 2 ;;
        *) version=$arg ;;
    esac
done
[[ -n $version ]] || version=$(git describe --tags --always --dirty)
[[ $version =~ ^[A-Za-z0-9._+-]+$ ]] || { echo "invalid version: $version" >&2; exit 2; }
name="NaughtyBearRecomp-$version"
out="$root/out/release"
title_id=PPSA99862

# The game, linked into place for a fresh checkout.
if [[ -n ${NB_GAME_DIR:-} && ! -e game/Naughty_Bear ]]; then
    [[ -f $NB_GAME_DIR/Default.xex ]] || { echo "NB_GAME_DIR has no Default.xex: $NB_GAME_DIR" >&2; exit 2; }
    ln -s "$NB_GAME_DIR" game/Naughty_Bear
fi
[[ -f game/Naughty_Bear/Default.xex ]] || { echo "No game/Naughty_Bear/Default.xex: copy the game there or set NB_GAME_DIR" >&2; exit 2; }
if [[ -n ${NB_PS5_DEPS:-} && ! -e ps5/.deps ]]; then
    mkdir -p "$NB_PS5_DEPS"
    ln -s "$NB_PS5_DEPS" ps5/.deps
fi

rm -rf "$out"
mkdir -p "$out"
assets=()

# Fails if a package holds any of the game's files.
check_contents() {
    local listing=$1 package=$2
    if grep -iE '(\.xex|\.lu|\.str|\.xma|shaders\.bin)$|(^|/)(save|mods|generated)/' "$listing"; then
        echo "error: $package holds game data (above)" >&2
        exit 1
    fi
}

echo "==> Recompiling the game"
git submodule update --init --recursive
tools/recompile.sh

if (( build_pc )); then
    echo "==> PC version"
    CC=clang CXX=clang++ cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build build/release --target NaughtyBear
    stage="$out/stage/$name-linux-x64"
    mkdir -p "$stage/game"
    cp build/release/NaughtyBear build/release/libdxcompiler.so "$stage/"
    strip "$stage/NaughtyBear"
    cp README.md NOTICE.md "$stage/"
    cp docs/modding.md "$stage/MODDING.md"
    cp game/README.md "$stage/game/README.md"
    tar -C "$out/stage" -czf "$out/$name-linux-x64.tar.gz" "$name-linux-x64"
    tar -tzf "$out/$name-linux-x64.tar.gz" > "$out/stage/pc.txt"
    check_contents "$out/stage/pc.txt" "the PC package"
    assets+=("$name-linux-x64.tar.gz")
fi

if (( build_ps5 )); then
    echo "==> PS5 version"
    make -C ps5 TITLE_ID=$title_id
    rm -f "$out/$name-ps5-$title_id.zip"
    (cd ps5/dist && zip -qr "$out/$name-ps5-$title_id.zip" "$title_id")
    zipinfo -1 "$out/$name-ps5-$title_id.zip" > "$out/stage/ps5.txt"
    check_contents "$out/stage/ps5.txt" "the PS5 package"
    assets+=("$name-ps5-$title_id.zip")
fi

(cd "$out" && sha256sum "${assets[@]}" > SHA256SUMS)

cat > "$out/RELEASE_NOTES.md" <<NOTES
Naughty Bear Recompiled $version, built from $(git rev-parse --short HEAD).

**You need your own copy of the game.** These downloads contain no game files: copy the
files from your own disc (the folder with \`Default.xex\`, \`lu/\` and the rest) into the
\`game\` folder.

### PC (Linux, x64)

\`$name-linux-x64.tar.gz\`: extract it, copy the game into its \`game\` folder and run
\`./NaughtyBear\`. Needs SDL2 and a Vulkan driver (\`sudo apt install libsdl2-2.0-0
libvulkan1\`). The first start prepares the game's shaders (a few seconds).

### PS5 (jailbroken, native homebrew)

\`$name-ps5-$title_id.zip\`: copy the \`$title_id\` folder to \`/data/homebrew/$title_id\` on
the console, then the game's files into \`/data/homebrew/$title_id/game\`. Start it from the
homebrew launcher. The first start translates the game's shaders, with a progress bar.

See README.md and NOTICE.md (in the PC download, and in the repository) for details and
licences. Checksums are in SHA256SUMS.
NOTES

rm -rf "$out/stage"
echo "==> Packaged into $out:"
ls -la "$out"
