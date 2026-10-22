#!/usr/bin/env bash
# Translate the shaders dumped from the game (shaders/dump, written while it runs) into a
# SPIR-V shader cache with XenosRecomp.
# Set XENOS_DUMP_HLSL=<dir> to also keep the intermediate HLSL.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

tools/apply_patches.sh > /dev/null
CC=clang CXX=clang++ cmake -S tools/XenosRecomp -B build/xenosrecomp -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build/xenosrecomp

mkdir -p build/shaders
# XenosRecomp looks for DXC next to its working directory.
cd build/xenosrecomp/XenosRecomp
./XenosRecomp "$root/shaders/dump" "$root/build/shaders/shader_cache.cpp" "$root/tools/XenosRecomp/XenosRecomp/shader_common.h"
echo "Shader cache: build/shaders/shader_cache.cpp ($(grep -c '{ 0x' "$root/build/shaders/shader_cache.cpp") shaders)"
