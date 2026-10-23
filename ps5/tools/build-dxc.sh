#!/usr/bin/env bash
# Builds Microsoft's DirectX Shader Compiler (DXC) for the PS5, as static libraries in
# .deps/dxc, so the PS5 version translates the game's shaders itself, as the PC version
# does. It's the release the PC version's prebuilt DXC (from XenosRecomp) is, v1.8.2407, so
# both produce the same SPIR-V.
#
#   1. DXC's TableGen tools, built for this machine (cross-compiling LLVM needs them);
#   2. dxcompiler and the LLVM, Clang and SPIRV-Tools libraries it uses, for the PS5 with the
#      payload SDK's CMake toolchain. dxcompiler is a shared library upstream; it's built
#      static here, since a native PS5 title can't load Linux shared libraries.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk="${PS5_PAYLOAD_SDK:-$root/.deps/native/ps5-payload-sdk}"
version=v1.8.2407
source="$root/.deps/src/DirectXShaderCompiler"
host="$root/.deps/work/dxc-host"
build="$root/.deps/work/dxc-ps5"
prefix="$root/.deps/dxc"

if [[ -f $prefix/.version && $(<"$prefix/.version") == "$version" ]]; then
    exit 0
fi

if [[ ! -d $source/.git ]]; then
    mkdir -p "$(dirname "$source")"
    git clone -q https://github.com/microsoft/DirectXShaderCompiler.git "$source"
fi
if [[ $(git -C "$source" describe --tags --exact-match 2>/dev/null) != "$version" ]]; then
    git -C "$source" fetch -q --tags origin
    git -C "$source" checkout -q "$version"
fi
git -C "$source" submodule update --init --recursive -q
# dxcompiler as a static library (only in this checkout, which the build owns).
sed -i 's/^add_clang_library(dxcompiler SHARED /add_clang_library(dxcompiler STATIC /' "$source/tools/clang/tools/dxcompiler/CMakeLists.txt"

echo "==> [dxc] TableGen tools for the host"
cmake -S "$source" -B "$host" -G Ninja -C "$source/cmake/caches/PredefinedParams.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DHLSL_INCLUDE_TESTS=OFF -DSPIRV_BUILD_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF \
    -DCLANG_INCLUDE_TESTS=OFF > "$root/.deps/work/dxc-host-configure.log"
cmake --build "$host" --target llvm-tblgen clang-tblgen > "$root/.deps/work/dxc-host-build.log"

echo "==> [dxc] dxcompiler for the PS5"
# The SDK's math functions are in libc and it has no libm, which LLVM's configure checks
# link with: an empty one.
stubs="$root/.deps/work/dxc-stubs"
mkdir -p "$stubs"
[[ -f $stubs/libm.a ]] || ar rc "$stubs/libm.a"
cmake -S "$source" -B "$build" -G Ninja -C "$source/cmake/caches/PredefinedParams.cmake" \
    -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF \
    -DLLVM_TABLEGEN="$host/bin/llvm-tblgen" -DCLANG_TABLEGEN="$host/bin/clang-tblgen" \
    -DHLSL_INCLUDE_TESTS=OFF -DSPIRV_BUILD_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_TESTS=OFF \
    -DLLVM_BUILD_TOOLS=OFF -DLLVM_INCLUDE_UTILS=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_APPEND_VC_REV=OFF -DCMAKE_C_FLAGS="-femulated-tls" -DCMAKE_CXX_FLAGS="-femulated-tls" \
    -DCMAKE_EXE_LINKER_FLAGS="-L$stubs" \
    > "$root/.deps/work/dxc-ps5-configure.log"
# dxcompiler, and the libraries it uses that its static build doesn't pull in itself.
cmake --build "$build" --target dxcompiler LLVMPassPrinters LLVMDxilCompression LLVMDxilPIXPasses > "$root/.deps/work/dxc-ps5-build.log"

rm -rf "$prefix"
mkdir -p "$prefix/lib" "$prefix/include"
cp "$build"/lib/*.a "$prefix/lib/"
cp -r "$source/include/dxc" "$prefix/include/"
printf '%s\n' "$version" > "$prefix/.version"
echo "==> [dxc] built DXC $version into $prefix"
