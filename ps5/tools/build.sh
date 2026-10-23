#!/usr/bin/env bash
# Builds the PS5 version: a native homebrew title in dist/<TITLE_ID>, ready to copy to the
# console's /data/homebrew/<TITLE_ID>.
#
#   1. dependencies: the PS5 payload SDK, PacBrew ports, RADV and FFmpeg (into ps5/.deps);
#   2. the game and runtime, compiled with ps5/CMakeLists.txt;
#   3. the link with RADV and the PS5 libc shims (tools/radv-link.sh), then the conversion
#      and signing into eboot.bin with the native app tool (tooling/native);
#   4. the title folder: eboot.bin, sce_sys (param.json, icon), sce_module/libc.prx, and the
#      shader cache the PC version built, if there is one.
#
# Adapted from UnleashedRecomp-Prospero's tools/build.sh (GPL-3.0), itself built on
# BlackBearReloaded's ps5-native-app-boilerplate.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
REPO=$(cd -- "$ROOT/.." && pwd)
TITLE_ID="${TITLE_ID:-PPSA99862}"
[[ $TITLE_ID =~ ^PPS[AB][0-9]{5}$ ]] || { echo "invalid PS5 title ID: $TITLE_ID" >&2; exit 2; }

DEPS="$ROOT/.deps"
SDK_ROOT="$DEPS/native/ps5-payload-sdk"
PACBREW="$DEPS/pacbrew/v0.40.2/sysroot/user/homebrew"
RADV_ARCHIVE="$DEPS/native/radv-release/lib/libvulkan_radeon.ps5.a"
FFMPEG="$DEPS/ffmpeg"
BUILD="$ROOT/build"
OBJ="$BUILD/obj"
DIST="$ROOT/dist/$TITLE_ID"
JOBS="${BUILD_JOBS:-$(nproc)}"

# The ELF's module SDK version, the companion libc's, and the fake SELF magic the
# homebrew loaders accept (as UnleashedRecomp-Prospero).
MODULE_SDK=0x02000009
COMPANION_SDK=0x08050001
FSELF_MAGIC=0x1D3D154F

echo "==> [1/4] Dependencies"
[[ -x $SDK_ROOT/bin/prospero-lld ]] || bash "$ROOT/tools/setup-native-dependencies.sh"
[[ -d $PACBREW ]] || bash "$ROOT/tools/setup-pacbrew-dependencies.sh" --all > /dev/null
# Mesa needs Meson 1.4 or newer (Ubuntu 24.04 has 1.3): a private copy, with the Python
# modules Mesa's build scripts use.
if [[ ! -x $DEPS/tools-venv/bin/meson ]]; then
    python3 -m venv "$DEPS/tools-venv"
    "$DEPS/tools-venv/bin/pip" install -q "meson==1.5.2" mako pyyaml ply packaging
fi
[[ -f $RADV_ARCHIVE ]] || MESON="$DEPS/tools-venv/bin/meson" PATH="$DEPS/tools-venv/bin:$PATH" bash "$ROOT/tools/build-radv.sh" release
bash "$ROOT/tools/build-ffmpeg.sh"
export PS5_PAYLOAD_SDK="$SDK_ROOT"

echo "==> [2/4] Compiling the game and runtime"
[[ -f $REPO/generated/ppc/ppc_func_mapping.cpp ]] || { echo "No recompiled code in generated/ppc: run tools/recompile.sh first." >&2; exit 2; }
cmake -S "$ROOT" -B "$BUILD/cmake" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$SDK_ROOT/toolchain/prospero.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF > "$BUILD/cmake-configure.log"
cmake --build "$BUILD/cmake" -j "$JOBS"

echo "==> [3/4] Linking and signing eboot.bin"
mkdir -p "$OBJ" "$BUILD/stubs" "$BUILD/host"
NATIVE="$ROOT/tooling/native"
NATIVE_TOOL="$BUILD/host/ps5-native-tool"
if [[ ! -x $NATIVE_TOOL ]]; then
    zlib=$(find "$DEPS/native/zlib/root" -type f -name libz.a -print -quit)
    "${CXX:-c++}" -std=c++20 -O2 -I "$DEPS/native/zlib/root/usr/include" \
        "$NATIVE/native_app_builder.cpp" "$NATIVE/self_container.cpp" "$NATIVE/elf_object.cpp" "$NATIVE/sce_module_writer.cpp" \
        "$zlib" -o "$NATIVE_TOOL"
