#!/usr/bin/env bash
# Builds Microsoft's DirectX Shader Compiler (DXC) for the Switch 2, as static libraries in
# switch2/.deps/dxc, so the Switch 2 version translates the game's shaders itself, as the PC
# and PS5 versions do. The same release as theirs, v1.8.2407, so all produce the same
# SPIR-V. Runs in the devkit image (switch2/tools/devkit.sh):
#
#   1. DXC's TableGen tools, built for the image's own system;
#   2. dxcompiler and the libraries it uses, with libounce's CMake toolchain (devkitA64's
#      GCC). dxcompiler is a shared library upstream; it's built static here. Functions
#      newlib lacks are in switch2/compat (headers in include/, definitions in posix_extra.c).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
repo=$(cd -- "$root/.." && pwd)
version=v1.8.2407
source="$root/.deps/src/DirectXShaderCompiler"
host="$root/.deps/work/dxc-host"
build="$root/.deps/work/dxc-switch2"
prefix="$root/.deps/dxc"
logs="$root/.deps/work"

# The patches in switch2/patches/dxc, and the build script's own changes below, are part of
# the version stamp: changing them rebuilds the libraries.
stamp="$version+$(cat "$root"/patches/dxc/*.patch "${BASH_SOURCE[0]}" | sha1sum | cut -c1-12)"
if [[ -f $prefix/.version && $(<"$prefix/.version") == "$stamp" ]]; then
    exit 0
fi

mkdir -p "$logs"
if [[ ! -d $source/.git ]]; then
    mkdir -p "$(dirname "$source")"
    if [[ -d $repo/ps5/.deps/src/DirectXShaderCompiler/.git ]]; then
        cp -a "$repo/ps5/.deps/src/DirectXShaderCompiler" "$source"
    else
        git clone -q https://github.com/microsoft/DirectXShaderCompiler.git "$source"
    fi
fi
if [[ $(git -C "$source" describe --tags --exact-match 2>/dev/null) != "$version" ]]; then
    git -C "$source" fetch -q --tags origin
    git -C "$source" checkout -q -f "$version"
fi
git -C "$source" submodule update --init --recursive -q
# dxcompiler as a static library (only in this checkout, which the build owns).
sed -i 's/^add_clang_library(dxcompiler SHARED /add_clang_library(dxcompiler STATIC /' "$source/tools/clang/tools/dxcompiler/CMakeLists.txt"
# DXC converts its wide strings after setlocale(LC_ALL, "en_US.UTF-8") and puts the old
# locale back, around every conversion: process-wide changes, so translations on several
# threads change it under each other. newlib's default locale converts the ASCII shaders
# DXC handles, so on the Switch the locale is left alone.
grep -q "__SWITCH__" "$source/lib/DxcSupport/Unicode.cpp" || sed -i \
    -e 's/^  const char \*prevLocale = setlocale(LC_ALL, nullptr);$/#ifndef __SWITCH__\n  const char *prevLocale = setlocale(LC_ALL, nullptr);/' \
    -e 's/^  setlocale(LC_ALL, "en_US.UTF-8");$/  setlocale(LC_ALL, "en_US.UTF-8");\n#endif/' \
    -e 's/^  if (prevLocale)$/#ifndef __SWITCH__\n  if (prevLocale)/' \
    -e 's/^    setlocale(LC_ALL, prevLocale);$/    setlocale(LC_ALL, prevLocale);\n#endif/' \
    "$source/lib/DxcSupport/Unicode.cpp"
for patch in "$root"/patches/dxc/*.patch; do
    git -C "$source" apply --reverse --check "$patch" 2> /dev/null || git -C "$source" apply "$patch"
done

# SPIRV-Tools lists the systems it builds for: the Switch is built as FreeBSD (which only
# decides whether its timers are on).
grep -q NintendoSwitch "$source/external/SPIRV-Tools/CMakeLists.txt" || sed -i \
    's/^elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "FreeBSD")$/elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "FreeBSD" OR "${CMAKE_SYSTEM_NAME}" STREQUAL "NintendoSwitch")/' \
    "$source/external/SPIRV-Tools/CMakeLists.txt"

echo "==> [dxc] TableGen tools for the image"
cmake -S "$source" -B "$host" -G Ninja -C "$source/cmake/caches/PredefinedParams.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DHLSL_INCLUDE_TESTS=OFF -DSPIRV_BUILD_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF \
    -DCLANG_INCLUDE_TESTS=OFF > "$logs/dxc-host-configure.log"
cmake --build "$host" --target llvm-tblgen clang-tblgen > "$logs/dxc-host-build.log"

echo "==> [dxc] dxcompiler for the Switch 2"
# newlib's headers lack a few POSIX declarations: compat/include has them, and two signal
# flags LLVM's crash handler sets (it never runs). They're given in CFLAGS and CXXFLAGS,
# which CMake adds to the toolchain's flags: -DCMAKE_CXX_FLAGS would replace those, and
# with them -mtp=soft and -ftls-model=local-exec, libnx's thread-local storage (without
# them DXC's thread_local variables fault), and __SWITCH__.
compat_flags="-isystem $root/compat/include -DSA_NODEFER=0x40000000 -DSA_RESETHAND=0x80000000"
rm -rf "$build"
CFLAGS="$compat_flags" CXXFLAGS="$compat_flags -Wno-error" \
cmake -S "$source" -B "$build" -G Ninja -C "$source/cmake/caches/PredefinedParams.cmake" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/libounce/cmake/Switch2.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=NintendoSwitch -DUNIX=ON \
    -DLLVM_TABLEGEN="$host/bin/llvm-tblgen" -DCLANG_TABLEGEN="$host/bin/clang-tblgen" \
    -DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-none-elf -DLLVM_HOST_TRIPLE=aarch64-none-elf \
    -DHLSL_INCLUDE_TESTS=OFF -DSPIRV_BUILD_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_TESTS=OFF \
    -DLLVM_BUILD_TOOLS=OFF -DLLVM_INCLUDE_UTILS=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_THREADS=ON -DLLVM_APPEND_VC_REV=OFF -DLLVM_ENABLE_WERROR=OFF \
    -DHAVE_GETPAGESIZE=1 \
    > "$logs/dxc-switch2-configure.log"
# dxcompiler, and the libraries it uses that its static build doesn't pull in itself.
cmake --build "$build" --target dxcompiler LLVMPassPrinters LLVMDxilCompression LLVMDxilPIXPasses > "$logs/dxc-switch2-build.log"

rm -rf "$prefix"
mkdir -p "$prefix/lib" "$prefix/include"
cp "$build"/lib/*.a "$prefix/lib/"
cp -r "$source/include/dxc" "$prefix/include/"
printf '%s\n' "$stamp" > "$prefix/.version"
echo "==> [dxc] built DXC $version into $prefix"
