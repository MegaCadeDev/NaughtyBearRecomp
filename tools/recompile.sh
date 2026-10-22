#!/usr/bin/env bash
# Regenerate generated/ppc from the game executable: patch and build XenonRecomp,
# refresh the jump tables, then run the recompiler.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

tools/apply_patches.sh
CC=clang CXX=clang++ cmake -S tools/XenonRecomp -B tools/XenonRecomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build tools/XenonRecomp/build
CC=clang CXX=clang++ cmake -S tools/nbanalyse -B build/nbanalyse -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build/nbanalyse

build/nbanalyse/nb_switch_tables game/Naughty_Bear/Default.xex config/NB_switch_tables.toml config/NB.toml
rm -rf generated/ppc && mkdir -p generated/ppc
tools/XenonRecomp/build/XenonRecomp/XenonRecomp config/NB.toml tools/XenonRecomp/XenonUtils/ppc_context.h > generated/recomp.log 2>&1
echo "Recompiled. Warnings: $(grep -vc '^Recompiling' generated/recomp.log || true) (see generated/recomp.log)"