fi
cc() { sh "$ROOT/tooling/prospero-clang18" "$@"; }

# The title's C runtime start-up, and link stubs for the AGC libraries RADV calls into.
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$NATIVE/app_crt.cpp" -o "$OBJ/app_crt.o"
cc -std=c11 -O2 -c "$NATIVE/nb-libc-extra.c" -o "$OBJ/nb-libc-extra.o"
for stub in libSceAgc:agc_canary_link_stub.c libSceAgcDriver:agc_driver_canary_link_stub.c; do
    library=${stub%%:*}
    source=${stub#*:}
    cc -std=c11 -O2 -fPIC -c "$ROOT/vendor/ps5/sdk/stubs/$source" -o "$OBJ/${library}_stub.o"
    "$SDK_ROOT/bin/prospero-lld" --shared -soname "${library}.prx" -o "$BUILD/stubs/${library}.so" "$OBJ/${library}_stub.o"
done

source "$ROOT/tools/radv-link.sh"
radv_link_recipe "$ROOT" "$SDK_ROOT" "$RADV_ARCHIVE"
# The runtime's PPC_FUNC overrides replace the generated code's weak definitions, so both
# libraries go in whole.
"$SDK_ROOT/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$NATIVE/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$BUILD/naughtybear.elf" \
    "$OBJ/app_crt.o" "$OBJ/nb-libc-extra.o" \
    --whole-archive "$BUILD/cmake/libnb_runtime.a" "$BUILD/cmake/libnb_recomp.a" --no-whole-archive \
    "$BUILD/cmake/libnb_plume.a" "$BUILD/cmake/libnb_thirdparty.a" \
    --start-group "$FFMPEG/lib/libavcodec.a" "$FFMPEG/lib/libavutil.a" "$PACBREW/lib/libzstd.a" --end-group \
    "$BUILD/stubs/libSceAgc.so" "$BUILD/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$SDK_ROOT"/target/lib/*.so

"$NATIVE_TOOL" link --in "$BUILD/naughtybear.elf" --out "$BUILD/eboot.elf" \
    --stub-dir "$SDK_ROOT/target/lib" --stub "$BUILD/stubs/libSceAgc.so" --stub "$BUILD/stubs/libSceAgcDriver.so" \
    --module-sdk "$MODULE_SDK" --companion-sdk "$COMPANION_SDK" --file-name eboot.elf

echo "==> [4/4] Staging $DIST"
rm -rf -- "$DIST"
mkdir -p "$DIST/sce_sys" "$DIST/sce_module"
"$NATIVE_TOOL" self --sign --in "$BUILD/eboot.elf" --out "$DIST/eboot.bin" --magic "$FSELF_MAGIC"
cp "$ROOT/sce_sys/param.json" "$ROOT/sce_sys/icon0.png" "$DIST/sce_sys/"
[[ -f $ROOT/runtime/libc.prx ]] || bash "$ROOT/tools/rebuild-libc.sh"
cp "$ROOT/runtime/libc.prx" "$DIST/sce_module/libc.prx"
# The shader cache comes from the PC version, built from the same game files (it's derived
# from the game, so it's never committed or distributed: it stays on this machine).
if [[ -f $REPO/cache/shaders.bin ]]; then
    mkdir -p "$DIST/cache"
    cp "$REPO/cache/shaders.bin" "$DIST/cache/shaders.bin"
else
    echo "warning: no cache/shaders.bin: run the PC version once first, or the PS5 version will have no shaders" >&2
fi
"$NATIVE_TOOL" self --inspect --file "$DIST/eboot.bin" > /dev/null
printf 'Built %s (eboot.bin %s bytes)\n' "$DIST" "$(stat -c %s "$DIST/eboot.bin")"
